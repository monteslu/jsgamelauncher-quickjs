/*
 * Runtime bootstrap: assembles the browser-shaped globals on top of the host
 * bindings, then hands control to the game's entry module.
 *
 * This layer is JS on purpose. The fast paths (GL calls, asset reads, timers) are C;
 * the API *shape* — argument overloading, property getters, spec-shaped objects —
 * is far cheaper to get right and to keep right in JS. That split is exactly what
 * wasmcart-jsgame proved out, and it is why ~50 browser globals fit in a small
 * amount of code there.
 */

import { installCanvas } from './shims/canvas.js';
import { installDocument } from './shims/document.js';
import { installFetch } from './shims/fetch.js';
import { installMisc } from './shims/misc.js';
import { installEvents } from './shims/events.js';
import { installImage } from './shims/image.js';
import { installWorker } from './shims/worker.js';
import { installAudio } from './shims/audio.js';
import { installStorage } from './shims/storage.js';
import { installXHR } from './shims/xhr.js';
import { installGamepads } from './shims/gamepad.js';
import { installPath2D } from './shims/path2d.js';
import { installWebSocket } from './shims/websocket.js';

export function bootstrap({ width, height }) {
  installMisc(globalThis);
  installFetch(globalThis);
  installStorage(globalThis);
  installXHR(globalThis);
  const canvas = installCanvas(globalThis, { width, height });
  installImage(globalThis);   // before document: createElement('img') needs it
  installDocument(globalThis, { canvas, width, height });
  // Events last: it installs over document/window/canvas, all of which must exist.
  installEvents(globalThis, { canvas });
  /* After installDocument (which creates navigator) and installEvents (which
     provides Event + dispatchEvent for gamepadconnected). */
  installGamepads(globalThis);
  installPath2D(globalThis);
  // Returns a pump (or null when built without SDL_net); WebSocket events
  // only exist because this drains the host queue each frame.
  const pumpWebSockets = installWebSocket(globalThis);

  /* One hook the host calls per frame, before rAF callbacks. Registered only
     when something needs it, so the common case costs a property lookup. */
  const frameHooks = [];
  if (pumpWebSockets) frameHooks.push(pumpWebSockets);
  if (globalThis.__jsglq_pumpGamepadEvents) {
    frameHooks.push(globalThis.__jsglq_pumpGamepadEvents);
  }
  if (frameHooks.length) {
    globalThis.__jsglq_frameHook = () => { for (const h of frameHooks) h(); };
  }
  if (globalThis.__jsglq_worker) installWorker(globalThis);
  if (globalThis.__jsglq_audio) installAudio(globalThis);
  return canvas;
}
