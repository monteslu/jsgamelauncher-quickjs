/*
 * Gamepad host layer.
 *
 * SDL owns both the devices AND the mapping. At init we feed SDL the community
 * controller database (SDL_GameControllerDB, zlib-licensed, ~2200 devices),
 * which turns almost every pad — including the no-name handheld controllers
 * this project targets — into a recognized SDL_GameController with the standard
 * layout. That layout is the same one the W3C Standard Gamepad defines, so the
 * JS side becomes a thin translation instead of a device database.
 *
 * This is why there is no controller-matching logic here or in JS: letting SDL
 * do it means the mapping data stays current by updating one text file, rather
 * than by maintaining our own GUID-matching heuristics.
 *
 * A raw joystick view is still exposed for devices even the DB does not cover,
 * so JS can fall back rather than report nothing.
 *
 * Everything is POLLED rather than event-driven. The Gamepad API is a polling
 * API — navigator.getGamepads() returns a snapshot — so keeping state in SDL and
 * reading it once per frame avoids an event queue that would have to be drained
 * and reconciled anyway.
 */
#include "host.h"

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* SDL supports far more, but a pad past the 8th on a handheld is not a real
   scenario, and a fixed array keeps this allocation-free on the frame path. */
#define MAX_PADS 8

typedef struct {
    SDL_JoystickID   id;          /* SDL's stable instance id, -1 when the slot is free */
    SDL_GameController *ctrl;     /* non-NULL when SDL recognized a standard layout */
    SDL_Joystick    *joy;         /* always set; the controller's underlying joystick */
    char             name[128];
    char             guid[64];
    bool             is_controller;
    bool             has_rumble;
} Pad;

static Pad  g_pads[MAX_PADS];
static bool g_inited = false;

static int slot_for_instance(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_PADS; i++) if (g_pads[i].id == id) return i;
    return -1;
}

static int free_slot(void)
{
    for (int i = 0; i < MAX_PADS; i++) if (g_pads[i].id == -1) return i;
    return -1;
}

static void pad_close(int slot)
{
    if (slot < 0 || slot >= MAX_PADS) return;
    Pad *p = &g_pads[slot];
    /* Closing the controller closes its joystick too; doing both is a double
       free. Only close the joystick when we opened it directly. */
    if (p->ctrl)      SDL_GameControllerClose(p->ctrl);
    else if (p->joy)  SDL_JoystickClose(p->joy);
    memset(p, 0, sizeof(*p));
    p->id = -1;
}

/* Open a device by its SDL device index (the value carried by DEVICEADDED). */
static void pad_open(int device_index)
{
    SDL_Joystick *joy = NULL;
    SDL_GameController *ctrl = NULL;

    if (SDL_IsGameController(device_index)) {
        ctrl = SDL_GameControllerOpen(device_index);
        if (ctrl) joy = SDL_GameControllerGetJoystick(ctrl);
    }
    if (!joy) {
        joy = SDL_JoystickOpen(device_index);
    }
    if (!joy) return;   /* device vanished between enumeration and open */

    SDL_JoystickID inst = SDL_JoystickInstanceID(joy);
    if (slot_for_instance(inst) >= 0) {   /* already open */
        if (ctrl) SDL_GameControllerClose(ctrl);
        else      SDL_JoystickClose(joy);
        return;
    }

    int slot = free_slot();
    if (slot < 0) {                        /* more pads than we track */
        if (ctrl) SDL_GameControllerClose(ctrl);
        else      SDL_JoystickClose(joy);
        return;
    }

    Pad *p = &g_pads[slot];
    p->id   = inst;
    p->ctrl = ctrl;
    p->joy  = joy;
    p->is_controller = (ctrl != NULL);

    const char *nm = ctrl ? SDL_GameControllerName(ctrl) : SDL_JoystickName(joy);
    snprintf(p->name, sizeof(p->name), "%s", nm ? nm : "Unknown Gamepad");

    /* The GUID is how the JS database identifies a device; it is stable across
       reconnects and machines in a way the name is not. */
    SDL_JoystickGUID guid = SDL_JoystickGetGUID(joy);
    SDL_JoystickGetGUIDString(guid, p->guid, sizeof(p->guid));

#if SDL_VERSION_ATLEAST(2, 0, 18)
    p->has_rumble = ctrl ? SDL_GameControllerHasRumble(ctrl)
                         : SDL_JoystickHasRumble(joy);
#else
    p->has_rumble = false;
#endif
}

