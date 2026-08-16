#!/usr/bin/env node
/**
 * Fetch pinned dependencies. Versions come from scripts/versions.json and nothing
 * floats: a build either uses the pinned commit or fails, so a green CI run means
 * the same thing tomorrow as it does today.
 */
import { execFileSync } from 'node:child_process';
import { readFileSync, existsSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const versions = JSON.parse(readFileSync(join(ROOT, 'scripts', 'versions.json'), 'utf8'));

const dest = join(ROOT, 'deps', 'quickjs');
if (existsSync(join(dest, 'quickjs.c'))) {
  console.log('quickjs already present');
} else {
  mkdirSync(join(ROOT, 'deps'), { recursive: true });
  const { repo, tag, commit } = versions.quickjs;
  console.log(`cloning ${repo} @ ${tag}`);
  execFileSync('git', ['clone', '--depth', '1', '--branch', tag, repo, dest], { stdio: 'inherit' });
  const head = execFileSync('git', ['-C', dest, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
  if (head !== commit) {
    console.error(`PIN MISMATCH: ${tag} is ${head}, expected ${commit}`);
    console.error('Either the tag moved or versions.json is stale. Refusing to build.');
    process.exit(1);
  }
  console.log(`quickjs at ${commit}`);
}
