/*
 * CanvasRenderingContext2D over the sprite2d renderer.
 *
 * Scope is measured, not guessed. Instrumenting a 14-game corpus at runtime found
 * a very small hot surface (fillRect, fillText, drawImage, save/restore/translate/
 * rotate, getImageData/putImageData), while scanning the same games' SHIPPED
 * bundles additionally found heavy path use — 187 beginPath, 173 lineTo, 152 fill,
 * 112 arc. Both are implemented here. Anything still missing THROWS BY NAME.
 *
 * Worth remembering: the runtime sample alone concluded "no game uses paths",
 * because 30 frames of gameplay never reached that code. A sampling window is not
 * coverage.
 *
 * Throwing rather than no-op'ing is the important part. A silent no-op is how you
 * get a green test suite over an invisible game — the exact failure recorded in the
 * spike notes ("10/10 green while every logo was invisible"). An unimplemented
 * method that names itself is a bug report; one that returns undefined is a mystery.
 */
#include "host.h"
#include "sprite2d.h"

/* window.cpp */
void jsglq_window_size(int *w, int *h);

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ colors --- */

/*
 * CSS colour parsing, limited to the forms the corpus actually uses:
 * #rgb, #rrggbb, #rrggbbaa, rgb()/rgba(), and a small set of named colours.
 * An unrecognized colour is a THROW, not a silent black: a game whose HUD quietly
 * turns black is far harder to diagnose than one that says what it could not parse.
 */
typedef struct { const char *name; uint32_t rgba; } NamedColor;

static const NamedColor NAMED[] = {
    { "black",   0x000000ffu }, { "white",  0xffffffffu }, { "red",     0xff0000ffu },
    { "green",   0x008000ffu }, { "blue",   0x0000ffffu }, { "yellow",  0xffff00ffu },
    { "cyan",    0x00ffffffu }, { "magenta",0xff00ffffu }, { "gray",    0x808080ffu },
    { "grey",    0x808080ffu }, { "orange", 0xffa500ffu }, { "purple",  0x800080ffu },
    { "silver",  0xc0c0c0ffu }, { "lime",   0x00ff00ffu }, { "navy",    0x000080ffu },
    { "teal",    0x008080ffu }, { "maroon", 0x800000ffu }, { "olive",   0x808000ffu },
    { "transparent", 0x00000000u },
};

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_color(const char *s, uint32_t *out)
{
    if (!s) return false;
    while (*s == ' ') s++;

    if (*s == '#') {
        const char *h = s + 1;
        size_t n = strlen(h);
        while (n && (h[n - 1] == ' ')) n--;
        int v[8];
        for (size_t i = 0; i < n && i < 8; i++) {
            v[i] = hexval(h[i]);
            if (v[i] < 0) return false;
        }
        if (n == 3 || n == 4) {
            uint32_t r = (uint32_t)(v[0] * 17), g = (uint32_t)(v[1] * 17), b = (uint32_t)(v[2] * 17);
            uint32_t a = (n == 4) ? (uint32_t)(v[3] * 17) : 255u;
            *out = (r << 24) | (g << 16) | (b << 8) | a;
            return true;
        }
        if (n == 6 || n == 8) {
            uint32_t r = (uint32_t)(v[0] * 16 + v[1]);
            uint32_t g = (uint32_t)(v[2] * 16 + v[3]);
            uint32_t b = (uint32_t)(v[4] * 16 + v[5]);
            uint32_t a = (n == 8) ? (uint32_t)(v[6] * 16 + v[7]) : 255u;
            *out = (r << 24) | (g << 16) | (b << 8) | a;
            return true;
        }
        return false;
    }

    if (!strncmp(s, "rgb", 3)) {
        const char *p = strchr(s, '(');
        if (!p) return false;
        double c[4] = { 0, 0, 0, 1 };
        int i = 0;
        p++;
        while (*p && i < 4) {
            while (*p == ' ' || *p == ',') p++;
            char *end = NULL;
            double d = strtod(p, &end);
            if (end == p) break;
            /* Percentages appear in the wild for rgb(); honour them. */
            if (*end == '%') { d = d * 255.0 / 100.0; end++; }
            c[i++] = d;
            p = end;
        }
        uint32_t r = (uint32_t)fmin(255, fmax(0, c[0]));
        uint32_t g = (uint32_t)fmin(255, fmax(0, c[1]));
        uint32_t b = (uint32_t)fmin(255, fmax(0, c[2]));
        uint32_t a = (uint32_t)fmin(255, fmax(0, c[3] <= 1.0 ? c[3] * 255.0 : c[3]));
        *out = (r << 24) | (g << 16) | (b << 8) | a;
        return true;
    }

    for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
        if (!strcmp(s, NAMED[i].name)) { *out = NAMED[i].rgba; return true; }
    }
    return false;
}