/*
 * Feed SDL one mapping line from the community database.
 *
 * The modern database carries crc:/platform:/hint:/type: fields that SDL 2.x's
 * parser rejects with "Unexpected controller element", so they are stripped
 * here. Without this the DB still loads but emits a warning per line, which is
 * both noise and a signal that some entries were skipped.
 */
static int add_mapping_line(const char *line)
{
    if (!line || !*line || *line == '#') return 0;

    char buf[1024];
    size_t out = 0;
    for (const char *p = line; *p && out + 1 < sizeof(buf);) {
        if (*p == ',') {
            /* Look ahead: drop the whole field if its key is unsupported. */
            static const char *DROP[] = { "crc:", "platform:", "hint:", "type:", NULL };
            const char *q = p + 1;
            int dropped = 0;
            for (int k = 0; DROP[k]; k++) {
                size_t klen = strlen(DROP[k]);
                if (strncmp(q, DROP[k], klen) == 0) {
                    const char *end = strchr(q, ',');
                    p = end ? end : q + strlen(q);
                    dropped = 1;
                    break;
                }
            }
            if (dropped) continue;
        }
        buf[out++] = *p++;
    }
    buf[out] = 0;

    /* Returns 1 = added, 0 = updated an existing mapping, -1 = error. */
    return SDL_GameControllerAddMapping(buf) >= 0 ? 1 : 0;
}

/*
 * Load the controller database from the runtime directory.
 *
 * Done in C rather than JS deliberately: the runtime is sandboxed with no file
 * access by design (quickjs-libc is not compiled in), and adding a general
 * read-any-file capability so the gamepad shim could load one text file would
 * punch a hole straight through that. The host already knows where the runtime
 * lives in every layout, so it reads the file itself.
 *
 * A missing database is a degradation, not a failure — SDL still recognizes the
 * pads it knows natively — so this warns rather than aborting.
 */
/* Defined below; declared here because the file loader calls it. */
int jsglq_gamepad_load_db(const char *text);

int jsglq_gamepad_load_db_file(const char *runtime_dir)
{
    if (!runtime_dir || !*runtime_dir) return 0;

    char path[4096];
    snprintf(path, sizeof(path), "%s/data/gamecontrollerdb.txt", runtime_dir);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "jsglq: controller database not found at %s; "
                        "unusual pads may not map correctly\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (16 << 20)) { fclose(f); return 0; }

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = 0;

    int added = jsglq_gamepad_load_db(buf);
    free(buf);

    /* Devices connected before the mappings loaded are held as raw joysticks;
       reopening promotes them to recognized controllers. Without this the
       database has no effect on anything plugged in at startup — which is
       every device in the normal case. */
    if (g_inited) {
        for (int i = 0; i < MAX_PADS; i++) pad_close(i);
        for (int i = 0; i < SDL_NumJoysticks(); i++) pad_open(i);
    }
    return added;
}

int jsglq_gamepad_load_db(const char *text)
{
    if (!text) return 0;
    int added = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && len < 1023) {
            char line[1024];
            memcpy(line, p, len);
            line[len] = 0;
            /* Trim trailing CR so a CRLF database still parses. */
            if (len && line[len - 1] == '\r') line[len - 1] = 0;
            added += add_mapping_line(line);
        }
        if (!nl) break;
        p = nl + 1;
    }
    return added;
}

void jsglq_gamepad_init(void)
{
    if (g_inited) return;
    for (int i = 0; i < MAX_PADS; i++) g_pads[i].id = -1;

    /* VIDEO is already up; these are the subsystems this file needs. Joystick
       is required even for controllers, because a device SDL does not recognize
       still has to be readable as a raw joystick. */
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "jsglq: gamepad subsystem unavailable: %s\n", SDL_GetError());
        return;
    }
    g_inited = true;

    for (int i = 0; i < SDL_NumJoysticks(); i++) pad_open(i);
}

