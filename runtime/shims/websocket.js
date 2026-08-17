/*
 * WebSocket.
 *
 * The host (src/bind_websocket.c) owns the socket, the RFC 6455 handshake and
 * the frame codec, and runs each connection on its own thread. This class is
 * the browser-shaped surface over it: the events fire from a per-frame drain of
 * the host's inbound queue, which is why installWebSocket returns a pump that
 * bootstrap calls once per frame.
 *
 * ws:// only. wss:// throws by name rather than connecting in plaintext or
 * failing obscurely — a game that thinks it has TLS and does not is a worse
 * outcome than one that is told the runtime cannot do it.
 */

const CONNECTING = 0, OPEN = 1, CLOSING = 2, CLOSED = 3;

export function installWebSocket(g) {
  const host = g.__jsglq_ws;
  if (!host) return null;   // built without SDL_net; leave WebSocket undefined

  const live = new Set();

  class WebSocket {
    constructor(url, protocols) {
      this.url = String(url);
      this.protocol = '';
      this.extensions = '';
      this.binaryType = 'arraybuffer';
      this.bufferedAmount = 0;
      this.readyState = CONNECTING;
      this.onopen = null;
      this.onmessage = null;
      this.onerror = null;
      this.onclose = null;
      this._listeners = new Map();
      this._id = -1;
      this._reported = false;

      const m = /^(wss?):\/\/([^/:?#]+)(?::(\d+))?([^?#]*)?/i.exec(this.url);
      if (!m) throw new SyntaxError(`WebSocket: invalid URL '${this.url}'`);

      const scheme = m[1].toLowerCase();
      if (scheme === 'wss') {
        throw new Error(
          `WebSocket: wss:// requires TLS, which this runtime does not include ` +
          `(${this.url}). Use ws:// on a trusted network, or run the game on ` +
          `jsgamelauncher, which has full TLS.`);
      }

      const hostname = m[2];
      const port = m[3] ? parseInt(m[3], 10) : 80;
      const path = m[4] && m[4].length ? m[4] : '/';

      this._id = host.connect(hostname, port, path);
      live.add(this);
      if (protocols) {
        // Subprotocol negotiation is not implemented; the server's choice is
        // what matters and we do not read it back, so report none rather than
        // echoing what was requested and implying it was accepted.
        this.protocol = '';
      }
    }

    addEventListener(type, fn) {
      if (!this._listeners.has(type)) this._listeners.set(type, []);
      this._listeners.get(type).push(fn);
    }
    removeEventListener(type, fn) {
      const l = this._listeners.get(type);
      if (l) this._listeners.set(type, l.filter((f) => f !== fn));
    }

    _emit(type, ev) {
      ev.type = type;
      ev.target = this;
      const direct = this['on' + type];
      if (typeof direct === 'function') {
        try { direct.call(this, ev); } catch (e) { reportHandlerError(g, e); }
      }
      const l = this._listeners.get(type);
      if (l) for (const fn of l.slice()) {
        try { fn.call(this, ev); } catch (e) { reportHandlerError(g, e); }
      }
    }

    send(data) {
      if (this.readyState !== OPEN) {
        throw new Error("WebSocket.send: socket is not open");
      }
      host.send(this._id, data);
    }

    close(code, reason) {
      if (this.readyState === CLOSED || this.readyState === CLOSING) return;
      this.readyState = CLOSING;
      host.close(this._id);
      this.readyState = CLOSED;
      live.delete(this);
      this._emit('close', { code: code ?? 1000, reason: reason || '', wasClean: true });
    }

    _poll() {
      if (this._id < 0) return;
      const state = host.state(this._id);

      if (this.readyState === CONNECTING) {
        if (state === OPEN) {
          this.readyState = OPEN;
          this._emit('open', {});
        } else if (state === CLOSED) {
          // Never opened: report the host's reason, which distinguishes "no
          // route" from "server said 404" — both of which otherwise look like
          // a bare failed connection.
          this.readyState = CLOSED;
          live.delete(this);
          const err = host.error(this._id);
          if (!this._reported) {
            this._reported = true;
            this._emit('error', { message: err || 'connection failed' });
            this._emit('close', { code: 1006, reason: err || '', wasClean: false });
          }
          return;
        }
      }

      for (const m of host.recv(this._id)) {
        this._emit('message', { data: m.data });
      }

      if (this.readyState === OPEN && host.state(this._id) === CLOSED) {
        this.readyState = CLOSED;
        live.delete(this);
        const err = host.error(this._id);
        if (err) this._emit('error', { message: err });
        this._emit('close', { code: 1006, reason: err || '', wasClean: false });
      }
    }
  }

  WebSocket.CONNECTING = CONNECTING;
  WebSocket.OPEN = OPEN;
  WebSocket.CLOSING = CLOSING;
  WebSocket.CLOSED = CLOSED;
  for (const k of ['CONNECTING', 'OPEN', 'CLOSING', 'CLOSED']) {
    WebSocket.prototype[k] = WebSocket[k];
  }

  g.WebSocket = WebSocket;

  /* Called once per frame by bootstrap: events only exist because something
     drains the host queue, so a runtime that forgot this would have sockets
     that connect and never deliver a message. */
  return () => { for (const ws of Array.from(live)) ws._poll(); };
}

function reportHandlerError(g, e) {
  // A throw inside a user handler must not be swallowed: it would look exactly
  // like the event never arriving.
  if (g.__jsglq_reportError) g.__jsglq_reportError(e);
  else console.error(e);
}
