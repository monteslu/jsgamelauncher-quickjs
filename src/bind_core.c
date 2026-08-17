/*
 * Core browser globals: console, performance, timers, requestAnimationFrame.
 *
 * Two deliberate departures from rungame, both of which are bug fixes rather than
 * design choices:
 *
 *  1. rAF is a LIST, not a single slot. rungame's requestAnimationFrame overwrites
 *     `currentRafCallback`, so a second registration in the same frame silently
 *     discards the first — a library and a game cannot both animate. The browser
 *     fires every callback registered before the frame, in order, and that is the
 *     spec this implements.
 *
 *  2. Callbacks registered DURING a frame run on the NEXT frame, never this one.
 *     The browser snapshots the callback list before dispatch; without that, the
 *     usual `function frame(){ ...; requestAnimationFrame(frame); }` pattern spins
 *     forever inside a single tick.
 */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RAF     256
#define MAX_TIMERS  512

typedef struct {
    JSValue fn;
    uint32_t id;
    bool active;
} RafEntry;

typedef struct {
    JSValue fn;
    double due_ms;
    double interval_ms;   /* 0 for setTimeout */
    uint32_t id;
    bool active;
    bool repeating;
} TimerEntry;

typedef struct {
    RafEntry raf[MAX_RAF];
    RafEntry raf_pending[MAX_RAF];   /* registered during dispatch -> next frame */
    int raf_count;
    int raf_pending_count;
    bool dispatching;

    TimerEntry timers[MAX_TIMERS];
    uint32_t next_id;
    double frame_time_ms;            /* the ONE clock the realm sees */
} CoreState;

static CoreState g_core;

/* ------------------------------------------------------------------ console -- */

static JSValue js_console_write(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    FILE *out = magic ? stderr : stdout;
    for (int i = 0; i < argc; i++) {
        if (i) fputc(' ', out);
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) { fputs(s, out); JS_FreeCString(ctx, s); }
        else   { fputs("<unprintable>", out); }
    }
    fputc('\n', out);
    fflush(out);   /* the runner parses stdout live; a buffered result never arrives */
    return JS_UNDEFINED;
}

/* -------------------------------------------------------------- performance -- */

static JSValue js_performance_now(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return JS_NewFloat64(ctx, jsglq_now_ms());
}

static JSValue js_date_now_shim(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    /* Verified natively rather than assumed: the WASM build of QuickJS returned 0
       from the Date.now builtin and had to be overridden. Native builds are fine,
       but the regression test for it lives in test/conformance. */
    return JS_NewFloat64(ctx, jsglq_now_ms());
}

/* ------------------------------------------------------------------- timers -- */

static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int repeating)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return jsglq_throw(ctx, "setTimeout/setInterval requires a function");

    double delay = 0;
    if (argc >= 2) JS_ToFloat64(ctx, &delay, argv[1]);
    if (delay < 0) delay = 0;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_core.timers[i].active) continue;
        g_core.timers[i].fn = JS_DupValue(ctx, argv[0]);
        g_core.timers[i].due_ms = jsglq_now_ms() + delay;
        g_core.timers[i].interval_ms = repeating ? (delay > 0 ? delay : 1) : 0;
        g_core.timers[i].repeating = repeating != 0;
        g_core.timers[i].id = ++g_core.next_id;
        g_core.timers[i].active = true;
        return JS_NewUint32(ctx, g_core.timers[i].id);
    }
    return jsglq_throw(ctx, "timer table full (%d): a game leaking timers, or a "
                            "runaway setInterval", MAX_TIMERS);
}

static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    uint32_t id = 0;
    JS_ToUint32(ctx, &id, argv[0]);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_core.timers[i].active && g_core.timers[i].id == id) {
            JS_FreeValue(ctx, g_core.timers[i].fn);
            g_core.timers[i].active = false;
            g_core.timers[i].fn = JS_UNDEFINED;
            break;
        }
    }
    return JS_UNDEFINED;
}

static int jsglq_core_pump_timers_once(JSContext *ctx);

/*
 * Fire due timers, then keep firing any that came due DURING this pump, within a
 * time budget.
 *
 * A single pass per frame means a chain of `setTimeout(fn, 0)` advances exactly one
 * link per frame: 60 chained zero-delay timeouts take ~1 SECOND instead of the ~1 ms
 * a browser needs. Asset loaders written that way appear to hang, and the symptom
 * (a game that loads a few files then stops) points nowhere near timers.
 *
 * The budget is what keeps this safe: a self-rescheduling `setTimeout(f, 0)` would
 * otherwise spin forever inside one frame and the game would never draw again.
 */