void jsglq_gamepad_shutdown(void)
{
    if (!g_inited) return;
    for (int i = 0; i < MAX_PADS; i++) pad_close(i);
    g_inited = false;
}

/* Called from the SDL event pump for hotplug events. */
void jsglq_gamepad_device_event(const SDL_Event *ev)
{
    if (!g_inited) return;
    if (ev->type == SDL_JOYDEVICEADDED) {
        /* Fires for controllers too; pad_open picks the right API and
           deduplicates by instance id, so handling one event type is enough. */
        pad_open(ev->jdevice.which);
    } else if (ev->type == SDL_JOYDEVICEREMOVED) {
        int slot = slot_for_instance(ev->jdevice.which);
        if (slot >= 0) pad_close(slot);
    }
}

/* ------------------------------------------------------------------ JS API -- */

/*
 * Returns an array of raw device snapshots:
 *
 *   { index, id, guid, name, isController, hasRumble,
 *     buttons: [0|1, ...],      raw joystick button states
 *     axes:    [-1..1, ...],    raw joystick axis values
 *     hats:    [bitmask, ...],  raw hat switch states
 *     ctrl: { a, b, x, y, ... } present ONLY when SDL recognized the device,
 *                               already in SDL's standard layout
 *   }
 *
 * Mapping into the W3C Standard Gamepad layout happens in JS.
 */
