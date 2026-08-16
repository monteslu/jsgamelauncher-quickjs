/**
 * Tri-runtime bench harness.
 *
 * The SAME file runs under all three runtimes under test (RUTs):
 *   qjs      - jsgamelauncher-quickjs (the product)
 *   node     - rungame on Node (the incumbent)
 *   browser  - Chrome via Playwright (correctness oracle + JIT upper bound)
 *
 * Rules this file exists to enforce:
 *  - Busy time and frame interval are measured SEPARATELY. Busy time answers the
 *    interpreter question; interval answers the pacing question. Conflating them
 *    hides both.
 *  - Workloads are deterministic: seeded PRNG, fixed virtual dt. Wall clock is
 *    measured, never consumed by the workload, so every runtime executes the same
 *    instruction stream and captures are comparable byte-for-byte.
 *  - Captures come from the frame that was scored, out of the buffer the scene drew
 *    into. A later read is a different frame.
 *  - Every scene carries an orientation canary so a flipped or mirrored frame can
 *    never pass a numeric check.
 */

/* ---------------------------------------------------------------- environment */

/**
 * Runtime detection.
 *
 * Order matters. rungame's vm realm has `window` AND `document` and deliberately
 * hides `process`, so a naive "has document and no process => browser" test
 * misidentifies it — which then mislabels every result file. Its tell is the `sdl`
 * escape hatch it hands the realm (launcher.js exposes `globalThis.sdl` and `_jsg`),
 * so that check has to come BEFORE the browser check, not after.
 */
export const RUNTIME = (() => {
  if (typeof globalThis.__JSGLQ__ !== 'undefined') return 'qjs';
  if (typeof globalThis.sdl !== 'undefined' || typeof globalThis._jsg !== 'undefined') return 'node';
  if (typeof globalThis.window !== 'undefined' && typeof globalThis.document !== 'undefined'
      && typeof globalThis.location !== 'undefined' && globalThis.location.search !== undefined) {
    return 'browser';
  }
  if (typeof globalThis.process !== 'undefined') return 'node';
  return 'unknown';
})();

const NOW = (() => {
  if (typeof performance !== 'undefined' && typeof performance.now === 'function') {
    return () => performance.now();
  }
  return () => Date.now();
})();

const DEFAULT_OPTS = {
  scene: 'unknown',
  mode: 'capped',     // 'capped' | 'uncapped'
  frames: 1800,       // capped mode: frames to measure (30s @60)
  seconds: 10,        // uncapped mode: wall seconds to measure
  warmup: 120,        // discarded frames
  capture: [],        // frame indices to capture (post-warmup numbering)
  width: 960,
  height: 540,
};

/**
 * Options channel.
 *
 * The browser gets them from the URL. The launchers CANNOT use process.env: rungame
 * runs games in a vm realm that deliberately hides `process`, so an env var set by
 * the runner is invisible to the scene. The portable channel is a file the runner
 * writes into the scene dir, fetched with the same `fetch` every runtime provides.
 * `readOptions()` returns whatever is already known synchronously; `loadOptions()`
 * is the async form the autorun path awaits.
 */
export function readOptions() {
  const defaults = { ...DEFAULT_OPTS };
  let raw = {};
  if (RUNTIME === 'browser' && typeof location !== 'undefined') {
    const p = new URLSearchParams(location.search);
    for (const [k, v] of p.entries()) raw[k] = v;
  } else if (globalThis.__BENCH_OPTS__) {
    raw = globalThis.__BENCH_OPTS__;
  }
  const o = { ...defaults };
  for (const k of Object.keys(defaults)) {
    if (raw[k] === undefined) continue;
    const d = defaults[k];
    if (Array.isArray(d)) {
      o[k] = String(raw[k]).split(',').filter(Boolean).map(Number);
    } else if (typeof d === 'number') {
      o[k] = Number(raw[k]);
    } else {
      o[k] = String(raw[k]);
    }
  }
  if (!o.capture.length) {
    // Default captures: one early, one mid, one late. Enough to catch a scene that
    // renders correctly once and then drifts.
    o.capture = [0, Math.floor(o.frames / 2), o.frames - 1];
  }
  return o;
}

