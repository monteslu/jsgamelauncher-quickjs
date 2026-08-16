/*
 * Image decoding via stb_image.
 *
 * Decodes PNG/JPEG/GIF/BMP/TGA from a byte buffer to RGBA8 and uploads to the
 * renderer, returning both a GL handle (for the display canvas) and the CPU pixels
 * (for offscreen canvases and any game that reads them back).
 *
 * stb_image over SkCodec is the choice the wasmcart work landed on for WASM, and it
 * holds here for a different reason: it is 3 files with no dependency graph, and the
 * corpus only ships PNG and JPEG. Note that decoding runs on UNTRUSTED bytes, so
 * this path is a fuzz target in the test plan — natively a bad decode is memory
 * corruption, not a garbled sprite.
 */
#include "host.h"

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#include "stb_image.h"

#include "sprite2d.h"

/*
 * __jsglq_decodeImage(bytes) -> { width, height, rgba, handle } | throws
 *
 * Throws with the decoder's own reason rather than returning null: a game that
 * silently gets no image draws nothing and looks like a rendering bug, which is
 * exactly the class of failure this project keeps refusing to ship.
 */
static JSValue js_decode_image(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (argc < 1) return jsglq_throw(ctx, "__jsglq_decodeImage(bytes) requires a buffer");

    size_t len = 0;
    uint8_t *data = NULL;

    /* Accept ArrayBuffer or any view over one. */
    data = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!data) {
        /* JS_GetException RETURNS the exception value; discarding it without
           JS_FreeValue leaks an object per failed probe, and this probe fails on
           every typed array. That leak is what made JS_FreeRuntime assert. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t off = 0, blen = 0, bpe = 0;
        JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
        if (JS_IsException(buf)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return jsglq_throw(ctx, "__jsglq_decodeImage: argument is not an "
                                    "ArrayBuffer or typed array");
        }
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
        JS_FreeValue(ctx, buf);
        if (!base) return jsglq_throw(ctx, "__jsglq_decodeImage: cannot access buffer");
        data = base + off;
        len = blen;
    }

    if (len == 0) return jsglq_throw(ctx, "__jsglq_decodeImage: empty buffer");

    int w = 0, h = 0, comp = 0;
    /* Force 4 components: the renderer and every downstream path expect RGBA8. */
    unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!px) {
        return jsglq_throw(ctx, "image decode failed (%zu bytes): %s",
                           len, stbi_failure_reason() ? stbi_failure_reason() : "unknown format");
    }

    if (w <= 0 || h <= 0 || (int64_t)w * h > (int64_t)64 * 1024 * 1024) {
        stbi_image_free(px);
        return jsglq_throw_range(ctx, "image dimensions out of range: %dx%d", w, h);
    }

    const size_t nbytes = (size_t)w * (size_t)h * 4u;
    int handle = s2d_image_create(px, w, h);

    JSValue rgba = JS_NewArrayBufferCopy(ctx, px, nbytes);
    stbi_image_free(px);

    if (handle < 0) {
        JS_FreeValue(ctx, rgba);
        return jsglq_throw(ctx, "image upload failed (%dx%d)", w, h);
    }

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, out, "height", JS_NewInt32(ctx, h));
    JS_SetPropertyStr(ctx, out, "handle", JS_NewInt32(ctx, handle));
    JS_SetPropertyStr(ctx, out, "rgba", rgba);
    return out;
}

int jsglq_bind_image(JsglqEngine *e)
{
    JSContext *ctx = jsglq_engine_ctx(e);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__jsglq_decodeImage",
        JS_NewCFunction(ctx, js_decode_image, "__jsglq_decodeImage", 1));
    JS_FreeValue(ctx, global);
    return 0;
}
