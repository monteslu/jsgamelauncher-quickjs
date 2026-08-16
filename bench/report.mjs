#!/usr/bin/env node
/**
 * Render the tri-runtime comparison from stored JSONL results.
 *
 * Reports the LATEST record per (runtime, scene). Ratios are qjs relative to each
 * reference, so >1 means qjs is slower and <1 means qjs is faster. Missing cells
 * are printed as "-" rather than omitted: a gap in coverage is information, and
 * silently dropping it makes a partial run look complete.
 */
import { readFileSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const RESULTS = join(__dirname, 'results');

const latest = new Map();   // `${runtime}|${scene}` -> record

for (const f of readdirSync(RESULTS).filter((x) => x.endsWith('.jsonl'))) {
  for (const line of readFileSync(join(RESULTS, f), 'utf8').split('\n')) {
    if (!line.trim()) continue;
    let r;
    try { r = JSON.parse(line); } catch { continue; }
    if (!r.agg || r.agg.error) continue;
    const key = `${r.runtime}|${r.scene}`;
    const prev = latest.get(key);
    if (!prev || new Date(r.ts) > new Date(prev.ts)) latest.set(key, r);
  }
}

const scenes = [...new Set([...latest.keys()].map((k) => k.split('|')[1]))].sort();
const num = (v, d = 3) => (typeof v === 'number' ? v.toFixed(d) : '-');
const get = (rt, sc) => latest.get(`${rt}|${sc}`);

console.log('# Tri-runtime comparison (busy ms/frame, lower is better)\n');
console.log('| scene | qjs p50 | node p50 | chrome p50 | qjs/node | qjs/chrome | canary |');
console.log('|---|---|---|---|---|---|---|');

for (const sc of scenes) {
  const q = get('qjs', sc), n = get('node', sc), b = get('browser', sc);
  const qp = q?.agg?.busyP50, np = n?.agg?.busyP50, bp = b?.agg?.busyP50;
  const ratio = (a, c) => (typeof a === 'number' && typeof c === 'number' && c > 0
    ? (a / c).toFixed(2) + 'x' : '-');
  const canary = [q, n, b].filter(Boolean).every((r) => r.agg.canaryOk !== false) ? 'ok' : 'FAIL';
  console.log(`| ${sc} | ${num(qp)} | ${num(np)} | ${num(bp)} | ` +
              `${ratio(qp, np)} | ${ratio(qp, bp)} | ${canary} |`);
}

console.log('\n## p95 (tail matters more than the median for frame pacing)\n');
console.log('| scene | qjs p95 | node p95 | chrome p95 |');
console.log('|---|---|---|---|');
for (const sc of scenes) {
  console.log(`| ${sc} | ${num(get('qjs', sc)?.agg?.busyP95)} | ` +
              `${num(get('node', sc)?.agg?.busyP95)} | ${num(get('browser', sc)?.agg?.busyP95)} |`);
}

const anyEnv = [...latest.values()].find((r) => r.lockdown);
if (anyEnv) {
  console.log('\n## environment\n');
  console.log('```');
  console.log(JSON.stringify(anyEnv.lockdown, null, 2));
  console.log('```');
}
