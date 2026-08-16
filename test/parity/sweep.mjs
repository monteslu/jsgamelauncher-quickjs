#!/usr/bin/env node
/**
 * Phase 6: corpus parity sweep.
 *
 * Runs every real game in ../jsgames under this runtime and records what actually
 * happens — not whether the process exited zero.
 *
 * The rules this encodes, all of which were learned the expensive way:
 *
 *  - A blank frame is a FAILURE. A game that boots, clears, and draws nothing has
 *    not run. Colour count is the liveness signal (>1 means it drew), and it is a
 *    signal, never a fixture: exact counts drift between runs and GPUs.
 *  - Distinct CRCs across captured frames prove ANIMATION. A game that renders one
 *    correct frame and then freezes passes every static check ever written.
 *  - The three wasm titles are EXPECTED to fail, and must fail CLEANLY: a named
 *    error pointing at jsgamelauncher, never a crash, hang, or silent black screen.
 *    That expectation is asserted, so "wasm support quietly appeared" would also
 *    show up here.
 *  - Every failure records its actual error text. "Failed" with no reason is a
 *    result nobody can act on.
 */
import { spawnSync } from 'node:child_process';
import { readdirSync, existsSync, writeFileSync, mkdirSync, statSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const CORPUS = resolve(ROOT, '..', 'jsgames');
const BIN = process.env.JSGLQ_BIN || join(ROOT, 'build', 'jsglq');
const OUT = join(ROOT, 'test', 'parity', 'results');

/* Games whose runtime need is WebAssembly, which is deferred post-v1 on purpose:
   WAMR trails V8 at wasm by more than QuickJS trails V8 at JS, so these belong on
   jsgamelauncher. They must fail cleanly and say so. */
const WASM_TITLES = new Set(['simple-box2d', 'simple-box2d3', 'box3d-pyramid']);

/* Injected into each game to report what was actually rendered. Uses only the
   public API a game itself would use, so it measures the shipped path. */
const PROBE = `
(function () {
  /*
   * Sample the canvas on a TIMER, not by wrapping requestAnimationFrame.
   *
   * A bundled game commonly captures requestAnimationFrame into a local before
   * this probe installs, so a wrapper never sees a single frame and the game looks
   * like it never rendered. A timer observes the surface no matter how the game
   * scheduled its loop.
   */
  const AT_MS = [1500, 2500, 3500];
  const shots = [];
  let idx = 0;
  function sample() {
        try {
          /*
           * Find the canvas that is actually being drawn into.
           *
           * A game may render into its own canvas rather than the document's (an
           * engine building its own is common), and reading the wrong one reports
           * "never rendered" for a game that is rendering perfectly. Prefer the
           * display canvas, but fall back to any canvas that has a live context.
           */
          let c = globalThis.__jsglq_displayCanvas;
          if (c && !c._ctx && globalThis.__jsglq_allCanvases) {
            c = globalThis.__jsglq_allCanvases.find((k) => k && k._ctx) || c;
          }
          if (c) {
            let px = null;
            if (c._ctxType === '2d' && c._ctx) {
              px = c._ctx.getImageData(0, 0, c.width, c.height);
            } else if (c._ctxType === 'webgl2' && c._ctx) {
              const gl = c._ctx;
              const w = gl.drawingBufferWidth, h = gl.drawingBufferHeight;
              const buf = new Uint8Array(w * h * 4);
              gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, buf);
              px = { width: w, height: h, data: buf };
            }
            if (px) {
              // Stride-sample: a full 2M-pixel scan in interpreted JS costs more
              // than a frame on its own. Colour count is a liveness signal, not a
              // fixture, so every 16th pixel answers the same question far cheaper.
              const seen = new Set();
              let hash = 0x811c9dc5;
              for (let i = 0; i < px.data.length; i += 64) {
                seen.add((px.data[i]<<24|px.data[i+1]<<16|px.data[i+2]<<8|px.data[i+3])>>>0);
                hash ^= px.data[i]; hash = Math.imul(hash, 0x01000193) >>> 0;
              }
              shots.push({ at: AT_MS[idx], colors: seen.size, crc: hash.toString(16) });
            } else {
              shots.push({ at: AT_MS[idx], colors: 0, crc: 'nocontext' });
            }
          } else {
            shots.push({ at: AT_MS[idx], colors: 0, crc: 'nocanvas' });
          }
        } catch (e) {
          shots.push({ at: AT_MS[idx], colors: 0, crc: 'error', err: String(e && e.message) });
        }
    idx++;
    if (idx < AT_MS.length) {
      setTimeout(sample, AT_MS[idx] - AT_MS[idx - 1]);
    } else {
      console.log('PARITY:' + JSON.stringify(shots));
      // Nothing further to observe: let the host stop rather than idle to its cap.
      if (globalThis.__jsglq_requestExit) globalThis.__jsglq_requestExit();
    }
  }
  setTimeout(sample, AT_MS[0]);
})();
`;

function runGame(dir, name) {
  const started = Date.now();
  const r = spawnSync(BIN,
    ['--headless', '--frames=4000', '--max-seconds=11', '--width=640', '--height=480', dir],
    { encoding: 'utf8', timeout: 25000, killSignal: 'SIGKILL',
      env: { ...process.env, JSGLQ_PROBE: PROBE } });
  const elapsed = Date.now() - started;
  const out = (r.stdout || '') + (r.stderr || '');

  const parity = /PARITY:(\[.*?\])/.exec(out);
  let shots = null;
  if (parity) { try { shots = JSON.parse(parity[1]); } catch { /* keep null */ } }

  // First real error line, for a failure that can be acted on.
  const errLine = (out.split('\n').find((l) =>
    /threw:|UNHANDLED|not implemented|cannot resolve|Error:|FAILED/.test(l)) || '').trim();

  return { out, shots, elapsed, exit: r.status, error: errLine, timedOut: r.error?.code === 'ETIMEDOUT' };
}

function classify(name, res) {
  const isWasm = WASM_TITLES.has(name);

  if (isWasm) {
    // Expected to fail, and specifically to fail CLEANLY.
    const named = /WebAssembly|wasm/i.test(res.out);
    if (res.timedOut) {
      return { status: 'BAD-WASM-HANG', detail: 'hung instead of failing cleanly' };
    }
    if (named) {
      return { status: 'EXPECTED-WASM', detail: 'declined with a wasm-related error, as designed' };
    }
    return { status: 'BAD-WASM-SILENT',
             detail: res.error || 'failed without naming WebAssembly' };
  }

  if (res.timedOut) return { status: 'HANG', detail: `no result in ${res.elapsed}ms` };
  if (!res.shots || !res.shots.length) {
    return { status: 'NO-RENDER', detail: res.error || 'never produced a frame report' };
  }

  const colors = res.shots.map((s) => s.colors);
  const crcs = res.shots.map((s) => s.crc);
  const drew = colors.every((c) => c > 1);
  const animated = new Set(crcs).size > 1;

  if (!drew) {
    return { status: 'BLANK',
             detail: `colour counts ${colors.join('/')} — blank frames are failures` };
  }
  if (!animated) {
    /*
     * A static frame is NOT automatically a failure.
     *
     * The sweep drives no input, so a title screen, a menu, or a turn-based game
     * waiting on a keypress legitimately renders the same frame forever. Calling
     * that "frozen" would be a false failure, and false failures are how a suite
     * stops being believed.
     *
     * What distinguishes them is whether a real scene was drawn at all: a game
     * showing a rich static screen (many colours) is idle; one showing a nearly
     * empty frame is much more likely stuck. The distinction is reported honestly
     * either way rather than being hidden inside a PASS.
     */
    const rich = colors[0] > 16;
    return {
      status: rich ? 'STATIC-IDLE' : 'STATIC-SPARSE',
      detail: rich
        ? `rendered ${colors[0]} colours but never changed — consistent with a ` +
          `title/menu screen awaiting input (the sweep drives none)`
        : `only ${colors[0]} colours and no change — likely stuck`,
    };
  }
  return { status: 'PASS', detail: `colours ${colors.join('/')}, ${new Set(crcs).size} distinct frames` };
}

function main() {
  if (!existsSync(BIN)) { console.error(`binary not found: ${BIN}`); process.exit(1); }
  if (!existsSync(CORPUS)) { console.error(`corpus not found: ${CORPUS}`); process.exit(1); }

  const only = process.argv.slice(2).filter((a) => !a.startsWith('--'));
  const games = readdirSync(CORPUS)
    .filter((n) => statSync(join(CORPUS, n)).isDirectory())
    .filter((n) => !n.startsWith('.') && n !== 'node_modules' && n !== 'installers')
    .filter((n) => existsSync(join(CORPUS, n, 'package.json')))
    .filter((n) => !only.length || only.includes(n))
    .sort();

  console.log(`=== corpus parity sweep: ${games.length} games ===\n`);

  const results = [];
  for (const name of games) {
    process.stdout.write(name.padEnd(22));
    const res = runGame(join(CORPUS, name), name);
    const verdict = classify(name, res);
    results.push({ name, ...verdict, elapsed: res.elapsed, shots: res.shots });
    const tag = verdict.status.padEnd(16);
    console.log(`${tag} ${verdict.detail}`);
  }

  mkdirSync(OUT, { recursive: true });
  writeFileSync(join(OUT, 'sweep.json'), JSON.stringify(results, null, 2));

  const pass = results.filter((r) => r.status === 'PASS').length;
  const idle = results.filter((r) => r.status === 'STATIC-IDLE').length;
  const expected = results.filter((r) => r.status === 'EXPECTED-WASM').length;
  const bad = results.filter((r) =>
    !['PASS', 'STATIC-IDLE', 'EXPECTED-WASM'].includes(r.status));

  console.log(`\n${pass} animating, ${idle} rendering (idle without input), ` +
              `${expected} correctly declined (wasm), ${bad.length} failing`);
  if (bad.length) {
    console.log('\nFailures:');
    for (const b of bad) console.log(`  ${b.name}: ${b.status} — ${b.detail}`);
  }
  console.log(`\nwrote ${join(OUT, 'sweep.json')}`);
  process.exit(bad.length ? 1 : 0);
}

main();
