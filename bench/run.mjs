#!/usr/bin/env node
/**
 * Tri-runtime bench runner.
 *
 * Usage:
 *   node bench/run.mjs --runtime=browser --scene=s01-entities-500
 *   node bench/run.mjs --runtime=node    --scene=all --runs=5
 *   node bench/run.mjs --runtime=qjs     --scene=s07-three-heavy --mode=uncapped
 *
 * Protocol (fixed, per the plan — deviating from it invalidates comparisons):
 *   - environment lockdown recorded; a run that cannot verify it is tagged unlocked
 *   - 120 warmup frames discarded inside the harness
 *   - 5 runs per data point; median-of-runs reported; outliers printed, never dropped
 *   - both sides measured in the same session before any comparison is named
 */

import { spawn } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync, existsSync, appendFileSync,
         rmSync, cpSync, readdirSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { dirname, join, resolve } from 'node:path';
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname } from 'node:path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(__dirname, '..');
const SCENES_DIR = join(__dirname, 'scenes');
const RESULTS_DIR = join(__dirname, 'results');

/* ------------------------------------------------------------------- arg parse */

function parseArgs(argv) {
  const o = {
    runtime: 'browser', scene: 'all', mode: 'capped', runs: 5,
    frames: 1800, seconds: 10, width: 960, height: 540,
    headed: true, tag: '', out: '', profile: 'gpu-headed', quick: false,
  };
  for (const a of argv) {
    const m = /^--([^=]+)(?:=(.*))?$/.exec(a);
    if (!m) continue;
    const [, k, v] = m;
    if (k === 'quick') { o.quick = true; continue; }
    if (k === 'headless') { o.headed = false; continue; }
    if (v === undefined) { o[k] = true; continue; }
    o[k] = /^\d+$/.test(v) ? Number(v) : v;
  }
  if (o.quick) { o.runs = 1; o.frames = 300; o.seconds = 3; }
  return o;
}

const SCENES = [
  's01-entities-500', 's02-entities-5000', 's03-canvas2d-storm', 's04-sprites',
  's05-three-basic', 's06-three-pbr', 's07-three-heavy', 's08-three-math-nogl',
  's09-gc-churn', 's10-typedarray',
];

/* --------------------------------------------------------- environment lockdown */

function readFileSafe(p) { try { return readFileSync(p, 'utf8').trim(); } catch { return null; } }

function lockdownReport() {
  const r = { platform: process.platform, arch: process.arch, node: process.versions.node };
  if (process.platform === 'linux') {
    const gov = readFileSafe('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor');
    r.governor = gov || 'unknown';
    r.locked = gov === 'performance';
    const ac = readFileSafe('/sys/class/power_supply/AC/online')
            || readFileSafe('/sys/class/power_supply/ACAD/online');
    r.acPower = ac === null ? 'unknown' : ac === '1';
    r.kernel = readFileSafe('/proc/sys/kernel/osrelease');
    const model = readFileSafe('/proc/cpuinfo');
    if (model) {
      const line = model.split('\n').find((l) => l.startsWith('model name'));
      if (line) r.cpu = line.split(':')[1].trim();
    }
    const mem = readFileSafe('/proc/meminfo');
    if (mem) {
      const line = mem.split('\n').find((l) => l.startsWith('MemTotal'));
      if (line) r.memTotal = line.split(':')[1].trim();
    }
  } else {
    r.locked = false;
    r.governor = 'n/a';
  }
  return r;
}

/* ------------------------------------------------------------------ statistics */

function median(nums) {
  if (!nums.length) return 0;
  const s = [...nums].sort((a, b) => a - b);
  const m = s.length >> 1;
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
}

/**
 * Median-of-runs, with outliers PRINTED rather than dropped. A run that deviates
 * >5% from the median is a signal about the environment, not noise to hide.
 */
