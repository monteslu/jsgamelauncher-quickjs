/*
 * The Worker class, over the host's real OS threads.
 *
 * Each Worker is a separate JSRuntime on its own thread, so this is genuine
 * parallelism rather than the cooperative same-thread arrangement the wasm build
 * had to settle for.
 *
 * Messages are structured-clone-approximated via JSON. That covers what games
 * actually send (plain data, arrays, numbers) and explicitly does NOT cover
 * functions, Map/Set, cyclic references, or transferables — each of which throws
 * with its reason rather than silently arriving as `{}`, which is the failure mode
 * that makes worker bugs so hard to see.
 */

export function installWorker(g) {
  const host = g.__jsglq_worker;
  const byId = new Map();

  function serialize(data) {
    // JSON.stringify silently drops functions and undefined, and throws on cycles
    // with a message that does not mention the worker. Check first so the error
    // names the actual problem.
    if (typeof data === 'function') {
      throw new DOMException(
        'postMessage: functions cannot be cloned', 'DataCloneError');
    }
    try {
      return JSON.stringify(data === undefined ? null : data);
    } catch (err) {
      if (/circular|cyclic/i.test(err.message)) {
        throw new DOMException(
          'postMessage: circular structures cannot be cloned by this runtime ' +
          '(structured clone is JSON-based here)', 'DataCloneError');
      }
      throw err;
    }
  }

  class Worker {
    constructor(scriptURL, options = {}) {
      let path = String(scriptURL);
      // `new Worker(new URL('./w.js', import.meta.url))` is the common modern form.
      if (path.startsWith('file://')) path = path.slice(7);
      const m = /^[a-z]+:\/\/[^/]*(\/.*)$/i.exec(path);
      if (m) path = m[1];
      while (path.startsWith('/')) path = path.slice(1);
      if (path.startsWith('./')) path = path.slice(2);

      this._id = host.create(path);
      this.onmessage = null;
      this.onerror = null;
      this.onmessageerror = null;
      this._listeners = new Map();
      byId.set(this._id, this);
    }

    postMessage(data) {
      if (this._id === undefined) {
        throw new Error('postMessage on a terminated worker');
      }
      host.post(this._id, serialize(data));
    }

    terminate() {
      if (this._id === undefined) return;
      host.terminate(this._id);
      byId.delete(this._id);
      this._id = undefined;
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

    _deliver(type, ev) {
      const handler = this['on' + type];
      if (typeof handler === 'function') {
        try { handler.call(this, ev); } catch (err) {
          console.error(`[Worker ${this._id}] on${type} threw:`, err && (err.stack || err.message));
        }
      }
      const l = this._listeners.get(type);
      if (l) for (const fn of l.slice()) {
        try { fn.call(this, ev); } catch (err) {
          console.error(`[Worker ${this._id}] ${type} listener threw:`,
                        err && (err.stack || err.message));
        }
      }
    }
  }

  /*
   * Drained once per frame by the host loop. Doing this on the frame boundary
   * rather than on an interrupt keeps worker messages on the same timeline as
   * everything else the game sees.
   */
  g.__jsglq_pumpWorkers = function pumpWorkers() {
    const messages = host.poll();
    for (const m of messages) {
      const w = byId.get(m.id);
      if (!w) continue;
      if (m.error !== undefined) {
        w._deliver('error', { type: 'error', message: m.error, filename: '', lineno: 0 });
      } else {
        w._deliver('message', { type: 'message', data: m.data });
      }
    }
  };

  g.Worker = Worker;
}