static JSValue js_gamepad_poll(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;

    JSValue arr = JS_NewArray(ctx);
    if (!g_inited) return arr;

    /* SDL caches device state; without this the values never change. */
    SDL_JoystickUpdate();
    SDL_GameControllerUpdate();

    uint32_t n = 0;
    for (int i = 0; i < MAX_PADS; i++) {
        Pad *p = &g_pads[i];
        if (p->id == -1 || !p->joy) continue;

        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "index", JS_NewInt32(ctx, i));
        JS_SetPropertyStr(ctx, o, "id", JS_NewInt32(ctx, (int)p->id));
        JS_SetPropertyStr(ctx, o, "guid", JS_NewString(ctx, p->guid));
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, p->name));
        JS_SetPropertyStr(ctx, o, "isController", JS_NewBool(ctx, p->is_controller));
        JS_SetPropertyStr(ctx, o, "hasRumble", JS_NewBool(ctx, p->has_rumble));

        int nbtn = SDL_JoystickNumButtons(p->joy);
        JSValue btns = JS_NewArray(ctx);
        for (int b = 0; b < nbtn; b++) {
            JS_SetPropertyUint32(ctx, btns, (uint32_t)b,
                                 JS_NewInt32(ctx, SDL_JoystickGetButton(p->joy, b)));
        }
        JS_SetPropertyStr(ctx, o, "buttons", btns);

        int nax = SDL_JoystickNumAxes(p->joy);
        JSValue axes = JS_NewArray(ctx);
        for (int a = 0; a < nax; a++) {
            /* SDL reports int16; the Gamepad API wants -1..1. Divide by 32767
               so a full-scale positive reads exactly 1.0, and clamp the
               asymmetric negative end (-32768) rather than returning -1.000031. */
            double v = SDL_JoystickGetAxis(p->joy, a) / 32767.0;
            if (v < -1.0) v = -1.0;
            JS_SetPropertyUint32(ctx, axes, (uint32_t)a, JS_NewFloat64(ctx, v));
        }
        JS_SetPropertyStr(ctx, o, "axes", axes);

        int nhat = SDL_JoystickNumHats(p->joy);
        JSValue hats = JS_NewArray(ctx);
        for (int h = 0; h < nhat; h++) {
            JS_SetPropertyUint32(ctx, hats, (uint32_t)h,
                                 JS_NewInt32(ctx, SDL_JoystickGetHat(p->joy, h)));
        }
        JS_SetPropertyStr(ctx, o, "hats", hats);

        /* When SDL recognized the pad, hand JS the already-normalized view so it
           can skip the database entirely. */
        if (p->ctrl) {
            JSValue c = JS_NewObject(ctx);
            static const struct { const char *name; SDL_GameControllerButton b; } BTN[] = {
                { "a", SDL_CONTROLLER_BUTTON_A },
                { "b", SDL_CONTROLLER_BUTTON_B },
                { "x", SDL_CONTROLLER_BUTTON_X },
                { "y", SDL_CONTROLLER_BUTTON_Y },
                { "back", SDL_CONTROLLER_BUTTON_BACK },
                { "guide", SDL_CONTROLLER_BUTTON_GUIDE },
                { "start", SDL_CONTROLLER_BUTTON_START },
                { "leftStick", SDL_CONTROLLER_BUTTON_LEFTSTICK },
                { "rightStick", SDL_CONTROLLER_BUTTON_RIGHTSTICK },
                { "leftShoulder", SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
                { "rightShoulder", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
                { "dpadUp", SDL_CONTROLLER_BUTTON_DPAD_UP },
                { "dpadDown", SDL_CONTROLLER_BUTTON_DPAD_DOWN },
                { "dpadLeft", SDL_CONTROLLER_BUTTON_DPAD_LEFT },
                { "dpadRight", SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
            };
            for (size_t k = 0; k < sizeof(BTN) / sizeof(BTN[0]); k++) {
                JS_SetPropertyStr(ctx, c, BTN[k].name,
                    JS_NewInt32(ctx, SDL_GameControllerGetButton(p->ctrl, BTN[k].b)));
            }
            static const struct { const char *name; SDL_GameControllerAxis a; int is_trigger; } AX[] = {
                { "leftStickX", SDL_CONTROLLER_AXIS_LEFTX, 0 },
                { "leftStickY", SDL_CONTROLLER_AXIS_LEFTY, 0 },
                { "rightStickX", SDL_CONTROLLER_AXIS_RIGHTX, 0 },
                { "rightStickY", SDL_CONTROLLER_AXIS_RIGHTY, 0 },
                { "leftTrigger", SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1 },
                { "rightTrigger", SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1 },
            };
            for (size_t k = 0; k < sizeof(AX) / sizeof(AX[0]); k++) {
                double v = SDL_GameControllerGetAxis(p->ctrl, AX[k].a) / 32767.0;
                if (v < -1.0) v = -1.0;
                if (AX[k].is_trigger) {
                    /* SDL documents triggers as 0..32767, but what actually
                       arrives depends on the mapping: a pad described with a
                       full-range axis rests at -1, and SDL's own VIRTUAL gamepad
                       rests at 0.5. Clamp to the documented range here and let
                       the JS side apply a deadzone — guessing a resting value in
                       C was wrong twice. */
                    if (v < 0.0) v = 0.0;
                }
                JS_SetPropertyStr(ctx, c, AX[k].name, JS_NewFloat64(ctx, v));
            }
            JS_SetPropertyStr(ctx, o, "ctrl", c);
        }

        JS_SetPropertyUint32(ctx, arr, n++, o);
    }
    return arr;
}

/* navigator.getGamepads() exposes vibrationActuator; back it with real rumble
   where the device supports it. */
static JSValue js_gamepad_rumble(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4) return jsglq_throw(ctx, "rumble(index, low, high, ms)");

    int32_t idx = 0, ms = 0;
    double low = 0, high = 0;
    if (JS_ToInt32(ctx, &idx, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &low, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &high, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &ms, argv[3])) return JS_EXCEPTION;

    if (idx < 0 || idx >= MAX_PADS || g_pads[idx].id == -1) return JS_FALSE;
    if (!g_pads[idx].has_rumble) return JS_FALSE;

    if (low  < 0) low  = 0; if (low  > 1) low  = 1;
    if (high < 0) high = 0; if (high > 1) high = 1;

    int rc = SDL_JoystickRumble(g_pads[idx].joy,
                                (Uint16)(low  * 0xFFFF),
                                (Uint16)(high * 0xFFFF),
                                (Uint32)(ms < 0 ? 0 : ms));
    return rc == 0 ? JS_TRUE : JS_FALSE;
}

