#!/usr/bin/env node
/**
 * Harness self-test: the controls that must fail, and the oracle-vs-itself floor.
 *
 * A green test suite proves nothing unless the suite can go red. Every check here
 * either (a) deliberately breaks something and demands the harness notice, or
 * (b) runs the oracle against itself to measure the noise floor that all later
 * cross-runtime thresholds are judged against.
 *
 * If any control PASSES when it should fail, the harness is broken and every number
 * it has ever produced is void. That is a louder failure than a missed regression.
 */

import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { selfTestMustFail, diff, measureNoiseFloor, isVerticalFlip } from './differ.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const TMP = join(__dirname, '.selftest');

const results = [];
function check(name, ok, detail) {
  results.push({ name, ok, detail });
  console.log(`${ok ? '  ok  ' : ' FAIL '} ${name}${detail ? '  — ' + detail : ''}`);
}

function runNode(args, opts = {}) {
  return new Promise((res) => {
    const c = spawn(process.execPath, args, { cwd: __dirname, ...opts });
    let out = '', err = '';
    c.stdout.on('data', (d) => { out += d; });
    c.stderr.on('data', (d) => { err += d; });
    c.on('close', (code) => res({ code, out, err }));
  });
}

async function runBrowserScene(scene, extra = {}) {
  const { chromium } = await import('playwright');
  const { createServer } = await import('node:http');
  const { readFile, stat } = await import('node:fs/promises');
  const { extname } = await import('node:path');

  const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.json': 'application/json' };
  const server = createServer(async (req, res) => {
    const url = new URL(req.url, 'http://localhost');
    const file = join(__dirname, decodeURIComponent(url.pathname));
    const s = await stat(file).catch(() => null);
    if (!s || !s.isFile()) { res.writeHead(404).end(); return; }
    res.writeHead(200, { 'Content-Type': MIME[extname(file)] || 'application/octet-stream' });
    res.end(await readFile(file));
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;

  const browser = await chromium.launch({ headless: false, args: ['--use-angle=default'] }).catch(
    () => chromium.launch({ headless: true }));
  const page = await browser.newPage({ viewport: { width: 480, height: 270 } });
  const q = new URLSearchParams({ frames: '90', width: '480', height: '270', ...extra });
  let result = null, captures = null;
  try {
    await page.goto(`http://127.0.0.1:${port}/scenes/${scene}/index.html?${q}`, { timeout: 30000 });
    await page.waitForFunction('window.__benchResult !== undefined', null, { timeout: 60000 });
    result = await page.evaluate(() => window.__benchResult);
    captures = await page.evaluate(() => (window.__benchCaptures || []).map((c) => ({
      frame: c.frame, width: c.width, height: c.height,
      data: c.data ? Array.from(c.data) : null,
    })));
  } finally {
    await browser.close().catch(() => {});
    server.close();
  }
  if (captures) {
    for (const c of captures) if (c.data) c.data = new Uint8Array(c.data);
  }
  return { result, captures };
}

async function main() {
  console.log('=== harness self-test ===\n');
  mkdirSync(TMP, { recursive: true });

  /* ---- control 1: the differ must catch a 1px shift ------------------------- */
  const dr = selfTestMustFail();
  check('differ catches a 1px-shifted frame (MUST-FAIL control)', dr.ok,
    dr.ok ? 'shift detected, identity matched, flip detected' : dr.problems.join('; '));

  /* ---- control 2: the differ must catch a vertical flip --------------------- */
  {
    const w = 32, h = 32;
    const a = new Uint8Array(w * h * 4);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      a[i] = y * 8; a[i + 1] = 40; a[i + 2] = 80; a[i + 3] = 255;
    }
    const flipped = new Uint8Array(a.length);
    const row = w * 4;
    for (let y = 0; y < h; y++) flipped.set(a.subarray((h - 1 - y) * row, (h - y) * row), y * row);
    const d = diff({ width: w, height: h, data: a }, { width: w, height: h, data: flipped });
    const detected = !d.ok && isVerticalFlip({ width: w, height: h, data: a },
                                             { width: w, height: h, data: flipped });
    check('differ catches a vertically flipped frame (MUST-FAIL control)', detected,
      detected ? 'flip reported as mismatch + identified as flip' : 'FLIP WENT UNDETECTED');
  }

  /* ---- control 3: the canary must reject a flipped capture ------------------ */
  {
    const mod = await import('./harness.js');
    const w = 64, h = 64;
    const data = new Uint8Array(w * h * 4);
    const put = (c, x, y) => {
      for (let dy = 0; dy < 8; dy++) for (let dx = 0; dx < 8; dx++) {
        const i = ((y + dy) * w + x + dx) * 4;
        data[i] = c[0]; data[i + 1] = c[1]; data[i + 2] = c[2]; data[i + 3] = 255;
      }
    };
    const C = mod.CANARY;
    put(C.tl, 0, 0); put(C.tr, w - 8, 0); put(C.bl, 0, h - 8); put(C.br, w - 8, h - 8);
    const good = mod.checkCanary({ width: w, height: h, data });

    const flipped = new Uint8Array(data.length);
    const row = w * 4;
    for (let y = 0; y < h; y++) flipped.set(data.subarray((h - 1 - y) * row, (h - y) * row), y * row);
    const bad = mod.checkCanary({ width: w, height: h, data: flipped });

    check('canary accepts correct orientation', good.ok, good.ok ? '' : good.bad.join(','));
    check('canary REJECTS a flipped frame (MUST-FAIL control)', !bad.ok,
      !bad.ok ? `rejected: ${bad.bad.join(', ')}` : 'FLIPPED FRAME PASSED THE CANARY');
  }

  /* ---- control 4: the bench must SEE an injected 5ms stall ------------------ */
  {
    const sceneDir = join(__dirname, 'scenes', '_control-slow');
    mkdirSync(sceneDir, { recursive: true });
    writeFileSync(join(sceneDir, 'main.js'), `
import { autorun, drawCanary2d } from '../../harness.js';
import { get2d } from '../lib/scaffold.js';
// Deliberately burns ~5ms of wall time per frame. If the harness cannot see this,
// it cannot see a real regression either.
export const scene = {
  name: '_control-slow',
  setup({ width, height }) { return { ...get2d(width, height), width, height }; },
  step(t) {
    const end = performance.now() + 5;
    let x = 0;
    while (performance.now() < end) x += Math.sqrt(x + 1);
    t.sink = x;
    t.ctx.fillStyle = '#0d1014';
    t.ctx.fillRect(0, 0, t.width, t.height);
    drawCanary2d(t.ctx, t.width, t.height);
  },
};
autorun(scene);
`);
    await runNode([join(__dirname, 'scripts', 'gen-html.mjs')]);
    const { result } = await runBrowserScene('_control-slow');
    const seen = result && result.busy && result.busy.p50 >= 4.0;
    check('bench detects an injected 5ms/frame stall (MUST-FAIL control)', !!seen,
      result?.busy ? `measured p50 ${result.busy.p50.toFixed(2)}ms (expected >=4)` : 'no result');
  }

  /* ---- oracle vs itself: the noise floor ----------------------------------- */
  {
    const a = await runBrowserScene('s01-entities-500');
    const b = await runBrowserScene('s01-entities-500');
    if (!a.captures?.length || !b.captures?.length) {
      check('oracle-vs-itself 2D noise floor', false, 'no captures returned');
    } else {
      const floor = measureNoiseFloor(a.captures, b.captures);
      check('oracle-vs-itself: 2D canvas is byte-exact across runs', floor.exact,
        floor.exact ? 'maxDelta 0 — 2D comparisons can use tolerance 0'
                    : `maxDelta ${floor.maxDelta}, ratio ${floor.maxRatio.toFixed(6)}`);
      writeFileSync(join(__dirname, 'results', 'noise-floor-2d.json'),
        JSON.stringify({ scene: 's01-entities-500', ...floor }, null, 2));

      const sameCrc = a.result.captures.every((c, i) => c.crc === b.result.captures[i].crc);
      check('determinism: identical CRCs across two independent runs', sameCrc,
        sameCrc ? 'seeded PRNG + fixed dt confirmed deterministic' : 'CRC drift between runs');
    }
  }

  /* ---- GL noise floor (three.js scene) ------------------------------------- */
  {
    const a = await runBrowserScene('s05-three-basic');
    const b = await runBrowserScene('s05-three-basic');
    if (!a.captures?.length || !b.captures?.length) {
      check('oracle-vs-itself GL noise floor', false,
        'no captures — ' + (a.result?.error || b.result?.error || 'unknown'));
    } else {
      const floor = measureNoiseFloor(a.captures, b.captures);
      check('oracle-vs-itself: GL noise floor measured', true,
        `maxDelta ${floor.maxDelta}, differing ratio ${floor.maxRatio.toFixed(6)}` +
        (floor.exact ? ' (exact)' : ' — cross-runtime GL tolerance derives from this'));
      writeFileSync(join(__dirname, 'results', 'noise-floor-gl.json'),
        JSON.stringify({ scene: 's05-three-basic', ...floor }, null, 2));
    }
  }

  rmSync(join(__dirname, 'scenes', '_control-slow'), { recursive: true, force: true });

  console.log('');
  const failed = results.filter((r) => !r.ok);
  if (failed.length) {
    console.log(`SELF-TEST FAILED: ${failed.length}/${results.length}`);
    process.exit(1);
  }
  console.log(`self-test passed: ${results.length}/${results.length}`);
}

main().catch((e) => { console.error(e); process.exit(1); });