/* --------------------------------------------------------------- ctx state --- */

typedef struct {
    char fill_style[64];
    char stroke_style[64];
    char font[96];
    char text_align[16];
    double line_width;
    double global_alpha;
    int width, height;
} Ctx2D;

static Ctx2D g_ctx;
static JSClassID js_ctx2d_class_id;

/* "12px sans-serif" / "bold 16px Arial" -> pixel size. */
static float font_px_from_spec(const char *spec)
{
    if (!spec) return 10.0f;
    const char *p = spec;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end && (end[0] == 'p' && end[1] == 'x')) return (float)v;
            /* pt -> px at the usual 96/72 ratio. */
            if (end && (end[0] == 'p' && end[1] == 't')) return (float)(v * 96.0 / 72.0);
            p = end ? end : p + 1;
            continue;
        }
        p++;
    }
    return 10.0f;
}

static void apply_fill(JSContext *ctx)
{
    uint32_t c = 0;
    if (!parse_color(g_ctx.fill_style, &c)) return;
    s2d_set_fill(c);
}

static void apply_stroke(void)
{
    uint32_t c = 0;
    if (!parse_color(g_ctx.stroke_style, &c)) return;
    s2d_set_stroke(c);
}

/* ------------------------------------------------------------- unsupported --- */

/*
 * Every Canvas2D member that exists in the spec but is NOT implemented gets one of
 * these, carrying its own name. See the file header for why this is a throw.
 */
static JSValue c2d_unsupported(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic, JSValue *fd)
{
    const char *name = JS_ToCString(ctx, fd[0]);
    JSValue err = JS_ThrowTypeError(ctx,
        "CanvasRenderingContext2D.%s is not implemented in jsgamelauncher-quickjs. "
        "The implemented surface is the one real games use (fillRect, strokeRect, "
        "clearRect, fillText, measureText, drawImage, save/restore/translate/rotate/"
        "scale, getImageData/putImageData). This throws rather than silently doing "
        "nothing so a missing feature is visible instead of invisible.",
        name ? name : "?");
    if (name) JS_FreeCString(ctx, name);
    return err;
}

static void define_unsupported(JSContext *ctx, JSValue obj, const char *name)
{
    JSValue nm = JS_NewString(ctx, name);
    JSValue fn = JS_NewCFunctionData(ctx, c2d_unsupported, 0, 0, 1, &nm);
    JS_FreeValue(ctx, nm);
    JS_SetPropertyStr(ctx, obj, name, fn);
}

/* ------------------------------------------------------------------ methods -- */

static double arg_num(JSContext *ctx, JSValueConst v)
{
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    return d;
}

