/*
 * Event dispatch: EventTarget semantics over the host's SDL pump.
 *
 * The host builds plain objects with the right properties and calls
 * `__jsglq_dispatchEvent`. This layer gives them spec behaviour — listener
 * registration on window/document/canvas, `once`, `preventDefault`,
 * `stopPropagation` — and routes each event to the targets a browser would.
 *
 * Routing matters more than it looks: the corpus's audio-unlock pattern is
 * `document.addEventListener('click', fn, {once: true})`, so a click that only
 * reached `window` would leave the audio silent with no error anywhere.
 */

export function installEvents(g, { canvas }) {
  const targets = new Map();   // target object -> Map<type, [{fn, opts}]>

  function listenersFor(target, type, create) {
    let byType = targets.get(target);
    if (!byType) {
      if (!create) return null;
      byType = new Map();
      targets.set(target, byType);
    }
    let list = byType.get(type);
    if (!list) {
      if (!create) return null;
      list = [];
      byType.set(type, list);
    }
    return list;
  }

  function addEventListener(target, type, fn, opts) {
    if (typeof fn !== 'function') return;
    const list = listenersFor(target, type, true);
    list.push({ fn, once: !!(opts && opts.once) });
  }

  function removeEventListener(target, type, fn) {
    const list = listenersFor(target, type, false);
    if (!list) return;
    const i = list.findIndex((l) => l.fn === fn);
    if (i >= 0) list.splice(i, 1);
  }

  function fireOn(target, ev) {
    const list = listenersFor(target, ev.type, false);
    if (!list || !list.length) return;
    // Copy first: a handler may add or remove listeners while dispatching.
    for (const l of list.slice()) {
      if (l.once) {
        const i = list.indexOf(l);
        if (i >= 0) list.splice(i, 1);
      }
      try {
        l.fn.call(target, ev);
      } catch (err) {
        // One bad handler must not take down the frame or swallow the rest.
        console.error(`[event] ${ev.type} handler threw:`, err && (err.stack || err.message));
      }
      if (ev.__stopImmediate) break;
    }
  }

  function decorate(ev) {
    ev.preventDefault = function () { this.defaultPrevented = true; };
    ev.stopPropagation = function () { this.__stopPropagation = true; };
    ev.stopImmediatePropagation = function () {
      this.__stopPropagation = true;
      this.__stopImmediate = true;
    };
    ev.target = ev.target || canvas;
    ev.currentTarget = null;
    ev.composedPath = () => [canvas, g.document, g];
    return ev;
  }

  /*
   * Dispatch order mirrors the browser's bubble phase: the canvas is the event's
   * target, then document, then window. A listener anywhere on that path sees it.
   */
  g.__jsglq_dispatchEvent = function dispatchFromHost(ev) {
    decorate(ev);
    const path = [canvas, g.document, g];
    for (const t of path) {
      ev.currentTarget = t;
      fireOn(t, ev);
      if (ev.__stopPropagation) break;
    }
    // `onclick`-style properties, which some games use instead of listeners.
    const handlerProp = 'on' + ev.type;
    for (const t of path) {
      if (typeof t[handlerProp] === 'function') {
        try { t[handlerProp].call(t, ev); } catch (err) {
          console.error(`[event] ${handlerProp} threw:`, err && (err.stack || err.message));
        }
      }
    }
    return !ev.defaultPrevented;
  };

  // Install EventTarget methods on the three real targets.
  for (const target of [g, g.document, canvas]) {
    if (!target) continue;
    target.addEventListener = (type, fn, opts) => addEventListener(target, type, fn, opts);
    target.removeEventListener = (type, fn) => removeEventListener(target, type, fn);
    target.dispatchEvent = (ev) => g.__jsglq_dispatchEvent(ev);
  }
  if (g.document && g.document.body) {
    const b = g.document.body;
    b.addEventListener = (type, fn, opts) => addEventListener(g.document, type, fn, opts);
    b.removeEventListener = (type, fn) => removeEventListener(g.document, type, fn);
  }

  /* A minimal Event/CustomEvent so games can construct and dispatch their own. */
  class Event {
    constructor(type, init = {}) {
      this.type = String(type);
      this.bubbles = !!init.bubbles;
      this.cancelable = !!init.cancelable;
      this.defaultPrevented = false;
      this.timeStamp = g.performance ? g.performance.now() : 0;
    }
    preventDefault() { this.defaultPrevented = true; }
    stopPropagation() { this.__stopPropagation = true; }
    stopImmediatePropagation() { this.__stopPropagation = true; this.__stopImmediate = true; }
  }
  class CustomEvent extends Event {
    constructor(type, init = {}) { super(type, init); this.detail = init.detail ?? null; }
  }
  g.Event = Event;
  g.CustomEvent = CustomEvent;
  g.KeyboardEvent = class KeyboardEvent extends Event {};
  g.MouseEvent = class MouseEvent extends Event {};
  g.PointerEvent = class PointerEvent extends Event {};
  g.WheelEvent = class WheelEvent extends Event {};

  class EventTarget {
    constructor() { this.__t = {}; }
    addEventListener(type, fn, opts) { addEventListener(this, type, fn, opts); }
    removeEventListener(type, fn) { removeEventListener(this, type, fn); }
    dispatchEvent(ev) { decorate(ev); ev.currentTarget = this; fireOn(this, ev); return true; }
  }
  g.EventTarget = EventTarget;
}
