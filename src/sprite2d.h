/*
 * sprite2d.h - the Canvas2D subset real games use, as textured quads on GL.
 *
 * Scope comes from measurement, not guesswork. Instrumenting a 14-game corpus at
 * runtime found fillRect (2550 calls), fillText (270), drawImage (60), and
 * save/restore/translate/rotate (30 each), plus fillStyle/font. Scanning the same
 * games' shipped bundles additionally found heavy path use (187 beginPath, 173
 * lineTo, 152 fill, 112 arc) that the 30-frame runtime window never reached.
 *
 * So this is a SPRITE-AND-TEXT renderer, not a vector one. NanoVG was the
 * original plan and is the wrong tool: it is a path renderer for a corpus that
 * draws no paths, and it cannot do getImageData/putImageData at all.
 *
 * Everything here goes through wasmcart's existing `gl` imports, so a cart
 * gains NO new host surface -- which is the whole point when the goal is
 * containment for untrusted feed games.
 */
#ifndef SPRITE2D_H
#define SPRITE2D_H

#include <stdint.h>

/* Init against a framebuffer of this size. Returns 0 on failure. */
int  s2d_init(int width, int height);

/* Per-frame. begin() sets up state + clears nothing; end() flushes. */
void s2d_begin(void);
void s2d_end(void);

/* ── state (Canvas2D semantics) ──────────────────────────────────── */
void s2d_set_fill(uint32_t rgba);        /* fillStyle   */
void s2d_set_stroke(uint32_t rgba);      /* strokeStyle */
void s2d_set_line_width(float w);        /* lineWidth   */
void s2d_set_alpha(float a);             /* globalAlpha */
void s2d_set_font_px(float px);          /* font size   */
void s2d_set_text_align(int align);      /* 0=left 1=center 2=right */

/* ── transform stack (save/restore/translate/rotate) ─────────────── */
void s2d_save(void);
void s2d_restore(void);
void s2d_translate(float x, float y);
void s2d_rotate(float rad);
void s2d_scale(float x, float y);

/* ── drawing ─────────────────────────────────────────────────────── */
void s2d_fill_rect(float x, float y, float w, float h);
void s2d_stroke_rect(float x, float y, float w, float h);
void s2d_fill_text(const char *utf8, float x, float y);
float s2d_measure_text(const char *utf8);

/* Images. Decoded by the caller (stb_image) into RGBA8; returns a handle. */
int  s2d_image_create(const uint8_t *rgba, int w, int h);
void s2d_image_size(int handle, int *w, int *h);
/* drawImage(img, dx,dy,dw,dh) with the current transform applied. sw<0 means
 * "whole image" (the 3-arg Canvas2D form). */
void s2d_draw_image(int handle, float sx, float sy, float sw, float sh,
                    float dx, float dy, float dw, float dh);

/* ── raw pixels (the WebGL-readback path space-blaster/three-game use) ── */
void s2d_get_pixels(int x, int y, int w, int h, uint8_t *out_rgba);

/* Path API. Measured as heavily used by the shipped corpus bundles (187
   beginPath / 173 lineTo / 152 fill / 112 arc), which a 30-frame runtime sample
   had missed. Fill is a triangle fan: exact for convex shapes, approximate for
   concave ones. */
void s2d_begin_path(void);
void s2d_move_to(float x, float y);
void s2d_line_to(float x, float y);
void s2d_close_path(void);
void s2d_arc(float cx, float cy, float r, float a0, float a1, int ccw);
void s2d_ellipse(float cx, float cy, float rx, float ry, float rot,
                 float a0, float a1, int ccw);
void s2d_quadratic_to(float cx, float cy, float x, float y);
void s2d_bezier_to(float c1x, float c1y, float c2x, float c2y, float x, float y);
void s2d_path_rect(float x, float y, float w, float h);
void s2d_fill_path(void);
void s2d_stroke_path(void);
void s2d_set_transform(float a, float b, float c, float d, float e, float f);
void s2d_multiply_transform(float a, float b, float c, float d, float e, float f);
void s2d_put_pixels(int x, int y, int w, int h, const uint8_t *rgba);

#endif /* SPRITE2D_H */
