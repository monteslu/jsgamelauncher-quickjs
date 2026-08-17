/*
 * jsgamelauncher-quickjs — host interfaces.
 *
 * Deliberately small. The host owns the window, the GL context, the audio device,
 * the event pump, and the frame clock; QuickJS owns everything above that.
 */
#ifndef JSGLQ_HOST_H
#define JSGLQ_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ engine ---- */

typedef struct JsglqEngine JsglqEngine;

typedef struct {
    const char *game_dir;      /* rooted asset access; nothing outside this is reachable */
    const char *entry_file;    /* resolved entry (main.js, index.js, ...) */
    const char *runtime_dir;   /* the launcher JS layer (bootstrap + shims + vendor) */
    bool bench_mode;           /* propagates BENCH_OPTS into the realm */
    int  width;
    int  height;
} JsglqConfig;

JsglqEngine *jsglq_engine_new(const JsglqConfig *cfg);
void         jsglq_engine_free(JsglqEngine *e);
JSContext   *jsglq_engine_ctx(JsglqEngine *e);
JSRuntime   *jsglq_engine_rt(JsglqEngine *e);

/* Evaluate the game entry as an ES module. Returns 0 on success. */
int jsglq_engine_run_entry(JsglqEngine *e);
int jsglq_engine_run_bootstrap(JsglqEngine *e, int width, int height);
bool jsglq_engine_had_error(JsglqEngine *e);

/*
 * Drain pending jobs (promises) with a TIME budget, not a count budget.
 * 4096 trivial microtasks cost ~5ms which is 30% of a frame handed to the game for
 * free; a time bound is the only one that means anything. The clock is sampled every
 * 64 jobs because clock_gettime costs more than the microtasks do.
 */
int jsglq_engine_drain_jobs(JsglqEngine *e, double budget_ms);

/* Interrupt handler: bounds a runaway `while(true)` in game code. */
void jsglq_engine_arm_watchdog(JsglqEngine *e, double deadline_ms);
void jsglq_engine_disarm_watchdog(JsglqEngine *e);

/* Asset IO, rooted at game_dir. Caller frees. NULL on miss. */
uint8_t *jsglq_asset_read(JsglqEngine *e, const char *rel, size_t *out_len);
bool     jsglq_asset_exists(JsglqEngine *e, const char *rel);
bool     jsglq_asset_write(JsglqEngine *e, const char *rel, const uint8_t *data, size_t len);

/* Throw a JS TypeError/RangeError with a formatted message. Always returns JS_EXCEPTION. */
JSValue jsglq_throw(JSContext *ctx, const char *fmt, ...);
JSValue jsglq_throw_range(JSContext *ctx, const char *fmt, ...);

/* --------------------------------------------------------------- frame clock --- */

typedef struct {
    double now_ms;        /* monotonic, host-owned: the ONE clock the realm sees */
    double delta_ms;      /* clamped frame delta */
    uint64_t frame;
} JsglqFrameTime;

double jsglq_now_ms(void);

/* ------------------------------------------------------------------- bindings -- */

/* Each binding module installs itself onto the global object. */
int jsglq_bind_core(JsglqEngine *e);      /* console, performance, timers, rAF */
int jsglq_bind_gl(JsglqEngine *e);        /* the flat gl.* namespace over native-gles */
int jsglq_bind_io(JsglqEngine *e);        /* asset reads backing fetch/Image */
int jsglq_bind_canvas2d(JsglqEngine *e, int width, int height);  /* Canvas 2D over GL */
int jsglq_bind_image(JsglqEngine *e);      /* stb_image decode -> renderer handle */
int jsglq_bind_audio(JsglqEngine *e);      /* webaudio-node engine + SDL callback */
int jsglq_bind_audio_decode(JsglqEngine *e);  /* mp3/wav/flac/ogg/aac/opus decode */
void jsglq_audio_shutdown(void);
int jsglq_bind_worker(JsglqEngine *e);     /* real OS-thread Workers */
int jsglq_bind_gamepad(JsglqEngine *e);    /* SDL gamepads + controller DB */
void jsglq_gamepad_init(void);
void jsglq_gamepad_shutdown(void);
int  jsglq_gamepad_load_db(const char *text);
int  jsglq_gamepad_load_db_file(const char *runtime_dir);
int jsglq_bind_websocket(JsglqEngine *e);  /* ws:// over SDL_net */
void jsglq_websocket_shutdown(void);
int jsglq_bind_http(JsglqEngine *e);      /* fetch/XHR over HTTP(S) */
void jsglq_http_shutdown(void);
void jsglq_worker_shutdown(JSContext *ctx);
void jsglq_pump_workers(JSContext *ctx);

/* Fused-binary support (src/bundle.c). Returns false when not fused. */
bool jsglq_bundle_prepare(const char *exe_path, char *out_dir, size_t out_sz);
int  jsglq_canvas2d_resize(JsglqEngine *e, int width, int height);
void jsglq_canvas2d_begin_frame(void);
void jsglq_canvas2d_end_frame(void);
void jsglq_core_shutdown(JSContext *ctx);
bool jsglq_core_exit_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* JSGLQ_HOST_H */
