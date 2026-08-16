/*
 * Web Workers on real OS threads.
 *
 * Each worker gets its OWN JSRuntime and JSContext. That is not an optimization,
 * it is the only correct arrangement: QuickJS runtimes are single-threaded and its
 * values carry no cross-runtime synchronization, so sharing a runtime between
 * threads is a data race by construction.
 *
 * Messages are serialized to JSON and copied across a mutex-guarded queue, drained
 * on the main thread once per frame. Structured clone semantics are approximated
 * (see the note on transferables below); the conformance tests compare against the
 * browser, which is the oracle.
 *
 * ONE INHERITED BUG WORTH REPEATING, from wasmcart-jsgame's worker_shim:
 *
 *   QuickJS's JSON tokenizer reads PAST the length it is given when a value ends in
 *   a number — js_atof scans forward for more digits rather than stopping at the
 *   buffer end. A message that ends in a number therefore inherits trailing bytes
 *   from whatever was in the buffer before it, and JS_ParseJSON then rejects valid
 *   input. It needs BOTH conditions (ends in a number AND a longer predecessor), so
 *   `{"phase":"load"}` always worked and hid it.
 *
 *   This is ENGINE behaviour, not WASM behaviour, so it applies here in full. Every
 *   message buffer is NUL-terminated and sized len+1, and a regression test covers
 *   exactly that shape.
 */
#include "host.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_WORKERS      16
#define MSG_QUEUE_DEPTH  256

typedef struct {
    char *json;        /* NUL-terminated, allocated len+1 — see the header note */
    size_t len;
} Message;

typedef struct {
    Message items[MSG_QUEUE_DEPTH];
    int head, tail, count;
    SDL_mutex *lock;
} MsgQueue;

typedef struct WorkerCtx {
    int id;
    bool active;
    bool should_stop;

    SDL_Thread *thread;
    JSRuntime *rt;                /* the worker's OWN runtime */
    JSContext *ctx;

    char *script_path;            /* resolved inside the game dir */
    char *script_src;

    MsgQueue to_worker;           /* main -> worker */
    MsgQueue from_worker;         /* worker -> main */

    JSValue onmessage;            /* main-thread side handler */
    JSValue onerror;
    JsglqEngine *engine;          /* for asset reads; main thread only */

    char error[512];              /* set by the worker, read by the main thread */
    bool errored;
} WorkerCtx;

static WorkerCtx g_workers[MAX_WORKERS];
static JSContext *g_main_ctx;

/* ------------------------------------------------------------------- queue ---- */

static void queue_init(MsgQueue *q)
{
    memset(q, 0, sizeof(*q));
    q->lock = SDL_CreateMutex();
}

static void queue_free(MsgQueue *q)
{
    if (!q->lock) return;
    SDL_LockMutex(q->lock);
    while (q->count > 0) {
        free(q->items[q->head].json);
        q->items[q->head].json = NULL;
        q->head = (q->head + 1) % MSG_QUEUE_DEPTH;
        q->count--;
    }
    SDL_UnlockMutex(q->lock);
    SDL_DestroyMutex(q->lock);
    q->lock = NULL;
}

static bool queue_push(MsgQueue *q, const char *json, size_t len)
{
    SDL_LockMutex(q->lock);
    if (q->count >= MSG_QUEUE_DEPTH) {
        SDL_UnlockMutex(q->lock);
        return false;   /* caller reports; silently dropping a message is worse */
    }
    /* len + 1 and an explicit NUL: the JSON tokenizer over-reads past `len`. */
    char *copy = (char *)malloc(len + 1);
    if (!copy) { SDL_UnlockMutex(q->lock); return false; }
    memcpy(copy, json, len);
    copy[len] = '\0';

    q->items[q->tail].json = copy;
    q->items[q->tail].len = len;
    q->tail = (q->tail + 1) % MSG_QUEUE_DEPTH;
    q->count++;
    SDL_UnlockMutex(q->lock);
    return true;
}