#define TIMER_PUMP_BUDGET_MS 6.0

void jsglq_core_pump_timers(JSContext *ctx)
{
    const double pump_start = jsglq_now_ms();
    int rounds = 0;

    for (;;) {
        int fired = jsglq_core_pump_timers_once(ctx);
        if (fired == 0) break;                       /* nothing left due */
        if (++rounds >= 64) break;                   /* pathological chain */
        if (jsglq_now_ms() - pump_start > TIMER_PUMP_BUDGET_MS) break;
    }
}

static int jsglq_core_pump_timers_once(JSContext *ctx)
{
    int fired = 0;
    const double now = jsglq_now_ms();
    for (int i = 0; i < MAX_TIMERS; i++) {
        TimerEntry *t = &g_core.timers[i];
        if (!t->active || now < t->due_ms) continue;

        JSValue fn = t->fn;
        if (t->repeating) {
            /* Re-arm from NOW, not from due: a late frame must not queue a burst of
               catch-up callbacks the game never asked for. */
            t->due_ms = now + t->interval_ms;
            fn = JS_DupValue(ctx, t->fn);
        } else {
            t->active = false;
            t->fn = JS_UNDEFINED;
        }

        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            const char *s = JS_ToCString(ctx, exc);
            fprintf(stderr, "jsglq: timer callback threw: %s\n", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, fn);
        fired++;
    }
    return fired;
}

/* ---------------------------------------------------------------------- rAF -- */

static JSValue js_request_animation_frame(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return jsglq_throw(ctx, "requestAnimationFrame requires a function");

    RafEntry *table = g_core.dispatching ? g_core.raf_pending : g_core.raf;
    int *count = g_core.dispatching ? &g_core.raf_pending_count : &g_core.raf_count;

    if (*count >= MAX_RAF)
        return jsglq_throw(ctx, "too many pending animation frames (%d)", MAX_RAF);

    table[*count].fn = JS_DupValue(ctx, argv[0]);
    table[*count].id = ++g_core.next_id;
    table[*count].active = true;
    (*count)++;
    return JS_NewUint32(ctx, g_core.next_id);
}

static JSValue js_cancel_animation_frame(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    uint32_t id = 0;
    JS_ToUint32(ctx, &id, argv[0]);
    for (int i = 0; i < g_core.raf_count; i++) {
        if (g_core.raf[i].active && g_core.raf[i].id == id) g_core.raf[i].active = false;
    }
    for (int i = 0; i < g_core.raf_pending_count; i++) {
        if (g_core.raf_pending[i].active && g_core.raf_pending[i].id == id)
            g_core.raf_pending[i].active = false;
    }
    return JS_UNDEFINED;
}

/* Fire every callback registered before this frame, in registration order. */
int jsglq_core_pump_raf(JSContext *ctx, double timestamp_ms)
{
    g_core.frame_time_ms = timestamp_ms;

    /* Per-frame JS hooks (WebSocket queue drain, gamepad connect/disconnect
       events) run BEFORE the early return below: a game that only uses sockets
       registers no rAF callback, and hooking after the return would leave it
       connected but never delivered a message. */
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue hook = JS_GetPropertyStr(ctx, global, "__jsglq_frameHook");
        if (JS_IsFunction(ctx, hook)) {
            JSValue r = JS_Call(ctx, hook, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(r)) {
                JSValue exc = JS_GetException(ctx);
                const char *msg = JS_ToCString(ctx, exc);
                fprintf(stderr, "jsglq: frame hook threw: %s\n", msg ? msg : "?");
                if (msg) JS_FreeCString(ctx, msg);
                JS_FreeValue(ctx, exc);
            }
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, hook);
        JS_FreeValue(ctx, global);
    }

    const int n = g_core.raf_count;
    if (n == 0) return 0;

    g_core.dispatching = true;
    int fired = 0;
    for (int i = 0; i < n; i++) {
        if (!g_core.raf[i].active) { JS_FreeValue(ctx, g_core.raf[i].fn); continue; }
        JSValue arg = JS_NewFloat64(ctx, timestamp_ms);
        JSValue r = JS_Call(ctx, g_core.raf[i].fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            const char *s = JS_ToCString(ctx, exc);
            fprintf(stderr, "jsglq: rAF callback threw: %s\n", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
            if (!JS_IsUndefined(stack)) {
                const char *st = JS_ToCString(ctx, stack);
                if (st) { fprintf(stderr, "%s\n", st); JS_FreeCString(ctx, st); }
            }
            JS_FreeValue(ctx, stack);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, g_core.raf[i].fn);
        fired++;
    }
    g_core.dispatching = false;

    /* Promote callbacks registered during this frame. */
    memcpy(g_core.raf, g_core.raf_pending, sizeof(RafEntry) * (size_t)g_core.raf_pending_count);
    g_core.raf_count = g_core.raf_pending_count;
    g_core.raf_pending_count = 0;
    return fired;
}

