/*
 * localStorage / sessionStorage.
 *
 * Backed by a JSON file in the game directory, so save data survives a restart —
 * which is the entire reason games use it. Writes are debounced to a frame rather
 * than hitting disk on every setItem, because a game saving per-frame state would
 * otherwise do a synchronous write 60 times a second.
 *
 * sessionStorage is a SEPARATE in-memory store, not an alias. The wasm build
 * aliased them and noted the consequence: session data wrongly persisted across
 * runs. A game using sessionStorage for per-run state would then see stale values
 * from a previous session, which is a genuinely confusing bug to chase.
 */

const FILE = '.jsglq-storage.json';

function makeStorage(g, { persistent, load, save }) {
  const data = new Map();

  if (persistent) {
    try {
      const raw = load();
      if (raw) {
        const obj = JSON.parse(raw);
        for (const k of Object.keys(obj)) data.set(k, String(obj[k]));
      }
    } catch (err) {
      // A corrupt store must not take the game down; start empty and say so.
      console.error(`[storage] could not read saved data (${err.message}); starting empty`);
    }
  }

  let dirty = false;
  const flush = () => {
    if (!dirty || !persistent) return;
    dirty = false;
    try {
      save(JSON.stringify(Object.fromEntries(data)));
    } catch (err) {
      console.error(`[storage] save failed: ${err.message}`);
    }
  };

  const markDirty = () => {
    if (dirty) return;
    dirty = true;
    // Coalesce to the next tick: a game writing several keys in a row costs one
    // write, and a game writing every frame costs one write per frame instead of
    // one per key.
    g.setTimeout(flush, 0);
  };

  const storage = {
    getItem(k) {
      const key = String(k);
      return data.has(key) ? data.get(key) : null;
    },
    setItem(k, v) {
      data.set(String(k), String(v));
      markDirty();
    },
    removeItem(k) {
      if (data.delete(String(k))) markDirty();
    },
    clear() {
      if (data.size) { data.clear(); markDirty(); }
    },
    key(i) {
      const keys = [...data.keys()];
      return i >= 0 && i < keys.length ? keys[i] : null;
    },
    get length() { return data.size; },
    _flush: flush,
  };

  /*
   * A Proxy so `localStorage.score = 5` and `delete localStorage.score` work.
   * Games use property access at least as often as the method form, and without
   * this those writes silently land on the object and never persist.
   */
  return new Proxy(storage, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (typeof prop === 'string' && data.has(prop)) return data.get(prop);
      return undefined;
    },
    set(target, prop, value) {
      if (prop in target) { target[prop] = value; return true; }
      target.setItem(String(prop), value);
      return true;
    },
    has(target, prop) { return prop in target || data.has(String(prop)); },
    deleteProperty(target, prop) { target.removeItem(String(prop)); return true; },
    ownKeys() { return [...data.keys()]; },
    getOwnPropertyDescriptor(target, prop) {
      if (data.has(String(prop))) {
        return { value: data.get(String(prop)), enumerable: true, configurable: true };
      }
      return Object.getOwnPropertyDescriptor(target, prop);
    },
  });
}

export function installStorage(g) {
  const io = g.__jsglq_io;

  g.localStorage = makeStorage(g, {
    persistent: !!(io && io.writeAsset),
    load: () => (io && io.readAssetText ? io.readAssetText(FILE) : null),
    save: (json) => { if (io && io.writeAsset) io.writeAsset(FILE, json); },
  });

  // Separate store, deliberately not an alias: see the file header.
  g.sessionStorage = makeStorage(g, { persistent: false, load: () => null, save: () => {} });

  g.Storage = function Storage() {
    throw new TypeError('Illegal constructor');
  };
}
