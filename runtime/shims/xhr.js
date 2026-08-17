/*
 * XMLHttpRequest over the same game-dir asset IO that fetch uses.
 *
 * Still required in 2026 because engines predate fetch and never migrated:
 * Phaser's loader is XHR-based, and without this it cannot load a single asset
 * even though everything underneath works.
 *
 * Reads are synchronous underneath but the callbacks are delivered on a microtask,
 * because the async contract is what callers are written against — a handler
 * assigned on the line AFTER .send() must still fire.
 */

export function installXHR(g) {
  const UNSENT = 0, OPENED = 1, HEADERS_RECEIVED = 2, LOADING = 3, DONE = 4;

  class XMLHttpRequest {
    constructor() {
      this.readyState = UNSENT;
      this.status = 0;
      this.statusText = '';
      this.response = null;
      this.responseText = '';
      this.responseType = '';
      this.responseURL = '';
      this.timeout = 0;
      this.withCredentials = false;
      this.onreadystatechange = null;
      this.onload = null;
      this.onerror = null;
      this.onprogress = null;
      this.onloadend = null;
      this._headers = {};
      this._listeners = new Map();
      this._method = 'GET';
      this._url = '';
    }

    open(method, url) {
      this._method = String(method || 'GET').toUpperCase();
      this._url = String(url);
      this.readyState = OPENED;
      this._emitReadyState();
    }

    setRequestHeader(k, v) { this._headers[String(k).toLowerCase()] = String(v); }
    getAllResponseHeaders() {
      return Object.entries(this._respHeaders || {})
        .map(([k, v]) => `${k}: ${v}`).join('\r\n');
    }
    getResponseHeader(k) {
      return (this._respHeaders || {})[String(k).toLowerCase()] ?? null;
    }
    abort() { this.readyState = UNSENT; }
    overrideMimeType() { /* the asset layer decides the type */ }

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

    _fire(type) {
      const ev = { type, target: this, currentTarget: this,
                   loaded: this._len || 0, total: this._len || 0, lengthComputable: true };
      const h = this['on' + type];
      if (typeof h === 'function') {
        try { h.call(this, ev); } catch (err) {
          console.error(`[XHR] on${type} threw:`, err && (err.stack || err.message));
        }
      }
      const l = this._listeners.get(type);
      if (l) for (const fn of l.slice()) {
        try { fn.call(this, ev); } catch (err) {
          console.error(`[XHR] ${type} listener threw:`, err && (err.stack || err.message));
        }
      }
    }

    _emitReadyState() {
      if (typeof this.onreadystatechange === 'function') {
        try { this.onreadystatechange.call(this, { type: 'readystatechange', target: this }); }
        catch (err) { console.error('[XHR] onreadystatechange threw:', err && err.message); }
      }
      this._fire('readystatechange');
    }

    _sendRemote() {
      const http = g.__jsglq_http;
      if (!http) {
        this._failRemote(`XHR: remote URLs are not available in this build`);
        return;
      }
      if (/^https:/i.test(this._url) && !http.tls) {
        this._failRemote(
          `XHR: https:// requires TLS, which this build does not include`);
        return;
      }

      let headerBlock = '';
      for (const [k, v] of Object.entries(this._headers || {})) {
        headerBlock += `${k}: ${v}\r\n`;
      }

      let id;
      try {
        id = http.start(this._method, this._url, headerBlock, this._body);
      } catch (e) {
        this._failRemote(`XHR: ${e.message}`);
        return;
      }

      const tick = () => {
        const r = http.poll(id);
        if (r === null) { g.setTimeout(tick, 4); return; }
        if (!r.ok) { this._failRemote(`XHR: ${r.error}`); return; }

        const hdrs = {};
        for (const line of String(r.headers).split('\r\n').slice(1)) {
          const i = line.indexOf(':');
          if (i > 0) hdrs[line.slice(0, i).trim().toLowerCase()] = line.slice(i + 1).trim();
        }
        const buf = r.body;
        this.responseURL = this._url;
        this.status = r.status;
        this.statusText = r.statusText || '';
        this._respHeaders = hdrs;
        this._len = buf.byteLength;
        this.readyState = DONE;
        // Same decoding the local path uses, per responseType.
        const t = this.responseType;
        if (t === 'arraybuffer') {
          this.response = buf;
        } else if (t === 'blob') {
          this.response = new g.Blob([buf]);
        } else if (t === 'json') {
          this.responseText = new g.TextDecoder().decode(new Uint8Array(buf));
          try { this.response = JSON.parse(this.responseText); }
          catch { this.response = null; }
        } else {
          this.responseText = new g.TextDecoder().decode(new Uint8Array(buf));
          this.response = this.responseText;
        }
        this._emitReadyState();
        this._fire('load');
        this._fire('loadend');
      };
      tick();
    }

    _failRemote(message) {
      this.status = 0;
      this.statusText = '';
      this.readyState = DONE;
      this.response = null;
      this.responseText = '';
      this._respHeaders = {};
      this._emitReadyState();
      // A network failure is an error EVENT, not a throw: that is what XHR
      // callers handle, and throwing here would escape into the microtask.
      this._error = new Error(message);
      this._fire('error');
      this._fire('loadend');
    }

    send(body) {
      // Remote requests can carry a body (POST); the local asset path ignores it.
      this._body = body;
      // Deliver asynchronously: a caller that assigns onload AFTER send() must
      // still receive it, which is how essentially all XHR code is written.
      Promise.resolve().then(() => {
        if (this.readyState === UNSENT) return;   // aborted
        try {
          /* Remote URLs go through the same HTTP client fetch() uses, so the
             two cannot diverge in what they support. Local requests keep the
             synchronous asset path below. */
          if (/^https?:\/\//i.test(this._url)) {
            this._sendRemote();
            return;
          }
          if (this._method !== 'GET' && this._method !== 'HEAD') {
            throw new Error(
              `XHR: ${this._method} is not supported for local assets ` +
              `(${this._url}); only GET/HEAD read from the game directory`);
          }

          let path = this._url;
          if (path.startsWith('./')) path = path.slice(2);
          while (path.startsWith('/')) path = path.slice(1);
          const q = path.indexOf('?');
          if (q >= 0) path = path.slice(0, q);

          const buf = g.__jsglq_io.readAsset(path);
          this.responseURL = this._url;

          if (buf == null) {
            this.status = 404;
            this.statusText = 'Not Found';
            this.readyState = DONE;
            this.response = null;
            this.responseText = '';
            this._respHeaders = {};
            this._emitReadyState();
            this._fire('error');
            this._fire('loadend');
            return;
          }

          this._len = buf.byteLength;
          this.status = 200;
          this.statusText = 'OK';
          this._respHeaders = { 'content-length': String(buf.byteLength) };

          const t = this.responseType;
          if (t === 'arraybuffer') {
            this.response = buf;
          } else if (t === 'blob') {
            this.response = new g.Blob([buf]);
          } else if (t === 'json') {
            this.responseText = new g.TextDecoder().decode(new Uint8Array(buf));
            this.response = JSON.parse(this.responseText);
          } else {
            this.responseText = new g.TextDecoder().decode(new Uint8Array(buf));
            this.response = this.responseText;
          }

          this.readyState = HEADERS_RECEIVED; this._emitReadyState();
          this.readyState = LOADING;          this._emitReadyState();
          this.readyState = DONE;             this._emitReadyState();
          this._fire('progress');
          this._fire('load');
          this._fire('loadend');
        } catch (err) {
          this.status = 0;
          this.readyState = DONE;
          console.error(`[XHR] ${this._url}: ${err.message}`);
          this._emitReadyState();
          this._fire('error');
          this._fire('loadend');
        }
      });
    }
  }

  XMLHttpRequest.UNSENT = UNSENT;
  XMLHttpRequest.OPENED = OPENED;
  XMLHttpRequest.HEADERS_RECEIVED = HEADERS_RECEIVED;
  XMLHttpRequest.LOADING = LOADING;
  XMLHttpRequest.DONE = DONE;

  g.XMLHttpRequest = XMLHttpRequest;
}
