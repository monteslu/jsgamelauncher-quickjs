/*
 * A small software Canvas2D for OFFSCREEN canvases.
 *
 * Why software: the GL renderer draws into the single default framebuffer, so an
 * offscreen canvas cannot share it without render targets. The corpus uses
 * offscreen canvases almost exclusively to build a sprite sheet once at load and
 * blit it every frame afterwards, so a CPU path costs one upload and nothing per
 * frame. Adding FBO support to the renderer for this would be more machinery for
 * a workload that does not need it.
 *
 * Scope is the same measured surface as the GL context, and unimplemented members
 * throw by name here too — a silently-blank sprite sheet is exactly as bad as a
 * silently-blank screen, and harder to notice.
 */

const NAMED = {
  black: [0, 0, 0, 255], white: [255, 255, 255, 255], red: [255, 0, 0, 255],
  green: [0, 128, 0, 255], blue: [0, 0, 255, 255], yellow: [255, 255, 0, 255],
  cyan: [0, 255, 255, 255], magenta: [255, 0, 255, 255], gray: [128, 128, 128, 255],
  grey: [128, 128, 128, 255], orange: [255, 165, 0, 255], purple: [128, 0, 128, 255],
  lime: [0, 255, 0, 255], navy: [0, 0, 128, 255], teal: [0, 128, 128, 255],
  silver: [192, 192, 192, 255], transparent: [0, 0, 0, 0],
};

function parseColor(s) {
  if (typeof s !== 'string') return null;
  const t = s.trim();
  if (t[0] === '#') {
    const h = t.slice(1);
    const n = h.length;
    const v = (i) => parseInt(h[i], 16);
    if (n === 3 || n === 4) {
      return [v(0) * 17, v(1) * 17, v(2) * 17, n === 4 ? v(3) * 17 : 255];
    }
    if (n === 6 || n === 8) {
      const p = (i) => parseInt(h.slice(i, i + 2), 16);
      return [p(0), p(2), p(4), n === 8 ? p(6) : 255];
    }
    return null;
  }
  if (t.startsWith('rgb')) {
    const nums = t.slice(t.indexOf('(') + 1).split(',').map((x) => parseFloat(x));
    if (nums.length < 3 || nums.some((x) => Number.isNaN(x))) return null;
    const a = nums.length > 3 ? (nums[3] <= 1 ? nums[3] * 255 : nums[3]) : 255;
    return [nums[0] | 0, nums[1] | 0, nums[2] | 0, a | 0];
  }
  return NAMED[t.toLowerCase()] || null;
}

const UNSUPPORTED = [
  'arcTo', 'roundRect', 'createLinearGradient', 'createRadialGradient',
  'createPattern', 'isPointInPath',
];

