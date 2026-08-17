/*
 * jsgamelauncher-quickjs — host entry point.
 *
 * Usage: jsglq [options] <game-dir | game.js>
 *
 * The frame loop implements the pacing rules learned the hard way in
 * jsgame-libretro (doc/dev_notes.md), which rungame never got:
 *
 *   - swap interval is 0 and pacing is explicit. Interval 1 blocks the calling
 *     thread inside swapBuffers (~33 ms measured), which on a single-threaded host
 *     stalls everything including audio.
 *   - a MEASURED-INTERVAL floor guard, not a fixed sleep: sleep only when the real
 *     elapsed interval is under budget, then busy-trim the last ~1.5 ms because
 *     nanosleep overshoots at kernel tick granularity.
 *   - the rAF timestamp comes from one host-owned monotonic clock. A game must
 *     never read a second clock for timing, or fast-forward and pause break.
 */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include "platform.h"

/* window.cpp */
int  jsglq_window_open(int width, int height, const char *title,
                       bool headless, bool fullscreen);
void jsglq_window_swap(void);
void jsglq_window_close(void);
bool jsglq_window_pump(void);
bool jsglq_window_pump_ctx(JSContext *ctx);
bool jsglq_window_make_current(void);
void jsglq_window_size(int *w, int *h);

/* bind_gl.cpp */
int jsglq_bind_gl_object(JSContext *ctx, JSValue global);

/* bind_core.c */
int jsglq_core_pump_raf(JSContext *ctx, double timestamp_ms);
void jsglq_core_pump_timers(JSContext *ctx);

#define TARGET_HZ        60.0
#define FRAME_BUDGET_MS  (1000.0 / TARGET_HZ)
#define PACING_FLOOR_MS  14.0     /* only sleep if the measured interval is under this */
#define BUSY_TRIM_MS     1.5      /* nanosleep overshoots; spin the last stretch */
#define MICROTASK_MS     4.0      /* time-bounded drain, not count-bounded */
#define WATCHDOG_MS      2000.0

typedef struct {
    const char *game_path;
    const char *title;
    int width, height;
    bool headless;
    bool uncapped;
    bool fullscreen;
    bool quiet;
    uint64_t max_frames;
    double max_seconds;   /* hard wall-clock cap; 0 = unlimited */
} Options;

#define JSGLQ_VERSION "0.1.1"

/*
 * Prove the binary can actually START.
 *
 * --version prints compiled-in strings and touches nothing, so it passes on a
 * build whose SDL2 or ANGLE cannot be loaded at all. That is not hypothetical:
 * the 0.1.0 macOS archive shipped linked against a Homebrew absolute path and
 * died in the dynamic loader on every machine but the build runner, while CI
 * reported the binary "verified to load and report itself".
 *
 * This opens a real headless window and GL context, which forces the loader to
 * resolve every dependency and the driver to hand back a live context.
 */
static int run_self_check(void)
{
    if (jsglq_window_open(64, 64, "jsglq --check", /*headless=*/true,
                          /*fullscreen=*/false) != 0) {
        fprintf(stderr, "jsglq: self-check FAILED: could not create a GL context\n");
        return 1;
    }
    if (!jsglq_window_make_current()) {
        fprintf(stderr, "jsglq: self-check FAILED: could not make the context current\n");
        jsglq_window_close();
        return 1;
    }
    int w = 0, h = 0;
    jsglq_window_size(&w, &h);
    jsglq_window_close();

    if (w <= 0 || h <= 0) {
        fprintf(stderr, "jsglq: self-check FAILED: context reported %dx%d\n", w, h);
        return 1;
    }
    printf("jsglq: self-check OK (headless GL context %dx%d)\n", w, h);
    return 0;
}

static void print_version(void)
{
    printf("jsgamelauncher-quickjs %s\n", JSGLQ_VERSION);
    printf("  engine:   quickjs-ng %d.%d.%d\n",
           QJS_VERSION_MAJOR, QJS_VERSION_MINOR, QJS_VERSION_PATCH);
    printf("  graphics: OpenGL ES 3.0 via EGL (native-gles)\n");
    printf("  audio:    webaudio-node DSP engine, SDL2 callback\n");
}

