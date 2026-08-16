/*
 * Web Audio: webaudio-node's C++ DSP engine, compiled natively and driven by a real
 * SDL audio callback.
 *
 * The engine itself is reused verbatim (see emscripten_compat.h — it needed no
 * source edits at all). What changes here is the OUTPUT MODEL, and that change is
 * the point of doing this natively:
 *
 *   webaudio-node today: a JS `setTimeout(..., 10)` monitor loop renders ahead and
 *   pushes into an SDL queue, pre-rolling ~2 seconds. Its own README concedes
 *   1-2 SECONDS of latency and calls it "perfect for background music and
 *   non-interactive audio". For game SFX that is unusable — a jump sound arriving
 *   a second after the jump.
 *
 *   here: SDL calls us on its audio thread with a small buffer, and we render
 *   exactly that many frames on demand. Latency is the device buffer (~11 ms at
 *   512 frames / 44.1 kHz), and there is no JS timer in the path at all.
 *
 * THREADING: the callback runs on SDL's audio thread, NOT the JS thread. It must
 * never touch JSValues or call into QuickJS — QuickJS is single-threaded and any
 * JS access from here is a data race. It only calls processGraph(), which is plain
 * C++ operating on the engine's own state, under a mutex that the JS thread also
 * takes when it mutates the graph.
 */
#include "host.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* webaudio-node's C ABI (compiled natively; see CMakeLists.txt). */
/* The real signature has FOUR parameters. Declaring two meant buffer_size and
   is_realtime were read from whatever happened to be in those registers, and
   is_realtime gates time handling all through the engine. */
extern int   createAudioGraph(int sample_rate, int channels, int buffer_size,
                             bool is_realtime);
extern void  destroyAudioGraph(int graph_id);
extern int   createNode(int graph_id, const char *type);
extern void  connectNodes(int graph_id, int src, int dst);
extern void  disconnectNodes(int graph_id, int src);
extern void  processGraph(int graph_id, float *out, int num_frames);
extern void  setNodeParameter(int graph_id, int node_id, int param_id, float value);
extern void  startNode(int graph_id, int node_id, double when);
extern void  stopNode(int graph_id, int node_id, double when);
extern int   registerBuffer(int graph_id, float *data, int length, int channels, int sample_rate);
extern void  setNodeBufferId(int graph_id, int node_id, int buffer_id);
extern void  setWaveShaperCurve(int graph_id, int node_id, float *curve, int len);
extern void  setWaveShaperOversample(int graph_id, int node_id, const char *mode);
extern void  setIIRFilterCoefficients(int graph_id, int node_id,
                                      float *feedforward, int ff_len,
                                      float *feedback, int fb_len);
extern double getGraphCurrentTime(int graph_id);
extern void  setGraphCurrentTime(int graph_id, double t);
extern void  scheduleParamEvent(int graph_id, int node_id, int param_id,
                                int event_type, float value, double time, double duration);

#define AUDIO_SAMPLE_RATE  44100
#define AUDIO_CHANNELS     2
#define AUDIO_BUFFER_FRAMES 512   /* ~11.6 ms: small enough for SFX, large enough to be safe */

typedef struct {
    SDL_AudioDeviceID dev;
    SDL_mutex *lock;         /* guards graph mutation vs. the render callback */
    int graph;
    int sample_rate;
    int channels;
    bool running;
    uint64_t underruns;      /* callbacks that could not be served; must stay 0 */
} AudioState;

static AudioState g_audio;

/*
 * SDL audio callback. Runs on SDL's audio thread.
 * Absolutely no JS here — see the threading note at the top of this file.
 */
static void audio_callback(void *userdata, Uint8 *stream, int len_bytes)
{
    AudioState *a = (AudioState *)userdata;
    const int frames = len_bytes / (int)(sizeof(float) * (size_t)a->channels);
    float *out = (float *)stream;

    if (!a->running || a->graph < 0) {
        memset(stream, 0, (size_t)len_bytes);
        return;
    }

    /* try-lock, not lock: blocking the audio thread behind the JS thread is how
       you get a glitch instead of a late sample. A missed buffer is silence for
       11 ms and a counter increment, which the soak test asserts stays at zero. */
    if (SDL_TryLockMutex(a->lock) != 0) {
        memset(stream, 0, (size_t)len_bytes);
        a->underruns++;
        return;
    }

    processGraph(a->graph, out, frames);
    SDL_UnlockMutex(a->lock);
}