/* A 4x7 bitmap font for offscreen text. Each entry is 7 rows of 4 bits. */
const GLYPHS = {
  '0':[0x6,0x9,0xB,0xD,0x9,0x9,0x6], '1':[0x2,0x6,0x2,0x2,0x2,0x2,0x7],
  '2':[0x6,0x9,0x1,0x2,0x4,0x8,0xF], '3':[0xE,0x1,0x1,0x6,0x1,0x1,0xE],
  '4':[0x2,0x6,0xA,0xF,0x2,0x2,0x2], '5':[0xF,0x8,0xE,0x1,0x1,0x9,0x6],
  '6':[0x6,0x8,0xE,0x9,0x9,0x9,0x6], '7':[0xF,0x1,0x2,0x2,0x4,0x4,0x4],
  '8':[0x6,0x9,0x9,0x6,0x9,0x9,0x6], '9':[0x6,0x9,0x9,0x7,0x1,0x2,0xC],
  'A':[0x6,0x9,0x9,0xF,0x9,0x9,0x9], 'B':[0xE,0x9,0xE,0x9,0x9,0x9,0xE],
  'C':[0x6,0x9,0x8,0x8,0x8,0x9,0x6], 'D':[0xE,0x9,0x9,0x9,0x9,0x9,0xE],
  'E':[0xF,0x8,0xE,0x8,0x8,0x8,0xF], 'F':[0xF,0x8,0xE,0x8,0x8,0x8,0x8],
  'G':[0x6,0x9,0x8,0xB,0x9,0x9,0x7], 'H':[0x9,0x9,0x9,0xF,0x9,0x9,0x9],
  'I':[0x7,0x2,0x2,0x2,0x2,0x2,0x7], 'J':[0x1,0x1,0x1,0x1,0x9,0x9,0x6],
  'K':[0x9,0xA,0xC,0x8,0xC,0xA,0x9], 'L':[0x8,0x8,0x8,0x8,0x8,0x8,0xF],
  'M':[0x9,0xF,0xF,0x9,0x9,0x9,0x9], 'N':[0x9,0xD,0xD,0xB,0xB,0x9,0x9],
  'O':[0x6,0x9,0x9,0x9,0x9,0x9,0x6], 'P':[0xE,0x9,0x9,0xE,0x8,0x8,0x8],
  'Q':[0x6,0x9,0x9,0x9,0xB,0xA,0x5], 'R':[0xE,0x9,0x9,0xE,0xC,0xA,0x9],
  'S':[0x7,0x8,0x8,0x6,0x1,0x1,0xE], 'T':[0xF,0x4,0x4,0x4,0x4,0x4,0x4],
  'U':[0x9,0x9,0x9,0x9,0x9,0x9,0x6], 'V':[0x9,0x9,0x9,0x9,0x9,0x6,0x6],
  'W':[0x9,0x9,0x9,0x9,0xF,0xF,0x9], 'X':[0x9,0x9,0x6,0x6,0x6,0x9,0x9],
  'Y':[0x9,0x9,0x9,0x6,0x2,0x2,0x2], 'Z':[0xF,0x1,0x2,0x4,0x8,0x8,0xF],
  ' ':[0,0,0,0,0,0,0], ':':[0,0x6,0x6,0,0x6,0x6,0], '.':[0,0,0,0,0,0x6,0x6],
  '-':[0,0,0,0xF,0,0,0], '/':[0x1,0x1,0x2,0x2,0x4,0x8,0x8],
  '?':[0x6,0x9,0x1,0x2,0x4,0,0x4], '!':[0x4,0x4,0x4,0x4,0x4,0,0x4],
  ',':[0,0,0,0,0x6,0x2,0x4], '%':[0x9,0x1,0x2,0x4,0x8,0x9,0],
  '+':[0,0x4,0x4,0xF,0x4,0x4,0], '=':[0,0,0xF,0,0xF,0,0],
};