/* Returns a malloc'd, NUL-terminated string the caller frees, or NULL if empty. */
static char *queue_pop(MsgQueue *q, size_t *out_len)
{
    SDL_LockMutex(q->lock);
    if (q->count == 0) { SDL_UnlockMutex(q->lock); return NULL; }
    char *json = q->items[q->head].json;
    if (out_len) *out_len = q->items[q->head].len;
    q->items[q->head].json = NULL;
    q->head = (q->head + 1) % MSG_QUEUE_DEPTH;
    q->count--;
    SDL_UnlockMutex(q->lock);
    return json;
}

/* --------------------------------------------------------------- worker side -- */

/* postMessage() inside the worker: serialize and hand to the main thread. */
static JSValue worker_post_message(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    WorkerCtx *w = (WorkerCtx *)JS_GetContextOpaque(ctx);
    if (!w) return JS_UNDEFINED;
    if (argc < 1) return jsglq_throw(ctx, "postMessage(data) requires an argument");

    JSValue json = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) return json;

    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, json);
    JS_FreeValue(ctx, json);
    if (!s) return JS_EXCEPTION;

    bool ok = queue_push(&w->from_worker, s, len);
    JS_FreeCString(ctx, s);
    if (!ok) return jsglq_throw(ctx, "worker message queue full (%d pending)",
                                MSG_QUEUE_DEPTH);
    return JS_UNDEFINED;
}

