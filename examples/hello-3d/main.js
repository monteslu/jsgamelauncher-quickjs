/*
 * hello-3d — WebGL2 with no library at all.
 *
 * A shaded, spinning cube in ~150 lines: shader compilation, buffers, a VAO,
 * uniforms, depth testing, and per-frame matrix math. Deliberately dependency-free
 * so it proves the GL stack rather than a framework's ability to hide it.
 */

const canvas = document.getElementById('game-canvas');
const gl = canvas.getContext('webgl2');
if (!gl) throw new Error('WebGL2 is unavailable');

const VS = `#version 300 es
in vec3 a_pos;
in vec3 a_normal;
in vec3 a_color;
uniform mat4 u_mvp;
uniform mat4 u_model;
out vec3 v_normal;
out vec3 v_color;
void main() {
  gl_Position = u_mvp * vec4(a_pos, 1.0);
  v_normal = mat3(u_model) * a_normal;
  v_color = a_color;
}`;

const FS = `#version 300 es
precision mediump float;
in vec3 v_normal;
in vec3 v_color;
out vec4 outColor;
void main() {
  vec3 light = normalize(vec3(0.4, 0.8, 0.5));
  float diffuse = max(dot(normalize(v_normal), light), 0.0);
  outColor = vec4(v_color * (0.35 + 0.65 * diffuse), 1.0);
}`;

function compile(type, src) {
  const s = gl.createShader(type);
  gl.shaderSource(s, src);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
    // Fail loudly with the driver's own message: a silently-null shader shows up
    // later as a blank screen with nothing to go on.
    throw new Error('shader compile failed: ' + gl.getShaderInfoLog(s));
  }
  return s;
}

const prog = gl.createProgram();
gl.attachShader(prog, compile(gl.VERTEX_SHADER, VS));
gl.attachShader(prog, compile(gl.FRAGMENT_SHADER, FS));
gl.linkProgram(prog);
if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
  throw new Error('program link failed: ' + gl.getProgramInfoLog(prog));
}
gl.useProgram(prog);

// Cube: 6 faces, each with its own normal and colour.
const FACES = [
  { n: [0, 0, 1],  c: [0.95, 0.35, 0.35], v: [[-1,-1, 1], [1,-1, 1], [1, 1, 1], [-1, 1, 1]] },
  { n: [0, 0,-1],  c: [0.35, 0.75, 0.95], v: [[1,-1,-1], [-1,-1,-1], [-1, 1,-1], [1, 1,-1]] },
  { n: [0, 1, 0],  c: [0.95, 0.80, 0.35], v: [[-1, 1, 1], [1, 1, 1], [1, 1,-1], [-1, 1,-1]] },
  { n: [0,-1, 0],  c: [0.55, 0.85, 0.45], v: [[-1,-1,-1], [1,-1,-1], [1,-1, 1], [-1,-1, 1]] },
  { n: [1, 0, 0],  c: [0.80, 0.50, 0.95], v: [[1,-1, 1], [1,-1,-1], [1, 1,-1], [1, 1, 1]] },
  { n: [-1,0, 0],  c: [0.95, 0.60, 0.35], v: [[-1,-1,-1], [-1,-1, 1], [-1, 1, 1], [-1, 1,-1]] },
];

const verts = [];
const indices = [];
for (const f of FACES) {
  const base = verts.length / 9;
  for (const p of f.v) verts.push(...p, ...f.n, ...f.c);
  indices.push(base, base + 1, base + 2, base, base + 2, base + 3);
}

const vao = gl.createVertexArray();
gl.bindVertexArray(vao);

const vbo = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(verts), gl.STATIC_DRAW);

const stride = 9 * 4;
for (const [name, size, offset] of [['a_pos', 3, 0], ['a_normal', 3, 12], ['a_color', 3, 24]]) {
  const loc = gl.getAttribLocation(prog, name);
  gl.enableVertexAttribArray(loc);
  gl.vertexAttribPointer(loc, size, gl.FLOAT, false, stride, offset);
}

const ebo = gl.createBuffer();
gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ebo);
gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array(indices), gl.STATIC_DRAW);

const uMVP = gl.getUniformLocation(prog, 'u_mvp');
const uModel = gl.getUniformLocation(prog, 'u_model');

gl.enable(gl.DEPTH_TEST);
gl.clearColor(0.05, 0.06, 0.09, 1);

function multiply(a, b) {
  const out = new Float32Array(16);
  for (let r = 0; r < 4; r++) {
    for (let c = 0; c < 4; c++) {
      out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1]
                     + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    }
  }
  return out;
}

function perspective(fovy, aspect, near, far) {
  const f = 1 / Math.tan(fovy / 2);
  const out = new Float32Array(16);
  out[0] = f / aspect; out[5] = f;
  out[10] = (far + near) / (near - far); out[11] = -1;
  out[14] = (2 * far * near) / (near - far);
  return out;
}

function rotationYX(ry, rx) {
  const cy = Math.cos(ry), sy = Math.sin(ry);
  const cx = Math.cos(rx), sx = Math.sin(rx);
  const out = new Float32Array(16);
  out[0] = cy;       out[1] = sy * sx;  out[2] = sy * cx;
  out[4] = 0;        out[5] = cx;       out[6] = -sx;
  out[8] = -sy;      out[9] = cy * sx;  out[10] = cy * cx;
  out[15] = 1;
  return out;
}

let start = 0;
function frame(now) {
  if (!start) start = now;
  const t = (now - start) / 1000;

  const w = gl.drawingBufferWidth, h = gl.drawingBufferHeight;
  gl.viewport(0, 0, w, h);
  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

  const model = rotationYX(t * 0.7, t * 0.45);
  const view = new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-6,1]);
  const proj = perspective(Math.PI / 4, w / h, 0.1, 100);

  gl.uniformMatrix4fv(uModel, false, model);
  gl.uniformMatrix4fv(uMVP, false, multiply(proj, multiply(view, model)));

  gl.bindVertexArray(vao);
  gl.drawElements(gl.TRIANGLES, indices.length, gl.UNSIGNED_SHORT, 0);

  requestAnimationFrame(frame);
}

requestAnimationFrame(frame);
