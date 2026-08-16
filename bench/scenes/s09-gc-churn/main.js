/**
 * s09-gc-churn — allocation pressure and collector shape.
 *
 * V8 uses a generational, largely incremental collector. QuickJS uses reference
 * counting plus a stop-the-world cycle collector. Those have very different
 * *distributions*: V8 tends toward a higher mean with rare long pauses, QuickJS
 * toward steady cost with pauses only when cycles accumulate.
 *
 * So the number that matters here is NOT p50 but the p99/p50 spread. A runtime that
 * averages well but stalls 40 ms once a second is unshippable for a game, and only
 * the tail shows it. The harness records p99 for exactly this reason.
 *
 * The workload deliberately creates both:
 *   - acyclic garbage (objects + arrays that refcounting frees immediately)
 *   - reference cycles (which only the cycle collector can reclaim)
 */
import { autorun, drawCanary2d } from '../../harness.js';
import { get2d } from '../lib/scaffold.js';

const ACYCLIC_PER_FRAME = 30000;
const CYCLES_PER_FRAME = 2000;

export const scene = {
  name: 's09-gc-churn',

  setup({ width, height }) {
    const { canvas, ctx } = get2d(width, height);
    return { canvas, ctx, width, height, sink: 0, retained: [] };
  },

  step(t, frame) {
    let sink = 0;

    // Acyclic churn: short-lived objects and arrays. Refcounting reclaims these at
    // the moment the last reference drops; a tracing GC defers them.
    for (let i = 0; i < ACYCLIC_PER_FRAME; i++) {
      const o = { x: i, y: i * 2, tag: (i & 7) };
      const a = [o.x, o.y, o.tag];
      sink += a[0] + a[2];
    }

    // Cyclic churn: mutually referencing pairs. Refcounting CANNOT free these, so
    // they accumulate until the cycle collector runs. This is where a QuickJS pause
    // would appear if one is going to.
    for (let i = 0; i < CYCLES_PER_FRAME; i++) {
      const a = { id: i, peer: null, payload: null };
      const b = { id: -i, peer: a, payload: null };
      a.peer = b;
      sink += a.id + b.id;
    }

    // A small retained working set that turns over: models a game's live objects,
    // so the heap is not purely garbage (which would be an unrealistically easy
    // case for every collector).
    const retained = t.retained;
    retained.push({ frame, data: new Array(64).fill(frame) });
    if (retained.length > 240) retained.splice(0, 120);

    t.sink = sink;

    const { ctx, width, height } = t;
    ctx.fillStyle = '#0d1014';
    ctx.fillRect(0, 0, width, height);
    ctx.fillStyle = '#8fbf6f';
    ctx.fillRect(20, height / 2 - 10, ((frame % 120) / 120) * (width - 40), 20);
    ctx.fillStyle = '#c8d2dc';
    ctx.font = '14px sans-serif';
    ctx.fillText(`retained ${retained.length}`, 20, height / 2 + 34);
    drawCanary2d(ctx, width, height);
  },
};

autorun(scene);
