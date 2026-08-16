/**
 * Pixel differ for cross-runtime frame comparison.
 *
 * Discipline this file enforces:
 *  - Self-test before use. Oracle-vs-itself (browser run A vs browser run B) sets the
 *    noise floor. A threshold that was not measured is a fabricated threshold.
 *  - A must-fail control. `selfTestMustFail()` feeds the differ a deliberately shifted
 *    capture; if that reports a match, the differ is broken and everything it ever
 *    said is void.
 *  - Orientation is checked structurally (corner canary), never inferred from stats.
 *    A vertically flipped frame can have identical colour counts and a similar
 *    histogram; only the canary catches it.
 *
 * Capture file format (.cap): JSON header line + raw RGBA bytes, top-left origin.
 */

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import { deflateSync, inflateSync } from 'node:zlib';

/* ------------------------------------------------------------------- capture io */

export function writeCapture(path, { width, height, data, meta = {} }) {
  mkdirSync(dirname(path), { recursive: true });
  const header = JSON.stringify({ width, height, meta, encoding: 'deflate-rgba8' });
  const head = Buffer.from(header + '\n', 'utf8');
  const body = deflateSync(Buffer.from(data.buffer || data, data.byteOffset || 0, data.length));
  writeFileSync(path, Buffer.concat([head, body]));
}

export function readCapture(path) {
  const buf = readFileSync(path);
  const nl = buf.indexOf(0x0a);
  const header = JSON.parse(buf.subarray(0, nl).toString('utf8'));
  const body = inflateSync(buf.subarray(nl + 1));
  return { width: header.width, height: header.height, meta: header.meta, data: new Uint8Array(body) };
}

/* ---------------------------------------------------------------------- diffing */

/**
 * Compare two captures.
 * Returns per-channel max delta, differing-pixel ratio, and a bounding box of
 * differences (useful for spotting an offset-by-one blit vs a genuinely different
 * render).
 */
export function diff(a, b, { tolerance = 0 } = {}) {
  if (a.width !== b.width || a.height !== b.height) {
    return {
      ok: false, reason: 'dimension-mismatch',
      a: `${a.width}x${a.height}`, b: `${b.width}x${b.height}`,
    };
  }
  const n = a.width * a.height;
  let differing = 0, maxDelta = 0, sumDelta = 0;
  let minX = a.width, minY = a.height, maxX = -1, maxY = -1;

  for (let i = 0; i < n; i++) {
    const o = i * 4;
    const dr = Math.abs(a.data[o] - b.data[o]);
    const dg = Math.abs(a.data[o + 1] - b.data[o + 1]);
    const db = Math.abs(a.data[o + 2] - b.data[o + 2]);
    const da = Math.abs(a.data[o + 3] - b.data[o + 3]);
    const d = Math.max(dr, dg, db, da);
    if (d > maxDelta) maxDelta = d;
    sumDelta += d;
    if (d > tolerance) {
      differing++;
      const x = i % a.width, y = (i / a.width) | 0;
      if (x < minX) minX = x; if (x > maxX) maxX = x;
      if (y < minY) minY = y; if (y > maxY) maxY = y;
    }
  }

  return {
    ok: differing === 0,
    width: a.width, height: a.height,
    pixels: n,
    differing,
    differingRatio: differing / n,
    maxDelta,
    meanDelta: sumDelta / n,
    bbox: maxX < 0 ? null : { x: minX, y: minY, w: maxX - minX + 1, h: maxY - minY + 1 },
    tolerance,
  };
}

/**
 * Is B a vertically flipped A? Called on every mismatch, because "looks like noise"
 * and "is upside down" produce similar summary stats but need opposite fixes.
 */
export function isVerticalFlip(a, b) {
  if (a.width !== b.width || a.height !== b.height) return false;
  const row = a.width * 4;
  let bad = 0;
  const sample = Math.min(a.height, 64);
  for (let s = 0; s < sample; s++) {
    const y = Math.floor((s / sample) * a.height);
    const ay = y * row, by = (a.height - 1 - y) * row;
    for (let x = 0; x < row; x += 64) {
      if (Math.abs(a.data[ay + x] - b.data[by + x]) > 8) { bad++; break; }
    }
  }
  return bad < sample * 0.1;
}

/* ------------------------------------------------------------- side-by-side PNG */

/** Minimal PNG encoder (no deps). RGBA8, non-interlaced. */
function crc32(buf) {
  let c, table = crc32.table;
  if (!table) {
    table = crc32.table = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
      c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      table[n] = c;
    }
  }
  c = -1;
  for (let i = 0; i < buf.length; i++) c = table[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

function pngChunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td), 0);
  return Buffer.concat([len, td, crc]);
}