static JSValue worker_console_log(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    WorkerCtx *w = (WorkerCtx *)JS_GetContextOpaque(ctx);
    fprintf(stdout, "[worker %d]", w ? w->id : -1);
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        fprintf(stdout, " %s", s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return JS_UNDEFINED;
}

static int worker_thread_main(void *data)
{
    WorkerCtx *w = (WorkerCtx *)data;

    w->rt = JS_NewRuntime();
    if (!w->rt) {
        snprintf(w->error, sizeof(w->error), "worker runtime creation failed");
        w->errored = true;
        return 1;
    }
    JS_SetMemoryLimit(w->rt, 256u * 1024u * 1024u);
    JS_SetMaxStackSize(w->rt, 2u * 1024u * 1024u);

    w->ctx = JS_NewContext(w->rt);
    if (!w->ctx) {
        snprintf(w->error, sizeof(w->error), "worker context creation failed");
        w->errored = true;
        JS_FreeRuntime(w->rt);
        w->rt = NULL;
        return 1;
    }
    JS_SetContextOpaque(w->ctx, w);

    JSValue global = JS_GetGlobalObject(w->ctx);
    JS_SetPropertyStr(w->ctx, global, "postMessage",
        JS_NewCFunction(w->ctx, worker_post_message, "postMessage", 1));
    JS_SetPropertyStr(w->ctx, global, "self", JS_DupValue(w->ctx, global));

    JSValue console = JS_NewObject(w->ctx);
    JS_SetPropertyStr(w->ctx, console, "log",
        JS_NewCFunction(w->ctx, worker_console_log, "log", 0));
    JS_SetPropertyStr(w->ctx, console, "error",
        JS_NewCFunction(w->ctx, worker_console_log, "error", 0));
    JS_SetPropertyStr(w->ctx, global, "console", console);
    JS_FreeValue(w->ctx, global);

    JSValue res = JS_Eval(w->ctx, w->script_src, strlen(w->script_src),
                          w->script_path, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(res)) {
        JSValue exc = JS_GetException(w->ctx);
        const char *s = JS_ToCString(w->ctx, exc);
        snprintf(w->error, sizeof(w->error), "%s", s ? s : "worker script threw");
        if (s) JS_FreeCString(w->ctx, s);
        JS_FreeValue(w->ctx, exc);
        w->errored = true;
    }
    JS_FreeValue(w->ctx, res);

    /* Message loop: deliver inbound messages to self.onmessage until stopped. */
    while (!w->should_stop) {
        size_t len = 0;
        char *json = queue_pop(&w->to_worker, &len);
        if (!json) {
            JSContext *pctx;
            while (JS_ExecutePendingJob(w->rt, &pctx) > 0) { /* drain */ }
            SDL_Delay(1);
            continue;
        }

        JSValue g = JS_GetGlobalObject(w->ctx);
        JSValue handler = JS_GetPropertyStr(w->ctx, g, "onmessage");
        if (JS_IsFunction(w->ctx, handler)) {
            /* Parse with the length we actually have; the buffer is NUL-terminated
               because the tokenizer over-reads. */
            JSValue data = JS_ParseJSON(w->ctx, json, len, "<message>");
            if (JS_IsException(data)) {
                JSValue exc = JS_GetException(w->ctx);
                const char *s = JS_ToCString(w->ctx, exc);
                fprintf(stderr, "[worker %d] message parse failed: %s\n",
                        w->id, s ? s : "?");
                if (s) JS_FreeCString(w->ctx, s);
                JS_FreeValue(w->ctx, exc);
            } else {
                JSValue ev = JS_NewObject(w->ctx);
                JS_SetPropertyStr(w->ctx, ev, "data", data);
                JS_SetPropertyStr(w->ctx, ev, "type", JS_NewString(w->ctx, "message"));
                JSValue r = JS_Call(w->ctx, handler, JS_UNDEFINED, 1, (JSValueConst *)&ev);
                if (JS_IsException(r)) {
                    JSValue exc = JS_GetException(w->ctx);
                    const char *s = JS_ToCString(w->ctx, exc);
                    fprintf(stderr, "[worker %d] onmessage threw: %s\n", w->id, s ? s : "?");
                    if (s) JS_FreeCString(w->ctx, s);
                    JS_FreeValue(w->ctx, exc);
                }
                JS_FreeValue(w->ctx, r);
                JS_FreeValue(w->ctx, ev);
            }
        }
        JS_FreeValue(w->ctx, handler);
        JS_FreeValue(w->ctx, g);
        free(json);
    }

    JS_FreeContext(w->ctx);
    w->ctx = NULL;
    JS_RunGC(w->rt);
    JS_FreeRuntime(w->rt);
    w->rt = NULL;
    return 0;
}

/* ----------------------------------------------------------------- main side -- */

static JSValue js_worker_create(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JsglqEngine *e = (JsglqEngine *)JS_GetContextOpaque(ctx);
    if (argc < 1) return jsglq_throw(ctx, "__jsglq_workerCreate(path) requires a path");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    int slot = -1;
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (!g_workers[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        JS_FreeCString(ctx, path);
        return jsglq_throw(ctx, "too many workers (limit %d)", MAX_WORKERS);
    }

    size_t len = 0;
    uint8_t *src = jsglq_asset_read(e, path, &len);
    if (!src) {
        JSValue err = jsglq_throw(ctx, "worker script not found: '%s'", path);
        JS_FreeCString(ctx, path);
        return err;
    }

    WorkerCtx *w = &g_workers[slot];
    memset(w, 0, sizeof(*w));
    w->id = slot;
    w->active = true;
    w->engine = e;
    w->script_path = strdup(path);
    w->script_src = (char *)src;    /* asset_read already NUL-terminates */
    w->onmessage = JS_UNDEFINED;
    w->onerror = JS_UNDEFINED;
    queue_init(&w->to_worker);
    queue_init(&w->from_worker);
    JS_FreeCString(ctx, path);

    char name[32];
    snprintf(name, sizeof(name), "jsglq-worker-%d", slot);
    w->thread = SDL_CreateThread(worker_thread_main, name, w);
    if (!w->thread) {
        w->active = false;
        return jsglq_throw(ctx, "could not start worker thread: %s", SDL_GetError());
    }
    return JS_NewInt32(ctx, slot);
}

static JSValue js_worker_post(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    if (argc < 2) return jsglq_throw(ctx, "__jsglq_workerPost(id, json)");
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= MAX_WORKERS || !g_workers[id].active)
        return jsglq_throw(ctx, "worker %d is not running", id);

    size_t len = 0;
    const char *json = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!json) return JS_EXCEPTION;
    bool ok = queue_push(&g_workers[id].to_worker, json, len);
    JS_FreeCString(ctx, json);
    if (!ok) return jsglq_throw(ctx, "worker %d inbound queue full", id);
    return JS_UNDEFINED;
}

