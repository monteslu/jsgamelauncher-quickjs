/*
 * Image and createImageBitmap.
 *
 * `createImageBitmap` is the single highest-leverage shim for three.js: its
 * ImageBitmapLoader path (fetch -> blob -> createImageBitmap) is the documented
 * worker-safe route and avoids faking `document.createElementNS('img')` entirely.
 *
 * Decoding is synchronous under the hood but the API is async, as the spec requires.
 * `onload` is delivered on a microtask rather than inline, because a game that sets
 * `img.src` and then `img.onload` on the very next line would otherwise miss its own
 * callback — a race that only shows up as a missing sprite.
 */

function decodeBytes(bytes) {
  const info = globalThis.__jsglq_decodeImage(bytes);
  return info;
}

function dataUrlToBytes(url) {
  const comma = url.indexOf(',');
  if (comma < 0) throw new TypeError('malformed data: URL');
  const meta = url.slice(5, comma);
  const payload = url.slice(comma + 1);
  if (/;base64/i.test(meta)) {
    const bin = globalThis.atob(payload);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }
  return new globalThis.TextEncoder().encode(decodeURIComponent(payload));
}

export function installImage(g) {
  class HTMLImageElement {
    constructor(width, height) {
      this._src = '';
      this.width = width || 0;
      this.height = height || 0;
      this.naturalWidth = 0;
      this.naturalHeight = 0;
      this.complete = false;
      this.crossOrigin = null;
      this.onload = null;
      this.onerror = null;
      this._listeners = new Map();
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

    _fire(type, arg) {
      const handler = this['on' + type];
      if (typeof handler === 'function') handler.call(this, arg);
      const l = this._listeners.get(type);
      if (l) for (const fn of l.slice()) fn.call(this, arg);
    }

    get src() { return this._src; }

    set src(value) {
      this._src = String(value);
      // Deliver on a microtask: a game commonly writes `img.src = ...` and only
      // then assigns `img.onload`, and firing inline would run before the handler
      // exists. That failure is invisible — the image is simply never used.
      Promise.resolve().then(async () => {
        try {
          let bytes;
          if (this._src.startsWith('data:')) {
            bytes = dataUrlToBytes(this._src);
          } else if (this._src.startsWith('blob:')) {
            const blob = g.__jsglq_resolveObjectURL(this._src);
            if (!blob) throw new Error(`object URL not found: ${this._src}`);
            bytes = new Uint8Array(await blob.arrayBuffer());
          } else {
            const res = await g.fetch(this._src);
            if (!res.ok) throw new Error(`image fetch failed: ${this._src} (${res.status})`);
            bytes = new Uint8Array(await res.arrayBuffer());
          }

          const info = decodeBytes(bytes);
          this._s2d = info.handle;
          this._rgba = new Uint8ClampedArray(info.rgba);
          this.width = this.naturalWidth = info.width;
          this.height = this.naturalHeight = info.height;
          this.complete = true;
          this._fire('load', { type: 'load', target: this });
        } catch (err) {
          this.complete = false;
          // Report it: a silently-failed image is the exact bug that made a corpus
          // game report zero drawImage calls during measurement.
          console.error(`[Image] ${this._src.slice(0, 120)}: ${err.message}`);
          this._fire('error', { type: 'error', target: this, message: err.message });
        }
      });
    }

    decode() {
      if (this.complete) return Promise.resolve();
      return new Promise((resolve, reject) => {
        this.addEventListener('load', () => resolve());
        this.addEventListener('error', (e) => reject(new Error(e.message || 'decode failed')));
      });
    }
  }

  class ImageBitmap {
    constructor(info) {
      this._s2d = info.handle;
      this._rgba = new Uint8ClampedArray(info.rgba);
      this.width = info.width;
      this.height = info.height;
    }
    close() { /* the renderer owns the texture; nothing to release here yet */ }
  }

  /*
   * createImageBitmap accepts what three.js actually passes it: a Blob (from
   * ImageBitmapLoader), an Image, a canvas, or an ImageData.
   */
  g.createImageBitmap = async function createImageBitmap(source) {
    if (!source) throw new TypeError('createImageBitmap: source is required');

    if (source instanceof ImageBitmap) return source;

    if (source._rgba && source.width && source.height) {
      // Already-decoded Image / ImageBitmap-like: re-upload as its own bitmap.
      const handle = g.__jsglq_imageCreate(source._rgba, source.width, source.height);
      return new ImageBitmap({
        handle, width: source.width, height: source.height, rgba: source._rgba.buffer,
      });
    }

    if (source.data && source.width && source.height) {
      // ImageData
      const handle = g.__jsglq_imageCreate(source.data, source.width, source.height);
      return new ImageBitmap({
        handle, width: source.width, height: source.height, rgba: source.data.buffer,
      });
    }

    if (typeof source.arrayBuffer === 'function') {
      const bytes = new Uint8Array(await source.arrayBuffer());
      return new ImageBitmap(decodeBytes(bytes));
    }

    throw new TypeError(
      'createImageBitmap: unsupported source. Accepts a Blob, Image, ImageData, ' +
      'or ImageBitmap.');
  };

  g.Image = HTMLImageElement;
  g.HTMLImageElement = HTMLImageElement;
  g.ImageBitmap = ImageBitmap;

  // ImageData is constructible in the browser and games do construct it.
  g.ImageData = class ImageData {
    constructor(a, b, c) {
      if (typeof a === 'number') {
        this.width = a; this.height = b;
        this.data = new Uint8ClampedArray(a * b * 4);
      } else {
        this.data = a; this.width = b;
        this.height = c ?? (a.length / 4 / b);
        const need = this.width * this.height * 4;
        if (a.length < need) {
          throw new RangeError(
            `ImageData: ${this.width}x${this.height} needs ${need} bytes but got ${a.length}`);
        }
      }
      this.colorSpace = 'srgb';
    }
  };
}
