/*
 * sprite2d.c - see sprite2d.h for scope and why this is not NanoVG.
 *
 * One shader, one batched vertex buffer, one texture unit. Every primitive is
 * a textured quad:
 *   fillRect  -> quad sampling a 1x1 white texture, tinted
 *   drawImage -> quad sampling the image texture
 *   fillText  -> one quad per glyph sampling the font atlas
 * so there is a single draw path and a single flush.
 */
/*
 * Ported from a WASM prototype that validated this design. The only change needed
 * was this include: the renderer already calls GL by its standard names, so against
 * real GLES3 headers it compiles unchanged.
 */
#include <GLES3/gl3.h>
#include "sprite2d.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "font_ttf.inc"

#define MAX_QUADS   4096
#define VERTS_PER   6            /* two triangles, no index buffer */
#define FLOATS_PER  8            /* x,y,u,v,rgba(4) -- rgba as 4 floats */
#define STACK_DEPTH 32
#define MAX_IMAGES  64

/* ── text: real TTF glyphs via stb_truetype ───────────────────────
 * Replaces a 5x7 bitfont that was legible but visibly worse than Skia
 * side-by-side: uppercase-only, fixed-width, and with NO heart glyph, so
 * star-catcher's "Lives: ♥♥♥" rendered as "Lives:" and nothing after it.
 * A real font fixes proportional metrics, lowercase and coverage at once.
 *
 * Atlases bake per pixel-size on demand (games use 16/24/32/52px), are cached,
 * and upload as white-RGB + coverage-in-alpha so the vertex tint colours the
 * text through the same shader as everything else.
 */
/* TWO SMALL RANGES, not one huge one. stbtt_BakeFontBitmap bakes a CONTIGUOUS
 * run, and 32..0x2700 is ~9700 glyphs: it truncates at EVERY size (measured:
 * -7724 glyphs at 16px in a 1024 atlas, -9699 at 64px in 4096), and U+2665
 * fell outside what fit -- so "Lives: ♥♥♥" rendered as "Lives:" and a gap.
 * Latin-1 plus a 32-glyph symbol window costs a fraction of the space and
 * covers everything the corpus uses. */
#define LAT_FIRST 32
#define LAT_COUNT (0x100 - LAT_FIRST)     /* ASCII + Latin-1 */
#define SYM_FIRST 0x2660                  /* card suits: ♠♡♢♣ ... ♥ is 0x2665 */
#define SYM_COUNT 32
#define CP_COUNT  (LAT_COUNT + SYM_COUNT)
#define MAX_FONTS 8

/* codepoint -> index in the baked table, or -1 */
static int cp_slot(unsigned cp) {
    if (cp >= LAT_FIRST && cp < LAT_FIRST + LAT_COUNT) return (int)(cp - LAT_FIRST);
    if (cp >= SYM_FIRST && cp < SYM_FIRST + SYM_COUNT) return LAT_COUNT + (int)(cp - SYM_FIRST);
    return -1;
}

typedef struct {
    int px, aw, ah, active;
    GLuint tex;
    stbtt_bakedchar *chars;
} font_t;
static font_t fonts[MAX_FONTS];
static int font_count;

/* Decode one UTF-8 sequence, advancing *p. The heart is U+2665: a byte-wise
 * reader would emit three garbage glyphs instead of one. */
static unsigned utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = *s++;
    if (c < 0x80) { *p = (const char *)s; return c; }
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
    c &= (unsigned)(0x3F >> extra);
    while (extra-- > 0 && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3F);
    *p = (const char *)s;
    return c;
}

