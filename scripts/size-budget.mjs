#!/usr/bin/env node
/**
 * Binary size budget.
 *
 * The whole argument for this runtime is that it is small (9.6 MB against 123 MB
 * for the libnode-embedded sibling and 60-95 MB for a Bun build). A size budget
 * that is not enforced stops being true within a few months, so this fails the
 * build rather than printing a number nobody reads.
 */
import { statSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const WARN_MB = 15;
const FAIL_MB = 20;

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
// build/ is pinned in CMakeLists for every generator, but multi-config
// generators historically drop a config subdirectory in, so accept both.
const candidates = [
  join(ROOT, 'build', 'jsglq'),
  join(ROOT, 'build', 'jsglq.exe'),
  join(ROOT, 'build', 'Release', 'jsglq'),
  join(ROOT, 'build', 'Release', 'jsglq.exe'),
];
const bin = candidates.find((p) => existsSync(p));
if (!bin) {
  console.error('no binary found; build first');
  process.exit(1);
}

const mb = statSync(bin).size / 1024 / 1024;
console.log(`${bin}: ${mb.toFixed(2)} MB (warn ${WARN_MB}, fail ${FAIL_MB})`);
if (mb > FAIL_MB) {
  console.error(`OVER BUDGET: ${mb.toFixed(2)} MB exceeds the ${FAIL_MB} MB limit.`);
  process.exit(1);
}
if (mb > WARN_MB) console.warn(`warning: over the ${WARN_MB} MB soft budget`);