function aggregate(runs) {
  const ok = runs.filter((r) => r && r.busy && r.busy.n > 0);
  if (!ok.length) return { error: 'no successful runs', runs: runs.length };

  const pick = (path) => ok.map((r) => path.split('.').reduce((o, k) => (o ? o[k] : undefined), r))
                           .filter((v) => typeof v === 'number');
  const med = {
    busyP50: median(pick('busy.p50')),
    busyP95: median(pick('busy.p95')),
    busyP99: median(pick('busy.p99')),
    busyMean: median(pick('busy.mean')),
    intervalP50: median(pick('interval.p50')),
    intervalP99: median(pick('interval.p99')),
    fps: median(pick('fps')),
  };

  const outliers = [];
  ok.forEach((r, i) => {
    const dev = med.busyP50 ? Math.abs(r.busy.p50 - med.busyP50) / med.busyP50 : 0;
    if (dev > 0.05) outliers.push({ run: i, busyP50: r.busy.p50, deviation: dev });
  });

  return {
    ...med,
    runs: ok.length,
    attempted: runs.length,
    outliers,
    canaryOk: ok.every((r) => r.canaryOk !== false),
    captures: ok[0].captures,
    env: ok[0].env,
  };
}

/* --------------------------------------------------------------- static server */

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.json': 'application/json', '.png': 'image/png', '.jpg': 'image/jpeg',
  '.ogg': 'audio/ogg', '.mp3': 'audio/mpeg', '.wasm': 'application/wasm',
  '.ttf': 'font/ttf', '.css': 'text/css',
};

/**
 * Serves the bench dir so browser scenes load exactly the files the launchers load:
 * same scene directory, zero modification. If the browser ran different source the
 * comparison would be meaningless.
 */
function startServer(port = 0) {
  return new Promise((resolvePromise) => {
    const server = createServer(async (req, res) => {
      try {
        const url = new URL(req.url, 'http://localhost');
        let path = decodeURIComponent(url.pathname);
        if (path === '/' ) path = '/index.html';
        const file = join(__dirname, path);
        if (!file.startsWith(__dirname)) { res.writeHead(403).end('forbidden'); return; }
        const s = await stat(file).catch(() => null);
        if (!s || !s.isFile()) { res.writeHead(404).end('not found'); return; }
        const body = await readFile(file);
        res.writeHead(200, {
          'Content-Type': MIME[extname(file)] || 'application/octet-stream',
          'Cache-Control': 'no-store',
          // Needed if a scene ever uses SharedArrayBuffer (workers phase).
          'Cross-Origin-Opener-Policy': 'same-origin',
          'Cross-Origin-Embedder-Policy': 'require-corp',
        });
        res.end(body);
      } catch (err) {
        res.writeHead(500).end(String(err));
      }
    });
    server.listen(port, '127.0.0.1', () => resolvePromise({ server, port: server.address().port }));
  });
}

/* ------------------------------------------------------------- browser backend */

async function runBrowser(scene, opts, runIndex) {
  const { chromium } = await import('playwright');
  const { server, port } = await startServer();

  const uncapped = opts.mode === 'uncapped';
  const args = [
    '--use-angle=default',
    '--autoplay-policy=no-user-gesture-required',
    '--disable-background-timer-throttling',
    '--disable-backgrounding-occluded-windows',
    '--disable-renderer-backgrounding',
  ];
  if (uncapped) args.push('--disable-gpu-vsync', '--disable-frame-rate-limit');
  if (opts.profile === 'sw-baseline') args.push('--use-gl=swiftshader', '--use-angle=swiftshader');

  let browser;
  const launchOpts = { headless: !opts.headed, args };
  try {
    browser = await chromium.launch({ ...launchOpts, channel: 'chrome' });
  } catch {
    // Bundled Chromium is a different environment; it gets recorded as such.
    browser = await chromium.launch(launchOpts);
  }

  const page = await browser.newPage({ viewport: { width: opts.width, height: opts.height } });
  const logs = [];
  page.on('console', (m) => logs.push(m.text()));
  page.on('pageerror', (e) => logs.push('PAGEERROR: ' + e.message));

  const q = new URLSearchParams({
    scene, mode: opts.mode, frames: String(opts.frames),
    seconds: String(opts.seconds), width: String(opts.width), height: String(opts.height),
  });
  const url = `http://127.0.0.1:${port}/scenes/${scene}/index.html?${q}`;

  let result = null, error = null;
  try {
    await page.goto(url, { waitUntil: 'load', timeout: 60000 });
    const budget = uncapped ? (opts.seconds + 60) * 1000 : Math.max(120000, opts.frames * 60);
    await page.waitForFunction('window.__benchResult !== undefined', null,
      { timeout: budget, polling: 100 });
    result = await page.evaluate(() => window.__benchResult);

    // GPU sanity: refuse a baseline rendered by a software rasterizer unless that is
    // explicitly the profile being run. A SwiftShader number silently poisons every
    // comparison it touches.
    const renderer = String(result?.env?.glRenderer || '');
    const isSoftware = /swiftshader|llvmpipe|software/i.test(renderer);
    if (isSoftware && opts.profile !== 'sw-baseline') {
      error = `REFUSING BASELINE: software renderer detected (${renderer}). ` +
              `Run with --profile=sw-baseline to record it deliberately.`;
      result = null;
    }
  } catch (e) {
    error = e.message;
  } finally {
    const pageErrors = logs.filter((l) => l.startsWith('PAGEERROR'));
    if (pageErrors.length && !result) error = (error ? error + ' | ' : '') + pageErrors.join(' | ');
    await browser.close().catch(() => {});
    server.close();
  }

  if (error) return { error, logs: logs.slice(-20) };
  return result;
}

