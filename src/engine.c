/*
 * QuickJS runtime setup, module loading, and the sandbox posture.
 *
 * Sandbox note: quickjs-libc is NOT compiled in. There is no os module, no std
 * module, no fs, no process — not because they were deleted at the JS level, but
 * because they were never built. That is the structural difference from rungame's
 * node:vm realm, where `this.constructor.constructor('return process')()` walks out
 * in one line. Here there is no host realm behind the wall to reach.
 */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <limits.h>
#include "platform.h"

#define MAX_PATH_LEN 4096

struct JsglqEngine {
    JSRuntime *rt;
    JSContext *ctx;
    char game_dir[MAX_PATH_LEN];
    char entry_file[MAX_PATH_LEN];
    char runtime_dir[MAX_PATH_LEN];   /* the launcher's own JS layer */
    bool bench_mode;
    int width, height;

    /* watchdog */
    double deadline_ms;
    bool watchdog_armed;

    bool had_unhandled_rejection;
};

bool jsglq_engine_had_error(JsglqEngine *e) { return e && e->had_unhandled_rejection; }

/* Read a file by absolute path (runtime layer only; game assets go through
   jsglq_asset_read, which enforces the game-dir root). */
static uint8_t *read_file_abs(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

/* ------------------------------------------------------------------ clock ----- */

double jsglq_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* ------------------------------------------------------------------ errors ---- */

static JSValue throw_va(JSContext *ctx, JSValue (*mk)(JSContext *, const char *, ...),
                        const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    return mk(ctx, "%s", buf);
}

JSValue jsglq_throw(JSContext *ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    JSValue v = throw_va(ctx, JS_ThrowTypeError, fmt, ap);
    va_end(ap);
    return v;
}

JSValue jsglq_throw_range(JSContext *ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    JSValue v = throw_va(ctx, JS_ThrowRangeError, fmt, ap);
    va_end(ap);
    return v;
}

/* --------------------------------------------------------------- path safety -- */

/*
 * Resolve `rel` inside game_dir and refuse anything that escapes it.
 * A game gets its own directory and nothing else: no ../, no absolute paths, no
 * symlink hops out. This is checked on the REALPATH, because ../ can be laundered
 * through a symlink and a purely lexical check would miss it.
 */
static bool resolve_in_root(JsglqEngine *e, const char *rel, char *out, size_t out_sz)
{
    if (!rel || !*rel) return false;

    /* Strip a leading ./ and any leading / (treat as game-dir relative). */
    while (*rel == '/' ) rel++;
    if (rel[0] == '.' && rel[1] == '/') rel += 2;

    char joined[MAX_PATH_LEN];
    int n = snprintf(joined, sizeof(joined), "%s/%s", e->game_dir, rel);
    if (n < 0 || (size_t)n >= sizeof(joined)) return false;

    char resolved[MAX_PATH_LEN];
    if (!jsglq_realpath(joined, resolved, sizeof(resolved))) return false;

    size_t root_len = strlen(e->game_dir);
    if (strncmp(resolved, e->game_dir, root_len) != 0) return false;
    if (resolved[root_len] != '/' && resolved[root_len] != '\0') return false;

    if (strlen(resolved) >= out_sz) return false;
    strcpy(out, resolved);
    return true;
}

/* ------------------------------------------------------------------ asset io -- */

/*
 * Asset roots, tried in order.
 *
 * A built game is laid out with its entry in dist/ and its assets beside it
 * (dist/sounds/laser.mp3), while the source tree keeps them in public/. A game
 * fetching 'sounds/laser.mp3' means "relative to where my code lives", so the
 * entry's own directory is tried FIRST, then public/, then the game root.
 *
 * Getting this wrong is quiet in the worst way: fetch returns a correct 404, the
 * game hands 0 bytes to decodeAudioData, and the failure surfaces as a decoder
 * error about an empty buffer — pointing at audio when the problem is a path.
 */
static bool resolve_asset(JsglqEngine *e, const char *rel, char *out, size_t out_sz)
{
    char candidate[MAX_PATH_LEN];

    /* 1. Beside the entry file (dist/ in a built game). */
    const char *slash = strrchr(e->entry_file, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - e->entry_file);
        snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)dlen, e->entry_file, rel);
        if (resolve_in_root(e, candidate, out, out_sz)) return true;
    }

    /* 2. public/, which is where an unbuilt source tree keeps them. */
    snprintf(candidate, sizeof(candidate), "public/%s", rel);
    if (resolve_in_root(e, candidate, out, out_sz)) return true;

    /* 3. The game root. */
    return resolve_in_root(e, rel, out, out_sz);
}

