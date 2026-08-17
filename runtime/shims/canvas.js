/*
 * Canvas + WebGL2 context wiring.
 *
 * The display canvas is backed by webgl-node running UNMODIFIED on top of the host's
 * `gl` object. webgl-node is 2058 lines of pure ESM with zero Node imports, so it
 * needs no porting at all — it just needs a native-gles-shaped module, which the
 * host provides.
 *
 * Canvas 2D is a phase-2 deliverable. Until then `getContext('2d')` throws BY NAME
 * rather than returning a stub: a silent no-op context is how "every check green,
 * every logo invisible" happens.
 */

import { createWebGL2Context } from 'webgl-node';
import { createOffscreen2D } from './offscreen2d.js';

export function installCanvas(g, { width, height }) {
  let displayContext = null;

  class HTMLCanvasElement {
    constructor(w, h, isMain) {
      this._width = w;
      this._height = h;
      this._isMain = !!isMain;
      this._ctx = null;
      this._ctxType = null;
      this.style = {};
      this._listeners = new Map();
    }

    get width() { return this._width; }
    set width(v) {
      const n = v | 0;
      if (n === this._width) return;
      this._width = n;
      this._resize();
    }

    get height() { return this._height; }
    set height(v) {
      const n = v | 0;
      if (n === this._height) return;
      this._height = n;
      this._resize();
    }

    get clientWidth() { return this._width; }
    get clientHeight() { return this._height; }

    _resize() {
      // The host owns the real surface. A game may set canvas.width to anything;
      // what it gets back is the drawing buffer that actually exists, and
      // gl.drawingBufferWidth/Height report that truth. Silently pretending the
      // requested size took effect is what produces readbacks of pixels that were
      // never rendered.
      if (this._ctx && this._ctxType === 'webgl2' && displayContext) {
        displayContext.resize?.(this._width, this._height);
      }
      /*
       * The 2D renderer is sized at startup from the host window. A game that
       * sets canvas.width afterwards would otherwise draw outside the surface
       * and read back black — the same failure just fixed in rungame's GL FBO.
       */
      if (this._isMain && g.__jsglq_canvasResize) {
        g.__jsglq_canvasResize(this._width, this._height);
      }
    }

    getContext(type, attrs) {
      if (type === 'webgl2' || type === 'webgl' || type === 'experimental-webgl') {
        if (this._ctx && this._ctxType === 'webgl2') return this._ctx;
        if (this._ctx) {
          throw new Error(
            `canvas already has a '${this._ctxType}' context; a canvas cannot host both`);
        }

        /*
         * An engine that creates its OWN canvas still has to reach the screen.
         *
         * Phaser (and anything else that builds its own canvas rather than taking
         * one from the document) would otherwise render into a surface nothing
         * ever presents: the game runs perfectly and the window stays black, with
         * no error to explain it. There is exactly one GL surface here, so the
         * first canvas to ask for a GL context becomes the display canvas.
         */
        /*
         * Only a REAL canvas takes the display surface. Engines probe GL support
         * with a 1x1 throwaway canvas first (Phaser does exactly this), and
         * handing that the display would point presentation at a one-pixel
         * surface — the game then renders correctly into something invisible.
         */
        /*
         * A REAL canvas claims the display surface, and may take it over from a
         * probe-sized one. Engines create a 1x1 canvas to test GL support before
         * building the canvas they actually render into (Phaser does exactly
         * this), so "first GL context wins" points presentation at a one-pixel
         * surface and the game renders correctly into something invisible.
         */
        const isReal = this._width > 16 && this._height > 16;
        const current = g.__jsglq_displayCanvas;
        /*
         * Take the display surface if the current holder is not actually drawing:
         * either it is a probe-sized canvas, or it is the document's canvas that
         * the game never asked for a context on. An engine that builds its own
         * canvas (Phaser) leaves the document one untouched, so insisting the
         * document canvas is the display means presentation reads a surface
         * nothing ever drew into — a black window with no error.
         */
        const displayUnused = !current
          || (current !== this && !current._ctx)
          || (current._ctxType === 'webgl2' && current._width <= 16);
        if (!this._isMain && isReal && displayUnused) {
          this._isMain = true;
          g.__jsglq_displayCanvas = this;
          console.log('[canvas] a game-created canvas took the display surface ' +
                      `(${this._width}x${this._height})`);
        }
        /*
         * There is ONE GL surface, so every GL context shares it. An engine
         * probing support with a 1x1 canvas and then building its real 640x480
         * one must end up presenting from the real one; the probe context is
         * handed the same underlying GL and simply never becomes the display.
         */
        if (!displayContext) {
          displayContext = createWebGL2Context(this._width, this._height, attrs || {});
        } else if (this._width > 16 && this._height > 16) {
          /*
           * Resize the shared surface to this canvas.
           *
           * The context may have been created for a 1x1 probe canvas, and the
           * real canvas arrives afterwards. Without this the game renders into a
           * one-pixel drawing buffer: every call succeeds, readback returns a
           * single black pixel, and nothing explains why the screen is empty.
           */
          displayContext.resize?.(this._width, this._height);
        }
        this._ctx = displayContext.gl;
        this._ctxType = 'webgl2';
        // Point the context's canvas at THIS element so `gl.canvas.width` and
        // three.js's canvas probing see the game's canvas, not webgl-node's mock.
        this._ctx.canvas = this;
        return this._ctx;
      }

      if (type === '2d') {
        if (this._ctx && this._ctxType === '2d') return this._ctx;
        if (this._ctx) {
          throw new Error(
            `canvas already has a '${this._ctxType}' context; a canvas cannot host both`);
        }
        // Offscreen canvases get the CPU path: the GL renderer owns the single
        // default framebuffer and cannot draw into a second surface.
        if (!this._isMain) {
          const soft = createOffscreen2D(this);
          this._ctx = soft;
          this._ctxType = '2d';
          return soft;
        }
        /*
         * Backed by the sprite2d renderer (batched textured quads on GL). The
         * implemented surface is the one the measured corpus uses; every other
         * spec member throws by name rather than no-op'ing.
         *
         * KNOWN GAP (tracked): the display canvas is NOT yet persistent across
         * frames. It renders into the default framebuffer, so content a game
         * draws on one frame is not guaranteed to still be there on the next.
         * Every game in the measured corpus redraws its full frame every frame,
         * so this does not affect them — but a game that draws a background once
         * and relies on it persisting will see it disappear.
         *
         * The fix is to render into an owned FBO and blit that to the default
         * framebuffer for presentation. An attempt at this is reverted rather
         * than half-landed: it regressed working rendering, and a broken
         * persistent surface is worse than a documented non-persistent one.
         */
        const c2d = Object.create(g.__jsglq_ctx2dProto);
        c2d.canvas = this;
        this._ctx = c2d;
        this._ctxType = '2d';
        /*
         * Size the renderer to THIS canvas at context creation.
         *
         * A game commonly sets canvas.width/height BEFORE asking for a context,
         * so relying only on the width setter leaves the renderer at the host's
         * startup size: every draw then lands outside the surface and reads back
         * black, with no error anywhere.
         */
        if (g.__jsglq_canvasResize) {
          g.__jsglq_canvasResize(this._width, this._height);
          // Report what was actually granted: the surface may be smaller than
          // requested, and a game reading canvas.width deserves the truth.
          const real = g.__jsglq_canvasSize && g.__jsglq_canvasSize();
          if (real && (real.width !== this._width || real.height !== this._height)) {
            this._width = real.width;
            this._height = real.height;
          }
        }
        return c2d;
      }

      return null;
    }

    addEventListener(type, fn) {
      if (!this._listeners.has(type)) this._listeners.set(type, []);
      this._listeners.get(type).push(fn);
    }

    removeEventListener(type, fn) {
      const l = this._listeners.get(type);
      if (!l) return;
      const i = l.indexOf(fn);
      if (i >= 0) l.splice(i, 1);
    }

    dispatchEvent(ev) {
      const l = this._listeners.get(ev && ev.type);
      if (l) for (const fn of l.slice()) fn.call(this, ev);
      return true;
    }

    getBoundingClientRect() {
      return {
        x: 0, y: 0, left: 0, top: 0,
        width: this._width, height: this._height,
        right: this._width, bottom: this._height,
      };
    }

    /*
     * Offscreen canvases are legal drawImage sources and the corpus uses them —
     * typically to build a sprite sheet once at load and blit it thereafter.
     *
     * They are backed by a CPU pixel buffer rather than a GL render target,
     * because the renderer draws into a single default framebuffer. Uploading
     * happens lazily on first use and again only when the buffer changed, so the
     * common "draw once, blit many" pattern costs one upload.
     *
     * The DISPLAY canvas is deliberately excluded: sourcing it would mean a full
     * framebuffer readback inside the frame, exactly the CPU round-trip the
     * GL-direct path exists to avoid.
     */
    get _s2d() {
      if (this._isMain) {
        throw new Error(
          'drawImage(displayCanvas) is not supported: it would require a full ' +
          'framebuffer readback every draw. Render to an offscreen canvas instead.');
      }
      if (!this._pixels) {
        throw new Error(
          'drawImage(canvas): this canvas has nothing drawn on it yet ' +
          "(no 2d context was created, so there are no pixels to read)");
      }
      if (this._s2dHandle === undefined || this._s2dDirty) {
        this._s2dHandle = g.__jsglq_imageCreate(this._pixels, this._width, this._height);
        this._s2dDirty = false;
      }
      return this._s2dHandle;
    }

    // Games and libraries probe for these; returning something honest beats
    // throwing during feature detection.
    toDataURL() { return 'data:,'; }
  }

  /* Every canvas is registered so tooling can find the one actually being drawn
     into; a game may render into its own rather than the document's. */
  g.__jsglq_allCanvases = [];
  const origCtor = HTMLCanvasElement;
  const displayCanvas = new HTMLCanvasElement(width, height, true);
  g.__jsglq_allCanvases.push(displayCanvas);

  g.HTMLCanvasElement = HTMLCanvasElement;

  /*
   * Engines feature-detect by checking for these CONSTRUCTORS on window (Phaser
   * does exactly `!!window.CanvasRenderingContext2D` before deciding it can render
   * at all). Without them a fully working 2D context is reported as unavailable
   * and the engine refuses to boot.
   */
  if (!g.CanvasRenderingContext2D) {
    g.CanvasRenderingContext2D = function CanvasRenderingContext2D() {
      throw new TypeError('Illegal constructor');
    };
  }
  if (!g.WebGLRenderingContext) {
    g.WebGLRenderingContext = function WebGLRenderingContext() {
      throw new TypeError('Illegal constructor');
    };
  }
  // The context this runtime actually hands out is WebGL2. Libraries branch on
  // `typeof WebGL2RenderingContext !== 'undefined'` to pick their WebGL2 path,
  // and without the global they silently fall back to the WebGL1 path — working,
  // but slower and without the features the runtime does support.
  if (!g.WebGL2RenderingContext) {
    g.WebGL2RenderingContext = function WebGL2RenderingContext() {
      throw new TypeError('Illegal constructor');
    };
  }
  g.__jsglq_createCanvas = (w, h) => {
    const c = new HTMLCanvasElement(w | 0, h | 0, false);
    g.__jsglq_allCanvases.push(c);
    return c;
  };

  /*
   * OffscreenCanvas.
   *
   * Backed by the same offscreen canvas document.createElement('canvas')
   * returns, which is what jsgamelauncher does too. It is not transferable to a
   * Worker here (the GL context and the 2D backing store live on the main
   * thread), so transferControlToOffscreen is deliberately absent rather than
   * present and broken.
   */
  if (!g.OffscreenCanvas) {
    g.OffscreenCanvas = class OffscreenCanvas {
      constructor(width, height) {
        const c = g.__jsglq_createCanvas(width | 0, height | 0);
        // convertToBlob is the one OffscreenCanvas-specific method games reach
        // for; name it rather than letting it be a silent undefined.
        c.convertToBlob = () => {
          throw new Error('OffscreenCanvas.convertToBlob is not implemented');
        };
        return c;
      }
    };
  }
  g.__jsglq_displayCanvas = displayCanvas;

  return displayCanvas;
}