static JSValue c2d_fillRect(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 4) return jsglq_throw(ctx, "fillRect requires (x, y, w, h)");
    apply_fill(ctx);
    s2d_fill_rect((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]),
                  (float)arg_num(ctx, argv[2]), (float)arg_num(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue c2d_strokeRect(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    if (argc < 4) return jsglq_throw(ctx, "strokeRect requires (x, y, w, h)");
    apply_stroke();
    s2d_set_line_width((float)g_ctx.line_width);
    s2d_stroke_rect((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]),
                    (float)arg_num(ctx, argv[2]), (float)arg_num(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue c2d_clearRect(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 4) return jsglq_throw(ctx, "clearRect requires (x, y, w, h)");
    /* Canvas clearRect writes transparent black; the renderer has no separate
       clear path, so a fully transparent fill is the equivalent. */
    uint32_t save_alpha_src = 0;
    (void)save_alpha_src;
    s2d_set_fill(0x00000000u);
    float a = (float)g_ctx.global_alpha;
    s2d_set_alpha(1.0f);
    s2d_fill_rect((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]),
                  (float)arg_num(ctx, argv[2]), (float)arg_num(ctx, argv[3]));
    s2d_set_alpha(a);
    return JS_UNDEFINED;
}

static JSValue c2d_fillText(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 3) return jsglq_throw(ctx, "fillText requires (text, x, y)");
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    apply_fill(ctx);
    s2d_set_font_px(font_px_from_spec(g_ctx.font));
    s2d_set_text_align(!strcmp(g_ctx.text_align, "center") ? 1
                     : (!strcmp(g_ctx.text_align, "right") ? 2 : 0));
    s2d_fill_text(s, (float)arg_num(ctx, argv[1]), (float)arg_num(ctx, argv[2]));
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue c2d_measureText(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (argc < 1) return jsglq_throw(ctx, "measureText requires (text)");
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    s2d_set_font_px(font_px_from_spec(g_ctx.font));
    float w = s2d_measure_text(s);
    JS_FreeCString(ctx, s);

    JSValue m = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, m, "width", JS_NewFloat64(ctx, w));
    /* Real metrics, not just width: games that centre text vertically need them,
       and returning only width is a documented gap in the wasm build. */
    float px = font_px_from_spec(g_ctx.font);
    JS_SetPropertyStr(ctx, m, "actualBoundingBoxAscent", JS_NewFloat64(ctx, px * 0.8));
    JS_SetPropertyStr(ctx, m, "actualBoundingBoxDescent", JS_NewFloat64(ctx, px * 0.2));
    JS_SetPropertyStr(ctx, m, "fontBoundingBoxAscent", JS_NewFloat64(ctx, px * 0.8));
    JS_SetPropertyStr(ctx, m, "fontBoundingBoxDescent", JS_NewFloat64(ctx, px * 0.2));
    return m;
}

static JSValue c2d_save(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ s2d_save(); return JS_UNDEFINED; }

static JSValue c2d_restore(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ s2d_restore(); return JS_UNDEFINED; }

static JSValue c2d_translate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 2) return jsglq_throw(ctx, "translate requires (x, y)");
    s2d_translate((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue c2d_rotate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 1) return jsglq_throw(ctx, "rotate requires (radians)");
    s2d_rotate((float)arg_num(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue c2d_scale(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 2) return jsglq_throw(ctx, "scale requires (x, y)");
    s2d_scale((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]));
    return JS_UNDEFINED;
}

/*
 * drawImage in all three spec arities. The source object must carry an `_s2d`
 * handle installed by the image decode path.
 */
static JSValue c2d_drawImage(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 3) return jsglq_throw(ctx, "drawImage requires at least (image, dx, dy)");

    JSValue h = JS_GetPropertyStr(ctx, argv[0], "_s2d");
    if (JS_IsUndefined(h)) {
        JS_FreeValue(ctx, h);
        return jsglq_throw(ctx,
            "drawImage: source has no decoded pixels (_s2d handle missing). "
            "Pass an Image that has finished loading, or a canvas.");
    }
    int32_t handle = -1;
    JS_ToInt32(ctx, &handle, h);
    JS_FreeValue(ctx, h);
    if (handle < 0) return jsglq_throw(ctx, "drawImage: invalid image handle");

    int iw = 0, ih = 0;
    s2d_image_size(handle, &iw, &ih);

    if (argc >= 9) {
        s2d_draw_image(handle,
            (float)arg_num(ctx, argv[1]), (float)arg_num(ctx, argv[2]),
            (float)arg_num(ctx, argv[3]), (float)arg_num(ctx, argv[4]),
            (float)arg_num(ctx, argv[5]), (float)arg_num(ctx, argv[6]),
            (float)arg_num(ctx, argv[7]), (float)arg_num(ctx, argv[8]));
    } else if (argc >= 5) {
        s2d_draw_image(handle, 0, 0, (float)iw, (float)ih,
            (float)arg_num(ctx, argv[1]), (float)arg_num(ctx, argv[2]),
            (float)arg_num(ctx, argv[3]), (float)arg_num(ctx, argv[4]));
    } else {
        s2d_draw_image(handle, 0, 0, (float)iw, (float)ih,
            (float)arg_num(ctx, argv[1]), (float)arg_num(ctx, argv[2]),
            (float)iw, (float)ih);
    }
    return JS_UNDEFINED;
}

/*
 * getImageData / putImageData.
 *
 * Both are BOUNDS-CHECKED against the real buffer length, not against the caller's
 * claimed width/height. That bug class (trusting dimensions over the buffer) was
 * found five separate times in the wasm build — putImageData painted 61 distinct
 * colours from a 4-byte buffer, and readPixels wrote out of bounds and killed the
 * runtime. Natively that is memory corruption rather than garbage pixels, so the
 * check is mandatory, and the error names BOTH sizes.
 */