uint8_t *jsglq_asset_read(JsglqEngine *e, const char *rel, size_t *out_len)
{
    char path[MAX_PATH_LEN];
    if (!resolve_asset(e, rel, path, sizeof(path))) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

/*
 * Write a file under the game root.
 *
 * Unlike a read, the target need not already exist, so the root check is done on
 * the DIRECTORY rather than the file: realpath() cannot resolve a path that is not
 * there yet, and skipping the check for missing files would be exactly the hole
 * worth avoiding.
 */
bool jsglq_asset_write(JsglqEngine *e, const char *rel, const uint8_t *data, size_t len)
{
    if (!rel || !*rel) return false;
    if (strstr(rel, "..") || rel[0] == '/') return false;

    char joined[MAX_PATH_LEN];
    if (snprintf(joined, sizeof(joined), "%s/%s", e->game_dir, rel) >= (int)sizeof(joined))
        return false;

    /* Confirm the parent directory really is inside the game root. */
    char parent[MAX_PATH_LEN];
    snprintf(parent, sizeof(parent), "%s", joined);
    char *slash = strrchr(parent, '/');
    if (!slash) return false;
    *slash = 0;
    char resolved_parent[MAX_PATH_LEN];
    if (!jsglq_realpath(parent, resolved_parent, sizeof(resolved_parent))) return false;
    size_t root_len = strlen(e->game_dir);
    if (strncmp(resolved_parent, e->game_dir, root_len) != 0) return false;
    if (resolved_parent[root_len] != '/' && resolved_parent[root_len] != '\0') return false;

    FILE *f = fopen(joined, "wb");
    if (!f) return false;
    size_t wrote = len ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    return wrote == len;
}

bool jsglq_asset_exists(JsglqEngine *e, const char *rel)
{
    char path[MAX_PATH_LEN];
    if (!resolve_asset(e, rel, path, sizeof(path))) return false;
    return jsglq_is_file(path);
}

/* -------------------------------------------------------------- module loader - */

/*
 * Specifier resolution.
 *
 * Relative and absolute specifiers resolve inside the game dir. Bare specifiers get
 * a minimal node-style walk of node_modules reading package.json's "module"/"main"
 * (and the "." export), which is what the corpus actually needs: games import three
 * unbundled in dev. Full node resolution is deliberately not reimplemented.
 */
static char *module_normalize(JSContext *ctx, const char *base, const char *name, void *opaque)
{
    (void)opaque;
    char *result = NULL;

    /*
     * A relative import from inside the runtime layer stays in the runtime layer.
     * Without this, `import './shims/canvas.js'` from "jsglq:bootstrap.js"
     * normalizes to "/shims/canvas.js" — the prefix is lost, the loader then looks
     * under the game dir, and the error names a path the game never referenced.
     */
    /*
     * Same for vendored packages loaded under a bare name: webgl-node's internal
     * `./lib/webgl2-context.mjs` must resolve inside webgl-node, not the game dir.
     */
    if (name[0] == '.' && !strncmp(base, "webgl-node", 10)) {
        char joined[MAX_PATH_LEN];
        const char *slash = strrchr(base, '/');
        const char *n = name;
        if (n[0] == '.' && n[1] == '/') n += 2;
        if (slash)
            snprintf(joined, sizeof(joined), "%.*s/%s", (int)(slash - base), base, n);
        else
            snprintf(joined, sizeof(joined), "webgl-node/%s", n);
        return js_strdup(ctx, joined);
    }

    if (name[0] == '.' && !strncmp(base, "jsglq:", 6)) {
        char joined[MAX_PATH_LEN];
        const char *base_rel = base + 6;
        const char *slash = strrchr(base_rel, '/');
        size_t dlen = slash ? (size_t)(slash - base_rel) : 0;
        const char *n = name;
        if (n[0] == '.' && n[1] == '/') n += 2;
        if (dlen)
            snprintf(joined, sizeof(joined), "jsglq:%.*s/%s", (int)dlen, base_rel, n);
        else
            snprintf(joined, sizeof(joined), "jsglq:%s", n);
        return js_strdup(ctx, joined);
    }

    if (name[0] == '.' ) {
        if (getenv("JSGLQ_DEBUG_MODULES"))
            fprintf(stderr, "[mod] normalize base=[%s] name=[%s]\n", base, name);
        /* Relative: join against base's directory, then lexically normalize. */
        char tmp[MAX_PATH_LEN];
        const char *slash = strrchr(base, '/');
        size_t dlen = slash ? (size_t)(slash - base) : 0;
        if (dlen >= sizeof(tmp)) return NULL;
        memcpy(tmp, base, dlen);
        tmp[dlen] = 0;

        char joined[MAX_PATH_LEN];
        snprintf(joined, sizeof(joined), "%.*s/%s", (int)dlen, tmp, name);

        /*
         * Collapse "." and ".." segment-wise.
         *
         * Segment-wise rather than character-wise on purpose: a char-by-char scan
         * has to guess where the previous segment started when it hits "..", and
         * getting that off by one silently produces a sibling directory — which
         * then fails with an error naming a path the game never wrote.
         * (The realpath check in the loader is the security boundary; this only
         * produces a tidy, stable module key.)
         */
        char out[MAX_PATH_LEN];
        char work[MAX_PATH_LEN];
        snprintf(work, sizeof(work), "%s", joined);

        const char *segs[512];
        int nsegs = 0;
        for (char *tok = strtok(work, "/"); tok; tok = strtok(NULL, "/")) {
            if (!strcmp(tok, ".")) continue;
            if (!strcmp(tok, "..")) { if (nsegs > 0) nsegs--; continue; }
            if (nsegs < (int)(sizeof(segs) / sizeof(segs[0]))) segs[nsegs++] = tok;
        }

        size_t pos = 0;
        out[0] = 0;
        for (int i = 0; i < nsegs; i++) {
            int w = snprintf(out + pos, sizeof(out) - pos, "/%s", segs[i]);
            if (w < 0 || (size_t)w >= sizeof(out) - pos) break;
            pos += (size_t)w;
        }
        result = js_strdup(ctx, out[0] ? out : "/");
    } else {
        result = js_strdup(ctx, name);
    }
    return result;
}

static JSModuleDef *module_loader(JSContext *ctx, const char *module_name, void *opaque)
{
    JsglqEngine *e = (JsglqEngine *)opaque;
    size_t len = 0;
    uint8_t *src = NULL;
    char rel[MAX_PATH_LEN];

    /*
     * The launcher's own runtime layer (bootstrap, shims, webgl-node) lives outside
     * the game dir and resolves first. It is addressed by a reserved prefix so a
     * game cannot shadow it with a file of the same name, and so these lookups never
     * touch the game-dir root check.
     */
    if (!strncmp(module_name, "jsglq:", 6)) {
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", e->runtime_dir, module_name + 6);
        src = read_file_abs(path, &len);
        if (!src) {
            JS_ThrowReferenceError(ctx, "runtime module '%s' missing (looked in %s)",
                                   module_name + 6, e->runtime_dir);
            return NULL;
        }
        JSValue fn = JS_Eval(ctx, (char *)src, len, module_name,
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        free(src);
        if (JS_IsException(fn)) return NULL;
        JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(fn);
        JS_FreeValue(ctx, fn);
        return m;
    }

    /*
     * Relative imports originating inside a vendored runtime module.
     * module_normalize joins them against the importer's name, so a webgl-node
     * internal import arrives as e.g. "webgl-node/lib/webgl2-context.mjs" — but a
     * bare './lib/x.mjs' can also arrive unqualified when the importer had no path.
     */
    if (!src && !strncmp(module_name, "vendor/", 7)) {
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", e->runtime_dir, module_name);
        src = read_file_abs(path, &len);
        if (src) {
            JSValue fn = JS_Eval(ctx, (char *)src, len, module_name,
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            free(src);
            if (JS_IsException(fn)) return NULL;
            JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(fn);
            JS_FreeValue(ctx, fn);
            return m;
        }
    }

    /* Direct hit inside the game dir. */
    const char *try_rel = module_name;
    if (strncmp(module_name, e->game_dir, strlen(e->game_dir)) == 0) {
        try_rel = module_name + strlen(e->game_dir);
        while (*try_rel == '/') try_rel++;
    }
    src = jsglq_asset_read(e, try_rel, &len);

    /*
     * Runtime-provided packages.
     *
     * webgl-node ships INSIDE the launcher (it is pure ESM with zero Node imports,
     * so it runs unmodified), and it imports 'native-gles' as a bare specifier.
     * Both are resolved here rather than from the game's node_modules, so a game
     * gets the launcher's tested versions and cannot substitute its own.
     */
    if (!src && module_name[0] != '.' && module_name[0] != '/') {
        char path[MAX_PATH_LEN];
        if (!strcmp(module_name, "native-gles")) {
            snprintf(path, sizeof(path), "%s/vendor/native-gles-shim.js", e->runtime_dir);
            src = read_file_abs(path, &len);
        } else if (!strcmp(module_name, "webgl-node")) {
            snprintf(path, sizeof(path), "%s/vendor/webgl-node/index.mjs", e->runtime_dir);
            src = read_file_abs(path, &len);
        } else if (!strncmp(module_name, "webgl-node/", 11)) {
            snprintf(path, sizeof(path), "%s/vendor/webgl-node/%s",
                     e->runtime_dir, module_name + 11);
            src = read_file_abs(path, &len);
        }
        if (src) {
            JSValue fn = JS_Eval(ctx, (char *)src, len, module_name,
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            free(src);
            if (JS_IsException(fn)) return NULL;
            JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(fn);
            JS_FreeValue(ctx, fn);
            return m;
        }
    }

    /* Bare specifier: walk node_modules for package.json module/main. */
    if (!src && module_name[0] != '.' && module_name[0] != '/') {
        char pkg[MAX_PATH_LEN];
        snprintf(pkg, sizeof(pkg), "node_modules/%s/package.json", module_name);
        size_t plen = 0;
        uint8_t *pj = jsglq_asset_read(e, pkg, &plen);
        if (pj) {
            /* Tiny, tolerant extraction: "module" wins over "main" (ESM first). */
            const char *keys[] = { "\"module\"", "\"main\"" };
            char entry[512] = {0};
            for (int k = 0; k < 2 && !entry[0]; k++) {
                const char *hit = strstr((char *)pj, keys[k]);
                if (!hit) continue;
                const char *colon = strchr(hit, ':');
                if (!colon) continue;
                const char *q1 = strchr(colon, '"');
                if (!q1) continue;
                const char *q2 = strchr(q1 + 1, '"');
                if (!q2 || (size_t)(q2 - q1 - 1) >= sizeof(entry)) continue;
                memcpy(entry, q1 + 1, (size_t)(q2 - q1 - 1));
                entry[q2 - q1 - 1] = 0;
            }
            free(pj);
            if (entry[0]) {
                snprintf(rel, sizeof(rel), "node_modules/%s/%s", module_name, entry);
                src = jsglq_asset_read(e, rel, &len);
            }
            if (!src) {
                snprintf(rel, sizeof(rel), "node_modules/%s/index.js", module_name);
                src = jsglq_asset_read(e, rel, &len);
            }
        }
    }

    /* Try common extensions for extensionless relative specifiers. */
    if (!src) {
        const char *exts[] = { ".js", ".mjs", "/index.js", "/index.mjs" };
        for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]) && !src; i++) {
            snprintf(rel, sizeof(rel), "%s%s", try_rel, exts[i]);
            src = jsglq_asset_read(e, rel, &len);
        }
    }

    if (!src) {
        /* Name the specifier AND the root: "cannot find module" with neither is the
           least useful error message in computing. */
        JS_ThrowReferenceError(ctx, "cannot resolve module '%s' under game dir '%s'",
                               module_name, e->game_dir);
        return NULL;
    }

    JSValue fn = JS_Eval(ctx, (char *)src, len, module_name,
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(src);
    if (JS_IsException(fn)) return NULL;

    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(fn);
    JS_FreeValue(ctx, fn);
    return m;
}


/*
 * Unhandled promise rejections must be LOUD.
 *
 * Without this tracker a rejected promise is simply dropped: an async entry point
 * that throws produces a clean exit and a blank window, which reads as a rendering
 * bug rather than the startup exception it is. That failure mode cost real time
 * during bring-up, so it is now impossible by construction.
 */
static void promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                      JSValueConst reason, bool is_handled, void *opaque)
{
    JsglqEngine *e = (JsglqEngine *)opaque;
    if (is_handled) return;   /* caught later; not a failure */

    const char *s = JS_ToCString(ctx, reason);
    fprintf(stderr, "jsglq: UNHANDLED PROMISE REJECTION: %s\n", s ? s : "?");
    if (s) JS_FreeCString(ctx, s);

    JSValue stack = JS_GetPropertyStr(ctx, reason, "stack");
    if (!JS_IsUndefined(stack) && !JS_IsException(stack)) {
        const char *st = JS_ToCString(ctx, stack);
        if (st) { fprintf(stderr, "%s\n", st); JS_FreeCString(ctx, st); }
    }
    JS_FreeValue(ctx, stack);

    if (e) e->had_unhandled_rejection = true;
}

/* ------------------------------------------------------------------ watchdog -- */

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    JsglqEngine *e = (JsglqEngine *)opaque;
    if (!e->watchdog_armed) return 0;
    if (jsglq_now_ms() > e->deadline_ms) {
        return 1;   /* non-zero: interrupt the running script */
    }
    return 0;
}

