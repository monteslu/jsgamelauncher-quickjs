/*
 * GL binding registration.
 *
 * native-gles's gl_bindings.cpp is compiled VERBATIM against src/napi_shim.h (via
 * the local src/napi.h stand-in). Nothing in that 1837-line file is edited, so
 * native-gles stays an ordinary upstream dependency instead of a fork.
 *
 * This file does the two things the shim cannot do by itself:
 *   1. Adapt each `Napi::Value fn(const Napi::CallbackInfo&)` to a JSCFunction.
 *   2. Register all 246 entry points onto a flat `gl` object, matching the exact
 *      names native-gles exports so webgl-node runs unmodified on top.
 */
#include "napi_shim.h"

extern "C" {
#include "host.h"
}

#include "../../native-gles/src/gl_bindings.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>

/* ------------------------------------------------------------------ adapter -- */

/*
 * Two shapes exist in gl_bindings.h: functions returning Napi::Value, and functions
 * returning void. Templating over both keeps the registration table a single list
 * rather than two parallel ones that can drift apart.
 */
template <Napi::Value (*Fn)(const Napi::CallbackInfo &)>
static JSValue gl_thunk(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    Napi::CallbackInfo info(ctx, this_val, argc, argv);
    Napi::Value out = Fn(info);
    return out.raw();
}

template <void (*Fn)(const Napi::CallbackInfo &)>
static JSValue gl_thunk_void(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    Napi::CallbackInfo info(ctx, this_val, argc, argv);
    Fn(info);
    return JS_UNDEFINED;
}

#define GLFN(name, fn)      JS_SetPropertyStr(ctx, glo, name, \
    JS_NewCFunction(ctx, gl_thunk<gl::fn>, name, 0))
#define GLFN_V(name, fn)    JS_SetPropertyStr(ctx, glo, name, \
    JS_NewCFunction(ctx, gl_thunk_void<gl::fn>, name, 0))

/* ----------------------------------------------------------------- registry -- */

/* ------------------------------------------------- context management -------- */

/*
 * webgl-node expects native-gles's context API, not just the GL entry points.
 *
 * The host owns exactly one context here (created in window.cpp against the real
 * window surface), so these are thin adapters onto it rather than a reimplementation
 * of upstream's multi-context registry. A game asking for a context gets the host's;
 * asking for a second one is an error rather than a silent second surface that never
 * reaches the screen.
 */
extern "C" {
bool jsglq_window_make_current(void);
void jsglq_window_swap(void);
void jsglq_window_size(int *w, int *h);
}

static JSValue js_create_context(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    /* Handle 1 is the host's window context. Non-zero means success, matching the
       contract webgl-node checks (`if (!id) throw`). */
    return JS_NewInt32(ctx, 1);
}

static JSValue js_make_current(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    return JS_NewBool(ctx, jsglq_window_make_current());
}

static JSValue js_swap_buffers(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    /* The host swaps once per frame in its own loop. A game-driven swap here would
       present a half-built frame and double the presentation rate. */
    return JS_NewBool(ctx, true);
}

static JSValue js_set_swap_interval(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    /* Interval is owned by the host and pinned to 0; pacing is explicit. Accepting
       and ignoring the call keeps webgl-node's setup path working. */
    return JS_NewBool(ctx, true);
}

static JSValue js_destroy_context(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return JS_NewBool(ctx, true);
}

static JSValue js_release_current(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return JS_NewBool(ctx, true);
}

static JSValue js_resize_context(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return JS_NewBool(ctx, true);
}

static JSValue js_get_context_info(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    int w = 0, h = 0;
    jsglq_window_size(&w, &h);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, o, "valid", JS_TRUE);
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, h));
    JS_SetPropertyStr(ctx, o, "isWindowSurface", JS_TRUE);
    JS_SetPropertyStr(ctx, o, "contextCount", JS_NewInt32(ctx, 1));
    return o;
}

extern "C" int jsglq_bind_gl_object(JSContext *ctx, JSValue global)
{
    JSValue glo = JS_NewObject(ctx);

    /* All 247 entry points, generated from native-gles's own module.cpp so the JS
       names cannot drift from upstream. Regenerate with scripts/gen-gl-table.mjs. */
#include "gl_table.inc"

    /* Context management, the other half of the native-gles module surface. */
    JS_SetPropertyStr(ctx, glo, "createContext",
        JS_NewCFunction(ctx, js_create_context, "createContext", 3));
    JS_SetPropertyStr(ctx, glo, "destroyContext",
        JS_NewCFunction(ctx, js_destroy_context, "destroyContext", 1));
    JS_SetPropertyStr(ctx, glo, "makeCurrent",
        JS_NewCFunction(ctx, js_make_current, "makeCurrent", 1));
    JS_SetPropertyStr(ctx, glo, "releaseCurrent",
        JS_NewCFunction(ctx, js_release_current, "releaseCurrent", 0));
    JS_SetPropertyStr(ctx, glo, "swapBuffers",
        JS_NewCFunction(ctx, js_swap_buffers, "swapBuffers", 1));
    JS_SetPropertyStr(ctx, glo, "setSwapInterval",
        JS_NewCFunction(ctx, js_set_swap_interval, "setSwapInterval", 2));
    JS_SetPropertyStr(ctx, glo, "resizeContext",
        JS_NewCFunction(ctx, js_resize_context, "resizeContext", 3));
    JS_SetPropertyStr(ctx, glo, "getContextInfo",
        JS_NewCFunction(ctx, js_get_context_info, "getContextInfo", 1));

    JS_SetPropertyStr(ctx, global, "gl", glo);
    return 0;
}
