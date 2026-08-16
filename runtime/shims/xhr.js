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

    send() {
      // Deliver asynchronously: a caller that assigns onload AFTER send() must
      // still receive it, which is how essentially all XHR code is written.
      Promise.resolve().then(() => {
        if (this.readyState === UNSENT) return;   // aborted
        try {
          if (this._method !== 'GET' && this._method !== 'HEAD') {
            throw new Error(`XHR: only GET/HEAD are supported (got ${this._method})`);
          }
          if (/^https?:\/\//i.test(this._url)) {
            throw new Error(`XHR: remote URLs are not supported (${this._url})`);
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