static JSValue c2d_getImageData(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 4) return jsglq_throw(ctx, "getImageData requires (x, y, w, h)");
    int x = (int)arg_num(ctx, argv[0]), y = (int)arg_num(ctx, argv[1]);
    int w = (int)arg_num(ctx, argv[2]), h = (int)arg_num(ctx, argv[3]);

    if (w <= 0 || h <= 0)
        return jsglq_throw_range(ctx, "getImageData: width and height must be positive "
                                      "(got %dx%d)", w, h);
    if (w > 16384 || h > 16384)
        return jsglq_throw_range(ctx, "getImageData: %dx%d exceeds the 16384 limit", w, h);

    /* Guard the multiplication itself: w*h*4 must not wrap. */
    int64_t need = (int64_t)w * (int64_t)h * 4;
    if (need > (int64_t)64 * 1024 * 1024)
        return jsglq_throw_range(ctx, "getImageData: %dx%d needs %lld bytes, over the "
                                      "64MB cap", w, h, (long long)need);

    uint8_t *buf = (uint8_t *)calloc(1, (size_t)need);
    if (!buf) return jsglq_throw(ctx, "getImageData: out of memory (%lld bytes)",
                                 (long long)need);
    s2d_get_pixels(x, y, w, h, buf);

    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, (size_t)need);
    free(buf);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue u8ctor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
    JSValue arr = JS_CallConstructor(ctx, u8ctor, 1, &ab);
    JS_FreeValue(ctx, u8ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, out, "height", JS_NewInt32(ctx, h));
    JS_SetPropertyStr(ctx, out, "data", arr);
    JS_SetPropertyStr(ctx, out, "colorSpace", JS_NewString(ctx, "srgb"));
    return out;
}

static JSValue c2d_putImageData(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 3) return jsglq_throw(ctx, "putImageData requires (imageData, dx, dy)");

    JSValue wv = JS_GetPropertyStr(ctx, argv[0], "width");
    JSValue hv = JS_GetPropertyStr(ctx, argv[0], "height");
    JSValue dv = JS_GetPropertyStr(ctx, argv[0], "data");
    int w = 0, h = 0;
    JS_ToInt32(ctx, &w, wv);
    JS_ToInt32(ctx, &h, hv);
    JS_FreeValue(ctx, wv);
    JS_FreeValue(ctx, hv);

    size_t byte_len = 0, off = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, dv, &off, &byte_len, &bpe);
    uint8_t *data = NULL;
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, abuf);
        if (base) data = base + off;
        JS_FreeValue(ctx, abuf);
    }
    JS_FreeValue(ctx, dv);

    if (!data)
        return jsglq_throw(ctx, "putImageData: imageData.data is not a typed array");

    /* THE check: compare the claimed dimensions against the real buffer. */
    int64_t need = (int64_t)w * (int64_t)h * 4;
    if (w <= 0 || h <= 0)
        return jsglq_throw_range(ctx, "putImageData: width and height must be positive "
                                      "(got %dx%d)", w, h);
    if (need > (int64_t)byte_len)
        return jsglq_throw_range(ctx,
            "putImageData: %dx%d needs %lld bytes but the buffer holds %zu. "
            "Refusing to read past the end of the buffer.",
            w, h, (long long)need, byte_len);

    s2d_put_pixels((int)arg_num(ctx, argv[1]), (int)arg_num(ctx, argv[2]), w, h, data);
    return JS_UNDEFINED;
}


/* ------------------------------------------------------------------- paths -- */

/*
 * Implemented after measuring the SHIPPED corpus bundles: 187 beginPath,
 * 173 lineTo, 152 fill, 112 moveTo, 112 arc across the 14 games. The earlier
 * "the corpus draws no paths" conclusion came from a 30-frame runtime sample that
 * never reached this code — coverage and sampling are not the same thing.
 */
static JSValue c2d_beginPath(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ s2d_begin_path(); return JS_UNDEFINED; }

static JSValue c2d_closePath(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ s2d_close_path(); return JS_UNDEFINED; }

