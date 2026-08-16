/**
 * Shared entity workload for s01/s02.
 *
 * Kept in one place so the 500 and 5000 variants differ ONLY in count. If the two
 * scenes drifted apart the scaling curve between them would mean nothing.
 */
import { drawCanary2d } from '../../harness.js';
import { get2d, palette, rgbCss } from './scaffold.js';

export function makeEntityScene(count, name) {
  return {
    name,
    setup({ width, height, rng }) {
      const { canvas, ctx } = get2d(width, height);
      const entities = [];
      for (let i = 0; i < count; i++) {
        entities.push({
          x: rng() * width, y: rng() * height,
          vx: (rng() - 0.5) * 120, vy: (rng() - 0.5) * 120,
          size: 3 + rng() * 5,
          depth: 0,
          color: rgbCss(palette(i)),
        });
      }
      return { canvas, ctx, entities, width, height };
    },
    step(t, frame, dt) {
      const { ctx, entities, width, height } = t;
      const s = dt / 1000;
      const cx = width / 2, cy = height / 2;

      for (let i = 0; i < entities.length; i++) {
        const e = entities[i];
        e.x += e.vx * s;
        e.y += e.vy * s;
        if (e.x < 0) { e.x = 0; e.vx = -e.vx; }
        else if (e.x > width) { e.x = width; e.vx = -e.vx; }
        if (e.y < 0) { e.y = 0; e.vy = -e.vy; }
        else if (e.y > height) { e.y = height; e.vy = -e.vy; }
        const dx = e.x - cx, dy = e.y - cy;
        e.depth = Math.sqrt(dx * dx + dy * dy);
      }

      entities.sort((a, b) => a.depth - b.depth);

      ctx.fillStyle = '#101216';
      ctx.fillRect(0, 0, width, height);
      for (let i = 0; i < entities.length; i++) {
        const e = entities[i];
        ctx.fillStyle = e.color;
        ctx.fillRect(e.x | 0, e.y | 0, e.size | 0, e.size | 0);
      }
      drawCanary2d(ctx, width, height);
    },
  };
}