/* ------------------------------------------------------------------ bindings -- */

static JSValue js_audio_init(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (g_audio.running) return JS_NewInt32(ctx, g_audio.sample_rate);

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        return jsglq_throw(ctx, "audio init failed: %s", SDL_GetError());
    }

    g_audio.lock = SDL_CreateMutex();
    if (!g_audio.lock) return jsglq_throw(ctx, "audio mutex creation failed");

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = AUDIO_CHANNELS;
    want.samples = AUDIO_BUFFER_FRAMES;
    want.callback = audio_callback;
    want.userdata = &g_audio;

    g_audio.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_audio.dev) {
        return jsglq_throw(ctx, "could not open audio device: %s", SDL_GetError());
    }

    g_audio.sample_rate = have.freq;
    g_audio.channels = have.channels;
    /* is_realtime = true: this graph is driven by the SDL device callback,
       so the host advances time rather than the engine. */
    g_audio.graph = createAudioGraph(have.freq, have.channels, have.samples, true);
    if (g_audio.graph < 0) {
        SDL_CloseAudioDevice(g_audio.dev);
        return jsglq_throw(ctx, "audio graph creation failed");
    }

    g_audio.running = true;
    SDL_PauseAudioDevice(g_audio.dev, 0);
    return JS_NewInt32(ctx, g_audio.sample_rate);
}

/* Every graph mutation from JS takes the lock, pairing with the callback's try-lock. */
#define WITH_GRAPH_LOCK(body) do {              \
        if (!g_audio.running) return JS_UNDEFINED; \
        SDL_LockMutex(g_audio.lock);            \
        body;                                   \
        SDL_UnlockMutex(g_audio.lock);          \
    } while (0)

static JSValue js_create_node(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    if (!g_audio.running) return jsglq_throw(ctx, "audio not initialized");
    if (argc < 1) return jsglq_throw(ctx, "createNode(type) requires a type");

    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_EXCEPTION;

    SDL_LockMutex(g_audio.lock);
    int id = createNode(g_audio.graph, type);
    SDL_UnlockMutex(g_audio.lock);

    if (id < 0) {
        JSValue err = jsglq_throw(ctx, "unknown or unsupported audio node type '%s'", type);
        JS_FreeCString(ctx, type);
        return err;
    }
    JS_FreeCString(ctx, type);
    return JS_NewInt32(ctx, id);
}

static JSValue js_connect(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    if (argc < 2) return jsglq_throw(ctx, "connect(src, dst) requires two node ids");
    int32_t src = 0, dst = 0;
    JS_ToInt32(ctx, &src, argv[0]);
    JS_ToInt32(ctx, &dst, argv[1]);
    WITH_GRAPH_LOCK({ connectNodes(g_audio.graph, src, dst); });
    return JS_UNDEFINED;
}

static JSValue js_disconnect(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 1) return jsglq_throw(ctx, "disconnect(src) requires a node id");
    int32_t src = 0;
    JS_ToInt32(ctx, &src, argv[0]);
    WITH_GRAPH_LOCK({ disconnectNodes(g_audio.graph, src); });
    return JS_UNDEFINED;
}

static JSValue js_set_param(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 3) return jsglq_throw(ctx, "setParam(node, paramId, value)");
    int32_t node = 0, param = 0;
    double value = 0;
    JS_ToInt32(ctx, &node, argv[0]);
    JS_ToInt32(ctx, &param, argv[1]);
    JS_ToFloat64(ctx, &value, argv[2]);
    WITH_GRAPH_LOCK({ setNodeParameter(g_audio.graph, node, param, (float)value); });
    return JS_UNDEFINED;
}

static JSValue js_start_node(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 1) return jsglq_throw(ctx, "startNode(node, when?)");
    int32_t node = 0;
    double when = 0;
    JS_ToInt32(ctx, &node, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &when, argv[1]);
    WITH_GRAPH_LOCK({ startNode(g_audio.graph, node, when); });
    return JS_UNDEFINED;
}

static JSValue js_stop_node(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 1) return jsglq_throw(ctx, "stopNode(node, when?)");
    int32_t node = 0;
    double when = 0;
    JS_ToInt32(ctx, &node, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &when, argv[1]);
    WITH_GRAPH_LOCK({ stopNode(g_audio.graph, node, when); });
    return JS_UNDEFINED;
}