/*
 * Print the embedded components and their licenses.
 *
 * Someone fusing a commercial game into this binary needs to know what is inside
 * it, and "read the repo" is a poor answer once the binary is the only artifact
 * they have. This makes it answerable from the shipped executable.
 */
static void print_licenses(void)
{
    printf(
        "jsgamelauncher-quickjs " JSGLQ_VERSION " — embedded components\n"
        "\n"
        "  This launcher is MIT licensed. Every component below permits commercial\n"
        "  use and redistribution; the only obligation is attribution.\n"
        "\n"
        "  quickjs-ng          MIT           JavaScript engine\n"
        "  SDL2                zlib          window, input, audio device\n"
        "  stb_image           public domain image decoding\n"
        "  stb_truetype        public domain font rasterization\n"
        "  native-gles         ISC           EGL + OpenGL ES 3.0\n"
        "  webgl-node          ISC           WebGL2 layer\n"
        "  webaudio-node       ISC           Web Audio DSP engine\n"
        "  dr_mp3/wav/flac     public domain audio decoders\n"
        "  stb_vorbis          public domain OGG Vorbis decoding\n"
        "  DejaVu Sans (subset) Bitstream Vera + Arev — embedded fallback font\n"
        "\n"
        "  DejaVu Sans: Copyright (c) 2003 Bitstream, Inc. Bitstream Vera is a\n"
        "  trademark of Bitstream, Inc. Copyright (c) 2006 Tavmjong Bah.\n"
        "  Embedding, redistribution and subsetting are permitted; the font may not\n"
        "  be sold by itself. Full text: https://dejavu-fonts.github.io/License.html\n"
        "\n"
        "  On macOS and Windows, ANGLE (BSD-3-Clause) ships alongside the binary.\n"
        "\n"
        "  Shipping a game? Include THIRD-PARTY.md with your distribution. Nothing\n"
        "  here requires you to open-source your game or pay a royalty.\n");
}

static void usage(void)
{
    printf(
        "jsgamelauncher-quickjs " JSGLQ_VERSION " — browser-API games, no browser, no Node\n"
        "\n"
        "USAGE\n"
        "  jsglq [options] <game-dir|entry.js>\n"
        "\n"
        "  A game directory needs an entry point: package.json \"main\", or one of\n"
        "  main.js, index.js, src/main.js, src/index.js, game.js.\n"
        "\n"
        "  A FUSED binary carries its own game and takes no path argument.\n"
        "\n"
        "DISPLAY\n"
        "  --width=N --height=N   surface size (default 960x540)\n"
        "  --fullscreen           borderless fullscreen at the desktop resolution\n"
        "  --title=TEXT           window title (default: the game's directory name)\n"
        "  --headless             render offscreen, open no window\n"
        "\n"
        "TIMING\n"
        "  --uncapped             no frame pacing; run as fast as possible\n"
        "  --frames=N             stop after N frames\n"
        "  --max-seconds=S        hard wall-clock cap; a stalled game still exits\n"
        "\n"
        "OTHER\n"
        "  --quiet                suppress launcher chatter (game output still prints)\n"
        "  --version              print version and component details\n"
        "  --check                verify the binary can open a GL context and exit\n"
        "  --licenses             print embedded components and their licenses\n"
        "  --help                 this text\n"
        "\n"
        "EXAMPLES\n"
        "  jsglq ./my-game                    play a game directory\n"
        "  jsglq --fullscreen ./my-game       play fullscreen\n"
        "  jsglq --headless --frames=60 ./g   render 60 frames with no window (CI)\n");
}

static bool is_dir(const char *p) { return jsglq_is_dir(p); }