/*
 * Virtual-device hooks, for tests.
 *
 * Gamepad code is otherwise only exercisable by a human holding a controller,
 * which means it is exercised approximately never — and this project already
 * shipped a getGamepads() that returned nothing while the README claimed the
 * API worked. SDL's virtual joysticks make the whole pipeline (open, map via
 * the database, poll, translate to standard layout) testable with no hardware,
 * so the mapping cannot silently rot.
 *
 * These are inert unless a test calls them; nothing in the normal path does.
 */
#if SDL_VERSION_ATLEAST(2, 0, 14)
static JSValue js_gamepad_virtual_attach(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (!g_inited) return JS_NewInt32(ctx, -1);

    /* A gamepad-typed virtual device with the standard complement: SDL applies
       its own default mapping, so it arrives as a recognized controller. */
    int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                        /*naxes=*/6, /*nbuttons=*/15, /*nhats=*/1);
    if (idx < 0) return JS_NewInt32(ctx, -1);
    pad_open(idx);
    return JS_NewInt32(ctx, idx);
}

static JSValue js_gamepad_virtual_set(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4) return jsglq_throw(ctx, "virtualSet(slot, kind, index, value)");

    int32_t slot = 0, index = 0, value = 0;
    if (JS_ToInt32(ctx, &slot, argv[0])) return JS_EXCEPTION;
    const char *kind = JS_ToCString(ctx, argv[1]);
    if (!kind) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &index, argv[2])) { JS_FreeCString(ctx, kind); return JS_EXCEPTION; }
    if (JS_ToInt32(ctx, &value, argv[3])) { JS_FreeCString(ctx, kind); return JS_EXCEPTION; }

    int rc = -1;
    if (slot >= 0 && slot < MAX_PADS && g_pads[slot].joy) {
        SDL_Joystick *j = g_pads[slot].joy;
        if      (!strcmp(kind, "button")) rc = SDL_JoystickSetVirtualButton(j, index, (Uint8)value);
        else if (!strcmp(kind, "axis"))   rc = SDL_JoystickSetVirtualAxis(j, index, (Sint16)value);
        else if (!strcmp(kind, "hat"))    rc = SDL_JoystickSetVirtualHat(j, index, (Uint8)value);
    }
    JS_FreeCString(ctx, kind);
    return rc == 0 ? JS_TRUE : JS_FALSE;
}

static JSValue js_gamepad_virtual_detach(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return jsglq_throw(ctx, "virtualDetach(deviceIndex)");
    int32_t idx = 0;
    if (JS_ToInt32(ctx, &idx, argv[0])) return JS_EXCEPTION;
    /* Close our handle first: detaching a device we still hold open leaves a
       dangling joystick pointer that the next poll would read. */
    for (int i = 0; i < MAX_PADS; i++) {
        if (g_pads[i].joy && SDL_JoystickGetDeviceInstanceID(idx) == g_pads[i].id) {
            pad_close(i);
            break;
        }
    }
    return SDL_JoystickDetachVirtual(idx) == 0 ? JS_TRUE : JS_FALSE;
}
#endif /* SDL >= 2.0.14 */

int jsglq_bind_gamepad(JsglqEngine *e)
{
    JSContext *ctx = jsglq_engine_ctx(e);
    JSValue global = JS_GetGlobalObject(ctx);

    jsglq_gamepad_init();

    JSValue gp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, gp, "poll",
                      JS_NewCFunction(ctx, js_gamepad_poll, "poll", 0));
    JS_SetPropertyStr(ctx, gp, "rumble",
                      JS_NewCFunction(ctx, js_gamepad_rumble, "rumble", 4));
#if SDL_VERSION_ATLEAST(2, 0, 14)
    JS_SetPropertyStr(ctx, gp, "virtualAttach",
                      JS_NewCFunction(ctx, js_gamepad_virtual_attach, "virtualAttach", 0));
    JS_SetPropertyStr(ctx, gp, "virtualSet",
                      JS_NewCFunction(ctx, js_gamepad_virtual_set, "virtualSet", 4));
    JS_SetPropertyStr(ctx, gp, "virtualDetach",
                      JS_NewCFunction(ctx, js_gamepad_virtual_detach, "virtualDetach", 1));
#endif

    JS_SetPropertyStr(ctx, global, "__jsglq_gamepad", gp);
    JS_FreeValue(ctx, global);
    return 0;
}