void jsglq_engine_arm_watchdog(JsglqEngine *e, double deadline_ms)
{
    e->deadline_ms = deadline_ms;
    e->watchdog_armed = true;
}

void jsglq_engine_disarm_watchdog(JsglqEngine *e)
{
    e->watchdog_armed = false;
}

/* -------------------------------------------------------------------- engine -- */

JsglqEngine *jsglq_engine_new(const JsglqConfig *cfg)
{
    JsglqEngine *e = (JsglqEngine *)calloc(1, sizeof(JsglqEngine));
    if (!e) return NULL;

    if (!jsglq_realpath(cfg->game_dir, e->game_dir, sizeof(e->game_dir))) {
        fprintf(stderr, "jsglq: cannot resolve game dir '%s'\n", cfg->game_dir);
        free(e);
        return NULL;
    }
    snprintf(e->entry_file, sizeof(e->entry_file), "%s", cfg->entry_file);
    snprintf(e->runtime_dir, sizeof(e->runtime_dir), "%s",
             cfg->runtime_dir ? cfg->runtime_dir : "");
    e->bench_mode = cfg->bench_mode;
    e->width = cfg->width;
    e->height = cfg->height;

    e->rt = JS_NewRuntime();
    if (!e->rt) { free(e); return NULL; }

    /* Memory ceiling: generous for a game, bounded against a runaway. */
    /*
     * Memory ceiling.
     *
     * Decoded audio dominates: a 194-second stereo track at 48 kHz is ~74 MB of
     * float32, and a game loading several such tracks exceeded a 512 MB limit
     * during the corpus sweep (the wasm build hit the same wall and raised its
     * cap to 2 GB for exactly this reason). The limit still exists so a runaway
     * allocation fails cleanly rather than taking the machine down with it.
     */
    JS_SetMemoryLimit(e->rt, 2048u * 1024u * 1024u);
    JS_SetMaxStackSize(e->rt, 4u * 1024u * 1024u);
    JS_SetInterruptHandler(e->rt, interrupt_handler, e);
    JS_SetHostPromiseRejectionTracker(e->rt, promise_rejection_tracker, e);
    JS_SetModuleLoaderFunc(e->rt, module_normalize, module_loader, e);

    e->ctx = JS_NewContext(e->rt);
    if (!e->ctx) { JS_FreeRuntime(e->rt); free(e); return NULL; }

    JS_SetContextOpaque(e->ctx, e);
    return e;
}

