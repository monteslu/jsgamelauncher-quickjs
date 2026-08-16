/**
 * s01-entities-500 — the interpreter-gap probe.
 *
 * 500 entities, per-frame vector math, and a full Array.sort with a JS comparator.
 * Mirrors the quickjs-ng spike workload that measured 0.240 ms/frame in WASM, so
 * this is the one scene with a known-good prior datapoint to sanity-check against.
 *
 * Drawing is deliberately trivial (one fillRect per entity) so busy time is
 * dominated by JS execution, not the drawing binding. s03 measures the binding.
 */
import { autorun } from '../../harness.js';
import { makeEntityScene } from '../lib/entities.js';

export const scene = makeEntityScene(500, 's01-entities-500');
autorun(scene);