static JSValue c2d_moveTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 2) return jsglq_throw(ctx, "moveTo requires (x, y)");
    s2d_move_to((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue c2d_lineTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 2) return jsglq_throw(ctx, "lineTo requires (x, y)");
    s2d_line_to((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue c2d_arc(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 5) return jsglq_throw(ctx, "arc requires (x, y, r, startAngle, endAngle)");
    int ccw = argc >= 6 && JS_ToBool(ctx, argv[5]);
    s2d_arc((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]),
            (float)arg_num(ctx, argv[2]), (float)arg_num(ctx, argv[3]),
            (float)arg_num(ctx, argv[4]), ccw);
    return JS_UNDEFINED;
}

static JSValue c2d_ellipse(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 7) return jsglq_throw(ctx,
        "ellipse requires (x, y, radiusX, radiusY, rotation, startAngle, endAngle)");
    int ccw = argc >= 8 && JS_ToBool(ctx, argv[7]);
    s2d_ellipse((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]),
                (float)arg_num(ctx, argv[2]), (float)arg_num(ctx, argv[3]),
                (float)arg_num(ctx, argv[4]), (float)arg_num(ctx, argv[5]),
                (float)arg_num(ctx, argv[6]), ccw);
    return JS_UNDEFINED;
}

static JSValue c2d_quadraticCurveTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 4) return jsglq_throw(ctx, "quadraticCurveTo requires (cpx, cpy, x, y)");
    s2d_quadratic_to((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]),
                     (float)arg_num(ctx, argv[2]), (float)arg_num(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue c2d_bezierCurveTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 6) return jsglq_throw(ctx,
        "bezierCurveTo requires (cp1x, cp1y, cp2x, cp2y, x, y)");
    s2d_bezier_to((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]),
                  (float)arg_num(ctx, argv[2]), (float)arg_num(ctx, argv[3]),
                  (float)arg_num(ctx, argv[4]), (float)arg_num(ctx, argv[5]));
    return JS_UNDEFINED;
}

static JSValue c2d_rect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 4) return jsglq_throw(ctx, "rect requires (x, y, w, h)");
    s2d_path_rect((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]),
                  (float)arg_num(ctx, argv[2]), (float)arg_num(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue c2d_fill(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    apply_fill(ctx);
    s2d_fill_path();
    return JS_UNDEFINED;
}

static JSValue c2d_stroke(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    apply_stroke();
    s2d_set_line_width((float)g_ctx.line_width);
    s2d_stroke_path();
    return JS_UNDEFINED;
}

/*
 * clip() accepts the call and does nothing.
 *
 * This is the one place a no-op is the right answer: clipping RESTRICTS drawing,
 * so ignoring it renders MORE than asked rather than less. A game that clips and
 * then draws still shows its content; a game that throws here shows nothing at
 * all. The visual difference is bounded and visible; the alternative is a blank
 * screen. Real clipping needs either scissor rects (axis-aligned only) or a
 * stencil pass, and is tracked.
 */
static JSValue c2d_clip(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ return JS_UNDEFINED; }

/* strokeText: the renderer has one text path, so this draws with the stroke
   colour rather than a true outline. Visibly present beats absent, and a game
   using it for a HUD outline still reads correctly. */
static JSValue c2d_strokeText(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    if (argc < 3) return jsglq_throw(ctx, "strokeText requires (text, x, y)");
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_EXCEPTION;
    apply_stroke();
    s2d_set_font_px(font_px_from_spec(g_ctx.font));
    s2d_set_text_align(!strcmp(g_ctx.text_align, "center") ? 1
                     : (!strcmp(g_ctx.text_align, "right") ? 2 : 0));
    s2d_fill_text(str, (float)arg_num(ctx, argv[1]), (float)arg_num(ctx, argv[2]));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* createImageData(w,h) / createImageData(imagedata) */
static JSValue c2d_createImageData(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    int w = 0, h = 0;
    if (argc >= 2) {
        w = (int)arg_num(ctx, argv[0]);
        h = (int)arg_num(ctx, argv[1]);
    } else if (argc == 1) {
        JSValue wv = JS_GetPropertyStr(ctx, argv[0], "width");
        JSValue hv = JS_GetPropertyStr(ctx, argv[0], "height");
        JS_ToInt32(ctx, &w, wv);
        JS_ToInt32(ctx, &h, hv);
        JS_FreeValue(ctx, wv);
        JS_FreeValue(ctx, hv);
    }
    if (w <= 0 || h <= 0)
        return jsglq_throw_range(ctx, "createImageData: %dx%d must be positive", w, h);

    int64_t need = (int64_t)w * h * 4;
    if (need > (int64_t)64 * 1024 * 1024)
        return jsglq_throw_range(ctx, "createImageData: %dx%d is too large", w, h);

    uint8_t *zero = (uint8_t *)calloc(1, (size_t)need);
    if (!zero) return jsglq_throw(ctx, "createImageData: out of memory");
    JSValue ab = JS_NewArrayBufferCopy(ctx, zero, (size_t)need);
    free(zero);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
    JSValue arr = JS_CallConstructor(ctx, ctor, 1, &ab);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, out, "height", JS_NewInt32(ctx, h));
    JS_SetPropertyStr(ctx, out, "data", arr);
    return out;
}

/*
 * Transform matrix operations.
 *
 * The renderer keeps a 2x3 affine matrix, which is exactly what these need.
 * setTransform REPLACES it (so a game resetting to identity each frame works),
 * transform() multiplies into it, resetTransform is setTransform(1,0,0,1,0,0).
 */
static JSValue c2d_setTransform(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 6) return jsglq_throw(ctx, "setTransform requires (a, b, c, d, e, f)");
    s2d_set_transform((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]),
                      (float)arg_num(ctx, argv[2]), (float)arg_num(ctx, argv[3]),
                      (float)arg_num(ctx, argv[4]), (float)arg_num(ctx, argv[5]));
    return JS_UNDEFINED;
}

