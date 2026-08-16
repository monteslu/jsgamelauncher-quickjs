/**
 * Shared scene scaffold.
 *
 * A scene is a plain .jsgame-shaped module that runs unmodified in all three RUTs.
 * This file hides the only real difference between them: how you get a canvas.
 *
 *   browser  - <canvas id="game-canvas"> in the page
 *   node     - rungame's document.getElementById returns the game canvas for any id
 *   qjs      - same contract as rungame (deliberately: the .jsgame contract is the spec)
 */

export function getCanvas(width, height) {
  let canvas = null;
  if (typeof document !== 'undefined' && document.getElementById) {
    canvas = document.getElementById('game-canvas');
  }
  if (!canvas && typeof document !== 'undefined' && document.createElement) {
    canvas = document.createElement('canvas');
  }
  if (!canvas) throw new Error('no canvas available in this runtime');
  if (width) canvas.width = width;
  if (height) canvas.height = height;
  return canvas;
}

/**
 * An OFFSCREEN canvas, never the display canvas.
 *
 * This exists because getCanvas() must return the display canvas, and in a launcher
 * runtime `document.getElementById` returns that same canvas for ANY id — so a scene
 * asking for a scratch surface would silently resize and overpaint the one it draws
 * into. That failure is invisible in a frame-rate number and shows up only as a
 * wrong-sized capture; the orientation canary is what caught it here.
 */
export function makeOffscreen(width, height) {
  let c = null;
  if (typeof OffscreenCanvas !== 'undefined') {
    c = new OffscreenCanvas(width, height);
  } else if (typeof document !== 'undefined' && document.createElement) {
    c = document.createElement('canvas');
  }
  if (!c) throw new Error('no offscreen canvas available in this runtime');
  // A launcher's createElement may hand back the display canvas; refuse that rather
  // than corrupt the frame.
  if (typeof document !== 'undefined' && document.getElementById) {
    const display = document.getElementById('game-canvas');
    if (display && c === display) {
      throw new Error('makeOffscreen returned the display canvas: runtime lacks a real offscreen path');
    }
  }
  c.width = width;
  c.height = height;
  return c;
}

export function get2d(width, height) {
  const canvas = getCanvas(width, height);
  const ctx = canvas.getContext('2d');
  if (!ctx) throw new Error('2d context unavailable');
  return { canvas, ctx };
}

export function getGl(width, height, attrs) {
  const canvas = getCanvas(width, height);
  const gl = canvas.getContext('webgl2', attrs || { antialias: false, alpha: false, depth: true });
  if (!gl) throw new Error('webgl2 context unavailable');
  return { canvas, gl };
}

/** Deterministic colour from an index. No Math.random anywhere in a scene. */
export function palette(i) {
  const h = (i * 47) % 360;
  const c = 200, x = c * (1 - Math.abs(((h / 60) % 2) - 1));
  let r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else { r = c; b = x; }
  return [(r + 30) | 0, (g + 30) | 0, (b + 30) | 0];
}

export function rgbCss(c) { return `rgb(${c[0]},${c[1]},${c[2]})`; }

/** Compile a program; throws with the log on failure (never silently returns null). */
export function makeProgram(gl, vsSrc, fsSrc) {
  const compile = (type, src) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      throw new Error(`shader compile failed: ${gl.getShaderInfoLog(s)}\n${src}`);
    }
    return s;
  };
  const p = gl.createProgram();
  gl.attachShader(p, compile(gl.VERTEX_SHADER, vsSrc));
  gl.attachShader(p, compile(gl.FRAGMENT_SHADER, fsSrc));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
    throw new Error(`program link failed: ${gl.getProgramInfoLog(p)}`);
  }
  return p;
}