static bool g_exit_requested;

bool jsglq_core_exit_requested(void) { return g_exit_requested; }

static JSValue js_request_exit(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    g_exit_requested = true;
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ install -- */

int jsglq_bind_core(JsglqEngine *e)
{
    JSContext *ctx = jsglq_engine_ctx(e);
    JSValue global = JS_GetGlobalObject(ctx);

    /* console */
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunctionMagic(ctx, js_console_write, "log", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, console, "info",
        JS_NewCFunctionMagic(ctx, js_console_write, "info", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunctionMagic(ctx, js_console_write, "warn", 0, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunctionMagic(ctx, js_console_write, "error", 0, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, console, "debug",
        JS_NewCFunctionMagic(ctx, js_console_write, "debug", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "console", console);

    /* performance */
    JSValue perf = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, perf, "now",
        JS_NewCFunction(ctx, js_performance_now, "now", 0));
    JS_SetPropertyStr(ctx, global, "performance", perf);

    /* timers */
    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunctionMagic(ctx, js_set_timer, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunctionMagic(ctx, js_set_timer, "setInterval", 2, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, js_clear_timer, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, js_clear_timer, "clearInterval", 1));

    /* rAF */
    JS_SetPropertyStr(ctx, global, "requestAnimationFrame",
        JS_NewCFunction(ctx, js_request_animation_frame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, global, "cancelAnimationFrame",
        JS_NewCFunction(ctx, js_cancel_animation_frame, "cancelAnimationFrame", 1));

    /*
     * three.js calls self.requestAnimationFrame from setAnimationLoop, so `self`
     * must exist. Note this makes global reference itself: a genuine cycle that
     * refcounting alone cannot reclaim, which is why shutdown runs the cycle
     * collector before freeing the runtime (see jsglq_core_shutdown).
     * `globalThis` is already defined by the engine and must not be reassigned.
     */
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));

    /* Lets tooling end a run once it has what it needs, instead of waiting out a
       wall-clock cap. Not a game-facing API. */
    JS_SetPropertyStr(ctx, global, "__jsglq_requestExit",
        JS_NewCFunction(ctx, js_request_exit, "__jsglq_requestExit", 0));

    /* Runtime identity, used by the bench harness to label results. */
    JSValue id = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, id, "version", JS_NewString(ctx, "0.0.0"));
    JS_SetPropertyStr(ctx, id, "engine", JS_NewString(ctx, "quickjs-ng"));
    JS_SetPropertyStr(ctx, global, "__JSGLQ__", id);

    JS_FreeValue(ctx, global);
    return 0;
}

/*
 * Release every JSValue this module holds.
 *
 * Without this, QuickJS asserts on shutdown (`list_empty(&rt->gc_obj_list)`) because
 * pending rAF callbacks and armed timers still hold references. That assert is a
 * genuinely useful one: it catches exactly the kind of slow leak that would
 * otherwise only show up as growing RSS during a long play session.
 */
void jsglq_core_shutdown(JSContext *ctx)
{
    for (int i = 0; i < g_core.raf_count; i++) {
        if (g_core.raf[i].active) JS_FreeValue(ctx, g_core.raf[i].fn);
        g_core.raf[i].fn = JS_UNDEFINED;
        g_core.raf[i].active = false;
    }
    g_core.raf_count = 0;

    for (int i = 0; i < g_core.raf_pending_count; i++) {
        if (g_core.raf_pending[i].active) JS_FreeValue(ctx, g_core.raf_pending[i].fn);
        g_core.raf_pending[i].fn = JS_UNDEFINED;
        g_core.raf_pending[i].active = false;
    }
    g_core.raf_pending_count = 0;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_core.timers[i].active) {
            JS_FreeValue(ctx, g_core.timers[i].fn);
            g_core.timers[i].fn = JS_UNDEFINED;
            g_core.timers[i].active = false;
        }
    }

    /*
     * The realm deliberately contains cycles (window === window.self, and every
     * shim closure captures the global). Refcounting cannot reclaim those, so the
     * cycle collector has to run before JS_FreeRuntime or it asserts that objects
     * are still alive. Running it here also means a long session's accumulated
     * cycles are collected at least once at exit.
     */
    /* The cycle collector runs in jsglq_engine_free, after the context is dropped:
       while the context is alive the global is still reachable and nothing here
       would be collected. */
}
