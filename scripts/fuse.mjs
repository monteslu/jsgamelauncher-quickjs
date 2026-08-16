#!/usr/bin/env node
/**
 * Fuse a game into a copy of the launcher, producing a single self-contained
 * executable.
 *
 *   node scripts/fuse.mjs <game-dir> <output-binary> [--launcher build/jsglq]
 *
 * FORMAT
 *
 *   [ launcher binary ][ payload ][ 32-byte trailer ]
 *
 * The payload is a flat archive: for each file, a header (path length, path,
 * content length) followed by its bytes. Deliberately not zip — the launcher would
 * then need a zip decoder in C for no gain, since the fuser and the launcher are
 * shipped together and the payload never leaves that pair.
 *
 * MACOS SIGNING (important, and the reason section injection exists at all)
 *
 * Appending data after a Mach-O invalidates any existing code signature, and the
 * OS will refuse to run the result on arm64. So on macOS the order is:
 *
 *     fuse -> codesign -> notarize -> staple
 *
 * NEVER sign the launcher and then fuse. Deno solves this with real section
 * injection (Sui) so the payload is inside the signed image; that is the better
 * long-term answer and is tracked, but it needs a per-format object editor. The
 * append model works identically on ELF/PE/Mach-O today provided signing happens
 * last, which is what scripts/package.sh does.
 */
import { readFileSync, writeFileSync, readdirSync, statSync, chmodSync, existsSync } from 'node:fs';
import { join, relative, sep, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const MAGIC = Buffer.from('JSGLQBND', 'ascii');
const VERSION = 1;

/* Files that must never be fused into a shipped binary. */
const EXCLUDE_DIRS = new Set(['node_modules', '.git', 'dist', '.cache', 'build-cmake']);
const EXCLUDE_FILES = new Set(['package-lock.json', '.DS_Store', 'bench-opts.json']);

function collect(dir, root = dir, out = []) {
  for (const name of readdirSync(dir)) {
    const full = join(dir, name);
    const st = statSync(full);
    if (st.isDirectory()) {
      if (EXCLUDE_DIRS.has(name)) continue;
      collect(full, root, out);
    } else if (st.isFile()) {
      if (EXCLUDE_FILES.has(name)) continue;
      if (name.endsWith('.wasc')) continue;   // packed carts belong to wasmcart
      out.push({ abs: full, rel: relative(root, full).split(sep).join('/') });
    }
  }
  return out;
}

function crc32(buf) {
  let table = crc32.table;
  if (!table) {
    table = crc32.table = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      table[n] = c;
    }
  }
  let c = -1;
  for (let i = 0; i < buf.length; i++) c = table[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

function buildPayload(files) {
  const chunks = [];
  const count = Buffer.alloc(4);
  count.writeUInt32LE(files.length, 0);
  chunks.push(count);

  for (const f of files) {
    const path = Buffer.from(f.rel, 'utf8');
    const data = readFileSync(f.abs);
    const head = Buffer.alloc(8);
    head.writeUInt32LE(path.length, 0);
    head.writeUInt32LE(data.length, 4);
    chunks.push(head, path, data);
  }
  return Buffer.concat(chunks);
}

function main() {
  const args = process.argv.slice(2);
  const positional = args.filter((a) => !a.startsWith('--'));
  const gameDir = positional[0];
  const outPath = positional[1];
  const launcherArg = args.find((a) => a.startsWith('--launcher='));
  const launcher = launcherArg ? launcherArg.split('=')[1] : 'build/jsglq';

  if (!gameDir || !outPath) {
    console.error('usage: fuse.mjs <game-dir> <output-binary> [--launcher=path]');
    process.exit(2);
  }
  if (!existsSync(launcher)) {
    console.error(`launcher not found: ${launcher} (build it first)`);
    process.exit(1);
  }
  if (!existsSync(gameDir)) {
    console.error(`game dir not found: ${gameDir}`);
    process.exit(1);
  }

  /*
   * The runtime layer travels WITH the game.
   *
   * A fused binary can be copied anywhere, so resolving the runtime relative to
   * the executable's directory (which is what an unfused launcher does) fails the
   * moment someone moves it. Embedding runtime/ under a reserved prefix makes the
   * binary genuinely self-contained, which is the entire point of fusing.
   */
  const runtimeDir = join(dirname(fileURLToPath(import.meta.url)), '..', 'runtime');
  if (!existsSync(runtimeDir)) {
    console.error(`runtime layer not found at ${runtimeDir}`);
    process.exit(1);
  }
  const runtimeFiles = collect(runtimeDir).map((f) => ({
    abs: f.abs, rel: '.jsglq-runtime/' + f.rel,
  }));

  const files = collect(gameDir).concat(runtimeFiles);
  if (!files.length) {
    console.error(`no files to fuse in ${gameDir}`);
    process.exit(1);
  }
  // An entry point must exist, or the fused binary is a launcher that cannot start
  // anything — better to fail here than to ship it.
  const hasEntry = collect(gameDir).some((f) =>
    ['main.js', 'index.js', 'src/main.js', 'src/index.js', 'game.js', 'package.json']
      .includes(f.rel));
  if (!hasEntry) {
    console.error(`no entry point in ${gameDir} ` +
                  `(need package.json main, main.js, index.js, src/main.js, ...)`);
    process.exit(1);
  }

  const base = readFileSync(launcher);
  const payload = buildPayload(files);

  const trailer = Buffer.alloc(32);
  MAGIC.copy(trailer, 0);
  trailer.writeUInt32LE(VERSION, 8);
  trailer.writeBigUInt64LE(BigInt(base.length), 12);
  trailer.writeBigUInt64LE(BigInt(payload.length), 20);
  trailer.writeUInt32LE(crc32(payload), 28);

  writeFileSync(outPath, Buffer.concat([base, payload, trailer]));
  chmodSync(outPath, 0o755);

  const mb = (n) => (n / 1024 / 1024).toFixed(2) + ' MB';
  console.log(`fused ${files.length} files (${mb(payload.length)}) into ${outPath}`);
  console.log(`  launcher ${mb(base.length)} + payload ${mb(payload.length)} = ${mb(base.length + payload.length + 32)}`);
  if (process.platform === 'darwin') {
    console.log('  NOTE: sign AFTER fusing — appending invalidates a prior signature.');
  }
}

main();