/* Entry resolution, matching the .jsgame contract rungame established. */
static bool resolve_entry(const char *game_path, char *dir_out, size_t dir_sz,
                          char *entry_out, size_t entry_sz)
{
    static const char *candidates[] = {
        "main.js", "index.js", "src/main.js", "src/index.js", "game.js", "src/game.js",
    };

    if (!is_dir(game_path)) {
        /* A file: its directory is the game root. */
        jsglq_dirname(game_path, dir_out, dir_sz);
        jsglq_basename(game_path, entry_out, entry_sz);
        return true;
    }

    snprintf(dir_out, dir_sz, "%s", game_path);

    /* package.json "main" wins, as in rungame (issue #9). */
    char pkg[4096];
    snprintf(pkg, sizeof(pkg), "%s/package.json", game_path);
    FILE *f = fopen(pkg, "rb");
    if (f) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        const char *m = strstr(buf, "\"main\"");
        if (m) {
            const char *c = strchr(m, ':');
            const char *q1 = c ? strchr(c, '"') : NULL;
            const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q2 && (size_t)(q2 - q1 - 1) < entry_sz) {
                memcpy(entry_out, q1 + 1, (size_t)(q2 - q1 - 1));
                entry_out[q2 - q1 - 1] = 0;
                char full[8192];
                snprintf(full, sizeof(full), "%s/%s", dir_out, entry_out);
                        if (jsglq_is_file(full)) return true;
            }
        }
    }

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char full[8192];
        snprintf(full, sizeof(full), "%s/%s", game_path, candidates[i]);
        if (jsglq_is_file(full)) {
            snprintf(entry_out, entry_sz, "%s", candidates[i]);
            return true;
        }
    }
    return false;
}

static void sleep_ms(double ms)
{
    jsglq_sleep_ms(ms);
}

/*
 * Frame pacing.
 *
 * Sleeps only when the measured interval since the previous frame is under the
 * floor. With vsync doing the work (or in uncapped mode) this sleeps nothing by
 * construction, which is why it cannot double-pace — the failure mode that pinned
 * jsgame-libretro at exactly 30fps.
 */
static void pace_frame(double frame_start_ms)
{
    const double elapsed = jsglq_now_ms() - frame_start_ms;
    if (elapsed >= PACING_FLOOR_MS) return;

    const double target = frame_start_ms + FRAME_BUDGET_MS;
    const double coarse = target - jsglq_now_ms() - BUSY_TRIM_MS;
    if (coarse > 0) sleep_ms(coarse);
    /* Busy-trim: nanosleep overshoots at kernel tick granularity, and overshooting
       by 1ms every frame is a 6% frame-rate error. */
    while (jsglq_now_ms() < target) { /* spin */ }
}