/**
 * Async options load: reads bench-opts.json from the scene dir when present.
 * A missing file is normal (browser passes options by URL) and must not fail the run.
 */
export async function loadOptions() {
  const sync = readOptions();
  if (RUNTIME === 'browser') return sync;
  try {
    const res = await fetch('bench-opts.json');
    if (res && res.ok) {
      const fromFile = await res.json();
      globalThis.__BENCH_OPTS__ = fromFile;
      return readOptions();
    }
  } catch (_) {
    // No options file: run with defaults. Not an error.
  }
  return sync;
}

/* ------------------------------------------------------------------- prng/dt */

/** mulberry32: tiny, fast, and identical across engines (no engine Math.random). */
export function makeRng(seed) {
  let a = seed >>> 0;
  return function rng() {
    a |= 0; a = (a + 0x6D2B79F5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Fixed virtual timestep. Scenes MUST use this, never the wall clock. */
export const FIXED_DT = 1000 / 60;

/* ------------------------------------------------------------------ statistics */

function percentile(sorted, p) {
  if (!sorted.length) return 0;
  const idx = (sorted.length - 1) * p;
  const lo = Math.floor(idx), hi = Math.ceil(idx);
  if (lo === hi) return sorted[lo];
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (idx - lo);
}

function summarize(samples) {
  if (!samples.length) return { n: 0 };
  const s = Array.prototype.slice.call(samples).sort((a, b) => a - b);
  let sum = 0;
  for (let i = 0; i < s.length; i++) sum += s[i];
  return {
    n: s.length,
    min: s[0],
    p50: percentile(s, 0.5),
    p95: percentile(s, 0.95),
    p99: percentile(s, 0.99),
    max: s[s.length - 1],
    mean: sum / s.length,
  };
}

/* --------------------------------------------------------------- capture path */

/**
 * Read pixels out of the scene's own drawing surface.
 *
 * Both paths go through the same code in every runtime, so a differ mismatch is a
 * real difference and not an artifact of how the frame was fetched. Notably we never
 * use page.screenshot() in the browser: the compositor would scale and colour-manage
 * the result, which shows up as a diff against launcher output that is actually
 * identical at the canvas level.
 */
export function capturePixels(target) {
  if (!target) return null;
  const { canvas, gl, ctx } = target;
  /*
   * For GL, the drawing buffer is the truth — not canvas.width. A runtime may back a
   * "960x540" canvas with a smaller surface (rungame pins its GL display context to
   * 640x480), and reading canvas.width there asks for pixels that were never
   * rendered. The result is undefined data that looks like a rendering bug.
   */
  const w = gl ? (gl.drawingBufferWidth || canvas.width) : canvas.width;
  const h = gl ? (gl.drawingBufferHeight || canvas.height) : canvas.height;

  if (gl) {
    const buf = new Uint8Array(w * h * 4);
    // Read BEFORE any swap: after presentation the back buffer is undefined.
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, buf);
    // GL origin is bottom-left; normalize to top-left so all runtimes agree and so
    // the orientation canary means the same thing everywhere.
    const row = w * 4;
    const flipped = new Uint8Array(buf.length);
    for (let y = 0; y < h; y++) {
      flipped.set(buf.subarray((h - 1 - y) * row, (h - y) * row), y * row);
    }
    return { width: w, height: h, data: flipped };
  }

  const c = ctx || canvas.getContext('2d');
  const img = c.getImageData(0, 0, w, h);
  return { width: w, height: h, data: new Uint8Array(img.data.buffer.slice(0)) };
}

/** FNV-1a over the pixel buffer. Cheap, stable, and enough to spot a changed frame. */
export function crcPixels(px) {
  if (!px) return '0';
  const d = px.data;
  let hash = 0x811c9dc5;
  for (let i = 0; i < d.length; i++) {
    hash ^= d[i];
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return hash.toString(16).padStart(8, '0');
}

/** Count distinct RGBA colours. A liveness signal only: `>1` means "it drew". */
export function countColors(px) {
  if (!px) return 0;
  const seen = new Set();
  const d = px.data;
  for (let i = 0; i < d.length; i += 4) {
    seen.add((d[i] << 24 | d[i + 1] << 16 | d[i + 2] << 8 | d[i + 3]) >>> 0);
    if (seen.size > 4096) break;
  }
  return seen.size;
}

/**
 * Orientation canary: four distinct corner colours, asymmetric by construction.
 * A frame that is flipped, mirrored, or rotated cannot report the same corners, so
 * no amount of green pixel-count checks can hide it.
 */
export const CANARY = {
  tl: [255, 0, 0, 255],      // red
  tr: [0, 255, 0, 255],      // green
  bl: [0, 0, 255, 255],      // blue
  br: [255, 255, 0, 255],    // yellow
  size: 8,
};

export function drawCanary2d(ctx, w, h) {
  const s = CANARY.size;
  const put = (c, x, y) => {
    ctx.fillStyle = `rgb(${c[0]},${c[1]},${c[2]})`;
    ctx.fillRect(x, y, s, s);
  };
  put(CANARY.tl, 0, 0);
  put(CANARY.tr, w - s, 0);
  put(CANARY.bl, 0, h - s);
  put(CANARY.br, w - s, h - s);
}

export function drawCanaryGl(gl, w, h) {
  const s = CANARY.size;
  gl.enable(gl.SCISSOR_TEST);
  const put = (c, x, y) => {
    gl.scissor(x, y, s, s);
    gl.clearColor(c[0] / 255, c[1] / 255, c[2] / 255, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
  };
  // GL y is bottom-up: top-left in image space is y = h - s here.
  put(CANARY.tl, 0, h - s);
  put(CANARY.tr, w - s, h - s);
  put(CANARY.bl, 0, 0);
  put(CANARY.br, w - s, 0);
  gl.disable(gl.SCISSOR_TEST);
}

/** Verify canary corners in a captured (top-left origin) buffer. */
export function checkCanary(px) {
  if (!px) return { ok: false, reason: 'no capture' };
  const { width: w, height: h, data: d } = px;
  const at = (x, y) => {
    const i = (y * w + x) * 4;
    return [d[i], d[i + 1], d[i + 2]];
  };
  const near = (a, b) => Math.abs(a[0] - b[0]) < 24 && Math.abs(a[1] - b[1]) < 24
                      && Math.abs(a[2] - b[2]) < 24;
  const probe = Math.floor(CANARY.size / 2);
  const corners = {
    tl: at(probe, probe),
    tr: at(w - 1 - probe, probe),
    bl: at(probe, h - 1 - probe),
    br: at(w - 1 - probe, h - 1 - probe),
  };
  const bad = [];
  for (const k of ['tl', 'tr', 'bl', 'br']) {
    if (!near(corners[k], CANARY[k])) bad.push(`${k}=${corners[k].join(',')}`);
  }
  return { ok: bad.length === 0, corners, bad };
}

/* ----------------------------------------------------------- memory sampling */

function sampleMemory() {
  // Best-effort, per runtime. The runner also samples RSS externally, which is the
  // number we actually trust; this is a cheap in-process cross-check.
  try {
    if (RUNTIME === 'browser' && performance && performance.memory) {
      return { jsHeap: performance.memory.usedJSHeapSize };
    }
    if (typeof globalThis.__JSGLQ__ !== 'undefined' && globalThis.__JSGLQ__.memoryUsage) {
      return globalThis.__JSGLQ__.memoryUsage();
    }
    if (RUNTIME === 'node' && globalThis.process && globalThis.process.memoryUsage) {
      const m = globalThis.process.memoryUsage();
      return { rss: m.rss, jsHeap: m.heapUsed };
    }
  } catch (_) { /* sandboxes hide these; not fatal */ }
  return {};
}

/* -------------------------------------------------------------- environment id */

function describeEnv(target) {
  const env = { runtime: RUNTIME };
  try {
    if (RUNTIME === 'browser') {
      env.ua = navigator.userAgent;
    } else if (globalThis.process && globalThis.process.versions) {
      env.node = globalThis.process.versions.node;
    }
    if (globalThis.__JSGLQ__ && globalThis.__JSGLQ__.version) {
      env.qjs = globalThis.__JSGLQ__.version;
    }
    if (target && target.gl) {
      const gl = target.gl;
      const dbg = gl.getExtension('WEBGL_debug_renderer_info');
      env.glRenderer = dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL)
                           : gl.getParameter(gl.RENDERER);
      env.glVendor = dbg ? gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL)
                         : gl.getParameter(gl.VENDOR);
      env.glVersion = gl.getParameter(gl.VERSION);
    }
  } catch (_) { /* non-fatal */ }
  return env;
}

/* ------------------------------------------------------------------ the runner */

/**
 * Drive a scene and report.
 *
 * scene = {
 *   name, setup({width,height,rng}) -> target,
 *   step(target, frameIndex, dt, rng),   // fixed-dt simulation + draw
 *   teardown?(target)
 * }
 * target = { canvas, ctx?, gl?, ... }
 */
export function runScene(scene, optsOverride) {
  const opts = { ...readOptions(), ...(optsOverride || {}) };
  opts.scene = scene.name || opts.scene;

  const rng = makeRng(0x5eed1234);
  const target = scene.setup({ width: opts.width, height: opts.height, rng, opts });

  const busy = [];
  const interval = [];
  const captures = [];
  const captureSet = new Set(opts.capture);

  let frame = 0;               // counts warmup + measured
  let measured = 0;
  let lastFrameStart = 0;
  let startWall = 0;
  let memSamples = [];
  const uncapped = opts.mode === 'uncapped';
  const uncappedMs = opts.seconds * 1000;

  return new Promise((resolve) => {
    function finish() {
      const env = describeEnv(target);
      const wall = NOW() - startWall;
      let canaryResult = null;
      if (captures.length) canaryResult = captures[captures.length - 1].canary;

      const result = {
        scene: opts.scene,
        mode: opts.mode,
        frames: measured,
        wallMs: wall,
        fps: measured / (wall / 1000),
        busy: summarize(busy),
        interval: summarize(interval),
        memory: memSamples.length
          ? { first: memSamples[0], last: memSamples[memSamples.length - 1], samples: memSamples.length }
          : null,
        captures: captures.map((c) => ({
          frame: c.frame, crc: c.crc, colors: c.colors, canary: c.canary,
          width: c.width, height: c.height,
        })),
        canaryOk: canaryResult ? canaryResult.ok : null,
        env,
        harnessVersion: 1,
      };

      if (scene.teardown) { try { scene.teardown(target); } catch (_) {} }

      // Publish for every runtime's collection path.
      globalThis.__benchResult = result;
      globalThis.__benchCaptures = captures;   // raw pixels stay in-process for the differ hook
      const line = 'BENCH_JSON:' + JSON.stringify(result);
      if (typeof console !== 'undefined' && console.log) console.log(line);
      resolve(result);
    }

    function tick(_ts) {
      const now = NOW();
      if (frame === 0) { startWall = now; lastFrameStart = now; }
      else { interval.push(now - lastFrameStart); lastFrameStart = now; }

      const isWarmup = frame < opts.warmup;
      const t0 = NOW();
      scene.step(target, frame, FIXED_DT, rng);
      const t1 = NOW();

      if (!isWarmup) {
        busy.push(t1 - t0);
        // Capture from THIS frame, from the buffer the scene just drew into.
        if (captureSet.has(measured)) {
          const px = capturePixels(target);
          captures.push({
            frame: measured,
            crc: crcPixels(px),
            colors: countColors(px),
            canary: checkCanary(px),
            width: px ? px.width : 0,
            height: px ? px.height : 0,
            data: px ? px.data : null,
          });
        }
        if (measured % 60 === 0) {
          const m = sampleMemory();
          if (Object.keys(m).length) memSamples.push({ frame: measured, ...m });
        }
        measured++;
      }
      frame++;

      const done = uncapped
        ? (NOW() - startWall) >= uncappedMs + (opts.warmup * FIXED_DT)
        : measured >= opts.frames;
      if (done) { finish(); return; }

      requestAnimationFrame(tick);
    }

    // Interval measurement starts on the second frame; warmup absorbs the rest.
    requestAnimationFrame(tick);
  });
}

/** Scenes that want to be self-contained call this at module top level. */
export function autorun(scene) {
  const start = async () => {
    const opts = await loadOptions();
    return runScene(scene, opts);
  };
  if (RUNTIME === 'browser' && typeof document !== 'undefined' && document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', start);
  } else {
    start();
  }
}