static font_t *font_for_px(int px) {
    if (px < 6) px = 6;
    if (px > 128) px = 128;
    for (int i = 0; i < font_count; i++)
        if (fonts[i].active && fonts[i].px == px) return &fonts[i];
    if (font_count >= MAX_FONTS) return &fonts[0];

    font_t *f = &fonts[font_count];
    int aw = 512;
    while (aw < px * 40 && aw < 4096) aw *= 2;
    int ah = aw;
    unsigned char *gray = (unsigned char *)malloc((size_t)aw * ah);
    f->chars = (stbtt_bakedchar *)malloc(sizeof(stbtt_bakedchar) * CP_COUNT);
    if (!gray || !f->chars) { free(gray); free(f->chars); f->chars = 0; return &fonts[0]; }
    memset(gray, 0, (size_t)aw * ah);
    /* Bake Latin first, then continue the SAME atlas with the symbol window.
     * stbtt_BakeFontBitmap always restarts packing at the top, so the second
     * call is given the rows below what the first used. */
    int used = stbtt_BakeFontBitmap(FONT_TTF, 0, (float)px, gray, aw, ah,
                                    LAT_FIRST, LAT_COUNT, f->chars);
    int rows = used > 0 ? used : ah / 2;   /* bottom of the Latin block */
    stbtt_bakedchar sym[SYM_COUNT];
    stbtt_BakeFontBitmap(FONT_TTF, 0, (float)px, gray + (size_t)rows * aw,
                         aw, ah - rows, SYM_FIRST, SYM_COUNT, sym);
    for (int i = 0; i < SYM_COUNT; i++) {
        sym[i].y0 += (unsigned short)rows;      /* shift into atlas space */
        sym[i].y1 += (unsigned short)rows;
        f->chars[LAT_COUNT + i] = sym[i];
    }

    unsigned char *rgba = (unsigned char *)malloc((size_t)aw * ah * 4);
    if (!rgba) { free(gray); return &fonts[0]; }
    for (int i = 0; i < aw * ah; i++) {
        rgba[i*4+0] = 255; rgba[i*4+1] = 255; rgba[i*4+2] = 255; rgba[i*4+3] = gray[i];
    }
    glGenTextures(1, &f->tex);
    glBindTexture(GL_TEXTURE_2D, f->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, aw, ah, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(gray); free(rgba);
    f->px = px; f->aw = aw; f->ah = ah; f->active = 1;
    return &fonts[font_count++];
}
typedef struct { float m[6]; } mat2d;   /* a b c d e f, column-major-ish 2x3 */

static mat2d  xf_stack[STACK_DEPTH];
static int    xf_top;
static mat2d  xf;

static float  fb_w, fb_h;
static GLuint prog, vao, vbo, tex_white, tex_font;
static GLint  u_tex, u_res;
static float  verts[MAX_QUADS * VERTS_PER * FLOATS_PER];
static int    nquads;
static GLuint cur_tex;

static float  fill_r, fill_g, fill_b, fill_a;
static float  strokeR, strokeG, strokeB, strokeA;
static float  line_w = 1.0f, g_alpha = 1.0f, font_px = 10.0f;
static int    text_align;

static struct { GLuint tex; int w, h; } images[MAX_IMAGES];
static int nimages;

static const char *VS =
    "#version 300 es\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "uniform vec2 u_res;\n"
    "out vec2 v_uv; out vec4 v_col;\n"
    "void main(){\n"
    "  vec2 p = a_pos / u_res * 2.0 - 1.0;\n"
    "  gl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n"  /* y-down, like Canvas2D */
    "  v_uv = a_uv; v_col = a_col;\n"
    "}\n";

static const char *FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv; in vec4 v_col;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o;\n"
    "void main(){ o = texture(u_tex, v_uv) * v_col; }\n";

static void mat_identity(mat2d *m) {
    m->m[0]=1; m->m[1]=0; m->m[2]=0; m->m[3]=1; m->m[4]=0; m->m[5]=0;
}
static void mat_apply(const mat2d *m, float x, float y, float *ox, float *oy) {
    *ox = m->m[0]*x + m->m[2]*y + m->m[4];
    *oy = m->m[1]*x + m->m[3]*y + m->m[5];
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    return s;
}

static void flush(void) {
    if (!nquads) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(nquads * VERTS_PER * FLOATS_PER * sizeof(float)),
                 verts, GL_DYNAMIC_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cur_tex);
    glUniform1i(u_tex, 0);
    glDrawArrays(GL_TRIANGLES, 0, nquads * VERTS_PER);
    nquads = 0;
}

