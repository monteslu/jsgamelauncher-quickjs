/*
 * fetch + Response over game-dir-rooted asset IO.
 *
 * A missing asset must produce a real 404-shaped Response with the SAME shape as a
 * 200: body methods present, `ok` false. rungame got this wrong — its 404 path
 * returned an object with no `_data`, no body methods — so `fetch(missing).then(r =>
 * r.arrayBuffer())` threw "not a function" and a missing file crashed the game
 * instead of 404ing. `await fetch()` happened to work, which is exactly why the
 * bug survived: the common path hid it.
 */

const io = globalThis.__jsglq_io;

function guessType(path) {
  const ext = String(path).split('.').pop().toLowerCase();
  return {
    js: 'text/javascript', mjs: 'text/javascript', json: 'application/json',
    html: 'text/html', css: 'text/css', txt: 'text/plain',
    png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif',
    webp: 'image/webp', svg: 'image/svg+xml',
    ogg: 'audio/ogg', mp3: 'audio/mpeg', wav: 'audio/wav', flac: 'audio/flac',
    wasm: 'application/wasm', ttf: 'font/ttf', otf: 'font/otf',
  }[ext] || 'application/octet-stream';
}

function normalize(url) {
  let u = String(url);
  if (u.startsWith('file://')) u = u.slice(7);
  // Strip an origin if a game built an absolute URL from location.href.
  const m = /^[a-z]+:\/\/[^/]*(\/.*)$/i.exec(u);
  if (m) u = m[1];
  if (u.startsWith('./')) u = u.slice(2);
  while (u.startsWith('/')) u = u.slice(1);
  const q = u.indexOf('?');
  if (q >= 0) u = u.slice(0, q);
  const h = u.indexOf('#');
  if (h >= 0) u = u.slice(0, h);
  return u;
}

export function installFetch(g) {
  class Headers {
    constructor(init) {
      this._m = new Map();
      if (init) for (const k of Object.keys(init)) this._m.set(k.toLowerCase(), String(init[k]));
    }
    get(k) { return this._m.get(String(k).toLowerCase()) ?? null; }
    has(k) { return this._m.has(String(k).toLowerCase()); }
    set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
    forEach(fn) { for (const [k, v] of this._m) fn(v, k, this); }
    entries() { return this._m.entries(); }
    keys() { return this._m.keys(); }
    values() { return this._m.values(); }
    [Symbol.iterator]() { return this._m.entries(); }
  }

  class Response {
    constructor(body, init = {}) {
      this._body = body;                       // ArrayBuffer | string | null
      this.status = init.status ?? 200;
      this.statusText = init.statusText ?? (this.status === 200 ? 'OK' : '');
      this.ok = this.status >= 200 && this.status < 300;
      this.headers = new Headers(init.headers || {});
      this.url = init.url || '';
      this.bodyUsed = false;
      this.type = 'basic';
      this.redirected = false;
    }

    async arrayBuffer() {
      this.bodyUsed = true;
      if (this._body == null) return new ArrayBuffer(0);
      if (typeof this._body === 'string') return new TextEncoder().encode(this._body).buffer;
      return this._body;
    }

    async text() {
      this.bodyUsed = true;
      if (this._body == null) return '';
      if (typeof this._body === 'string') return this._body;
      return new TextDecoder().decode(new Uint8Array(this._body));
    }

    async json() { return JSON.parse(await this.text()); }

    async blob() {
      const ab = await this.arrayBuffer();
      return new g.Blob([ab], { type: this.headers.get('content-type') || '' });
    }

    async bytes() { return new Uint8Array(await this.arrayBuffer()); }

    clone() {
      return new Response(this._body, {
        status: this.status, statusText: this.statusText, url: this.url,
        headers: Object.fromEntries(this.headers.entries()),
      });
    }
  }

  class Request {
    constructor(input, init = {}) {
      this.url = typeof input === 'string' ? input : input.url;
      this.method = (init.method || 'GET').toUpperCase();
      this.headers = new Headers(init.headers || {});
    }
  }

  async function fetchImpl(input, init = {}) {
    const url = typeof input === 'string' ? input : (input && input.url) || String(input);
    const method = ((init && init.method) || 'GET').toUpperCase();

    if (method !== 'GET' && method !== 'HEAD') {
      // Honest failure: there is no network in v1, and pretending a POST succeeded
      // would be worse than saying so.
      throw new TypeError(
        `fetch: only GET/HEAD are supported (got ${method}); ` +
        'network requests are not available in this runtime');
    }

    if (/^https?:\/\//i.test(url)) {
      throw new TypeError(
        `fetch: remote URLs are not supported in this runtime (${url}); ` +
        'games load assets from their own directory');
    }

    const path = normalize(url);
    const data = io.readAsset(path);

    // Both branches construct the SAME Response shape. This is the rungame bug
    // that made a missing asset crash instead of 404.
    if (data == null) {
      return new Response(null, { status: 404, statusText: 'Not Found', url });
    }
    return new Response(data, {
      status: 200, url, headers: { 'content-type': guessType(path) },
    });
  }

  g.Headers = Headers;
  g.Response = Response;
  g.Request = Request;
  g.fetch = fetchImpl;
}