void jsglq_engine_free(JsglqEngine *e)
{
    if (!e) return;
    if (e->ctx) {
        JS_FreeContext(e->ctx);
        e->ctx = NULL;
    }
    if (e->rt) {
        /*
         * Collect AFTER the context is gone, not before.
         *
         * The realm is full of cycles by construction: `window === window.self`,
         * and every shim closure captures the global. While the context holds a
         * reference to the global those cycles are still reachable, so a GC run
         * before JS_FreeContext collects nothing and JS_FreeRuntime then asserts
         * that objects are still alive. Dropping the context first makes the whole
         * graph unreachable, and this pass reclaims it.
         */
        JS_RunGC(e->rt);
        JS_FreeRuntime(e->rt);
        e->rt = NULL;
    }
    free(e);
}

JSContext *jsglq_engine_ctx(JsglqEngine *e) { return e->ctx; }
JSRuntime *jsglq_engine_rt(JsglqEngine *e)  { return e->rt; }

int jsglq_engine_drain_jobs(JsglqEngine *e, double budget_ms)
{
    const double start = jsglq_now_ms();
    int executed = 0;
    JSContext *pctx;

    for (;;) {
        int r = JS_ExecutePendingJob(e->rt, &pctx);
        if (r <= 0) {
            if (r < 0) {
                /*
                 * A rejected job is a real failure and must be loud. An async
                 * entry point whose promise rejects otherwise produces a silent
                 * clean exit — the game "ran" and drew nothing, which reads as a
                 * rendering bug rather than the startup exception it actually is.
                 */
                JSValue exc = JS_GetException(pctx);
                const char *s = JS_ToCString(pctx, exc);
                fprintf(stderr, "jsglq: unhandled rejection: %s\n", s ? s : "?");
                if (s) JS_FreeCString(pctx, s);
                JSValue stack = JS_GetPropertyStr(pctx, exc, "stack");
                if (!JS_IsUndefined(stack)) {
                    const char *st = JS_ToCString(pctx, stack);
                    if (st) { fprintf(stderr, "%s\n", st); JS_FreeCString(pctx, st); }
                }
                JS_FreeValue(pctx, stack);
                JS_FreeValue(pctx, exc);
                e->had_unhandled_rejection = true;
            }
            break;
        }
        executed++;
        /* Sample the clock every 64 jobs: the syscall costs more than the jobs. */
        if ((executed & 63) == 0 && (jsglq_now_ms() - start) > budget_ms) break;
    }
    return executed;
}