/* Register decoded PCM (Float32, interleaved or per-channel as the engine expects). */
static JSValue js_register_buffer(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    if (!g_audio.running) return jsglq_throw(ctx, "audio not initialized");
    if (argc < 3) return jsglq_throw(ctx, "registerBuffer(float32, channels, sampleRate)");

    size_t off = 0, blen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return jsglq_throw(ctx, "registerBuffer: first argument must be a Float32Array");
    }
    size_t total = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &total, abuf);
    JS_FreeValue(ctx, abuf);
    if (!base) return jsglq_throw(ctx, "registerBuffer: cannot access buffer");

    int32_t channels = 2, rate = g_audio.sample_rate;
    JS_ToInt32(ctx, &channels, argv[1]);
    JS_ToInt32(ctx, &rate, argv[2]);
    if (channels <= 0 || channels > 32)
        return jsglq_throw_range(ctx, "registerBuffer: channel count %d out of range", channels);

    const int length = (int)(blen / sizeof(float));
    SDL_LockMutex(g_audio.lock);
    int id = registerBuffer(g_audio.graph, (float *)(base + off), length, channels, rate);
    SDL_UnlockMutex(g_audio.lock);

    if (id < 0) return jsglq_throw(ctx, "registerBuffer failed (%d frames)", length);
    return JS_NewInt32(ctx, id);
}

static JSValue js_set_node_buffer(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    if (argc < 2) return jsglq_throw(ctx, "setNodeBuffer(node, bufferId)");
    int32_t node = 0, buf = 0;
    JS_ToInt32(ctx, &node, argv[0]);
    JS_ToInt32(ctx, &buf, argv[1]);
    WITH_GRAPH_LOCK({ setNodeBufferId(g_audio.graph, node, buf); });
    return JS_UNDEFINED;
}

/* Borrow a Float32Array's backing memory. Returns NULL and leaves an exception
   pending when the argument is not one. The pointer is only valid until JS runs
   again, which is fine: every caller hands it straight to the engine, which
   copies. */