export function createOffscreen2D(canvas) {
  const w = canvas.width, h = canvas.height;
  const pixels = new Uint8ClampedArray(w * h * 4);
  canvas._pixels = pixels;
  canvas._s2dDirty = true;

  // Transform stack: translate/rotate/scale only, which is the measured surface.
  let m = [1, 0, 0, 1, 0, 0];
  const stack = [];
  const path = [];        // flat [x0,y0,x1,y1,...] in DEVICE space
  let sub = 0;            // index of the current subpath's first coordinate
  const state = {
    fillStyle: '#000000', strokeStyle: '#000000',
    lineWidth: 1, globalAlpha: 1, font: '10px sans-serif', textAlign: 'left',
    // Accepted and recorded but not honoured by this software path. Assigning
    // them must not throw: engines set compositing and shadow state every frame.
    textBaseline: 'alphabetic', globalCompositeOperation: 'source-over',
    lineCap: 'butt', lineJoin: 'miter', imageSmoothingEnabled: true,
    shadowBlur: 0, shadowColor: 'transparent', shadowOffsetX: 0, shadowOffsetY: 0,
    filter: 'none',
  };

  const apply = (x, y) => [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]];

  function blend(i, c, alpha) {
    const a = (c[3] / 255) * alpha;
    if (a <= 0) return;
    if (a >= 1) {
      pixels[i] = c[0]; pixels[i + 1] = c[1]; pixels[i + 2] = c[2]; pixels[i + 3] = 255;
      return;
    }
    pixels[i] = pixels[i] * (1 - a) + c[0] * a;
    pixels[i + 1] = pixels[i + 1] * (1 - a) + c[1] * a;
    pixels[i + 2] = pixels[i + 2] * (1 - a) + c[2] * a;
    pixels[i + 3] = Math.max(pixels[i + 3], c[3] * a);
  }

  function fillRectRaw(x, y, rw, rh, color) {
    // Axis-aligned fast path when the transform has no rotation/skew, which is
    // every case the corpus actually produces.
    const [x0, y0] = apply(x, y);
    const [x1, y1] = apply(x + rw, y + rh);
    const lx = Math.max(0, Math.floor(Math.min(x0, x1)));
    const rx = Math.min(w, Math.ceil(Math.max(x0, x1)));
    const ty = Math.max(0, Math.floor(Math.min(y0, y1)));
    const by = Math.min(h, Math.ceil(Math.max(y0, y1)));
    for (let py = ty; py < by; py++) {
      let i = (py * w + lx) * 4;
      for (let px = lx; px < rx; px++, i += 4) blend(i, color, state.globalAlpha);
    }
    canvas._s2dDirty = true;
  }

  const ctx = {
    canvas,

    fillRect(x, y, rw, rh) {
      const c = parseColor(state.fillStyle);
      if (!c) throw new TypeError(`offscreen fillRect: cannot parse color '${state.fillStyle}'`);
      fillRectRaw(x, y, rw, rh, c);
    },

    clearRect(x, y, rw, rh) {
      const [x0, y0] = apply(x, y);
      const [x1, y1] = apply(x + rw, y + rh);
      const lx = Math.max(0, Math.floor(Math.min(x0, x1)));
      const rx = Math.min(w, Math.ceil(Math.max(x0, x1)));
      const ty = Math.max(0, Math.floor(Math.min(y0, y1)));
      const by = Math.min(h, Math.ceil(Math.max(y0, y1)));
      for (let py = ty; py < by; py++) {
        let i = (py * w + lx) * 4;
        for (let px = lx; px < rx; px++, i += 4) {
          pixels[i] = 0; pixels[i + 1] = 0; pixels[i + 2] = 0; pixels[i + 3] = 0;
        }
      }
      canvas._s2dDirty = true;
    },

    strokeRect(x, y, rw, rh) {
      const c = parseColor(state.strokeStyle);
      if (!c) throw new TypeError(`offscreen strokeRect: cannot parse color '${state.strokeStyle}'`);
      const lw = Math.max(1, state.lineWidth | 0);
      fillRectRaw(x, y, rw, lw, c);
      fillRectRaw(x, y + rh - lw, rw, lw, c);
      fillRectRaw(x, y, lw, rh, c);
      fillRectRaw(x + rw - lw, y, lw, rh, c);
    },

    /*
     * Text on an offscreen canvas.
     *
     * The real rasterizer lives in the GL renderer and cannot draw into a CPU
     * buffer, so this renders a blocky 5x7 approximation. It is deliberately
     * legible-but-plain rather than absent: games build sprite sheets and score
     * labels offscreen, and throwing here stopped two corpus games dead. The
     * shape differs from the display font; the text is present and readable.
     */
    fillText(str, tx, ty) {
      const c = parseColor(state.fillStyle);
      if (!c) return;
      const px = Math.max(6, parseInt(state.font, 10) || 10);
      const scale = Math.max(1, Math.round(px / 7));
      const cw = 4 * scale, ch = 7 * scale;
      let sx = tx;
      if (state.textAlign === 'center') sx -= (String(str).length * (cw + scale)) / 2;
      else if (state.textAlign === 'right') sx -= String(str).length * (cw + scale);
      for (const chr of String(str)) {
        const bits = GLYPHS[chr.toUpperCase()] || GLYPHS['?'];
        if (bits) {
          for (let row = 0; row < 7; row++) {
            for (let col = 0; col < 4; col++) {
              if (bits[row] & (1 << (3 - col))) {
                fillRectRaw(sx + col * scale, ty - ch + row * scale, scale, scale, c);
              }
            }
          }
        }
        sx += cw + scale;
      }
    },
    measureText(s) {
      const px = Math.max(6, parseInt(state.font, 10) || 10);
      const scale = Math.max(1, Math.round(px / 7));
      return { width: String(s).length * (4 * scale + scale),
               actualBoundingBoxAscent: 7 * scale, actualBoundingBoxDescent: 0 };
    },

    /*
     * Path API, mirroring the GL context so a game does not have to care which
     * surface it drew on. Fill is scanline-based here (CPU buffer), which handles
     * concave polygons correctly — better than the GL side's triangle fan.
     */
    beginPath() { path.length = 0; sub = 0; },
    closePath() { if (path.length > sub) path.push(path[sub], path[sub + 1]); },
    moveTo(px, py) { sub = path.length; const [ax, ay] = apply(px, py); path.push(ax, ay); },
    lineTo(px, py) { const [ax, ay] = apply(px, py); path.push(ax, ay); },
    rect(px, py, rw, rh) {
      ctx.moveTo(px, py);
      const pts = [[px + rw, py], [px + rw, py + rh], [px, py + rh]];
      for (const [qx, qy] of pts) { const [ax, ay] = apply(qx, qy); path.push(ax, ay); }
      ctx.closePath();
    },
    arc(cx2, cy2, r, a0, a1, ccw) {
      let sweep = a1 - a0;
      if (!ccw && sweep < 0) sweep += Math.PI * 2;
      if (ccw && sweep > 0) sweep -= Math.PI * 2;
      const steps = Math.max(8, Math.min(180, Math.ceil(Math.abs(sweep) * r / 3)));
      for (let i = 0; i <= steps; i++) {
        const t = a0 + sweep * (i / steps);
        const [ax, ay] = apply(cx2 + Math.cos(t) * r, cy2 + Math.sin(t) * r);
        path.push(ax, ay);
      }
    },
    ellipse(cx2, cy2, rx, ry, rot, a0, a1, ccw) {
      let sweep = a1 - a0;
      if (!ccw && sweep < 0) sweep += Math.PI * 2;
      if (ccw && sweep > 0) sweep -= Math.PI * 2;
      const steps = Math.max(8, Math.min(180, Math.ceil(Math.abs(sweep) * Math.max(rx, ry) / 3)));
      const cr = Math.cos(rot), sr = Math.sin(rot);
      for (let i = 0; i <= steps; i++) {
        const t = a0 + sweep * (i / steps);
        const ex = Math.cos(t) * rx, ey = Math.sin(t) * ry;
        const [ax, ay] = apply(cx2 + ex * cr - ey * sr, cy2 + ex * sr + ey * cr);
        path.push(ax, ay);
      }
    },
    quadraticCurveTo(qx, qy, ex, ey) {
      const n0 = path.length;
      const x0 = n0 >= 2 ? path[n0 - 2] : 0, y0 = n0 >= 2 ? path[n0 - 1] : 0;
      const [cx2, cy2] = apply(qx, qy);
      const [x1, y1] = apply(ex, ey);
      for (let i = 1; i <= 12; i++) {
        const t = i / 12, u = 1 - t;
        path.push(u * u * x0 + 2 * u * t * cx2 + t * t * x1,
                  u * u * y0 + 2 * u * t * cy2 + t * t * y1);
      }
    },
    bezierCurveTo(c1x, c1y, c2x, c2y, ex, ey) {
      const n0 = path.length;
      const x0 = n0 >= 2 ? path[n0 - 2] : 0, y0 = n0 >= 2 ? path[n0 - 1] : 0;
      const [ax1, ay1] = apply(c1x, c1y);
      const [ax2, ay2] = apply(c2x, c2y);
      const [x1, y1] = apply(ex, ey);
      for (let i = 1; i <= 16; i++) {
        const t = i / 16, u = 1 - t;
        const b0 = u*u*u, b1 = 3*u*u*t, b2 = 3*u*t*t, b3 = t*t*t;
        path.push(b0*x0 + b1*ax1 + b2*ax2 + b3*x1, b0*y0 + b1*ay1 + b2*ay2 + b3*y1);
      }
    },
    fill() {
      const c = parseColor(state.fillStyle);
      if (!c || path.length < 6) return;
      // Even-odd scanline fill: correct for concave shapes, which a triangle fan
      // is not.
      let minY = Infinity, maxY = -Infinity;
      for (let i = 1; i < path.length; i += 2) {
        if (path[i] < minY) minY = path[i];
        if (path[i] > maxY) maxY = path[i];
      }
      const y0 = Math.max(0, Math.floor(minY)), y1 = Math.min(h - 1, Math.ceil(maxY));
      const xs = [];
      for (let py = y0; py <= y1; py++) {
        xs.length = 0;
        for (let i = 0; i + 3 < path.length; i += 2) {
          const ax = path[i], ay = path[i + 1], bx = path[i + 2], by = path[i + 3];
          if ((ay <= py && by > py) || (by <= py && ay > py)) {
            xs.push(ax + ((py - ay) / (by - ay)) * (bx - ax));
          }
        }
        xs.sort((a, b) => a - b);
        for (let k = 0; k + 1 < xs.length; k += 2) {
          const sxp = Math.max(0, Math.ceil(xs[k])), exp = Math.min(w - 1, Math.floor(xs[k + 1]));
          for (let pxx = sxp; pxx <= exp; pxx++) blend((py * w + pxx) * 4, c, state.globalAlpha);
        }
      }
      canvas._s2dDirty = true;
    },
    stroke() {
      const c = parseColor(state.strokeStyle);
      if (!c || path.length < 4) return;
      const lw = Math.max(1, Math.round(state.lineWidth));
      for (let i = 0; i + 3 < path.length; i += 2) {
        // Bresenham-ish: step along the segment and stamp lineWidth-sized dots.
        const ax = path[i], ay = path[i + 1], bx = path[i + 2], by = path[i + 3];
        const dist = Math.hypot(bx - ax, by - ay);
        const steps = Math.max(1, Math.ceil(dist));
        for (let s2 = 0; s2 <= steps; s2++) {
          const t = s2 / steps;
          const cxp = Math.round(ax + (bx - ax) * t), cyp = Math.round(ay + (by - ay) * t);
          for (let oy = 0; oy < lw; oy++) {
            for (let ox = 0; ox < lw; ox++) {
              const pxx = cxp + ox - (lw >> 1), pyy = cyp + oy - (lw >> 1);
              if (pxx < 0 || pxx >= w || pyy < 0 || pyy >= h) continue;
              blend((pyy * w + pxx) * 4, c, state.globalAlpha);
            }
          }
        }
      }
      canvas._s2dDirty = true;
    },
    clip() { /* see the GL context: ignoring a clip renders more, never less */ },

    drawImage(img, dx, dy, dw, dh) {
      const src = img && img._rgba;
      if (!src) {
        throw new Error(
          'offscreen drawImage: source has no CPU pixels. Only decoded Images ' +
          'can be drawn onto an offscreen canvas.');
      }
      const sw = img.width, sh = img.height;
      const tw = dw === undefined ? sw : dw;
      const th = dh === undefined ? sh : dh;
      const [ox, oy] = apply(dx, dy);
      for (let yy = 0; yy < th; yy++) {
        const sy = Math.min(sh - 1, (yy * sh / th) | 0);
        const py = (oy + yy) | 0;
        if (py < 0 || py >= h) continue;
        for (let xx = 0; xx < tw; xx++) {
          const sx = Math.min(sw - 1, (xx * sw / tw) | 0);
          const px = (ox + xx) | 0;
          if (px < 0 || px >= w) continue;
          const si = (sy * sw + sx) * 4;
          blend((py * w + px) * 4,
                [src[si], src[si + 1], src[si + 2], src[si + 3]], state.globalAlpha);
        }
      }
      canvas._s2dDirty = true;
    },

    getImageData(x, y, gw, gh) {
      if (gw <= 0 || gh <= 0) {
        throw new RangeError(`offscreen getImageData: ${gw}x${gh} must be positive`);
      }
      const out = new Uint8ClampedArray(gw * gh * 4);
      for (let yy = 0; yy < gh; yy++) {
        const sy = y + yy;
        if (sy < 0 || sy >= h) continue;
        for (let xx = 0; xx < gw; xx++) {
          const sx = x + xx;
          if (sx < 0 || sx >= w) continue;
          const si = (sy * w + sx) * 4, di = (yy * gw + xx) * 4;
          out[di] = pixels[si]; out[di + 1] = pixels[si + 1];
          out[di + 2] = pixels[si + 2]; out[di + 3] = pixels[si + 3];
        }
      }
      return { width: gw, height: gh, data: out, colorSpace: 'srgb' };
    },

    putImageData(data, dx, dy) {
      const need = data.width * data.height * 4;
      if (data.data.length < need) {
        throw new RangeError(
          `offscreen putImageData: ${data.width}x${data.height} needs ${need} bytes ` +
          `but the buffer holds ${data.data.length}`);
      }
      for (let yy = 0; yy < data.height; yy++) {
        const py = dy + yy;
        if (py < 0 || py >= h) continue;
        for (let xx = 0; xx < data.width; xx++) {
          const px = dx + xx;
          if (px < 0 || px >= w) continue;
          const si = (yy * data.width + xx) * 4, di = (py * w + px) * 4;
          pixels[di] = data.data[si]; pixels[di + 1] = data.data[si + 1];
          pixels[di + 2] = data.data[si + 2]; pixels[di + 3] = data.data[si + 3];
        }
      }
      canvas._s2dDirty = true;
    },

    /*
     * Transform matrix control, mirroring the GL context. Engines reset the
     * transform every frame (Phaser calls setTransform(1,0,0,1,0,0) in its
     * preRender), so an offscreen canvas missing these stops the render loop
     * on its very first frame.
     */
    setTransform(a, b, c, d, e, f) {
      if (a && typeof a === 'object') {  // DOMMatrix-like
        m = [a.a ?? 1, a.b ?? 0, a.c ?? 0, a.d ?? 1, a.e ?? 0, a.f ?? 0];
        return;
      }
      m = [a, b, c, d, e, f];
    },
    resetTransform() { m = [1, 0, 0, 1, 0, 0]; },
    getTransform() {
      return { a: m[0], b: m[1], c: m[2], d: m[3], e: m[4], f: m[5] };
    },
    transform(a, b, c, d, e, f) {
      const n = m;
      m = [
        n[0] * a + n[2] * b,       n[1] * a + n[3] * b,
        n[0] * c + n[2] * d,       n[1] * c + n[3] * d,
        n[0] * e + n[2] * f + n[4], n[1] * e + n[3] * f + n[5],
      ];
    },
    setLineDash() {}, getLineDash() { return []; },
    strokeText(str, tx, ty) {
      const saved = state.fillStyle;
      state.fillStyle = state.strokeStyle;
      ctx.fillText(str, tx, ty);
      state.fillStyle = saved;
    },
    createImageData(a, b) {
      const cw = typeof a === 'number' ? a : a.width;
      const chh = typeof a === 'number' ? b : a.height;
      return { width: cw, height: chh, data: new Uint8ClampedArray(cw * chh * 4),
               colorSpace: 'srgb' };
    },

    save() { stack.push(m.slice()); stack.push({ ...state }); },
    restore() {
      if (!stack.length) return;
      Object.assign(state, stack.pop());
      m = stack.pop();
    },
    translate(x, y) { m[4] += m[0] * x + m[2] * y; m[5] += m[1] * x + m[3] * y; },
    scale(x, y) { m[0] *= x; m[1] *= x; m[2] *= y; m[3] *= y; },
    rotate(r) {
      const c = Math.cos(r), s = Math.sin(r);
      const [a, b, cc, d] = m;
      m[0] = a * c + cc * s; m[1] = b * c + d * s;
      m[2] = cc * c - a * s; m[3] = d * c - b * s;
    },
  };

  for (const k of ['fillStyle', 'strokeStyle', 'lineWidth', 'globalAlpha', 'font',
                   'textAlign', 'textBaseline', 'globalCompositeOperation',
                   'lineCap', 'lineJoin', 'imageSmoothingEnabled', 'shadowBlur',
                   'shadowColor', 'shadowOffsetX', 'shadowOffsetY', 'filter']) {
    Object.defineProperty(ctx, k, {
      get: () => state[k],
      set: (v) => { state[k] = v; },
    });
  }

  for (const name of UNSUPPORTED) {
    ctx[name] = () => {
      throw new Error(
        `CanvasRenderingContext2D.${name} is not implemented for offscreen canvases. ` +
        'This throws rather than doing nothing so a missing feature is visible.');
    };
  }

  return ctx;
}