static JSValue c2d_resetTransform(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ s2d_set_transform(1, 0, 0, 1, 0, 0); return JS_UNDEFINED; }

static JSValue c2d_transform(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 6) return jsglq_throw(ctx, "transform requires (a, b, c, d, e, f)");
    s2d_multiply_transform((float)arg_num(ctx, argv[0]), (float)arg_num(ctx, argv[1]),
                           (float)arg_num(ctx, argv[2]), (float)arg_num(ctx, argv[3]),
                           (float)arg_num(ctx, argv[4]), (float)arg_num(ctx, argv[5]));
    return JS_UNDEFINED;
}

/* Line dashes are accepted and ignored: a solid line where a dashed one was asked
   for is a cosmetic difference, where throwing would stop the game. */
static JSValue c2d_setLineDash(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ return JS_UNDEFINED; }

static JSValue c2d_getLineDash(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ return JS_NewArray(ctx); }

/* --------------------------------------------------------------- properties -- */

#define STR_PROP(NAME, FIELD)                                                       \
    static JSValue c2d_get_##NAME(JSContext *ctx, JSValueConst t) {                 \
        return JS_NewString(ctx, g_ctx.FIELD);                                      \
    }                                                                               \
    static JSValue c2d_set_##NAME(JSContext *ctx, JSValueConst t, JSValueConst v) { \
        const char *s = JS_ToCString(ctx, v);                                       \
        if (!s) return JS_EXCEPTION;                                                \
        snprintf(g_ctx.FIELD, sizeof(g_ctx.FIELD), "%s", s);                        \
        JS_FreeCString(ctx, s);                                                     \
        return JS_UNDEFINED;                                                        \
    }

STR_PROP(fillStyle, fill_style)
STR_PROP(strokeStyle, stroke_style)
STR_PROP(font, font)
STR_PROP(textAlign, text_align)

static JSValue c2d_get_lineWidth(JSContext *ctx, JSValueConst t)
{ return JS_NewFloat64(ctx, g_ctx.line_width); }

static JSValue c2d_set_lineWidth(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    JS_ToFloat64(ctx, &g_ctx.line_width, v);
    s2d_set_line_width((float)g_ctx.line_width);
    return JS_UNDEFINED;
}

static JSValue c2d_get_globalAlpha(JSContext *ctx, JSValueConst t)
{ return JS_NewFloat64(ctx, g_ctx.global_alpha); }