static float *float32_data(JSContext *ctx, JSValueConst v, int *out_len)
{
    size_t off = 0, blen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
    if (JS_IsException(abuf)) {
        /* JS_GetException RETURNS the value; dropping it without freeing leaks
           one JSValue per failed call. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }
    size_t total = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &total, abuf);
    JS_FreeValue(ctx, abuf);
    if (!base) return NULL;
    *out_len = (int)(blen / sizeof(float));
    return (float *)(base + off);
}

static JSValue js_set_waveshaper_curve(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (!g_audio.running) return jsglq_throw(ctx, "audio not initialized");
    if (argc < 2) return jsglq_throw(ctx, "setWaveShaperCurve(node, Float32Array)");
    int32_t node = 0;
    JS_ToInt32(ctx, &node, argv[0]);

    /* A null curve is legal in the spec and means "pass through". */
    if (JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        WITH_GRAPH_LOCK({ setWaveShaperCurve(g_audio.graph, node, NULL, 0); });
        return JS_UNDEFINED;
    }
    int len = 0;
    float *data = float32_data(ctx, argv[1], &len);
    if (!data) return jsglq_throw(ctx, "setWaveShaperCurve: curve must be a Float32Array");
    WITH_GRAPH_LOCK({ setWaveShaperCurve(g_audio.graph, node, data, len); });
    return JS_UNDEFINED;
}

static JSValue js_set_waveshaper_oversample(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    if (!g_audio.running) return jsglq_throw(ctx, "audio not initialized");
    if (argc < 2) return jsglq_throw(ctx, "setWaveShaperOversample(node, mode)");
    int32_t node = 0;
    JS_ToInt32(ctx, &node, argv[0]);
    const char *mode = JS_ToCString(ctx, argv[1]);
    if (!mode) return JS_EXCEPTION;
    WITH_GRAPH_LOCK({ setWaveShaperOversample(g_audio.graph, node, mode); });
    JS_FreeCString(ctx, mode);
    return JS_UNDEFINED;
}

static JSValue js_set_iir_coefficients(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (!g_audio.running) return jsglq_throw(ctx, "audio not initialized");
    if (argc < 3)
        return jsglq_throw(ctx, "setIIRCoefficients(node, feedforward, feedback)");
    int32_t node = 0;
    JS_ToInt32(ctx, &node, argv[0]);

    int ff_len = 0, fb_len = 0;
    float *ff = float32_data(ctx, argv[1], &ff_len);
    if (!ff) return jsglq_throw(ctx, "setIIRCoefficients: feedforward must be a Float32Array");
    float *fb = float32_data(ctx, argv[2], &fb_len);
    if (!fb) return jsglq_throw(ctx, "setIIRCoefficients: feedback must be a Float32Array");

    /* The spec requires a non-zero leading feedback coefficient; a zero there
       divides by zero in the filter and produces silence or NaNs downstream,
       which is far harder to diagnose than a throw here. */
    if (fb_len < 1 || fb[0] == 0.0f)
        return jsglq_throw(ctx, "setIIRCoefficients: feedback[0] must be non-zero");

    WITH_GRAPH_LOCK({
        setIIRFilterCoefficients(g_audio.graph, node, ff, ff_len, fb, fb_len);
    });
    return JS_UNDEFINED;
}

/*
 * Offline rendering.
 *
 * renderOffline(sampleRate, channels, frames) -> Float32Array (interleaved)
 *
 * Builds a SEPARATE graph with is_realtime=false, so the engine advances its own
 * clock instead of waiting on a device callback, renders the requested frames in
 * 128-sample quanta (the Web Audio quantum the engine pre-sizes its buffers for),
 * and returns the result.
 *
 * This is what makes the audio graph testable at all: without a way to capture
 * output, "the node was created" is the only thing a test can assert, and this
 * project has already shipped three node types that created successfully and
 * processed nothing.
 */
static JSValue js_render_offline(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3) return jsglq_throw(ctx, "renderOffline(sampleRate, channels, frames)");

    int32_t rate = 44100, channels = 2, frames = 0;
    if (JS_ToInt32(ctx, &rate, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &channels, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &frames, argv[2])) return JS_EXCEPTION;

    if (rate <= 0 || channels <= 0 || channels > 32 || frames <= 0)
        return jsglq_throw_range(ctx, "renderOffline: bad rate/channels/frames");
    /* Guard the allocation rather than trusting the caller: frames comes from JS
       and a large value would otherwise be an unbounded malloc. 10 minutes of
       stereo at 48k is ~230 MB, well past anything a game needs offline. */
    if ((int64_t)frames * channels > (int64_t)(120 << 20))
        return jsglq_throw_range(ctx, "renderOffline: request too large");

    if (g_audio.graph < 0) return jsglq_throw(ctx, "audio not initialized");

    size_t total = (size_t)frames * (size_t)channels;
    float *out = (float *)calloc(total, sizeof(float));
    if (!out) return jsglq_throw(ctx, "renderOffline: out of memory");

    /* Render the LIVE graph in 128-frame quanta, the size the engine pre-sizes
       its scratch buffers for. Held under the same lock the device callback
       takes, so a concurrent callback cannot process the graph mid-render. */
    const int QUANTUM = 128;
    SDL_LockMutex(g_audio.lock);
    for (int done = 0; done < frames; done += QUANTUM) {
        int n = frames - done;
        if (n > QUANTUM) n = QUANTUM;
        processGraph(g_audio.graph, out + (size_t)done * (size_t)channels, n);
    }
    SDL_UnlockMutex(g_audio.lock);

    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)out,
                                       total * sizeof(float));
    free(out);
    if (JS_IsException(ab)) return ab;

    /* Hand back a Float32Array rather than a raw ArrayBuffer so callers can read
       samples without constructing a view themselves. */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Float32Array");
    JS_FreeValue(ctx, global);
    JSValue arr = JS_CallConstructor(ctx, ctor, 1, &ab);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, ab);
    return arr;
}

static JSValue js_current_time(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (!g_audio.running) return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, getGraphCurrentTime(g_audio.graph));
}

static JSValue js_schedule_param(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    if (argc < 5) return jsglq_throw(ctx,
        "scheduleParam(node, paramId, eventType, value, time, duration?)");
    int32_t node = 0, param = 0, type = 0;
    double value = 0, when = 0, dur = 0;
    JS_ToInt32(ctx, &node, argv[0]);
    JS_ToInt32(ctx, &param, argv[1]);
    JS_ToInt32(ctx, &type, argv[2]);
    JS_ToFloat64(ctx, &value, argv[3]);
    JS_ToFloat64(ctx, &when, argv[4]);
    if (argc >= 6) JS_ToFloat64(ctx, &dur, argv[5]);
    WITH_GRAPH_LOCK({
        scheduleParamEvent(g_audio.graph, node, param, type, (float)value, when, dur);
    });
    return JS_UNDEFINED;
}