int main(int argc, char **argv)
{
    Options opt = { NULL, NULL, 960, 540, false, false, false, false, 0, 0.0 };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--width=", 8))        opt.width = atoi(a + 8);
        else if (!strncmp(a, "--height=", 9))  opt.height = atoi(a + 9);
        else if (!strcmp(a, "--headless"))     opt.headless = true;
        else if (!strcmp(a, "--uncapped"))     opt.uncapped = true;
        else if (!strcmp(a, "--fullscreen"))   opt.fullscreen = true;
        else if (!strcmp(a, "--quiet"))        opt.quiet = true;
        else if (!strncmp(a, "--title=", 8))   opt.title = a + 8;
        else if (!strcmp(a, "--version") || !strcmp(a, "-v")) { print_version(); return 0; }
        else if (!strcmp(a, "--licenses"))     { print_licenses(); return 0; }
        else if (!strcmp(a, "--check"))        return run_self_check();
        else if (!strncmp(a, "--frames=", 9))  opt.max_frames = strtoull(a + 9, NULL, 10);
        else if (!strncmp(a, "--max-seconds=", 14)) opt.max_seconds = atof(a + 14);
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (a[0] == '-') {
            /* Name the option AND point at --help: a bare "unknown option" leaves
               the user guessing which spellings exist. */
            fprintf(stderr, "jsglq: unknown option '%s'\nTry --help.\n", a);
            return 2;
        }
        else                                    opt.game_path = a;
    }

    /*
     * Fused mode: if a game is embedded in this executable, run it and ignore any
     * path argument. A fused binary IS the game — accepting a different path there
     * would make the same binary behave differently depending on how it was
     * invoked, which is exactly the surprise a single-file distribution exists to
     * avoid.
     */
    char fused_dir[4096];
    {
        char self[4096];
        if (jsglq_exe_path(self, sizeof(self))) {
            if (jsglq_bundle_prepare(self, fused_dir, sizeof(fused_dir))) {
                opt.game_path = fused_dir;
                /* The fused payload carries the runtime layer alongside the game,
                   so a fused binary works from any directory. */
                static char fused_runtime[4096];
                snprintf(fused_runtime, sizeof(fused_runtime),
                         "%s/.jsglq-runtime", fused_dir);
                jsglq_setenv("JSGLQ_RUNTIME_DIR", fused_runtime);
            }
        }
    }

    if (!opt.game_path) {
        fprintf(stderr, "jsglq: no game specified.\n\n");
        usage();
        return 2;
    }

    char game_dir[4096], entry[1024];
    if (!resolve_entry(opt.game_path, game_dir, sizeof(game_dir), entry, sizeof(entry))) {
        fprintf(stderr, "jsglq: no entry point found in '%s'\n"
                        "       looked for package.json main, main.js, index.js, "
                        "src/main.js, src/index.js, game.js\n", opt.game_path);
        return 1;
    }

    char title_buf[512];
    if (opt.title) {
        snprintf(title_buf, sizeof(title_buf), "%s", opt.title);
    } else {
        /* Default to the game's own name: a window called after the launcher tells
           the player nothing, especially with several open. */
        jsglq_basename(game_dir, title_buf, sizeof(title_buf));
    }

    if (jsglq_window_open(opt.width, opt.height, title_buf,
                          opt.headless, opt.fullscreen) != 0)
        return 1;

    int bw = opt.width, bh = opt.height;
    jsglq_window_size(&bw, &bh);

    /*
     * Locate the runtime JS layer. JSGLQ_RUNTIME_DIR overrides for development so
     * shim edits are a relaunch rather than a C rebuild — the same escape hatch
     * jsgame-libretro found essential.
     */
    char runtime_dir[4096];
    const char *env_rt = getenv("JSGLQ_RUNTIME_DIR");
    if (env_rt && *env_rt) {
        snprintf(runtime_dir, sizeof(runtime_dir), "%s", env_rt);
    } else {
        /*
         * Find the runtime layer, trying both real layouts.
         *
         * A release archive puts runtime/ BESIDE the binary; a dev build leaves the
         * binary in build/ with runtime/ one level up. Checking only the dev layout
         * means a downloaded release cannot start at all — which is exactly what a
         * packaging test caught here.
         */
        char exe[4096], d[4096];
        runtime_dir[0] = 0;
        if (jsglq_exe_path(exe, sizeof(exe))) {
            jsglq_dirname(exe, d, sizeof(d));

            char candidate[4096], probe[4096];

            /* Beside the binary (release archive). */
            snprintf(candidate, sizeof(candidate), "%s/runtime", d);
            snprintf(probe, sizeof(probe), "%s/bootstrap.js", candidate);
            if (jsglq_is_file(probe)) {
                snprintf(runtime_dir, sizeof(runtime_dir), "%s", candidate);
            }

            /* One level up (dev tree: build/jsglq + runtime/). */
            if (!runtime_dir[0]) {
                snprintf(candidate, sizeof(candidate), "%s/../runtime", d);
                snprintf(probe, sizeof(probe), "%s/bootstrap.js", candidate);
                if (jsglq_is_file(probe)) {
                    snprintf(runtime_dir, sizeof(runtime_dir), "%s", candidate);
                }
            }
        }
        if (!runtime_dir[0]) snprintf(runtime_dir, sizeof(runtime_dir), "./runtime");
    }

    JsglqConfig cfg = {
        .game_dir = game_dir,
        .entry_file = entry,
        .runtime_dir = runtime_dir,
        .bench_mode = false,
        .width = bw,
        .height = bh,
    };

    JsglqEngine *engine = jsglq_engine_new(&cfg);
    if (!engine) { jsglq_window_close(); return 1; }

    JSContext *ctx = jsglq_engine_ctx(engine);
    JSValue global = JS_GetGlobalObject(ctx);

    jsglq_bind_core(engine);
    jsglq_bind_gl_object(ctx, global);
    jsglq_bind_io(engine);
    jsglq_bind_image(engine);
    jsglq_bind_audio(engine);
    jsglq_bind_audio_decode(engine);
    jsglq_bind_worker(engine);
    jsglq_bind_gamepad(engine);
    jsglq_bind_websocket(engine);
    /* After binding (which opens already-connected pads) so the rescan inside
       the loader promotes them from raw joysticks to mapped controllers. */
    jsglq_gamepad_load_db_file(runtime_dir);
    if (jsglq_bind_canvas2d(engine, bw, bh) != 0) {
        JS_FreeValue(ctx, global);
        jsglq_engine_free(engine);
        jsglq_window_close();
        return 1;
    }
    JS_FreeValue(ctx, global);

    /* Assemble the browser globals before the game's first line runs. */
    if (jsglq_engine_run_bootstrap(engine, bw, bh) != 0) {
        fprintf(stderr, "jsglq: runtime bootstrap failed (runtime dir: %s)\n", runtime_dir);
        jsglq_engine_free(engine);
        jsglq_window_close();
        return 1;
    }

    if (jsglq_engine_run_entry(engine) != 0) {
        jsglq_engine_free(engine);
        jsglq_window_close();
        return 1;
    }

    uint64_t frame = 0;
    bool running = true;
    const double loop_started_ms = jsglq_now_ms();
    double last_frame_ms = jsglq_now_ms();

    while (running) {
        const double frame_start = jsglq_now_ms();

        if (!opt.headless) running = jsglq_window_pump_ctx(ctx);
        if (!running) break;

        jsglq_window_make_current();

        /* One clock, host-owned. A game reading any other clock for timing breaks
           fast-forward, pause, and deterministic replay. */
        jsglq_engine_arm_watchdog(engine, frame_start + WATCHDOG_MS);
        jsglq_canvas2d_begin_frame();
        jsglq_pump_workers(ctx);

        /*
         * Interleave timers and microtasks.
         *
         * `await new Promise(r => setTimeout(r, 0))` needs BOTH to advance one
         * link: the timer resolves the promise, then a microtask drain runs the
         * continuation, which schedules the next timer. Pumping each once per
         * frame advances such a chain by exactly one link per frame, so a loader
         * doing 60 of them takes ~1 SECOND instead of ~1 ms — and presents as a
         * game that loads a few assets and then appears to hang.
         *
         * Alternating within a shared budget lets a chain complete in one frame
         * while still bounding how long the frame can spend here.
         */
        const double io_deadline = jsglq_now_ms() + MICROTASK_MS;
        for (int round = 0; round < 32; round++) {
            jsglq_core_pump_timers(ctx);
            int jobs = jsglq_engine_drain_jobs(engine, 1.0);
            if (jobs == 0 && round > 0) break;
            if (jsglq_now_ms() >= io_deadline) break;
        }

        jsglq_core_pump_raf(ctx, frame_start);
        jsglq_engine_drain_jobs(engine, MICROTASK_MS);
        jsglq_canvas2d_end_frame();
        jsglq_engine_disarm_watchdog(engine);

        /*
         * Headless mode does NOT swap.
         *
         * Canvas 2D is a persistent surface: a game may draw on one frame and call
         * getImageData several frames later, and the spec says those pixels are
         * still there. Swapping flips to the other buffer, so the next read sees
         * a stale one — which shows up as "my drawing vanished" rather than as
         * anything resembling a buffer-management problem.
         *
         * With a real window we must present, so the buffers are swapped and the
         * renderer's own surface is the source of truth for readback (see
         * s2d_get_pixels, which reads before any swap).
         */
        if (!opt.headless) jsglq_window_swap();

        frame++;
        last_frame_ms = frame_start;

        if (jsglq_core_exit_requested()) break;
        if (opt.max_frames && frame >= opt.max_frames) break;
        /* Wall-clock cap: a game that stalls or spins must still terminate, and a
           frame count alone cannot guarantee that if frames stop advancing. */
        if (opt.max_seconds > 0 &&
            (jsglq_now_ms() - loop_started_ms) > opt.max_seconds * 1000.0) {
            fprintf(stderr, "jsglq: stopping after %.1fs wall-clock cap\n", opt.max_seconds);
            break;
        }
        if (!opt.uncapped) pace_frame(frame_start);
    }

    /* Drop every JSValue the bindings hold before tearing down the runtime, or
       QuickJS asserts that objects are still alive. */
    jsglq_audio_shutdown();
    jsglq_worker_shutdown(ctx);
    jsglq_core_shutdown(ctx);
    jsglq_engine_free(engine);
    jsglq_window_close();
    return 0;
}