static void use_tex(GLuint t) {
    if (t != cur_tex) { flush(); cur_tex = t; }
}

/* Push one transformed, tinted, textured quad. Corners are given in local
 * space and run through the current transform, which is what makes
 * save/translate/rotate/drawImage compose the way Canvas2D does. */
static void push_quad(float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1,
                      float r, float g, float b, float a) {
    if (nquads >= MAX_QUADS) flush();
    float cx[4], cy[4];
    mat_apply(&xf, x0, y0, &cx[0], &cy[0]);
    mat_apply(&xf, x1, y0, &cx[1], &cy[1]);
    mat_apply(&xf, x1, y1, &cx[2], &cy[2]);
    mat_apply(&xf, x0, y1, &cx[3], &cy[3]);
    const int idx[6] = {0,1,2, 0,2,3};
    const float us[4] = {u0,u1,u1,u0}, vs[4] = {v0,v0,v1,v1};
    float *p = &verts[nquads * VERTS_PER * FLOATS_PER];
    for (int i = 0; i < 6; i++) {
        int k = idx[i];
        *p++ = cx[k]; *p++ = cy[k];
        *p++ = us[k]; *p++ = vs[k];
        *p++ = r; *p++ = g; *p++ = b; *p++ = a * g_alpha;
    }
    nquads++;
}

int s2d_init(int width, int height) {
    fb_w = (float)width; fb_h = (float)height;
    mat_identity(&xf); xf_top = 0;
    fill_r = fill_g = fill_b = fill_a = 1.0f;
    strokeR = strokeG = strokeB = strokeA = 1.0f;

    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    u_tex = glGetUniformLocation(prog, "u_tex");
    u_res = glGetUniformLocation(prog, "u_res");

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const GLsizei stride = FLOATS_PER * sizeof(float);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(4*sizeof(float)));
    glEnableVertexAttribArray(2);

    /* 1x1 white: lets fillRect share the textured-quad path. */
    const uint8_t white[4] = {255,255,255,255};
    glGenTextures(1, &tex_white);
    glBindTexture(GL_TEXTURE_2D, tex_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* Font atlases are baked lazily per pixel-size -- see font_for_px. */

    cur_tex = tex_white;
    return glGetError() == GL_NO_ERROR;
}

