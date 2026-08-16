/*
 * Asset IO: the backing store for fetch(), XHR, and Image.
 *
 * Everything is rooted at the game directory (see resolve_in_root in engine.c) and
 * resolved through realpath, so a symlink cannot launder a ../ escape. There is no
 * general filesystem API exposed to the realm — a game can read its own assets and
 * nothing else.
 *
 * Synchronous for now. Reads come off local disk during load, and the frame loop's
 * microtask budget bounds their effect on pacing. A thread pool lands with the
 * decode work in phase 2, where it actually matters.
 */
#include "host.h"

#include <stdlib.h>
#include <string.h>

/* Read a file into an ArrayBuffer. Returns null when missing so the JS layer can
   turn it into a proper 404-shaped Response rather than an exception. */
static JSValue js_read_asset(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JsglqEngine *e = (JsglqEngine *)JS_GetContextOpaque(ctx);
    if (argc < 1) return jsglq_throw(ctx, "readAsset(path) requires a path");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    size_t len = 0;
    uint8_t *data = jsglq_asset_read(e, path, &len);
    JS_FreeCString(ctx, path);

    if (!data) return JS_NULL;

    JSValue buf = JS_NewArrayBufferCopy(ctx, data, len);
    free(data);
    return buf;
}

static JSValue js_read_asset_text(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JsglqEngine *e = (JsglqEngine *)JS_GetContextOpaque(ctx);
    if (argc < 1) return jsglq_throw(ctx, "readAssetText(path) requires a path");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    size_t len = 0;
    uint8_t *data = jsglq_asset_read(e, path, &len);
    JS_FreeCString(ctx, path);

    if (!data) return JS_NULL;

    JSValue s = JS_NewStringLen(ctx, (const char *)data, len);
    free(data);
    return s;
}

static JSValue js_asset_exists(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JsglqEngine *e = (JsglqEngine *)JS_GetContextOpaque(ctx);
    if (argc < 1) return JS_FALSE;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    bool ok = jsglq_asset_exists(e, path);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ok);
}

/*
 * Write a file inside the game directory. Used by localStorage so save data
 * survives a restart.
 *
 * The path is deliberately NOT caller-controlled in the general sense: it goes
 * through the same game-dir root check as reads, so a game can persist its own
 * save file and nothing else.
 */
static JSValue js_write_asset(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JsglqEngine *e = (JsglqEngine *)JS_GetContextOpaque(ctx);
    if (argc < 2) return jsglq_throw(ctx, "writeAsset(path, text) requires both");

    const char *rel = JS_ToCString(ctx, argv[0]);
    if (!rel) return JS_EXCEPTION;
    size_t len = 0;
    const char *text = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!text) { JS_FreeCString(ctx, rel); return JS_EXCEPTION; }

    bool ok = jsglq_asset_write(e, rel, (const uint8_t *)text, len);
    JS_FreeCString(ctx, rel);
    JS_FreeCString(ctx, text);
    return JS_NewBool(ctx, ok);
}

int jsglq_bind_io(JsglqEngine *e)
{
    JSContext *ctx = jsglq_engine_ctx(e);
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue io = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, io, "readAsset",
        JS_NewCFunction(ctx, js_read_asset, "readAsset", 1));
    JS_SetPropertyStr(ctx, io, "readAssetText",
        JS_NewCFunction(ctx, js_read_asset_text, "readAssetText", 1));
    JS_SetPropertyStr(ctx, io, "exists",
        JS_NewCFunction(ctx, js_asset_exists, "exists", 1));
    JS_SetPropertyStr(ctx, io, "writeAsset",
        JS_NewCFunction(ctx, js_write_asset, "writeAsset", 2));
    JS_SetPropertyStr(ctx, global, "__jsglq_io", io);

    JS_FreeValue(ctx, global);
    return 0;
}