/* Diagnostics the soak test asserts on. */
static JSValue js_audio_stats(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "running", JS_NewBool(ctx, g_audio.running));
    JS_SetPropertyStr(ctx, o, "sampleRate", JS_NewInt32(ctx, g_audio.sample_rate));
    JS_SetPropertyStr(ctx, o, "channels", JS_NewInt32(ctx, g_audio.channels));
    JS_SetPropertyStr(ctx, o, "bufferFrames", JS_NewInt32(ctx, AUDIO_BUFFER_FRAMES));
    JS_SetPropertyStr(ctx, o, "latencyMs", JS_NewFloat64(ctx,
        g_audio.sample_rate ? (AUDIO_BUFFER_FRAMES * 1000.0 / g_audio.sample_rate) : 0));
    JS_SetPropertyStr(ctx, o, "underruns", JS_NewInt64(ctx, (int64_t)g_audio.underruns));
    return o;
}

int jsglq_bind_audio(JsglqEngine *e)
{
    JSContext *ctx = jsglq_engine_ctx(e);
    JSValue global = JS_GetGlobalObject(ctx);

    g_audio.graph = -1;

    JSValue a = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, a, "init", JS_NewCFunction(ctx, js_audio_init, "init", 0));
    JS_SetPropertyStr(ctx, a, "createNode", JS_NewCFunction(ctx, js_create_node, "createNode", 1));
    JS_SetPropertyStr(ctx, a, "connect", JS_NewCFunction(ctx, js_connect, "connect", 2));
    JS_SetPropertyStr(ctx, a, "disconnect", JS_NewCFunction(ctx, js_disconnect, "disconnect", 1));
    JS_SetPropertyStr(ctx, a, "setParam", JS_NewCFunction(ctx, js_set_param, "setParam", 3));
    JS_SetPropertyStr(ctx, a, "startNode", JS_NewCFunction(ctx, js_start_node, "startNode", 2));
    JS_SetPropertyStr(ctx, a, "stopNode", JS_NewCFunction(ctx, js_stop_node, "stopNode", 2));
    JS_SetPropertyStr(ctx, a, "registerBuffer",
        JS_NewCFunction(ctx, js_register_buffer, "registerBuffer", 3));
    JS_SetPropertyStr(ctx, a, "setNodeBuffer",
        JS_NewCFunction(ctx, js_set_node_buffer, "setNodeBuffer", 2));
    JS_SetPropertyStr(ctx, a, "setWaveShaperCurve",
        JS_NewCFunction(ctx, js_set_waveshaper_curve, "setWaveShaperCurve", 2));
    JS_SetPropertyStr(ctx, a, "setWaveShaperOversample",
        JS_NewCFunction(ctx, js_set_waveshaper_oversample, "setWaveShaperOversample", 2));
    JS_SetPropertyStr(ctx, a, "setIIRCoefficients",
        JS_NewCFunction(ctx, js_set_iir_coefficients, "setIIRCoefficients", 3));
    JS_SetPropertyStr(ctx, a, "renderOffline",
        JS_NewCFunction(ctx, js_render_offline, "renderOffline", 3));
    JS_SetPropertyStr(ctx, a, "currentTime",
        JS_NewCFunction(ctx, js_current_time, "currentTime", 0));
    JS_SetPropertyStr(ctx, a, "scheduleParam",
        JS_NewCFunction(ctx, js_schedule_param, "scheduleParam", 6));
    JS_SetPropertyStr(ctx, a, "stats", JS_NewCFunction(ctx, js_audio_stats, "stats", 0));

    JS_SetPropertyStr(ctx, global, "__jsglq_audio", a);
    JS_FreeValue(ctx, global);
    return 0;
}

void jsglq_audio_shutdown(void)
{
    if (!g_audio.running) return;
    g_audio.running = false;
    SDL_PauseAudioDevice(g_audio.dev, 1);
    SDL_CloseAudioDevice(g_audio.dev);
    if (g_audio.graph >= 0) destroyAudioGraph(g_audio.graph);
    if (g_audio.lock) SDL_DestroyMutex(g_audio.lock);
    g_audio.graph = -1;
    g_audio.lock = NULL;
}