/*
 * Optionally evaluate a probe script before the game's entry module.
 *
 * Used by the parity sweep to observe what a real game actually renders, using
 * only the public API the game itself uses. Kept behind an env var so it costs
 * nothing in normal runs and can never affect a shipped game.
 */
static void run_probe(JsglqEngine *e)
{
    const char *probe = getenv("JSGLQ_PROBE");
    if (!probe || !*probe) return;
    JSValue v = JS_Eval(e->ctx, probe, strlen(probe), "<probe>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(e->ctx);
        const char *s = JS_ToCString(e->ctx, exc);
        fprintf(stderr, "jsglq: probe threw: %s\n", s ? s : "?");
        if (s) JS_FreeCString(e->ctx, s);
        JS_FreeValue(e->ctx, exc);
    }
    JS_FreeValue(e->ctx, v);
}

int jsglq_engine_run_entry(JsglqEngine *e)
{
    run_probe(e);

    size_t len = 0;
    uint8_t *src = jsglq_asset_read(e, e->entry_file, &len);
    if (!src) {
        fprintf(stderr, "jsglq: cannot read entry '%s'\n", e->entry_file);
        return -1;
    }

    char modname[MAX_PATH_LEN];
    snprintf(modname, sizeof(modname), "%s/%s", e->game_dir, e->entry_file);

    JSValue v = JS_Eval(e->ctx, (char *)src, len, modname, JS_EVAL_TYPE_MODULE);
    free(src);

    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(e->ctx);
        const char *s = JS_ToCString(e->ctx, exc);
        fprintf(stderr, "jsglq: entry threw: %s\n", s ? s : "?");
        if (s) JS_FreeCString(e->ctx, s);
        JSValue stack = JS_GetPropertyStr(e->ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *st = JS_ToCString(e->ctx, stack);
            if (st) { fprintf(stderr, "%s\n", st); JS_FreeCString(e->ctx, st); }
        }
        JS_FreeValue(e->ctx, stack);
        JS_FreeValue(e->ctx, exc);
        JS_FreeValue(e->ctx, v);
        return -1;
    }
    JS_FreeValue(e->ctx, v);

    /* Startup awaits (asset loads) must settle before frame 0. */
    jsglq_engine_drain_jobs(e, 1000.0);
    return 0;
}