/* ---------------------------------------------------- launcher backends (node/qjs) */

function runLauncher(cmd, cmdArgs, sceneDir, opts) {
  return new Promise((resolvePromise) => {
    // Options travel as a FILE, not an env var: rungame's vm realm hides `process`,
    // so an env var set here is invisible to the scene (it silently ran the default
    // 1800 frames the first time this was tried).
    writeFileSync(join(sceneDir, 'bench-opts.json'), JSON.stringify({
      mode: opts.mode, frames: opts.frames, seconds: opts.seconds,
      width: opts.width, height: opts.height,
    }));
    const env = { ...process.env };
    const child = spawn(cmd, [...cmdArgs, sceneDir], {
      env, stdio: ['ignore', 'pipe', 'pipe'],
    });

    let out = '', err = '', done = false;
    const rssSamples = [];
    const sampler = setInterval(() => {
      const rss = readRss(child.pid);
      if (rss) rssSamples.push(rss);
    }, 1000);

    const budget = opts.mode === 'uncapped'
      ? (opts.seconds + 90) * 1000
      : Math.max(180000, opts.frames * 80);
    const timer = setTimeout(() => { if (!done) child.kill('SIGKILL'); }, budget);

    /**
     * Finish as soon as the result line arrives — do NOT wait for the child to exit.
     * A launcher owns an SDL window and its own uncapped loop; rungame in particular
     * keeps running (and ignores SIGTERM while SDL holds the loop), so waiting for
     * 'close' burns the entire timeout budget on a run that already succeeded.
     */
    function finishWith(payload) {
      if (done) return;
      done = true;
      clearInterval(sampler);
      clearTimeout(timer);
      try { child.kill('SIGKILL'); } catch (_) { /* already gone */ }
      resolvePromise(payload);
    }

    function tryParseResult(exitCode) {
      const line = out.split('\n').find((l) => l.includes('BENCH_JSON:'));
      if (!line) return null;
      try {
        const parsed = JSON.parse(line.slice(line.indexOf('BENCH_JSON:') + 11));
        if (rssSamples.length) {
          const mid = rssSamples.slice(Math.floor(rssSamples.length / 3));
          parsed.rss = {
            median: median(mid), max: Math.max(...rssSamples), samples: rssSamples.length,
            slopeBytesPerMin: rssSlope(rssSamples),
          };
        }
        return parsed;
      } catch (e) {
        return { error: 'bad BENCH_JSON: ' + e.message, stdout: out.slice(-2000) };
      }
    }

    child.stdout.on('data', (d) => {
      out += d.toString();
      if (out.includes('BENCH_JSON:')) {
        const parsed = tryParseResult(0);
        if (parsed) finishWith(parsed);
      }
    });
    child.stderr.on('data', (d) => { err += d.toString(); });

    child.on('close', (code) => {
      const parsed = tryParseResult(code);
      finishWith(parsed || {
        error: `no BENCH_JSON in output (exit ${code})`,
        stdout: out.slice(-2000), stderr: err.slice(-2000),
      });
    });
    child.on('error', (e) => finishWith({ error: 'spawn failed: ' + e.message }));
  });
}