/* ---------------------------------------------------------- audio decoding -- */

/*
 * webaudio-node's unified decoder: format is detected from magic bytes and the
 * output is interleaved float32. Handles mp3/wav/flac/ogg/aac/opus.
 * Returns the channel count, or -1 on failure.
 */
extern int decodeAudio(const uint8_t *input, size_t input_size, float **output,
                       size_t *total_samples, int *sample_rate);

/*
 * __jsglq_decodeAudio(arrayBuffer) -> { pcm, channels, frames, sampleRate }
 *
 * Throws on failure rather than returning null: a game whose music silently fails
 * to decode plays in silence with nothing to debug, and "no audio" is one of the
 * hardest symptoms to trace back to its cause.
 */
static JSValue js_decode_audio(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (argc < 1) return jsglq_throw(ctx, "__jsglq_decodeAudio(bytes) requires a buffer");

    size_t len = 0;
    uint8_t *data = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!data) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t off = 0, blen = 0, bpe = 0;
        JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
        if (JS_IsException(buf)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return jsglq_throw(ctx, "__jsglq_decodeAudio: not an ArrayBuffer or typed array");
        }
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
        JS_FreeValue(ctx, buf);
        if (!base) return jsglq_throw(ctx, "__jsglq_decodeAudio: cannot access buffer");
        data = base + off;
        len = blen;
    }
    if (len < 4) return jsglq_throw(ctx, "__jsglq_decodeAudio: buffer too small (%zu bytes)", len);

    /*
     * Decoded-audio budget.
     *
     * Decoding is synchronous and expensive in BOTH time and memory: a 194-second
     * stereo OGG measures 261 ms and 71 MB of float32 here. A game loading eight
     * such tracks at startup (space3d does) would block for ~2 s and allocate
     * ~570 MB, which is what made it appear to hang with no error.
     *
     * A budget converts that into a NAMED failure a developer can act on, instead
     * of a stall followed by an out-of-memory somewhere unrelated. Games that keep
     * their music reasonable never see it.
     */
    static size_t decoded_bytes_total = 0;
    const size_t DECODE_BUDGET = (size_t)768 * 1024 * 1024;

    float *pcm = NULL;
    size_t total_samples = 0;
    int rate = 0;
    int channels = decodeAudio(data, len, &pcm, &total_samples, &rate);

    if (channels < 1 || !pcm || total_samples == 0) {
        if (pcm) free(pcm);
        return jsglq_throw(ctx,
            "audio decode failed (%zu bytes). Supported: mp3, wav, flac, ogg, aac, opus.",
            len);
    }
    if (rate <= 0) rate = g_audio.sample_rate > 0 ? g_audio.sample_rate : 44100;

    const size_t pcm_bytes = total_samples * sizeof(float);
    if (decoded_bytes_total + pcm_bytes > DECODE_BUDGET) {
        free(pcm);
        return jsglq_throw(ctx,
            "decoded audio budget exceeded: this clip needs %zu MB and %zu MB are "
            "already decoded (limit %zu MB). Long music tracks decode to a lot of "
            "float32 (a 194s stereo track is ~71 MB); stream or shorten them, or "
            "decode fewer at once.",
            pcm_bytes / (1024 * 1024), decoded_bytes_total / (1024 * 1024),
            DECODE_BUDGET / (1024 * 1024));
    }
    decoded_bytes_total += pcm_bytes;

    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)pcm,
                                       total_samples * sizeof(float));
    free(pcm);

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "pcm", ab);
    JS_SetPropertyStr(ctx, out, "channels", JS_NewInt32(ctx, channels));
    JS_SetPropertyStr(ctx, out, "frames",
                      JS_NewInt64(ctx, (int64_t)(total_samples / (size_t)channels)));
    JS_SetPropertyStr(ctx, out, "sampleRate", JS_NewInt32(ctx, rate));
    return out;
}

int jsglq_bind_audio_decode(JsglqEngine *e)
{
    JSContext *ctx = jsglq_engine_ctx(e);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__jsglq_decodeAudio",
        JS_NewCFunction(ctx, js_decode_audio, "__jsglq_decodeAudio", 1));
    JS_FreeValue(ctx, global);
    return 0;
}