/*
 * Run the runtime bootstrap: installs the browser globals (canvas, document, fetch,
 * text codecs, Blob/URL, ...) before the game's entry module is evaluated.
 *
 * Loaded as a module through the reserved `jsglq:` prefix so a game cannot shadow
 * it, and evaluated to completion (including its own imports) before the game runs.
 */
int jsglq_engine_run_bootstrap(JsglqEngine *e, int width, int height)
{
    char src[512];
    snprintf(src, sizeof(src),
             "import { bootstrap } from 'jsglq:bootstrap.js';\n"
             "bootstrap({ width: %d, height: %d });\n",
             width, height);

    JSValue v = JS_Eval(e->ctx, src, strlen(src), "jsglq:internal-bootstrap",
                        JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(e->ctx);
        const char *s = JS_ToCString(e->ctx, exc);
        fprintf(stderr, "jsglq: bootstrap threw: %s\n", s ? s : "?");
        if (s) JS_FreeCString(e->ctx, s);
        JSValue stack = JS_GetPropertyStr(e->ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *st = JS_ToCString(e->ctx, stack);
            if (st) { fprintf(stderr, "%s\n", st); JS_FreeCString(e->ctx, st); }
        }
        JS_FreeValue(e->ctx, stack);
        JS_FreeValue(e->ctx, exc);
        JS_FreeValue(e->ctx, v);
        return -1;
    }
    JS_FreeValue(e->ctx, v);
    jsglq_engine_drain_jobs(e, 500.0);
    return 0;
}
