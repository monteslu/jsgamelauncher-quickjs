/**
 * s04-sprites — the drawImage-heavy path, matching adventure-ai's shape.
 *
 * adventure-ai measured ~450 fps under QuickJS-in-WASM, the slowest of the three
 * wasmcart-jsgame reference numbers, because sprite blitting hits the canvas binding
 * hardest. This scene reproduces that shape.
 *
 * Sprites come from a real decoded image (a data: URL PNG, decoded through the
 * runtime's Image path) rather than a procedurally drawn canvas, because image
 * decode + upload is precisely what the launcher must get right and what
 * CANVAS2D_SURFACE_MEASURED.md warned about: with a stubbed Image, star-catcher
 * reported ZERO drawImage calls since it guards every draw on `img.complete`.
 * A scene that never loads a real image would silently measure nothing.
 */
import { autorun, drawCanary2d } from '../../harness.js';
import { get2d } from '../lib/scaffold.js';

const SPRITES = 1200;

// A 16x16 RGBA PNG, base64-inlined so the scene has no external asset dependency
// and decodes identically in every runtime.
// A real 16x16 RGBA PNG, base64-inlined so the scene has no external asset and
// decodes identically in every runtime. It is a GENERATED, valid PNG: an earlier
// hand-typed placeholder was not decodable at all, which surfaced as "decode
// failed" rather than as a wrong image — a useful reminder that inline test
// fixtures need to be produced by an encoder, not typed.
const SPRITE_PNG = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAI0lEQVR42mPQ7tr6nxLMMGoAdgMeHJHEikcNoKcBoymRNAwALe50y1hW5DEAAAAASUVORK5CYII=';

function loadSprite() {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = (e) => reject(new Error('sprite decode failed: ' + (e && e.message)));
    img.src = SPRITE_PNG;
  });
}

export const scene = {
  name: 's04-sprites',

  setup({ width, height, rng }) {
    const { canvas, ctx } = get2d(width, height);
    const sprites = [];
    for (let i = 0; i < SPRITES; i++) {
      sprites.push({
        x: rng() * width, y: rng() * height,
        vx: (rng() - 0.5) * 90, vy: (rng() - 0.5) * 90,
        rot: rng() * Math.PI * 2,
        spin: (rng() - 0.5) * 2,
        scale: 0.6 + rng() * 1.1,
      });
    }
    const t = { canvas, ctx, sprites, width, height, img: null, ready: false };
    // Kick the decode; step() renders a "loading" frame until it lands. The harness
    // discards 120 warmup frames, which is far more than a data-URL decode needs.
    loadSprite().then((img) => { t.img = img; t.ready = true; })
      .catch((err) => { t.error = err; });
    return t;
  },

  step(t, frame, dt) {
    const { ctx, sprites, width, height } = t;
    const s = dt / 1000;

    ctx.fillStyle = '#0d1014';
    ctx.fillRect(0, 0, width, height);

    if (t.error) throw t.error;   // fail loudly; never silently measure an empty scene

    if (!t.ready) {
      ctx.fillStyle = '#c8d2dc';
      ctx.font = '16px sans-serif';
      ctx.fillText('decoding sprite...', 20, height / 2);
      drawCanary2d(ctx, width, height);
      return;
    }

    for (let i = 0; i < sprites.length; i++) {
      const p = sprites[i];
      p.x += p.vx * s; p.y += p.vy * s;
      if (p.x < -16) p.x = width + 16; else if (p.x > width + 16) p.x = -16;
      if (p.y < -16) p.y = height + 16; else if (p.y > height + 16) p.y = -16;
      p.rot += p.spin * s;

      ctx.save();
      ctx.translate(p.x | 0, p.y | 0);
      ctx.rotate(p.rot);
      ctx.scale(p.scale, p.scale);
      ctx.drawImage(t.img, -8, -8);
      ctx.restore();
    }

    ctx.fillStyle = '#c8d2dc';
    ctx.font = '12px sans-serif';
    ctx.fillText(`sprites ${sprites.length}`, 12, 20);
    drawCanary2d(ctx, width, height);
  },
};

autorun(scene);
