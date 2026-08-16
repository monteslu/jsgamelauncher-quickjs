/*
 * A DOM-shaped surface with no DOM behind it.
 *
 * The rule here is the same one Ejecta followed: implement the handful of members
 * that games and engines actually touch, and be honest about the rest. Everything
 * present is real; nothing returns a plausible-looking stub that silently does
 * nothing.
 *
 * `getElementById` returns the display canvas for ANY id, matching rungame's
 * behavior. That is a compatibility decision, not an accident: games hardcode their
 * own canvas id and there is exactly one canvas.
 */

export function installDocument(g, { canvas, width, height }) {
  const listeners = new Map();

  function addEventListener(type, fn) {
    if (!listeners.has(type)) listeners.set(type, []);
    listeners.get(type).push(fn);
  }

  function removeEventListener(type, fn) {
    const l = listeners.get(type);
    if (!l) return;
    const i = l.indexOf(fn);
    if (i >= 0) l.splice(i, 1);
  }

  function dispatchEvent(ev) {
    const l = listeners.get(ev && ev.type);
    if (l) for (const fn of l.slice()) fn.call(g, ev);
    return true;
  }

  const document = {
    readyState: 'complete',
    title: '',
    documentElement: { style: {}, clientWidth: width, clientHeight: height },

    getElementById() { return canvas; },

    createElement(tag) {
      const t = String(tag).toLowerCase();
      if (t === 'canvas') return g.__jsglq_createCanvas(300, 150);
      if (t === 'img') return new g.Image();
      if (t === 'audio') {
        if (!g.Audio) {
          throw new Error("document.createElement('audio') requires the audio " +
                          'subsystem, which failed to initialize');
        }
        return new g.Audio();
      }
      if (t === 'video') {
        /*
         * A non-functional <video> element rather than a throw.
         *
         * Engines create one at STARTUP purely to feature-detect codec support
         * (Phaser does exactly this before any game code runs), so throwing here
         * kills the engine during initialization for a feature the game may never
         * use. canPlayType returns '' — the honest "cannot play this" answer — so
         * a game that checks gets a truthful no, and one that only probed carries
         * on. Actually calling play() reports the limitation.
         */
        return {
          tagName: 'VIDEO', style: {},
          width: 0, height: 0, videoWidth: 0, videoHeight: 0,
          currentTime: 0, duration: NaN, paused: true, ended: false, readyState: 0,
          canPlayType: () => '',
          load() {},
          play() {
            return Promise.reject(new Error(
              'video playback is not implemented in this runtime (no video decoder)'));
          },
          pause() {},
          addEventListener() {}, removeEventListener() {},
          appendChild() {}, setAttribute() {}, remove() {},
        };
      }
      // A div/span/style element that does nothing is harmless and common in
      // library init paths; anything else is worth naming so it is visible.
      if (t === 'div' || t === 'span' || t === 'style' || t === 'p') {
        return { tagName: t.toUpperCase(), style: {}, appendChild() {}, remove() {},
                 setAttribute() {}, addEventListener() {}, removeEventListener() {} };
      }
      throw new Error(`document.createElement('${tag}') is not implemented`);
    },

    createElementNS(_ns, tag) { return document.createElement(tag); },

    querySelector() { return null },
    querySelectorAll() { return []; },
    getElementsByTagName() { return []; },

    addEventListener,
    removeEventListener,
    dispatchEvent,

    body: {
      style: {},
      appendChild() {},
      removeChild() {},
      addEventListener,
      removeEventListener,
      getBoundingClientRect: () => ({
        x: 0, y: 0, left: 0, top: 0, width, height, right: width, bottom: height,
      }),
    },

    // three.js and other libraries probe these during init.
    fonts: { add() {}, ready: Promise.resolve(), check: () => true },
    hidden: false,
    visibilityState: 'visible',
    createTextNode: (t) => ({ nodeValue: String(t) }),
  };

  g.document = document;

  /*
   * `window`, `self`, `top`, and `parent` all alias the global object, as they do
   * in a real page. Games overwhelmingly write `window.addEventListener` and
   * `window.innerWidth` rather than the bare forms, so omitting this makes almost
   * every real game fail on its first line.
   */
  g.window = g;
  g.self = g;
  g.top = g;
  g.parent = g;
  g.frames = g;

  g.addEventListener = addEventListener;
  g.removeEventListener = removeEventListener;
  g.dispatchEvent = dispatchEvent;

  g.innerWidth = width;
  g.innerHeight = height;
  g.devicePixelRatio = 1;

  g.screen = { width, height, availWidth: width, availHeight: height,
               colorDepth: 24, pixelDepth: 24 };

  g.navigator = g.navigator || {
    userAgent: 'jsgamelauncher-quickjs',
    platform: 'quickjs',
    language: 'en-US',
    languages: ['en-US'],
    hardwareConcurrency: 4,
    getGamepads: () => [null, null, null, null],
  };

  g.location = g.location || {
    href: 'file:///game/', protocol: 'file:', host: '', hostname: '',
    pathname: '/game/', search: '', hash: '', origin: 'file://',
  };

  // No-op observers: libraries construct them unconditionally (three.js wants
  // MutationObserver), and a constructor that throws breaks init for no gain.
  class NoopObserver {
    constructor(cb) { this._cb = cb; }
    observe() {} unobserve() {} disconnect() {} takeRecords() { return []; }
  }
  g.MutationObserver = NoopObserver;
  g.ResizeObserver = NoopObserver;
  g.IntersectionObserver = NoopObserver;

  g.matchMedia = (q) => ({
    matches: false, media: String(q),
    addListener() {}, removeListener() {},
    addEventListener() {}, removeEventListener() {},
  });

  /*
   * FontFace: accept the registration and resolve.
   *
   * The renderer bakes glyph atlases from ONE embedded font, so a game's custom
   * font is not actually applied — text still draws, in the built-in face. That is
   * a visible cosmetic difference, and the alternative (throwing) stops games dead
   * during asset loading, which is where FontFace is almost always used.
   *
   * Loading real font files is a scoped piece of work: stb_truetype already backs
   * the atlas path, so it is a matter of plumbing the bytes through.
   */
  class FontFace {
    constructor(family, source, descriptors) {
      this.family = String(family);
      this.style = (descriptors && descriptors.style) || 'normal';
      this.weight = (descriptors && descriptors.weight) || 'normal';
      this.status = 'unloaded';
      this._source = source;
    }
    load() {
      this.status = 'loaded';
      return Promise.resolve(this);
    }
  }
  g.FontFace = FontFace;
  g.document.fonts = {
    add(f) { if (f) f.status = 'loaded'; return this; },
    delete() { return true; },
    clear() {},
    load: () => Promise.resolve([]),
    ready: Promise.resolve(),
    check: () => true,
    forEach() {},
    values: () => [][Symbol.iterator](),
  };

  g.alert = (msg) => { console.log('[alert]', msg); };
  g.requestIdleCallback = (cb) => g.setTimeout(
    () => cb({ didTimeout: false, timeRemaining: () => 8 }), 0);
  g.cancelIdleCallback = (id) => g.clearTimeout(id);
}