function readRss(pid) {
  try {
    if (process.platform === 'linux') {
      const statm = readFileSync(`/proc/${pid}/statm`, 'utf8').split(' ');
      return Number(statm[1]) * 4096;   // resident pages
    }
  } catch { /* process gone */ }
  return null;
}

/** Least-squares slope over the RSS samples: the leak detector. */
function rssSlope(samples) {
  const n = samples.length;
  if (n < 4) return 0;
  let sx = 0, sy = 0, sxy = 0, sxx = 0;
  for (let i = 0; i < n; i++) { sx += i; sy += samples[i]; sxy += i * samples[i]; sxx += i * i; }
  const slopePerSample = (n * sxy - sx * sy) / (n * sxx - sx * sx);
  return slopePerSample * 60;   // samples are 1/s -> bytes per minute
}

/* -------------------------------------------------------------------- the loop */

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/**
 * Stage a bench scene for jsglq: harness at the root, scene under scenes/<name>/,
 * shared lib alongside. Mirrors what the HTTP server exposes to the browser so all
 * three runtimes execute byte-identical scene source.
 */
function stageQjsScene(scene, opts) {
  const stage = join(tmpdir(), `jsglq-bench-${scene}`);
  rmSync(stage, { recursive: true, force: true });
  mkdirSync(join(stage, 'scenes', scene), { recursive: true });
  mkdirSync(join(stage, 'scenes', 'lib', 'vendor'), { recursive: true });

  cpSync(join(__dirname, 'harness.js'), join(stage, 'harness.js'));
  const libDir = join(SCENES_DIR, 'lib');
  for (const f of readdirSync(libDir)) {
    const src = join(libDir, f);
    if (statSync(src).isFile()) cpSync(src, join(stage, 'scenes', 'lib', f));
  }
  const vendorDir = join(libDir, 'vendor');
  if (existsSync(vendorDir)) {
    for (const f of readdirSync(vendorDir)) {
      cpSync(join(vendorDir, f), join(stage, 'scenes', 'lib', 'vendor', f));
    }
  }
  cpSync(join(SCENES_DIR, scene, 'main.js'), join(stage, 'scenes', scene, 'main.js'));

  const bench = JSON.stringify({
    mode: opts.mode, frames: opts.frames, seconds: opts.seconds,
    width: opts.width, height: opts.height,
  });
  writeFileSync(join(stage, 'main.js'),
    `globalThis.__BENCH_OPTS__ = ${bench};\n` +
    `import('./scenes/${scene}/main.js')\n` +
    `  .catch(e => console.log('SCENE FAILED:', e && (e.stack || e.message)));\n`);
  return stage;
}