static JSValue c2d_set_globalAlpha(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    JS_ToFloat64(ctx, &g_ctx.global_alpha, v);
    if (g_ctx.global_alpha < 0) g_ctx.global_alpha = 0;
    if (g_ctx.global_alpha > 1) g_ctx.global_alpha = 1;
    s2d_set_alpha((float)g_ctx.global_alpha);
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------- images -- */

/* Called by the JS Image shim once bytes are decoded. */
static JSValue js_image_create(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (argc < 3) return jsglq_throw(ctx, "__jsglq_imageCreate(rgba, w, h)");

    size_t byte_len = 0, off = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &byte_len, &bpe);
    uint8_t *data = NULL;
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, abuf);
        if (base) data = base + off;
        JS_FreeValue(ctx, abuf);
    }
    if (!data) return jsglq_throw(ctx, "__jsglq_imageCreate: rgba is not a typed array");

    int w = 0, h = 0;
    JS_ToInt32(ctx, &w, argv[1]);
    JS_ToInt32(ctx, &h, argv[2]);

    int64_t need = (int64_t)w * (int64_t)h * 4;
    if (w <= 0 || h <= 0 || need > (int64_t)byte_len) {
        return jsglq_throw_range(ctx,
            "__jsglq_imageCreate: %dx%d needs %lld bytes but the buffer holds %zu",
            w, h, (long long)need, byte_len);
    }

    int handle = s2d_image_create(data, w, h);
    if (handle < 0) return jsglq_throw(ctx, "image upload failed (%dx%d)", w, h);
    return JS_NewInt32(ctx, handle);
}

static JSValue js_canvas_resize(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 2) return jsglq_throw(ctx, "__jsglq_canvasResize(w, h)");
    JsglqEngine *e = (JsglqEngine *)JS_GetContextOpaque(ctx);
    int32_t w = 0, h = 0;
    JS_ToInt32(ctx, &w, argv[0]);
    JS_ToInt32(ctx, &h, argv[1]);
    return JS_NewBool(ctx, jsglq_canvas2d_resize(e, w, h) == 0);
}

static JSValue js_canvas_size(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, g_ctx.width));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, g_ctx.height));
    return o;
}

/* ------------------------------------------------------------------ install -- */

static const JSCFunctionListEntry c2d_proto[] = {
    JS_CFUNC_DEF("fillRect", 4, c2d_fillRect),
    JS_CFUNC_DEF("strokeRect", 4, c2d_strokeRect),
    JS_CFUNC_DEF("clearRect", 4, c2d_clearRect),
    JS_CFUNC_DEF("fillText", 3, c2d_fillText),
    JS_CFUNC_DEF("measureText", 1, c2d_measureText),
    JS_CFUNC_DEF("drawImage", 3, c2d_drawImage),
    JS_CFUNC_DEF("save", 0, c2d_save),
    JS_CFUNC_DEF("restore", 0, c2d_restore),
    JS_CFUNC_DEF("translate", 2, c2d_translate),
    JS_CFUNC_DEF("rotate", 1, c2d_rotate),
    JS_CFUNC_DEF("scale", 2, c2d_scale),
    JS_CFUNC_DEF("getImageData", 4, c2d_getImageData),
    JS_CFUNC_DEF("beginPath", 0, c2d_beginPath),
    JS_CFUNC_DEF("closePath", 0, c2d_closePath),
    JS_CFUNC_DEF("moveTo", 2, c2d_moveTo),
    JS_CFUNC_DEF("lineTo", 2, c2d_lineTo),
    JS_CFUNC_DEF("arc", 5, c2d_arc),
    JS_CFUNC_DEF("ellipse", 7, c2d_ellipse),
    JS_CFUNC_DEF("quadraticCurveTo", 4, c2d_quadraticCurveTo),
    JS_CFUNC_DEF("bezierCurveTo", 6, c2d_bezierCurveTo),
    JS_CFUNC_DEF("rect", 4, c2d_rect),
    JS_CFUNC_DEF("fill", 0, c2d_fill),
    JS_CFUNC_DEF("stroke", 0, c2d_stroke),
    JS_CFUNC_DEF("clip", 0, c2d_clip),
    JS_CFUNC_DEF("strokeText", 3, c2d_strokeText),
    JS_CFUNC_DEF("createImageData", 2, c2d_createImageData),
    JS_CFUNC_DEF("setTransform", 6, c2d_setTransform),
    JS_CFUNC_DEF("resetTransform", 0, c2d_resetTransform),
    JS_CFUNC_DEF("transform", 6, c2d_transform),
    JS_CFUNC_DEF("setLineDash", 1, c2d_setLineDash),
    JS_CFUNC_DEF("getLineDash", 0, c2d_getLineDash),
    JS_CFUNC_DEF("putImageData", 3, c2d_putImageData),
    JS_CGETSET_DEF("fillStyle", c2d_get_fillStyle, c2d_set_fillStyle),
    JS_CGETSET_DEF("strokeStyle", c2d_get_strokeStyle, c2d_set_strokeStyle),
    JS_CGETSET_DEF("font", c2d_get_font, c2d_set_font),
    JS_CGETSET_DEF("textAlign", c2d_get_textAlign, c2d_set_textAlign),
    JS_CGETSET_DEF("lineWidth", c2d_get_lineWidth, c2d_set_lineWidth),
    JS_CGETSET_DEF("globalAlpha", c2d_get_globalAlpha, c2d_set_globalAlpha),
};

