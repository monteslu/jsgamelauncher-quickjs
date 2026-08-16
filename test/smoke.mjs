#!/usr/bin/env node
/**
 * Per-platform smoke test for CI.
 *
 * A build that produces a binary which cannot boot is not a passing build, and a
 * zero exit code proves almost nothing on its own. So this requires evidence that
 * real pixels reached the screen:
 *
 *   - the game must report a colour count > 1 (a cleared frame is ONE colour, so
 *     this distinguishes "drew something" from "started and cleared")
 *   - the orientation canary must be green in all four corners
 *   - it must boot within a time budget, since a hang and a slow start look the
 *     same to a CI runner until the job times out
 *
 * "Advance until real pixels" is the rule this encodes: blank frames are failures,
 * not neutral results.
 */
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.JSGLQ_BIN
  || [join(ROOT, 'build', 'jsglq'), join(ROOT, 'build', 'jsglq.exe')].find(existsSync);

if (!BIN) {
  console.error('smoke: no binary found; build first');
  process.exit(1);
}

const GAME = `
const c = document.getElementById('game-canvas');
const x = c.getContext('2d');
let n = 0;
(function f(){
  // A gradient of distinct rects, so the colour count is meaningfully > 1 and a
  // partially-working renderer cannot pass by clearing to a single colour.
  for (let i = 0; i < 32; i++) {
    x.fillStyle = 'rgb(' + (i * 8) + ',' + (255 - i * 8) + ',128)';
    x.fillRect(i * 28, 40, 26, 200);
  }
  x.fillStyle='red';    x.fillRect(0,0,12,12);
  x.fillStyle='lime';   x.fillRect(c.width-12,0,12,12);
  x.fillStyle='blue';   x.fillRect(0,c.height-12,12,12);
  x.fillStyle='yellow'; x.fillRect(c.width-12,c.height-12,12,12);

  if (n === 2) {
    const d = x.getImageData(0, 0, c.width, c.height);
    const seen = new Set();
    for (let i = 0; i < d.data.length; i += 4) {
      seen.add((d.data[i]<<24 | d.data[i+1]<<16 | d.data[i+2]<<8 | d.data[i+3]) >>> 0);
      if (seen.size > 4096) break;
    }
    const px = (ax, ay) => {
      const i = (ay * c.width + ax) * 4;
      return d.data[i] + ',' + d.data[i+1] + ',' + d.data[i+2];
    };
    console.log('SMOKE colors=' + seen.size
      + ' tl=' + px(4,4) + ' tr=' + px(c.width-5,4)
      + ' bl=' + px(4,c.height-5) + ' br=' + px(c.width-5,c.height-5));
  }
  if (++n < 5) requestAnimationFrame(f);
})();
`;

const dir = mkdtempSync(join(tmpdir(), 'jsglq-smoke-'));
let out = '';
try {
  writeFileSync(join(dir, 'main.js'), GAME);
  const started = Date.now();
  const r = spawnSync(BIN, ['--headless', '--frames=20', dir],
                      { encoding: 'utf8', timeout: 60000 });
  const elapsed = Date.now() - started;
  out = (r.stdout || '') + (r.stderr || '');

  const m = /SMOKE colors=(\d+) tl=(\S+) tr=(\S+) bl=(\S+) br=(\S+)/.exec(out);
  if (!m) {
    console.error('smoke FAILED: the game produced no result line');
    console.error(out.slice(-1500));
    process.exit(1);
  }

  const [, colorsRaw, tl, tr, bl, br] = m;
  const colors = Number(colorsRaw);
  const problems = [];

  // A cleared-but-not-drawn frame is exactly 1 colour. Anything at or below a
  // handful means the renderer started and then drew nothing worth shipping.
  if (colors < 8) problems.push(`only ${colors} distinct colours (blank or near-blank frame)`);
  if (tl !== '255,0,0')     problems.push(`top-left is ${tl}, expected 255,0,0`);
  if (tr !== '0,255,0')     problems.push(`top-right is ${tr}, expected 0,255,0`);
  if (bl !== '0,0,255')     problems.push(`bottom-left is ${bl}, expected 0,0,255`);
  if (br !== '255,255,0')   problems.push(`bottom-right is ${br}, expected 255,255,0`);
  if (/Assertion/.test(out)) problems.push('QuickJS asserted at shutdown (leaked values)');
  if (elapsed > 15000)      problems.push(`took ${elapsed}ms to run 20 frames`);

  if (problems.length) {
    console.error('smoke FAILED:');
    for (const p of problems) console.error('  - ' + p);
    console.error(out.slice(-1500));
    process.exit(1);
  }

  console.log(`smoke OK: ${colors} colours, orientation correct, ${elapsed}ms`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