export function encodePng(width, height, rgba) {
  const raw = Buffer.alloc((width * 4 + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (width * 4 + 1)] = 0; // filter: none
    Buffer.from(rgba.buffer, rgba.byteOffset + y * width * 4, width * 4)
      .copy(raw, y * (width * 4 + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk('IHDR', ihdr),
    pngChunk('IDAT', deflateSync(raw)),
    pngChunk('IEND', Buffer.alloc(0)),
  ]);
}

/**
 * Stitch A | B | amplified-delta into one PNG. Every mismatch gets one of these
 * written, because the rule is to look at the rendered output, not just the numbers.
 */
export function writeSideBySide(path, a, b, { gap = 8 } = {}) {
  const w = a.width * 3 + gap * 2;
  const h = Math.max(a.height, b.height);
  const out = new Uint8Array(w * h * 4);
  out.fill(24);
  for (let i = 3; i < out.length; i += 4) out[i] = 255;

  const blit = (src, ox) => {
    for (let y = 0; y < src.height; y++) {
      for (let x = 0; x < src.width; x++) {
        const s = (y * src.width + x) * 4;
        const d = (y * w + x + ox) * 4;
        out[d] = src.data[s]; out[d + 1] = src.data[s + 1];
        out[d + 2] = src.data[s + 2]; out[d + 3] = 255;
      }
    }
  };
  blit(a, 0);
  blit(b, a.width + gap);

  const ox = (a.width + gap) * 2;
  for (let y = 0; y < Math.min(a.height, b.height); y++) {
    for (let x = 0; x < Math.min(a.width, b.width); x++) {
      const s = (y * a.width + x) * 4;
      const d = (y * w + x + ox) * 4;
      const dr = Math.abs(a.data[s] - b.data[s]);
      const dg = Math.abs(a.data[s + 1] - b.data[s + 1]);
      const db = Math.abs(a.data[s + 2] - b.data[s + 2]);
      const m = Math.min(255, Math.max(dr, dg, db) * 8);   // amplified so 1-2 LSB shows
      out[d] = m; out[d + 1] = m > 0 ? 255 - m : 0; out[d + 2] = 0; out[d + 3] = 255;
    }
  }
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, encodePng(w, h, out));
  return path;
}

/* ------------------------------------------------------------------- self-tests */

/**
 * Oracle-vs-itself. Two captures of the same scene from the SAME runtime must agree.
 * For CPU-rasterized 2D that means byte-exact; for GPU scenes it establishes the
 * noise floor that cross-runtime comparisons are then judged against.
 *
 * Returns the measured floor. Never invent this number.
 */
export function measureNoiseFloor(capturesA, capturesB) {
  if (capturesA.length !== capturesB.length) {
    throw new Error(`self-test capture count mismatch: ${capturesA.length} vs ${capturesB.length}`);
  }
  let worstDelta = 0, worstRatio = 0;
  const perFrame = [];
  for (let i = 0; i < capturesA.length; i++) {
    const d = diff(capturesA[i], capturesB[i], { tolerance: 0 });
    if (d.reason) throw new Error(`self-test frame ${i}: ${d.reason}`);
    worstDelta = Math.max(worstDelta, d.maxDelta);
    worstRatio = Math.max(worstRatio, d.differingRatio);
    perFrame.push({ frame: i, maxDelta: d.maxDelta, differingRatio: d.differingRatio });
  }
  return { exact: worstDelta === 0, maxDelta: worstDelta, maxRatio: worstRatio, perFrame };
}

/**
 * The must-fail control. Shift a capture by one pixel and confirm the differ SEES it.
 * If this ever passes, the differ is broken; a green suite proves nothing.
 */
export function selfTestMustFail() {
  const w = 64, h = 64;
  const base = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      base[i] = (x * 4) & 255; base[i + 1] = (y * 4) & 255; base[i + 2] = 128; base[i + 3] = 255;
    }
  }
  const shifted = new Uint8Array(base.length);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const src = (y * w + Math.max(0, x - 1)) * 4;
      const dst = (y * w + x) * 4;
      shifted[dst] = base[src]; shifted[dst + 1] = base[src + 1];
      shifted[dst + 2] = base[src + 2]; shifted[dst + 3] = base[src + 3];
    }
  }
  const a = { width: w, height: h, data: base };
  const b = { width: w, height: h, data: shifted };

  const shiftedResult = diff(a, b, { tolerance: 0 });
  const identicalResult = diff(a, { width: w, height: h, data: base.slice() }, { tolerance: 0 });
  const flipCheck = (() => {
    const flipped = new Uint8Array(base.length);
    const row = w * 4;
    for (let y = 0; y < h; y++) flipped.set(base.subarray((h - 1 - y) * row, (h - y) * row), y * row);
    return isVerticalFlip(a, { width: w, height: h, data: flipped });
  })();

  const problems = [];
  if (shiftedResult.ok) problems.push('differ reported a 1px-shifted image as matching');
  if (!identicalResult.ok) problems.push('differ reported an identical image as differing');
  if (!flipCheck) problems.push('flip detector failed to recognize a vertically flipped image');

  return { ok: problems.length === 0, problems, shiftedResult, identicalResult };
}

/* -------------------------------------------------------------------------- cli */

if (import.meta.url === `file://${process.argv[1]}`) {
  const [cmd, ...args] = process.argv.slice(2);
  if (cmd === 'selftest') {
    const r = selfTestMustFail();
    console.log(JSON.stringify(r, null, 2));
    if (!r.ok) { console.error('DIFFER SELF-TEST FAILED'); process.exit(1); }
    console.log('differ self-test OK (shift detected, identity matched, flip detected)');
  } else if (cmd === 'compare') {
    const [pa, pb, out] = args;
    const a = readCapture(pa), b = readCapture(pb);
    const d = diff(a, b, { tolerance: Number(process.env.TOLERANCE || 0) });
    if (!d.ok && isVerticalFlip(a, b)) d.verticalFlip = true;
    console.log(JSON.stringify(d, null, 2));
    if (out) console.log('wrote', writeSideBySide(out, a, b));
    process.exit(d.ok ? 0 : 1);
  } else {
    console.log('usage: differ.mjs selftest | compare <a.cap> <b.cap> [out.png]');
    process.exit(2);
  }
}
