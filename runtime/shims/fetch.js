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

  /*
   * Remote request over the host's HTTP client.
   *
   * The host runs it on a worker thread and this polls for completion, so a
   * slow server cannot stall the frame loop. Local (game-directory) requests
   * still take the synchronous asset path below — they are a file read, and
   * making them async would change load ordering for every existing game.
   */
  function fetchRemote(url, init, method) {
    const http = g.__jsglq_http;
    if (!http) {
      throw new TypeError(
        `fetch: remote URLs are not available in this build (${url})`);
    }
    if (/^https:/i.test(url) && !http.tls) {
      throw new TypeError(
        `fetch: https:// requires TLS, which this build does not include (${url}). ` +
        'Use http:// or rebuild with mbedTLS.');
    }

    let headerBlock = '';
    const h = (init && init.headers) || {};
    const entries = h instanceof Headers ? [...h.entries()] : Object.entries(h);
    for (const [k, v] of entries) headerBlock += `${k}: ${v}\r\n`;

    const id = http.start(method, url, headerBlock, init && init.body);

    return new Promise((resolve, reject) => {
      const tick = () => {
        const r = http.poll(id);
        if (r === null) { g.setTimeout(tick, 4); return; }
        if (!r.ok) { reject(new TypeError(`fetch: ${r.error}`)); return; }

        // Parse the raw header block into a Headers object.
        const hdrs = {};
        for (const line of String(r.headers).split('\r\n').slice(1)) {
          const i = line.indexOf(':');
          if (i > 0) hdrs[line.slice(0, i).trim().toLowerCase()] = line.slice(i + 1).trim();
        }
        resolve(new Response(new Uint8Array(r.body), {
          status: r.status,
          statusText: r.statusText,
          url,
          headers: hdrs,
        }));
      };
      tick();
    });
  }

  async function fetchImpl(input, init = {}) {
    const url = typeof input === 'string' ? input : (input && input.url) || String(input);
    const method = ((init && init.method) || 'GET').toUpperCase();

    if (/^https?:\/\//i.test(url)) {
      return fetchRemote(url, init, method);
    }

    if (method !== 'GET' && method !== 'HEAD') {
      // A non-GET against the game's own directory has nowhere to go: assets are
      // read-only. Say so rather than appearing to succeed.
      throw new TypeError(
        `fetch: ${method} is not supported for local assets (${url}); ` +
        'only GET/HEAD read from the game directory');
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