static JSValue js_worker_terminate(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= MAX_WORKERS || !g_workers[id].active) return JS_UNDEFINED;

    WorkerCtx *w = &g_workers[id];
    w->should_stop = true;
    if (w->thread) { SDL_WaitThread(w->thread, NULL); w->thread = NULL; }
    queue_free(&w->to_worker);
    queue_free(&w->from_worker);
    free(w->script_path);
    free(w->script_src);
    JS_FreeValue(ctx, w->onmessage);
    JS_FreeValue(ctx, w->onerror);
    w->active = false;
    return JS_UNDEFINED;
}

/* Drain worker->main messages; called once per frame from the host loop. */
static JSValue js_worker_poll(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue out = JS_NewArray(ctx);
    uint32_t n = 0;

    for (int i = 0; i < MAX_WORKERS; i++) {
        WorkerCtx *w = &g_workers[i];
        if (!w->active) continue;

        if (w->errored) {
            JSValue item = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, item, "id", JS_NewInt32(ctx, i));
            JS_SetPropertyStr(ctx, item, "error", JS_NewString(ctx, w->error));
            JS_SetPropertyUint32(ctx, out, n++, item);
            w->errored = false;
        }

        for (;;) {
            size_t len = 0;
            char *json = queue_pop(&w->from_worker, &len);
            if (!json) break;
            JSValue data = JS_ParseJSON(ctx, json, len, "<worker-message>");
            free(json);
            if (JS_IsException(data)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                continue;
            }
            JSValue item = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, item, "id", JS_NewInt32(ctx, i));
            JS_SetPropertyStr(ctx, item, "data", data);
            JS_SetPropertyUint32(ctx, out, n++, item);
        }
    }
    return out;
}

int jsglq_bind_worker(JsglqEngine *e)
{
    JSContext *ctx = jsglq_engine_ctx(e);
    g_main_ctx = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue w = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, w, "create", JS_NewCFunction(ctx, js_worker_create, "create", 1));
    JS_SetPropertyStr(ctx, w, "post", JS_NewCFunction(ctx, js_worker_post, "post", 2));
    JS_SetPropertyStr(ctx, w, "terminate",
        JS_NewCFunction(ctx, js_worker_terminate, "terminate", 1));
    JS_SetPropertyStr(ctx, w, "poll", JS_NewCFunction(ctx, js_worker_poll, "poll", 0));
    JS_SetPropertyStr(ctx, global, "__jsglq_worker", w);
    JS_FreeValue(ctx, global);
    return 0;
}

void jsglq_worker_shutdown(JSContext *ctx)
{
    for (int i = 0; i < MAX_WORKERS; i++) {
        WorkerCtx *w = &g_workers[i];
        if (!w->active) continue;
        w->should_stop = true;
        if (w->thread) { SDL_WaitThread(w->thread, NULL); w->thread = NULL; }
        queue_free(&w->to_worker);
        queue_free(&w->from_worker);
        free(w->script_path);
        free(w->script_src);
        JS_FreeValue(ctx, w->onmessage);
        JS_FreeValue(ctx, w->onerror);
        w->active = false;
    }
}

/* Called once per frame by the host loop: hands queued worker messages to JS. */
void jsglq_pump_workers(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__jsglq_pumpWorkers");
    if (JS_IsFunction(ctx, fn)) {
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            const char *s = JS_ToCString(ctx, exc);
            fprintf(stderr, "jsglq: worker pump threw: %s\n", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
}