void s2d_begin(void) {
    glUseProgram(prog);
    glBindVertexArray(vao);
    glUniform2f(u_res, fb_w, fb_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    mat_identity(&xf);
    xf_top = 0;
    g_alpha = 1.0f;
    nquads = 0;
}

void s2d_end(void) { flush(); }

void s2d_set_fill(uint32_t c) {
    fill_r = ((c>>24)&0xff)/255.0f; fill_g = ((c>>16)&0xff)/255.0f;
    fill_b = ((c>>8)&0xff)/255.0f;  fill_a = (c&0xff)/255.0f;
}
void s2d_set_stroke(uint32_t c) {
    strokeR = ((c>>24)&0xff)/255.0f; strokeG = ((c>>16)&0xff)/255.0f;
    strokeB = ((c>>8)&0xff)/255.0f;  strokeA = (c&0xff)/255.0f;
}
void s2d_set_line_width(float w) { line_w = w > 0 ? w : 1.0f; }
void s2d_set_alpha(float a)      { g_alpha = a < 0 ? 0 : (a > 1 ? 1 : a); }
void s2d_set_font_px(float px)   { font_px = px > 0 ? px : 10.0f; }
void s2d_set_text_align(int a)   { text_align = a; }

void s2d_save(void) { if (xf_top < STACK_DEPTH) xf_stack[xf_top++] = xf; }
void s2d_restore(void) { if (xf_top > 0) xf = xf_stack[--xf_top]; }

void s2d_translate(float x, float y) {
    xf.m[4] += xf.m[0]*x + xf.m[2]*y;
    xf.m[5] += xf.m[1]*x + xf.m[3]*y;
}
void s2d_rotate(float rad) {
    float c = cosf(rad), s = sinf(rad);
    float a = xf.m[0], b = xf.m[1], cc = xf.m[2], d = xf.m[3];
    xf.m[0] = a*c + cc*s;  xf.m[1] = b*c + d*s;
    xf.m[2] = cc*c - a*s;  xf.m[3] = d*c - b*s;
}
void s2d_scale(float x, float y) {
    xf.m[0] *= x; xf.m[1] *= x; xf.m[2] *= y; xf.m[3] *= y;
}

void s2d_fill_rect(float x, float y, float w, float h) {
    use_tex(tex_white);
    push_quad(x, y, x+w, y+h, 0,0, 1,1, fill_r, fill_g, fill_b, fill_a);
}

void s2d_stroke_rect(float x, float y, float w, float h) {
    use_tex(tex_white);
    float t = line_w;
    /* four thin filled quads -- no path API, and none is needed */
    push_quad(x, y, x+w, y+t, 0,0,1,1, strokeR,strokeG,strokeB,strokeA);
    push_quad(x, y+h-t, x+w, y+h, 0,0,1,1, strokeR,strokeG,strokeB,strokeA);
    push_quad(x, y+t, x+t, y+h-t, 0,0,1,1, strokeR,strokeG,strokeB,strokeA);
    push_quad(x+w-t, y+t, x+w, y+h-t, 0,0,1,1, strokeR,strokeG,strokeB,strokeA);
}

float s2d_measure_text(const char *s) {
    font_t *f = font_for_px((int)(font_px + 0.5f));
    if (!f || !f->chars) return 0;
    float w = 0;
    const char *p = s;
    while (*p) {
        int slot = cp_slot(utf8_next(&p));
        if (slot < 0) { w += font_px * 0.4f; continue; }
        w += f->chars[slot].xadvance;   /* REAL proportional advance */
    }
    return w;
}

void s2d_fill_text(const char *s, float x, float y) {
    font_t *f = font_for_px((int)(font_px + 0.5f));
    if (!f || !f->chars) return;
    use_tex(f->tex);
    float pen = x;
    if (text_align == 1)      pen -= s2d_measure_text(s) * 0.5f;
    else if (text_align == 2) pen -= s2d_measure_text(s);
    const char *p = s;
    while (*p) {
        int slot = cp_slot(utf8_next(&p));
        if (slot < 0) { pen += font_px * 0.4f; continue; }
        stbtt_bakedchar *b = &f->chars[slot];
        /* stb gives pixel offsets from the pen at the BASELINE, which is
         * exactly Canvas2D's y origin -- no fudge factor needed. */
        float x0 = pen + b->xoff, y0 = y + b->yoff;
        float x1 = x0 + (b->x1 - b->x0), y1 = y0 + (b->y1 - b->y0);
        push_quad(x0, y0, x1, y1,
                  (float)b->x0 / f->aw, (float)b->y0 / f->ah,
                  (float)b->x1 / f->aw, (float)b->y1 / f->ah,
                  fill_r, fill_g, fill_b, fill_a);
        pen += b->xadvance;
    }
}

int s2d_image_create(const uint8_t *rgba, int w, int h) {
    if (nimages >= MAX_IMAGES) return -1;
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    images[nimages].tex = t; images[nimages].w = w; images[nimages].h = h;
    cur_tex = t;   /* glGenTextures left it bound */
    return nimages++;
}

void s2d_image_size(int h, int *w, int *ht) {
    if (h < 0 || h >= nimages) { *w = *ht = 0; return; }
    *w = images[h].w; *ht = images[h].h;
}

void s2d_draw_image(int handle, float sx, float sy, float sw, float sh,
                    float dx, float dy, float dw, float dh) {
    if (handle < 0 || handle >= nimages) return;
    float iw = (float)images[handle].w, ih = (float)images[handle].h;
    if (sw < 0) { sx = 0; sy = 0; sw = iw; sh = ih; }
    use_tex(images[handle].tex);
    push_quad(dx, dy, dx+dw, dy+dh,
              sx/iw, sy/ih, (sx+sw)/iw, (sy+sh)/ih,
              1,1,1,1);
}


/* ─── path API ─────────────────────────────────────────────────────
 *
 * Added after measuring the SHIPPED corpus bundles rather than 30 frames of
 * gameplay: across the 14 games there are 187 beginPath, 173 lineTo, 152 fill,
 * 112 moveTo and 112 arc calls. The earlier "no game uses paths" finding came
 * from a 30-frame runtime window that simply never reached the code drawing them
 * — a good reminder that a sampling window is not the same as coverage.
 *
 * Implementation is deliberately simple and matches the renderer it lives in:
 *   fill   -> triangle fan from the first point (correct for convex polygons and
 *             for the star/arc shapes games actually draw; concave polygons can
 *             show artefacts, which is a documented limitation, not silence)
 *   stroke -> one quad per segment, plus a round-ish joint quad at each vertex
 *   arc    -> flattened to line segments at a curvature-appropriate step
 *
 * Everything goes through the same batched vertex path as fillRect, so there is
 * still one shader, one buffer, and one flush.
 */
#define MAX_PATH_POINTS 4096

static float path_x[MAX_PATH_POINTS], path_y[MAX_PATH_POINTS];
static int   path_n;
static int   subpath_start;      /* index of the current subpath's first point */
static float path_cur_x, path_cur_y;
static int   path_has_cur;

static void path_push(float x, float y) {
    if (path_n >= MAX_PATH_POINTS) return;   /* drop rather than overrun */
    /* Skip exact duplicates: they contribute nothing and produce degenerate
       triangles that some drivers rasterize as stray pixels. */
    if (path_n > subpath_start &&
        path_x[path_n-1] == x && path_y[path_n-1] == y) return;
    path_x[path_n] = x; path_y[path_n] = y; path_n++;
    path_cur_x = x; path_cur_y = y; path_has_cur = 1;
}

void s2d_begin_path(void) { path_n = 0; subpath_start = 0; path_has_cur = 0; }
void s2d_move_to(float x, float y) { subpath_start = path_n; path_push(x, y); }
void s2d_line_to(float x, float y) {
    if (!path_has_cur) { s2d_move_to(x, y); return; }
    path_push(x, y);
}
void s2d_close_path(void) {
    if (path_n > subpath_start) path_push(path_x[subpath_start], path_y[subpath_start]);
}

void s2d_arc(float cx, float cy, float r, float a0, float a1, int ccw) {
    if (r <= 0) { path_push(cx, cy); return; }
    /* Step chosen from the radius so big arcs do not look faceted and small ones
       do not waste vertices. */
    float sweep = a1 - a0;
    if (!ccw && sweep < 0) sweep += 6.283185307179586f;
    if (ccw && sweep > 0)  sweep -= 6.283185307179586f;
    int steps = (int)(fabsf(sweep) * (r < 8.0f ? 4.0f : (r < 64.0f ? 8.0f : 16.0f)));
    if (steps < 3) steps = 3;
    if (steps > 512) steps = 512;
    for (int i = 0; i <= steps; i++) {
        float t = a0 + sweep * ((float)i / (float)steps);
        path_push(cx + cosf(t) * r, cy + sinf(t) * r);
    }
}

void s2d_ellipse(float cx, float cy, float rx, float ry, float rot,
                 float a0, float a1, int ccw) {
    float sweep = a1 - a0;
    if (!ccw && sweep < 0) sweep += 6.283185307179586f;
    if (ccw && sweep > 0)  sweep -= 6.283185307179586f;
    float rmax = rx > ry ? rx : ry;
    int steps = (int)(fabsf(sweep) * (rmax < 8.0f ? 4.0f : (rmax < 64.0f ? 8.0f : 16.0f)));
    if (steps < 3) steps = 3;
    if (steps > 512) steps = 512;
    float cr = cosf(rot), sr = sinf(rot);
    for (int i = 0; i <= steps; i++) {
        float t = a0 + sweep * ((float)i / (float)steps);
        float ex = cosf(t) * rx, ey = sinf(t) * ry;
        path_push(cx + ex * cr - ey * sr, cy + ex * sr + ey * cr);
    }
}

/* Quadratic and cubic beziers, flattened. Step count is fixed rather than
   adaptive: game curves are short, and a subdivision pass would cost more than
   the extra vertices. */
void s2d_quadratic_to(float cx, float cy, float x, float y) {
    if (!path_has_cur) { s2d_move_to(cx, cy); }
    float x0 = path_cur_x, y0 = path_cur_y;
    const int STEPS = 16;
    for (int i = 1; i <= STEPS; i++) {
        float t = (float)i / STEPS, u = 1.0f - t;
        path_push(u*u*x0 + 2*u*t*cx + t*t*x, u*u*y0 + 2*u*t*cy + t*t*y);
    }
}

void s2d_bezier_to(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    if (!path_has_cur) { s2d_move_to(c1x, c1y); }
    float x0 = path_cur_x, y0 = path_cur_y;
    const int STEPS = 24;
    for (int i = 1; i <= STEPS; i++) {
        float t = (float)i / STEPS, u = 1.0f - t;
        float b0 = u*u*u, b1 = 3*u*u*t, b2 = 3*u*t*t, b3 = t*t*t;
        path_push(b0*x0 + b1*c1x + b2*c2x + b3*x, b0*y0 + b1*c1y + b2*c2y + b3*y);
    }
}

void s2d_path_rect(float x, float y, float w, float h) {
    s2d_move_to(x, y);
    path_push(x + w, y);
    path_push(x + w, y + h);
    path_push(x, y + h);
    s2d_close_path();
}

/* Emit one triangle through the batched vertex path. */
static void push_tri(float ax, float ay, float bx, float by, float cx2, float cy2,
                     float r, float g, float b, float a) {
    if (nquads >= MAX_QUADS) flush();
    float tx[3], ty[3];
    mat_apply(&xf, ax, ay, &tx[0], &ty[0]);
    mat_apply(&xf, bx, by, &tx[1], &ty[1]);
    mat_apply(&xf, cx2, cy2, &tx[2], &ty[2]);
    float *p = &verts[nquads * VERTS_PER * FLOATS_PER];
    /* A quad slot holds 6 vertices; a triangle uses 3 and repeats the last one so
       the degenerate second triangle rasterizes nothing. */
    for (int i = 0; i < 6; i++) {
        int k = i < 3 ? i : 2;
        *p++ = tx[k]; *p++ = ty[k];
        *p++ = 0.0f;  *p++ = 0.0f;
        *p++ = r; *p++ = g; *p++ = b; *p++ = a * g_alpha;
    }
    nquads++;
}

void s2d_fill_path(void) {
    if (path_n < 3) return;
    use_tex(tex_white);
    for (int i = 1; i + 1 < path_n; i++) {
        push_tri(path_x[0], path_y[0], path_x[i], path_y[i],
                 path_x[i+1], path_y[i+1], fill_r, fill_g, fill_b, fill_a);
    }
}

void s2d_stroke_path(void) {
    if (path_n < 2) return;
    use_tex(tex_white);
    const float hw = (line_w > 0 ? line_w : 1.0f) * 0.5f;
    for (int i = 0; i + 1 < path_n; i++) {
        float dx = path_x[i+1] - path_x[i], dy = path_y[i+1] - path_y[i];
        float len = sqrtf(dx*dx + dy*dy);
        if (len < 1e-6f) continue;
        float nx = -dy / len * hw, ny = dx / len * hw;
        /* Two triangles forming the segment's quad. */
        push_tri(path_x[i]+nx,   path_y[i]+ny,   path_x[i+1]+nx, path_y[i+1]+ny,
                 path_x[i+1]-nx, path_y[i+1]-ny, strokeR, strokeG, strokeB, strokeA);
        push_tri(path_x[i]+nx,   path_y[i]+ny,   path_x[i+1]-nx, path_y[i+1]-ny,
                 path_x[i]-nx,   path_y[i]-ny,   strokeR, strokeG, strokeB, strokeA);
        /* A small square at each interior vertex so joints do not show gaps. */
        if (i + 2 < path_n && hw > 0.75f) {
            push_quad(path_x[i+1]-hw, path_y[i+1]-hw, path_x[i+1]+hw, path_y[i+1]+hw,
                      0,0,1,1, strokeR, strokeG, strokeB, strokeA);
        }
    }
}

void s2d_get_pixels(int x, int y, int w, int h, uint8_t *out) {
    flush();
    /*
     * GL's origin is bottom-left; Canvas2D's is top-left. Offsetting the read
     * rectangle picks the right REGION, but glReadPixels still returns its rows
     * bottom-up, so they must also be reversed. Doing only the offset yields a
     * correctly-positioned but vertically MIRRORED image — which passes every
     * colour-count and pixel-total check while being visibly upside down. Corner
     * markers caught it here; numbers alone never would.
     */
    glReadPixels(x, (int)fb_h - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);

    const size_t stride = (size_t)w * 4u;
    uint8_t *tmp = (uint8_t *)malloc(stride);
    if (!tmp) return;               /* leave as-is rather than corrupt the buffer */
    for (int row = 0; row < h / 2; row++) {
        uint8_t *a = out + (size_t)row * stride;
        uint8_t *b = out + (size_t)(h - 1 - row) * stride;
        memcpy(tmp, a, stride);
        memcpy(a, b, stride);
        memcpy(b, tmp, stride);
    }
    free(tmp);
}

void s2d_put_pixels(int x, int y, int w, int h, const uint8_t *rgba) {
    /*
     * Reuse ONE scratch texture rather than allocating an image handle per call.
     *
     * s2d_image_create consumes a slot from a 64-entry table that is never
     * reclaimed, so a game calling putImageData every frame exhausted it in about
     * a second and every later call then silently drew nothing — no error, no
     * pixels, just a copy that stopped appearing.
     */
    static GLuint scratch = 0;
    static int scratch_w = 0, scratch_h = 0;

    if (w <= 0 || h <= 0) return;

    if (!scratch) {
        glGenTextures(1, &scratch);
        glBindTexture(GL_TEXTURE_2D, scratch);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        scratch_w = scratch_h = 0;
    }

    /* getImageData hands back top-down rows (Canvas order); the sampler expects
       the same orientation drawImage uses, so upload as-is and let the shared
       quad path place it. */
    /*
     * Upload as-is: no row flip.
     *
     * s2d_get_pixels already normalizes its output to top-down (Canvas order), and
     * drawImage's UV mapping consumes top-down texture data, so the two agree.
     * Adding a flip here mirrors the result — measured directly: red at source
     * y=10 landed at y=290 inside a 120px block (200 + 120-10-20), which is the
     * mirror, not a missing copy.
     */
    flush();                       /* the scratch upload must not disturb a batch */
    glBindTexture(GL_TEXTURE_2D, scratch);
    if (w != scratch_w || h != scratch_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        scratch_w = w; scratch_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    mat2d saved = xf;
    mat_identity(&xf);             /* putImageData ignores the current transform */
    GLuint prev = cur_tex;
    cur_tex = scratch;
    push_quad((float)x, (float)y, (float)(x + w), (float)(y + h),
              0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    flush();                       /* submit before the scratch is reused */
    cur_tex = prev;
    xf = saved;
}

/* Direct matrix control for setTransform/transform. The renderer already keeps a
   2x3 affine, so these are assignment and multiply rather than new machinery. */
void s2d_set_transform(float a, float b, float c, float d, float e, float f) {
    xf.m[0]=a; xf.m[1]=b; xf.m[2]=c; xf.m[3]=d; xf.m[4]=e; xf.m[5]=f;
}

void s2d_multiply_transform(float a, float b, float c, float d, float e, float f) {
    mat2d m = xf;
    xf.m[0] = m.m[0]*a + m.m[2]*b;
    xf.m[1] = m.m[1]*a + m.m[3]*b;
    xf.m[2] = m.m[0]*c + m.m[2]*d;
    xf.m[3] = m.m[1]*c + m.m[3]*d;
    xf.m[4] = m.m[0]*e + m.m[2]*f + m.m[4];
    xf.m[5] = m.m[1]*e + m.m[3]*f + m.m[5];
}