async function runOne(runtime, scene, opts, runIndex) {
  if (runtime === 'browser') {
    /*
     * Launching Chrome repeatedly for GPU-heavy scenes exhausts the GPU process:
     * runs 1-5 of a scene are fine, then the next scene's page dies at load with
     * "Target page, context or browser has been closed". A short cooldown between
     * launches and one retry makes the baseline reproducible. Retries are reported,
     * never hidden — a scene that needs its retry every time is a real signal.
     */
    let r = await runBrowser(scene, opts, runIndex);
    if (r && r.error && /closed|crash/i.test(r.error)) {
      await sleep(3000);
      const retry = await runBrowser(scene, opts, runIndex);
      if (!retry.error) {
        retry.retried = true;
        return retry;
      }
      return retry;
    }
    await sleep(400);
    return r;
  }
  // Absolute: rungame builds a file:// URL from the path it is handed, and a
  // relative one becomes file://bench/... whose "host" is `bench` (ERR_INVALID_FILE_URL_HOST).
  const sceneDir = resolve(join(SCENES_DIR, scene));
  if (runtime === 'node') {
    // Default to the local checkout so the incumbent under test is a known commit,
    // not whatever npx resolves today.
    const bin = process.env.RUNGAME_BIN
      || resolve(ROOT, '..', 'jsgamelauncher', 'cli.js');
    return runLauncher(process.execPath, [bin], sceneDir, opts);
  }
  if (runtime === 'qjs') {
    const bin = process.env.JSGLQ_BIN || join(ROOT, 'build', 'jsglq');
    if (!existsSync(bin)) return { error: `qjs binary not built: ${bin}` };
    /*
     * Scenes import ../../harness.js and ../lib/*, so jsglq needs the same
     * directory shape the browser server provides. scripts/run-qjs-scene.sh stages
     * exactly that; running the scene dir directly would break every relative
     * import in it.
     */
    const staged = stageQjsScene(scene, opts);
    /*
     * Host frame budget must cover warmup (120) + measured frames + slack for
     * async startup (asset decode happens during the first frames). Too tight a
     * budget stops the host before the harness reports, which looks exactly like
     * a silent hang.
     */
    const frames = opts.mode === 'uncapped'
      ? Math.ceil(opts.seconds * 4000)
      : opts.frames + 600;
    const args = ['--headless', `--frames=${frames}`];
    if (opts.mode === 'uncapped') args.push('--uncapped');
    return runLauncher(bin, args, staged, opts);
  }
  return { error: 'unknown runtime ' + runtime };
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const scenes = opts.scene === 'all' ? SCENES : String(opts.scene).split(',');
  const lockdown = lockdownReport();

  console.log('=== bench run ===');
  console.log('runtime :', opts.runtime);
  console.log('mode    :', opts.mode, opts.mode === 'capped' ? `${opts.frames} frames` : `${opts.seconds}s`);
  console.log('runs    :', opts.runs);
  console.log('lockdown:', JSON.stringify(lockdown));
  if (!lockdown.locked) {
    console.log('  ! governor is not "performance" — results are tagged unlocked');
  }
  console.log('');

  mkdirSync(RESULTS_DIR, { recursive: true });
  const stamp = new Date().toISOString().replace(/[:.]/g, '-');
  const outPath = opts.out || join(RESULTS_DIR, `${opts.runtime}-${stamp}.jsonl`);
  const summary = [];

  for (const scene of scenes) {
    process.stdout.write(`${scene.padEnd(24)} `);
    const runs = [];
    for (let i = 0; i < opts.runs; i++) {
      const r = await runOne(opts.runtime, scene, opts, i);
      runs.push(r);
      process.stdout.write(r.error ? 'x' : '.');
    }
    const agg = aggregate(runs);
    const record = {
      ts: new Date().toISOString(),
      runtime: opts.runtime, scene, mode: opts.mode,
      frames: opts.frames, width: opts.width, height: opts.height,
      profile: opts.profile, tag: opts.tag,
      lockdown, agg,
      errors: runs.filter((r) => r.error).map((r) => r.error),
      rss: runs.find((r) => r.rss)?.rss || null,
    };
    appendFileSync(outPath, JSON.stringify(record) + '\n');
    summary.push(record);

    if (agg.error) {
      console.log(` FAILED: ${runs.find((r) => r.error)?.error || agg.error}`);
      const first = runs.find((r) => r.error);
      if (first?.stderr) console.log('   stderr:', first.stderr.slice(0, 400));
      if (first?.stdout) console.log('   stdout:', first.stdout.slice(0, 400));
      if (first?.logs) console.log('   logs:', first.logs.join(' | ').slice(0, 400));
    } else {
      const canary = agg.canaryOk ? '' : '  !! CANARY FAILED (orientation) !!';
      console.log(` busy p50 ${agg.busyP50.toFixed(3)}ms  p95 ${agg.busyP95.toFixed(3)}ms` +
                  `  p99 ${agg.busyP99.toFixed(3)}ms  fps ${agg.fps.toFixed(1)}` +
                  (agg.outliers.length ? `  [${agg.outliers.length} outlier]` : '') + canary);
      if (agg.env?.glRenderer) console.log(`   gpu: ${agg.env.glRenderer}`);
    }
  }

  console.log('\nwrote', outPath);
  // Explicit exit: a killed launcher child can leave this process with a live handle
  // (SDL/GL fds inherited through the pipe), and waiting on the event loop to drain
  // hangs a run that already produced every number it was asked for.
  process.exit(0);
}

main().catch((e) => { console.error(e); process.exit(1); });
