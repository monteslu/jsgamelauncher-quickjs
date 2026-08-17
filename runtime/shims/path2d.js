/*
 * Path2D.
 *
 * The 2D context's path support lives in C (src/bind_canvas2d.c), which keeps a
 * single current path per context — exactly what the canvas spec's implicit path
 * is. Path2D is a SEPARATE, reusable path object, so rather than adding a second
 * path store to the C layer, this records the path commands and replays them
 * onto the context immediately before the fill or stroke that uses them.
 *
 * The replay is not a performance compromise: it is the same sequence of C calls
 * the game would have made inline, and building the path is cheap next to
 * rasterizing it. What it buys is that a Path2D can be built once and drawn many
 * times, which is the entire reason the API exists.
 */

/* Methods that build a path. Anything else on a Path2D is not path construction
   and must not be silently accepted. */
const PATH_METHODS = [
  'moveTo', 'lineTo', 'closePath', 'arc', 'arcTo', 'ellipse',
  'quadraticCurveTo', 'bezierCurveTo', 'rect', 'roundRect',
];

export function installPath2D(g) {
  if (g.Path2D) return;

  class Path2D {
    constructor(source) {
      this._ops = [];
      if (source instanceof Path2D) {
        // Copy constructor: take a snapshot, so later edits to the source do
        // not retroactively change this path.
        this._ops = source._ops.slice();
      } else if (typeof source === 'string') {
        // SVG path data. Parsing it properly is a real job (arcs with flags,
        // relative commands, implicit repeats), and a half-parser that silently
        // mis-renders is worse than an honest refusal.
        throw new Error('Path2D(svgPathString) is not implemented; ' +
                        'build the path with moveTo/lineTo/arc instead');
      }
    }

    addPath(other) {
      if (other instanceof Path2D) this._ops.push(...other._ops);
    }

    /* Replay onto a real 2D context. Called by fill()/stroke() below. */
    _replay(ctx) {
      ctx.beginPath();
      for (const [name, args] of this._ops) {
        const fn = ctx[name];
        if (typeof fn === 'function') fn.apply(ctx, args);
      }
    }
  }

  for (const name of PATH_METHODS) {
    Path2D.prototype[name] = function (...args) {
      this._ops.push([name, args]);
      return undefined;
    };
  }

  g.Path2D = Path2D;

  /*
   * Teach the 2D context to accept a Path2D.
   *
   * fill(path), fill(path, rule), stroke(path) and clip(path) are the spec
   * overloads. Without this a game passing a Path2D would have it coerced to a
   * fill rule string, silently drawing the CURRENT path instead of the one it
   * asked for — a wrong picture rather than an error.
   */
  // The C layer gives each 2D context a prototype of its own — NOT
  // CanvasRenderingContext2D.prototype, which is only an "Illegal constructor"
  // marker. Worse, the DISPLAY context and OFFSCREEN contexts have DIFFERENT
  // prototypes (they come from separate implementations: the GL-backed
  // sprite2d path and the CPU offscreen path), so patching one leaves the other
  // silently unpatched. Collect every distinct prototype we can reach.
  const targets = [];
  const addProto = (ctx) => {
    if (!ctx) return;
    const proto = Object.getPrototypeOf(ctx);
    if (proto && typeof proto.fill === 'function' && !targets.includes(proto)) {
      targets.push(proto);
    }
  };
  try {
    const display = g.document && g.document.getElementById
      ? g.document.getElementById('game-canvas') : null;
    if (display) addProto(display.getContext('2d'));
  } catch { /* no display 2D context in this build */ }
  try {
    if (g.__jsglq_createCanvas) addProto(g.__jsglq_createCanvas(1, 1).getContext('2d'));
  } catch { /* no offscreen 2D context in this build */ }

  const patch = (obj, name) => {
    const original = obj[name];
    if (typeof original !== 'function') return;
    obj[name] = function (maybePath, ...rest) {
      if (maybePath instanceof Path2D) {
        maybePath._replay(this);
        return original.apply(this, rest);
      }
      return original.apply(this, [maybePath, ...rest]);
    };
  };

  for (const t of targets) {
    for (const name of ['fill', 'stroke', 'clip']) patch(t, name);
  }
  if (targets.length === 0) {
    // Nothing reachable to patch: say so rather than leaving Path2D looking
    // functional while fill(path) quietly draws the wrong thing.
    console.warn('jsglq: Path2D installed but no 2D context prototype was ' +
                 'reachable; fill(path)/stroke(path) will not work');
  }
}
