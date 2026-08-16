#!/usr/bin/env node
/*
 * Make a macOS build self-contained.
 *
 * A macOS binary records each dependency by the path baked into that library's
 * own "install name". Homebrew's SDL2 identifies itself with an ABSOLUTE path
 * (/opt/homebrew/opt/sdl2-compat/lib/libSDL2-2.0.0.dylib), so a binary linked
 * against it only runs on a machine that has Homebrew's SDL2 at exactly that
 * path. ANGLE does not have this problem because native-gles's downloader
 * already rewrites its install names to @rpath.
 *
 * This copies every non-system dependency next to the binary and rewrites the
 * references to @rpath, which resolves through the @loader_path rpath that
 * CMake sets. It also DELETES any rpath pointing outside the bundle: those
 * leak the build machine's directory layout into a shipped artifact, and one
 * of them was a CI runner's home directory.
 *
 *   node scripts/bundle-macos-libs.mjs <binary> [destination-dir]
 *
 * Idempotent: running it twice is a no-op.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, copyFileSync, chmodSync, statSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';

/* Anything under these prefixes ships with macOS itself and must NOT be
   bundled: copying a system library is both unnecessary and a licensing
   question, and the dynamic loader always finds them. */
const SYSTEM_PREFIXES = ['/usr/lib/', '/System/'];

const isSystem = (p) => SYSTEM_PREFIXES.some((s) => p.startsWith(s));
const isRelocated = (p) => p.startsWith('@'); // @rpath, @loader_path, @executable_path

function run(cmd, args) {
  return execFileSync(cmd, args, { encoding: 'utf8' });
}

/* otool -l is the only way to see rpaths; otool -L omits them. */
function loadCommands(file) {
  const out = run('otool', ['-l', file]);
  const deps = [];
  const rpaths = [];
  const lines = out.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const cmd = lines[i].trim();
    if (cmd === 'cmd LC_LOAD_DYLIB' || cmd === 'cmd LC_LOAD_WEAK_DYLIB') {
      const nameLine = lines.slice(i, i + 6).find((l) => l.trim().startsWith('name '));
      if (nameLine) deps.push(nameLine.trim().split(/\s+/)[1]);
    } else if (cmd === 'cmd LC_RPATH') {
      const pathLine = lines.slice(i, i + 6).find((l) => l.trim().startsWith('path '));
      if (pathLine) rpaths.push(pathLine.trim().split(/\s+/)[1]);
    }
  }
  return { deps, rpaths };
}

const binary = process.argv[2];
if (!binary || !existsSync(binary)) {
  console.error('usage: bundle-macos-libs.mjs <binary> [destination-dir]');
  process.exit(1);
}
if (process.platform !== 'darwin') {
  console.error('bundle-macos-libs.mjs only runs on macOS');
  process.exit(1);
}

const destDir = resolve(process.argv[3] || dirname(binary));
const bin = resolve(binary);

/* Walk the dependency graph: SDL2 itself pulls in libraries that need the same
   treatment, and a one-level pass would leave those absolute. */
const bundled = new Set();
const queue = [bin];
let copied = 0;
let rewritten = 0;

while (queue.length) {
  const file = queue.shift();
  const { deps, rpaths } = loadCommands(file);

  for (const dep of deps) {
    if (isSystem(dep) || isRelocated(dep)) continue;

    const name = basename(dep);
    const dest = join(destDir, name);

    if (!existsSync(dest)) {
      if (!existsSync(dep)) {
        console.error(`  MISSING ${dep} (referenced by ${basename(file)})`);
        process.exit(1);
      }
      copyFileSync(dep, dest);
      chmodSync(dest, 0o755);
      copied++;
      console.log(`  bundled ${name} (${(statSync(dest).size / 1024).toFixed(0)} KB)`);
    }

    /* Point the referrer at the copy beside the binary. */
    run('install_name_tool', ['-change', dep, `@rpath/${name}`, file]);
    rewritten++;

    if (!bundled.has(name)) {
      bundled.add(name);
      /* Give the copy an @rpath identity, then follow ITS dependencies. */
      run('install_name_tool', ['-id', `@rpath/${name}`, dest]);
      queue.push(dest);
    }
  }

  /* Strip rpaths that point outside the bundle. These are what leaked
     /Users/runner/work/... and /opt/homebrew/lib into the shipped binary. */
  for (const rp of rpaths) {
    if (rp.startsWith('@loader_path') || rp.startsWith('@executable_path')) continue;
    run('install_name_tool', ['-delete_rpath', rp, file]);
    console.log(`  removed rpath ${rp} from ${basename(file)}`);
  }

  /* Every bundled library resolves its siblings from its own directory. */
  const { rpaths: after } = loadCommands(file);
  if (!after.some((r) => r.startsWith('@loader_path'))) {
    run('install_name_tool', ['-add_rpath', '@loader_path', file]);
  }
}

/* Re-sign: any install_name_tool edit invalidates the existing signature, and on
   Apple silicon an INVALID signature is fatal where an absent one is not.
 *
 * ORDER MATTERS, and getting it wrong is not a visible error — it is a HANG.
 * Sign the dylibs FIRST and the executable LAST: the executable's signature
 * covers its dependencies, so signing a dylib afterwards invalidates the
 * executable again. Signing the binary first left every macOS runner stuck
 * inside the loader on the first execution, with no output and no exit. */
const signOrder = [...[...bundled].map((n) => join(destDir, n)), bin];
for (const f of signOrder) {
  try {
    run('codesign', ['--force', '--sign', '-', '--timestamp=none', f]);
  } catch (err) {
    console.error(`  codesign failed for ${basename(f)}: ${err.message}`);
    process.exit(1);
  }
}

/* Prove the signatures are actually valid rather than assuming. `codesign -v`
   catches the invalidated-by-later-signing case that otherwise only shows up as
   a hang at runtime. */
for (const f of signOrder) {
  try {
    run('codesign', ['--verify', '--strict', f]);
  } catch (err) {
    console.error(`  SIGNATURE INVALID for ${basename(f)}: ${err.message}`);
    process.exit(1);
  }
}

console.log(`macOS bundle: ${copied} libraries copied, ${rewritten} references rewritten, ${signOrder.length} signed and verified`);

/* Refuse to report success if anything absolute survived. */
const { deps: finalDeps, rpaths: finalRpaths } = loadCommands(bin);
const bad = finalDeps.filter((d) => !isSystem(d) && !isRelocated(d));
const badRpaths = finalRpaths.filter(
  (r) => !r.startsWith('@loader_path') && !r.startsWith('@executable_path'),
);
if (bad.length || badRpaths.length) {
  console.error('STILL NOT SELF-CONTAINED:');
  bad.forEach((b) => console.error(`  dependency ${b}`));
  badRpaths.forEach((b) => console.error(`  rpath ${b}`));
  process.exit(1);
}
