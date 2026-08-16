/*
 * Window, GL context, and the frame loop.
 *
 * SDL2 is called DIRECTLY from the host rather than through @kmamal/sdl. That drops
 * the most Node-coupled dependency in the stack (its JS side needs worker_threads,
 * EventEmitter, and setInterval polling) and, just as usefully, removes the fragile
 * "create the EGL context BEFORE SDL, then re-makeCurrent because SDL clobbered it"
 * ordering dance that both native-gles test repos document.
 *
 * native-gles's egl_context.cpp is compiled verbatim: it contains zero Napi and is
 * already an engine-agnostic C API over EGL.
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <stdio.h>
#include <string.h>

#include "../../native-gles/src/egl_context.h"

extern "C" {
#include "host.h"

/* events.c */
void jsglq_events_key(JSContext *ctx, const SDL_KeyboardEvent *k, bool down);
void jsglq_events_mouse_button(JSContext *ctx, const SDL_MouseButtonEvent *m, bool down);
void jsglq_events_mouse_motion(JSContext *ctx, const SDL_MouseMotionEvent *m);
void jsglq_events_wheel(JSContext *ctx, const SDL_MouseWheelEvent *w);
void jsglq_events_resize(JSContext *ctx, int width, int height);

/* bind_gamepad.c */
void jsglq_gamepad_device_event(const SDL_Event *ev);
}

struct JsglqWindow {
    SDL_Window *sdl = nullptr;
    GLESContext gl {};
    void *native_handle = nullptr;
    int width = 0, height = 0;
    bool gl_ready = false;
};

static JsglqWindow g_win;

extern "C" void *jsglq_window_native_handle(void)
{
    return g_win.native_handle;
}

extern "C" int jsglq_window_open(int width, int height, const char *title,
                                 bool headless, bool fullscreen)
{
#ifdef SDL_MAIN_HANDLED
    /* We keep our own main() (see SDL_MAIN_HANDLED in CMakeLists), so SDL never
       ran its entry-point setup. This tells it that was deliberate; without it
       SDL_Init refuses to start on Windows. */
    SDL_SetMainReady();
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "jsglq: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    g_win.width = width;
    g_win.height = height;

    if (headless) {
        /* Offscreen: pbuffer only, no window. Used by CI and by the differ when no
           display is available. Rendering is otherwise identical. */
        if (!gles_context_create(&g_win.gl, width, height, false, nullptr)) {
            fprintf(stderr, "jsglq: offscreen GL context creation failed\n");
            return -1;
        }
        g_win.gl_ready = true;
        gles_context_make_current(&g_win.gl);
        return 0;
    }

    /* Borderless-desktop fullscreen rather than a mode switch: it is instant, it
       cannot leave the display in a broken mode if the process dies, and it is what
       every modern game defaults to. */
    Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    g_win.sdl = SDL_CreateWindow(title ? title : "jsgamelauncher-quickjs",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 width, height, flags);
    if (!g_win.sdl) {
        fprintf(stderr, "jsglq: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Fullscreen gives the desktop size, not the requested one; everything
       downstream (GL surface, canvas, viewport) must use what we actually got. */
    if (fullscreen) {
        SDL_GetWindowSize(g_win.sdl, &width, &height);
        g_win.width = width;
        g_win.height = height;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_win.sdl, &info)) {
        fprintf(stderr, "jsglq: SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return -1;
    }

#if defined(SDL_VIDEO_DRIVER_X11)
    if (info.subsystem == SDL_SYSWM_X11) {
        g_win.native_handle = (void *)(uintptr_t)info.info.x11.window;
    }
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    if (info.subsystem == SDL_SYSWM_WAYLAND) {
        /* native-gles binds the X11 EGL platform explicitly (Mesa prefers Wayland
           when both are present and then rejects an X11 XID), so under Wayland we
           run through XWayland rather than a native Wayland surface. */
        g_win.native_handle = (void *)info.info.wl.surface;
    }
#endif

    if (!gles_context_create(&g_win.gl, width, height, true, g_win.native_handle)) {
        fprintf(stderr, "jsglq: windowed GL context creation failed\n");
        return -1;
    }
    g_win.gl_ready = true;
    gles_context_make_current(&g_win.gl);

    /*
     * Swap interval 0, always.
     *
     * The driver default of 1 parks the calling thread inside swapBuffers until
     * vblank — measured at ~33 ms/frame in native-gles's own README — and on a
     * single-threaded host that blocks the entire loop. Pacing is done explicitly
     * by the frame loop instead (see loop.c), which is also what lets uncapped
     * bench mode exist at all.
     */
    gles_context_set_swap_interval(&g_win.gl, 0);
    return 0;
}

extern "C" void jsglq_window_swap(void)
{
    if (g_win.gl_ready) gles_context_swap(&g_win.gl);
}

extern "C" void jsglq_window_size(int *w, int *h)
{
    if (w) *w = g_win.width;
    if (h) *h = g_win.height;
}

extern "C" bool jsglq_window_make_current(void)
{
    return g_win.gl_ready && gles_context_make_current(&g_win.gl);
}

extern "C" void jsglq_window_close(void)
{
    if (g_win.gl_ready) { gles_context_destroy(&g_win.gl); g_win.gl_ready = false; }
    if (g_win.sdl) { SDL_DestroyWindow(g_win.sdl); g_win.sdl = nullptr; }
    SDL_Quit();
}

/* Returns false when the user closed the window. */
extern "C" bool jsglq_window_pump_ctx(JSContext *ctx)
{
    SDL_Event ev;
    bool running = true;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            running = false;
            break;

        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                g_win.width = ev.window.data1;
                g_win.height = ev.window.data2;
                gles_context_resize(&g_win.gl, g_win.width, g_win.height);
                if (ctx) jsglq_events_resize(ctx, g_win.width, g_win.height);
            }
            break;

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (ctx) jsglq_events_key(ctx, &ev.key, ev.type == SDL_KEYDOWN);
            break;

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (ctx) jsglq_events_mouse_button(ctx, &ev.button, ev.type == SDL_MOUSEBUTTONDOWN);
            break;

        case SDL_MOUSEMOTION:
            if (ctx) jsglq_events_mouse_motion(ctx, &ev.motion);
            break;

        case SDL_MOUSEWHEEL:
            if (ctx) jsglq_events_wheel(ctx, &ev.wheel);
            break;

        /* Hotplug. Gamepad STATE is polled rather than event-driven (the Gamepad
           API is a polling API), but connect/disconnect has to be handled here
           or a pad plugged in mid-game is never opened. */
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED:
            jsglq_gamepad_device_event(&ev);
            break;

        default:
            break;
        }
    }
    return running;
}

extern "C" bool jsglq_window_pump(void) { return jsglq_window_pump_ctx(NULL); }
