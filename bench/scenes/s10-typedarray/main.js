/**
 * s10-typedarray — the marshaling boundary.
 *
 * Ejecta's hardest-won lesson (JSC + canvas/WebGL, no DOM) was that the
 * value-conversion boundary is the whole ballgame: moving from a bundled JSC to the
 * system JSC cost them ~7 ms/MB read and ~20 ms/MB write on typed arrays, and that
 * alone decided what was shippable.
 *
 * The same risk applies here. QuickJS's C-call overhead is famously low, but the
 * cost of getting a Float32Array's bytes across the binding matters more than call
 * overhead once a scene uploads real vertex data. wasmcart-native measured N-API
 * function-pointer dispatch BEATING V8's own FunctionCallback path on the GL hot
 * loop, so this is not a foregone conclusion in either direction.
 *
 * Measures three separate costs:
 *   1. pure JS typed-array arithmetic (no binding involved)
 *   2. JS -> native writes (bufferSubData: the upload path)
 *   3. native -> JS reads (getBufferSubData: the readback path)
 */
import { autorun, drawCanaryGl } from '../../harness.js';
import { getGl, makeProgram } from '../lib/scaffold.js';

const FLOATS = 1024 * 1024;      // 4 MB of Float32
const READBACK_FLOATS = 64 * 1024; // 256 KB read back per frame

const VS = `#version 300 es
in vec2 a_pos;
void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }`;

const FS = `#version 300 es
precision mediump float;
out vec4 o;
void main() { o = vec4(0.25, 0.55, 0.75, 1.0); }`;

export const scene = {
  name: 's10-typedarray',

  setup({ width, height, rng }) {
    const { canvas, gl } = getGl(width, height, {
      antialias: false, alpha: false, preserveDrawingBuffer: true,
    });

    const src = new Float32Array(FLOATS);
    for (let i = 0; i < FLOATS; i++) src[i] = rng() * 2 - 1;
    const scratch = new Float32Array(FLOATS);
    const readback = new Float32Array(READBACK_FLOATS);

    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, src.byteLength, gl.DYNAMIC_DRAW);

    const prog = makeProgram(gl, VS, FS);
    gl.useProgram(prog);
    const quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.bufferData(gl.ARRAY_BUFFER,
      new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(prog, 'a_pos');
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);

    return { canvas, gl, src, scratch, readback, buf, prog, vao, width, height, phases: {} };
  },

  step(t, frame) {
    const { gl, src, scratch, readback, buf, vao, width, height } = t;
    const time = frame / 60;

    // (1) Pure JS typed-array work: no binding crossed.
    const k = Math.sin(time) * 0.5 + 1.0;
    for (let i = 0; i < FLOATS; i += 4) {
      scratch[i] = src[i] * k;
      scratch[i + 1] = src[i + 1] * k;
      scratch[i + 2] = src[i + 2] * k;
      scratch[i + 3] = src[i + 3] * k;
    }

    // (2) JS -> native: the upload path every 3D game runs per frame.
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, scratch);

    // (3) native -> JS: readback. Rarer in games but it is the path that bit Ejecta
    // hardest, and it is how a differ or a screenshot tool gets its pixels.
    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, readback, 0, READBACK_FLOATS);

    gl.viewport(0, 0, width, height);
    gl.clearColor(0.05, 0.06, 0.08, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

    drawCanaryGl(gl, width, height);
  },
};

autorun(scene);
