/*
 * The small globals: text codecs, Blob/URL, crypto, structuredClone, base64.
 *
 * QuickJS ships the language, not the web platform, so these are genuinely absent
 * rather than merely different. Each one here is present because something in the
 * corpus or in three.js touches it during init.
 */

export function installMisc(g) {
  /* ---------------------------------------------------------- text codecs --- */

  if (typeof g.TextEncoder === 'undefined') {
    g.TextEncoder = class TextEncoder {
      get encoding() { return 'utf-8'; }
      encode(str = '') {
        const s = String(str);
        // Encode via the engine's own UTF-8 conversion rather than hand-rolling
        // surrogate-pair logic, which is where naive encoders break on emoji.
        const out = [];
        for (let i = 0; i < s.length; i++) {
          let c = s.codePointAt(i);
          if (c > 0xffff) i++;
          if (c < 0x80) out.push(c);
          else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
          else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
          else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63),
                        0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        }
        return new Uint8Array(out);
      }
      encodeInto(str, dest) {
        const enc = this.encode(str);
        const n = Math.min(enc.length, dest.length);
        dest.set(enc.subarray(0, n));
        return { read: str.length, written: n };
      }
    };
  }

  if (typeof g.TextDecoder === 'undefined') {
    g.TextDecoder = class TextDecoder {
      constructor(label = 'utf-8') { this.encoding = String(label).toLowerCase(); }
      decode(buf) {
        if (buf == null) return '';
        const u8 = buf instanceof Uint8Array ? buf
                 : ArrayBuffer.isView(buf) ? new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength)
                 : new Uint8Array(buf);
        let out = '';
        for (let i = 0; i < u8.length; ) {
          const b = u8[i];
          if (b < 0x80) { out += String.fromCharCode(b); i += 1; }
          else if (b < 0xe0) {
            out += String.fromCharCode(((b & 31) << 6) | (u8[i + 1] & 63)); i += 2;
          } else if (b < 0xf0) {
            out += String.fromCharCode(((b & 15) << 12) | ((u8[i + 1] & 63) << 6) | (u8[i + 2] & 63));
            i += 3;
          } else {
            const cp = ((b & 7) << 18) | ((u8[i + 1] & 63) << 12)
                     | ((u8[i + 2] & 63) << 6) | (u8[i + 3] & 63);
            out += String.fromCodePoint(cp); i += 4;
          }
        }
        return out;
      }
    };
  }

  /* --------------------------------------------------------------- base64 --- */

  const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

  if (typeof g.btoa === 'undefined') {
    g.btoa = (bin) => {
      const s = String(bin);
      let out = '';
      for (let i = 0; i < s.length; i += 3) {
        const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
        out += B64[a >> 2];
        out += B64[((a & 3) << 4) | (isNaN(b) ? 0 : b >> 4)];
        out += isNaN(b) ? '=' : B64[((b & 15) << 2) | (isNaN(c) ? 0 : c >> 6)];
        out += isNaN(c) ? '=' : B64[c & 63];
      }
      return out;
    };
  }

  if (typeof g.atob === 'undefined') {
    g.atob = (b64) => {
      const s = String(b64).replace(/[^A-Za-z0-9+/]/g, '');
      let out = '';
      for (let i = 0; i < s.length; i += 4) {
        const n = (B64.indexOf(s[i]) << 18) | (B64.indexOf(s[i + 1]) << 12)
                | ((B64.indexOf(s[i + 2]) & 63) << 6) | (B64.indexOf(s[i + 3]) & 63);
        out += String.fromCharCode((n >> 16) & 255);
        if (s[i + 2] !== undefined) out += String.fromCharCode((n >> 8) & 255);
        if (s[i + 3] !== undefined) out += String.fromCharCode(n & 255);
      }
      return out;
    };
  }

  /* ----------------------------------------------------------- Blob / URL --- */

  if (typeof g.Blob === 'undefined') {
    g.Blob = class Blob {
      constructor(parts = [], opts = {}) {
        this.type = opts.type || '';
        const chunks = [];
        let total = 0;
        for (const p of parts) {
          let u8;
          if (typeof p === 'string') u8 = new g.TextEncoder().encode(p);
          else if (p instanceof Uint8Array) u8 = p;
          else if (ArrayBuffer.isView(p)) u8 = new Uint8Array(p.buffer, p.byteOffset, p.byteLength);
          else if (p instanceof ArrayBuffer) u8 = new Uint8Array(p);
          else if (p && p._u8) u8 = p._u8;
          else u8 = new g.TextEncoder().encode(String(p));
          chunks.push(u8);
          total += u8.length;
        }
        const all = new Uint8Array(total);
        let off = 0;
        for (const c of chunks) { all.set(c, off); off += c.length; }
        this._u8 = all;
        this.size = total;
      }
      async arrayBuffer() { return this._u8.buffer.slice(this._u8.byteOffset,
                                                        this._u8.byteOffset + this._u8.byteLength); }
      async text() { return new g.TextDecoder().decode(this._u8); }
      async bytes() { return this._u8.slice(); }
      slice(start = 0, end = this.size, type = '') {
        const b = new g.Blob([], { type });
        b._u8 = this._u8.slice(start, end);
        b.size = b._u8.length;
        return b;
      }
    };
  }

  // Object URLs: an in-memory registry, since there is no browser to resolve them.
  const objectUrls = new Map();
  let objectUrlSeq = 0;

  if (typeof g.URL === 'undefined') {
    // QuickJS has no URL class. This is the minimum three.js and asset loaders use.
    g.URL = class URL {
      constructor(url, base) {
        let u = String(url);
        if (base && !/^[a-z]+:/i.test(u)) {
          const b = String(base).replace(/[^/]*$/, '');
          u = b + u;
        }
        const m = /^([a-z]+:)\/\/([^/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(u);
        if (m) {
          this.protocol = m[1]; this.host = m[2]; this.hostname = m[2];
          this.pathname = m[3] || '/'; this.search = m[4] || ''; this.hash = m[5] || '';
          this.origin = `${m[1]}//${m[2]}`;
        } else {
          this.protocol = ''; this.host = ''; this.hostname = '';
          this.pathname = u; this.search = ''; this.hash = ''; this.origin = '';
        }
        this.href = u;
      }
      toString() { return this.href; }
    };
  }

  g.URL.createObjectURL = (obj) => {
    const id = `blob:jsglq/${++objectUrlSeq}`;
    objectUrls.set(id, obj);
    return id;
  };
  g.URL.revokeObjectURL = (id) => { objectUrls.delete(id); };
  g.__jsglq_resolveObjectURL = (id) => objectUrls.get(id) || null;

  if (typeof g.URLSearchParams === 'undefined') {
    g.URLSearchParams = class URLSearchParams {
      constructor(init = '') {
        this._m = new Map();
        const s = String(init).replace(/^\?/, '');
        if (s) for (const pair of s.split('&')) {
          const i = pair.indexOf('=');
          const k = decodeURIComponent(i < 0 ? pair : pair.slice(0, i));
          const v = i < 0 ? '' : decodeURIComponent(pair.slice(i + 1).replace(/\+/g, ' '));
          this._m.set(k, v);
        }
      }
      get(k) { return this._m.has(k) ? this._m.get(k) : null; }
      has(k) { return this._m.has(k); }
      set(k, v) { this._m.set(k, String(v)); }
      entries() { return this._m.entries(); }
      [Symbol.iterator]() { return this._m.entries(); }
      toString() {
        return [...this._m].map(([k, v]) =>
          `${encodeURIComponent(k)}=${encodeURIComponent(v)}`).join('&');
      }
    };
  }

  /* --------------------------------------------------------------- crypto --- */

  if (typeof g.crypto === 'undefined') {
    g.crypto = {
      getRandomValues(arr) {
        // Math.random is NOT a CSPRNG. It is used here only because v1 has no
        // entropy source wired up yet; the moment anything security-relevant
        // depends on this, it must come from the host. Tracked for phase 2.
        const u8 = new Uint8Array(arr.buffer, arr.byteOffset, arr.byteLength);
        for (let i = 0; i < u8.length; i++) u8[i] = (Math.random() * 256) | 0;
        return arr;
      },
      randomUUID() {
        const h = [];
        for (let i = 0; i < 16; i++) h.push(((Math.random() * 256) | 0));
        h[6] = (h[6] & 0x0f) | 0x40;
        h[8] = (h[8] & 0x3f) | 0x80;
        const s = h.map((b) => b.toString(16).padStart(2, '0')).join('');
        return `${s.slice(0, 8)}-${s.slice(8, 12)}-${s.slice(12, 16)}-${s.slice(16, 20)}-${s.slice(20)}`;
      },
    };
  }

  /* ------------------------------------------------------ structuredClone --- */

  if (typeof g.structuredClone === 'undefined') {
    g.structuredClone = (v) => {
      // Handles the cases games actually clone (plain data + typed arrays).
      // Deliberately NOT a JSON round-trip: that silently drops typed arrays,
      // Map/Set, and Infinity, which is worse than failing.
      const seen = new Map();
      const walk = (x) => {
        if (x === null || typeof x !== 'object') return x;
        if (seen.has(x)) return seen.get(x);
        if (x instanceof ArrayBuffer) return x.slice(0);
        if (ArrayBuffer.isView(x)) {
          return new x.constructor(x.buffer.slice(0), x.byteOffset, x.length);
        }
        if (x instanceof Date) return new Date(x.getTime());
        if (x instanceof Map) {
          const m = new Map(); seen.set(x, m);
          for (const [k, v2] of x) m.set(walk(k), walk(v2));
          return m;
        }
        if (x instanceof Set) {
          const s = new Set(); seen.set(x, s);
          for (const v2 of x) s.add(walk(v2));
          return s;
        }
        if (Array.isArray(x)) {
          const a = []; seen.set(x, a);
          for (const v2 of x) a.push(walk(v2));
          return a;
        }
        const o = {}; seen.set(x, o);
        for (const k of Object.keys(x)) o[k] = walk(x[k]);
        return o;
      };
      return walk(v);
    };
  }

  /* ---------------------------------------------------------- queueMicrotask */

  if (typeof g.queueMicrotask === 'undefined') {
    g.queueMicrotask = (fn) => { Promise.resolve().then(fn); };
  }
}