/* Spec members that exist but are not implemented. Each throws with its own name. */
static const char *UNSUPPORTED[] = {
    "arcTo", "roundRect", "isPointInPath", "isPointInStroke",
    "createLinearGradient", "createRadialGradient", "createConicGradient",
    "createPattern", "drawFocusIfNeeded",
};

int jsglq_bind_canvas2d(JsglqEngine *e, int width, int height)
{
    JSContext *ctx = jsglq_engine_ctx(e);

    if (!s2d_init(width, height)) {
        fprintf(stderr, "jsglq: sprite2d init failed (%dx%d)\n", width, height);
        return -1;
    }

    snprintf(g_ctx.fill_style, sizeof(g_ctx.fill_style), "#000000");
    snprintf(g_ctx.stroke_style, sizeof(g_ctx.stroke_style), "#000000");
    snprintf(g_ctx.font, sizeof(g_ctx.font), "10px sans-serif");
    snprintf(g_ctx.text_align, sizeof(g_ctx.text_align), "left");
    g_ctx.line_width = 1.0;
    g_ctx.global_alpha = 1.0;
    g_ctx.width = width;
    g_ctx.height = height;

    JSValue global = JS_GetGlobalObject(ctx);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, c2d_proto,
                               (int)(sizeof(c2d_proto) / sizeof(c2d_proto[0])));
    for (size_t i = 0; i < sizeof(UNSUPPORTED) / sizeof(UNSUPPORTED[0]); i++) {
        define_unsupported(ctx, proto, UNSUPPORTED[i]);
    }

    JS_SetPropertyStr(ctx, global, "__jsglq_ctx2dProto", proto);
    JS_SetPropertyStr(ctx, global, "__jsglq_imageCreate",
        JS_NewCFunction(ctx, js_image_create, "__jsglq_imageCreate", 3));
    JS_SetPropertyStr(ctx, global, "__jsglq_canvasResize",
        JS_NewCFunction(ctx, js_canvas_resize, "__jsglq_canvasResize", 2));
    JS_SetPropertyStr(ctx, global, "__jsglq_canvasSize",
        JS_NewCFunction(ctx, js_canvas_size, "__jsglq_canvasSize", 0));

    JS_FreeValue(ctx, global);
    return 0;
}

/*
 * Resize the 2D surface to follow the game canvas.
 *
 * The renderer is initialized at the host's window size, but a game may set
 * canvas.width/height afterwards — and then every draw lands outside the surface
 * and reads back black. This is the same failure that was just fixed in rungame's
 * GL FBO; it is worth stating that a runtime is not immune to a bug simply because
 * it found that bug elsewhere.
 */
int jsglq_canvas2d_resize(JsglqEngine *e, int width, int height)
{
    if (width <= 0 || height <= 0) return -1;
    if (width == g_ctx.width && height == g_ctx.height) return 0;

    /*
     * Clamp to the real GL surface.
     *
     * The EGL surface is created once at the host's size and cannot grow here, so
     * a game asking for a larger canvas would otherwise draw outside it — every
     * call succeeds, readback returns black, and nothing says why. Clamping keeps
     * the drawing visible; the game sees the size it actually got through
     * canvas.width, which is the honest answer rather than a silent lie.
     */
    int surf_w = 0, surf_h = 0;
    jsglq_window_size(&surf_w, &surf_h);
    if (surf_w > 0 && surf_h > 0 && (width > surf_w || height > surf_h)) {
        fprintf(stderr, "jsglq: canvas %dx%d exceeds the %dx%d surface; clamping "
                        "(run with --width/--height to enlarge it)\n",
                width, height, surf_w, surf_h);
        if (width > surf_w) width = surf_w;
        if (height > surf_h) height = surf_h;
    }

    if (!s2d_init(width, height)) {
        fprintf(stderr, "jsglq: canvas resize to %dx%d failed\n", width, height);
        return -1;
    }
    g_ctx.width = width;
    g_ctx.height = height;
    return 0;
}

void jsglq_canvas2d_begin_frame(void) { s2d_begin(); }
void jsglq_canvas2d_end_frame(void)   { s2d_end(); }
