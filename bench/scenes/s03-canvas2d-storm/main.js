/**
 * s03-canvas2d-storm — the 2D binding cost, weighted to real corpus usage.
 *
 * The call mix comes from instrumenting a 14-game corpus at runtime (30 frames
 * each), which produced roughly:
 *
 *   fillRect 2550 | fillText 270 | drawImage 60 | save/restore/translate/rotate 30
 *   fillStyle 660 sets | font 210 sets
 *
 * Normalized per frame and scaled up, that is roughly 85:9:2:1 for
 * fillRect:fillText:drawImage:transform-pairs. Inventing a "balanced" mix here
 * would measure a workload no real game runs; this measures the one they do.
 *
 * drawImage uses an offscreen canvas as its source rather than a decoded PNG so the
 * scene has no asset dependency and stays byte-deterministic across runtimes. The
 * image path with real decoding is exercised by s04.
 */
import { autorun, drawCanary2d } from '../../harness.js';
import { get2d, makeOffscreen, palette, rgbCss } from '../lib/scaffold.js';

const FILL_RECTS = 850;
const FILL_TEXTS = 90;
const DRAW_IMAGES = 20;
const TRANSFORM_GROUPS = 10;

function makeSpriteSource() {
  // A small procedurally drawn canvas: deterministic in every runtime, no decode.
  // MUST be offscreen: getCanvas() returns the display canvas in launcher runtimes.
  const c = makeOffscreen(32, 32);
  const g = c.getContext('2d');
  g.fillStyle = '#2b8ab5';
  g.fillRect(0, 0, 32, 32);
  g.fillStyle = '#f0c419';
  g.fillRect(6, 6, 20, 20);
  g.fillStyle = '#e0524a';
  g.fillRect(12, 12, 8, 8);
  return c;
}

export const scene = {
  name: 's03-canvas2d-storm',

  setup({ width, height, rng }) {
    const { canvas, ctx } = get2d(width, height);
    // A second canvas for drawImage sources. getCanvas returns the game canvas in
    // launcher runtimes when asked by id, so build this one via createElement.
    const sprite = makeSpriteSource();

    const rects = [];
    for (let i = 0; i < FILL_RECTS; i++) {
      rects.push({
        x: rng() * width, y: rng() * height,
        w: 4 + rng() * 40, h: 4 + rng() * 24,
        color: rgbCss(palette(i)),
        phase: rng() * Math.PI * 2,
      });
    }
    const texts = [];
    for (let i = 0; i < FILL_TEXTS; i++) {
      texts.push({
        x: rng() * width, y: 12 + rng() * (height - 24),
        s: `score ${(rng() * 99999) | 0}`,
        color: rgbCss(palette(i * 7)),
      });
    }
    const sprites = [];
    for (let i = 0; i < DRAW_IMAGES; i++) {
      sprites.push({ x: rng() * width, y: rng() * height, spin: rng() * Math.PI * 2 });
    }
    return { canvas, ctx, sprite, rects, texts, sprites, width, height };
  },

  step(t, frame) {
    const { ctx, sprite, rects, texts, sprites, width, height } = t;
    const time = frame / 60;

    ctx.fillStyle = '#0d1014';
    ctx.fillRect(0, 0, width, height);

    // fillRect + fillStyle: the dominant pair in every measured game.
    for (let i = 0; i < rects.length; i++) {
      const r = rects[i];
      const wob = Math.sin(time + r.phase) * 6;
      ctx.fillStyle = r.color;
      ctx.fillRect((r.x + wob) | 0, r.y | 0, r.w | 0, r.h | 0);
    }

    // strokeRect + lineWidth: present in the corpus, cheap, included for coverage.
    ctx.strokeStyle = '#3a4450';
    ctx.lineWidth = 2;
    for (let i = 0; i < 40; i++) {
      ctx.strokeRect(8 + i * 12, 8, 10, 10);
    }

    // fillText + font: second most common call in the corpus.
    ctx.font = '12px sans-serif';
    ctx.textAlign = 'left';
    for (let i = 0; i < texts.length; i++) {
      const tx = texts[i];
      ctx.fillStyle = tx.color;
      ctx.fillText(tx.s, tx.x | 0, tx.y | 0);
    }

    // save/restore/translate/rotate + drawImage: the sprite path.
    for (let i = 0; i < sprites.length; i++) {
      const s = sprites[i];
      ctx.save();
      ctx.translate(s.x | 0, s.y | 0);
      ctx.rotate(s.spin + time);
      ctx.globalAlpha = 0.9;
      ctx.drawImage(sprite, -16, -16);
      ctx.restore();
    }
    ctx.globalAlpha = 1;

    // A measureText call per frame: real text layout needs it, and it is the one
    // method the 30-frame corpus window never reached but every real HUD uses.
    const m = ctx.measureText('score 00000');
    if (m && m.width > 0) {
      ctx.fillStyle = '#7f8a99';
      ctx.fillRect(width - 120, height - 20, Math.min(100, m.width) | 0, 3);
    }

    drawCanary2d(ctx, width, height);
  },
};

autorun(scene);
