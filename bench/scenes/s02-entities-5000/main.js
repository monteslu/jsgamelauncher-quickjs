/**
 * s02-entities-5000 — the same workload as s01 at 10x scale.
 *
 * Purpose is the scaling curve, not the absolute number: if qjs/node ratio holds
 * steady from 500 to 5000 the gap is a constant interpreter tax; if it widens, the
 * cost is allocation/GC pressure and the mitigation is different.
 */
import { autorun } from '../../harness.js';
import { makeEntityScene } from '../lib/entities.js';

export const scene = makeEntityScene(5000, 's02-entities-5000');
autorun(scene);
