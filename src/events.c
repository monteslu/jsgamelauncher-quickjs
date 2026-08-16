/*
 * Input events: keyboard, mouse, pointer, wheel.
 *
 * rungame supports FOUR event types total (keydown, keyup, load, resize) and has no
 * mouse, touch, or pointer support at all. That is not a minor gap: the corpus's own
 * audio-unlock pattern is `document.addEventListener('click', ..., {once:true})`,
 * which silently never fires there, and every game with a menu is unusable.
 *
 * Two correctness details rungame also gets wrong and this does not:
 *   - `code` is a real KeyboardEvent.code (`KeyA`, `ArrowLeft`), not SDL's raw key
 *     name. Games keyed on `code` are position-based and must work on AZERTY.
 *   - `key` follows the spec too: a printable key reports its character, everything
 *     else reports its named value.
 */
#include "host.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

/* SDL scancode -> KeyboardEvent.code. Position-based, which is the whole point:
   these must NOT depend on the user's keyboard layout. */
static const char *scancode_to_code(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_A: return "KeyA";  case SDL_SCANCODE_B: return "KeyB";
    case SDL_SCANCODE_C: return "KeyC";  case SDL_SCANCODE_D: return "KeyD";
    case SDL_SCANCODE_E: return "KeyE";  case SDL_SCANCODE_F: return "KeyF";
    case SDL_SCANCODE_G: return "KeyG";  case SDL_SCANCODE_H: return "KeyH";
    case SDL_SCANCODE_I: return "KeyI";  case SDL_SCANCODE_J: return "KeyJ";
    case SDL_SCANCODE_K: return "KeyK";  case SDL_SCANCODE_L: return "KeyL";
    case SDL_SCANCODE_M: return "KeyM";  case SDL_SCANCODE_N: return "KeyN";
    case SDL_SCANCODE_O: return "KeyO";  case SDL_SCANCODE_P: return "KeyP";
    case SDL_SCANCODE_Q: return "KeyQ";  case SDL_SCANCODE_R: return "KeyR";
    case SDL_SCANCODE_S: return "KeyS";  case SDL_SCANCODE_T: return "KeyT";
    case SDL_SCANCODE_U: return "KeyU";  case SDL_SCANCODE_V: return "KeyV";
    case SDL_SCANCODE_W: return "KeyW";  case SDL_SCANCODE_X: return "KeyX";
    case SDL_SCANCODE_Y: return "KeyY";  case SDL_SCANCODE_Z: return "KeyZ";
    case SDL_SCANCODE_1: return "Digit1"; case SDL_SCANCODE_2: return "Digit2";
    case SDL_SCANCODE_3: return "Digit3"; case SDL_SCANCODE_4: return "Digit4";
    case SDL_SCANCODE_5: return "Digit5"; case SDL_SCANCODE_6: return "Digit6";
    case SDL_SCANCODE_7: return "Digit7"; case SDL_SCANCODE_8: return "Digit8";
    case SDL_SCANCODE_9: return "Digit9"; case SDL_SCANCODE_0: return "Digit0";
    case SDL_SCANCODE_RETURN: return "Enter";
    case SDL_SCANCODE_ESCAPE: return "Escape";
    case SDL_SCANCODE_BACKSPACE: return "Backspace";
    case SDL_SCANCODE_TAB: return "Tab";
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_MINUS: return "Minus";
    case SDL_SCANCODE_EQUALS: return "Equal";
    case SDL_SCANCODE_LEFTBRACKET: return "BracketLeft";
    case SDL_SCANCODE_RIGHTBRACKET: return "BracketRight";
    case SDL_SCANCODE_BACKSLASH: return "Backslash";
    case SDL_SCANCODE_SEMICOLON: return "Semicolon";
    case SDL_SCANCODE_APOSTROPHE: return "Quote";
    case SDL_SCANCODE_GRAVE: return "Backquote";
    case SDL_SCANCODE_COMMA: return "Comma";
    case SDL_SCANCODE_PERIOD: return "Period";
    case SDL_SCANCODE_SLASH: return "Slash";
    case SDL_SCANCODE_CAPSLOCK: return "CapsLock";
    case SDL_SCANCODE_F1: return "F1";   case SDL_SCANCODE_F2: return "F2";
    case SDL_SCANCODE_F3: return "F3";   case SDL_SCANCODE_F4: return "F4";
    case SDL_SCANCODE_F5: return "F5";   case SDL_SCANCODE_F6: return "F6";
    case SDL_SCANCODE_F7: return "F7";   case SDL_SCANCODE_F8: return "F8";
    case SDL_SCANCODE_F9: return "F9";   case SDL_SCANCODE_F10: return "F10";
    case SDL_SCANCODE_F11: return "F11"; case SDL_SCANCODE_F12: return "F12";
    case SDL_SCANCODE_RIGHT: return "ArrowRight";
    case SDL_SCANCODE_LEFT: return "ArrowLeft";
    case SDL_SCANCODE_DOWN: return "ArrowDown";
    case SDL_SCANCODE_UP: return "ArrowUp";
    case SDL_SCANCODE_LCTRL: return "ControlLeft";
    case SDL_SCANCODE_LSHIFT: return "ShiftLeft";
    case SDL_SCANCODE_LALT: return "AltLeft";
    case SDL_SCANCODE_LGUI: return "MetaLeft";
    case SDL_SCANCODE_RCTRL: return "ControlRight";
    case SDL_SCANCODE_RSHIFT: return "ShiftRight";
    case SDL_SCANCODE_RALT: return "AltRight";
    case SDL_SCANCODE_RGUI: return "MetaRight";
    case SDL_SCANCODE_HOME: return "Home";
    case SDL_SCANCODE_END: return "End";
    case SDL_SCANCODE_PAGEUP: return "PageUp";
    case SDL_SCANCODE_PAGEDOWN: return "PageDown";
    case SDL_SCANCODE_INSERT: return "Insert";
    case SDL_SCANCODE_DELETE: return "Delete";
    default: return NULL;
    }
}

/* KeyboardEvent.key: the character produced, or the spec's named value. */
static const char *keycode_to_key(SDL_Keycode kc, char *buf, size_t buflen)
{
    switch (kc) {
    case SDLK_RETURN: return "Enter";
    case SDLK_ESCAPE: return "Escape";
    case SDLK_BACKSPACE: return "Backspace";
    case SDLK_TAB: return "Tab";
    case SDLK_SPACE: return " ";
    case SDLK_RIGHT: return "ArrowRight";
    case SDLK_LEFT: return "ArrowLeft";
    case SDLK_DOWN: return "ArrowDown";
    case SDLK_UP: return "ArrowUp";
    case SDLK_LCTRL: case SDLK_RCTRL: return "Control";
    case SDLK_LSHIFT: case SDLK_RSHIFT: return "Shift";
    case SDLK_LALT: case SDLK_RALT: return "Alt";
    case SDLK_LGUI: case SDLK_RGUI: return "Meta";
    case SDLK_CAPSLOCK: return "CapsLock";
    case SDLK_HOME: return "Home";
    case SDLK_END: return "End";
    case SDLK_PAGEUP: return "PageUp";
    case SDLK_PAGEDOWN: return "PageDown";
    case SDLK_INSERT: return "Insert";
    case SDLK_DELETE: return "Delete";
    default: break;
    }
    if (kc >= SDLK_F1 && kc <= SDLK_F12) {
        snprintf(buf, buflen, "F%d", (int)(kc - SDLK_F1 + 1));
        return buf;
    }
    if (kc >= 32 && kc < 127) {
        buf[0] = (char)kc;
        buf[1] = 0;
        return buf;
    }
    return "Unidentified";
}

static void set_modifiers(JSContext *ctx, JSValue ev, SDL_Keymod mod)
{
    JS_SetPropertyStr(ctx, ev, "altKey", JS_NewBool(ctx, (mod & KMOD_ALT) != 0));
    JS_SetPropertyStr(ctx, ev, "ctrlKey", JS_NewBool(ctx, (mod & KMOD_CTRL) != 0));
    JS_SetPropertyStr(ctx, ev, "shiftKey", JS_NewBool(ctx, (mod & KMOD_SHIFT) != 0));
    JS_SetPropertyStr(ctx, ev, "metaKey", JS_NewBool(ctx, (mod & KMOD_GUI) != 0));
}

/* Hand an event object to the JS dispatcher installed by the events shim. */
static void dispatch(JSContext *ctx, JSValue ev)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__jsglq_dispatchEvent");
    if (JS_IsFunction(ctx, fn)) {
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&ev);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            const char *s = JS_ToCString(ctx, exc);
            fprintf(stderr, "jsglq: event handler threw: %s\n", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ev);
}

static JSValue new_event(JSContext *ctx, const char *type)
{
    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, ev, "timeStamp", JS_NewFloat64(ctx, jsglq_now_ms()));
    JS_SetPropertyStr(ctx, ev, "bubbles", JS_TRUE);
    JS_SetPropertyStr(ctx, ev, "cancelable", JS_TRUE);
    JS_SetPropertyStr(ctx, ev, "defaultPrevented", JS_FALSE);
    return ev;
}

void jsglq_events_key(JSContext *ctx, const SDL_KeyboardEvent *k, bool down)
{
    JSValue ev = new_event(ctx, down ? "keydown" : "keyup");

    char kbuf[8];
    const char *key = keycode_to_key(k->keysym.sym, kbuf, sizeof(kbuf));
    const char *code = scancode_to_code(k->keysym.scancode);

    JS_SetPropertyStr(ctx, ev, "key", JS_NewString(ctx, key));
    JS_SetPropertyStr(ctx, ev, "code",
        JS_NewString(ctx, code ? code : SDL_GetScancodeName(k->keysym.scancode)));
    JS_SetPropertyStr(ctx, ev, "repeat", JS_NewBool(ctx, k->repeat != 0));
    JS_SetPropertyStr(ctx, ev, "keyCode", JS_NewInt32(ctx, (int)k->keysym.sym));
    JS_SetPropertyStr(ctx, ev, "which", JS_NewInt32(ctx, (int)k->keysym.sym));
    set_modifiers(ctx, ev, (SDL_Keymod)k->keysym.mod);

    dispatch(ctx, ev);
}

static void add_pointer_props(JSContext *ctx, JSValue ev, int x, int y, int button, uint32_t buttons)
{
    JS_SetPropertyStr(ctx, ev, "clientX", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, ev, "clientY", JS_NewInt32(ctx, y));
    JS_SetPropertyStr(ctx, ev, "offsetX", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, ev, "offsetY", JS_NewInt32(ctx, y));
    JS_SetPropertyStr(ctx, ev, "pageX", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, ev, "pageY", JS_NewInt32(ctx, y));
    JS_SetPropertyStr(ctx, ev, "screenX", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, ev, "screenY", JS_NewInt32(ctx, y));
    JS_SetPropertyStr(ctx, ev, "x", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, ev, "y", JS_NewInt32(ctx, y));
    JS_SetPropertyStr(ctx, ev, "button", JS_NewInt32(ctx, button));
    JS_SetPropertyStr(ctx, ev, "buttons", JS_NewUint32(ctx, buttons));
    set_modifiers(ctx, ev, SDL_GetModState());
}

/* SDL button ids are 1-based (left=1); the DOM's are 0-based (left=0). */
static int sdl_button_to_dom(uint8_t b)
{
    switch (b) {
    case SDL_BUTTON_LEFT:   return 0;
    case SDL_BUTTON_MIDDLE: return 1;
    case SDL_BUTTON_RIGHT:  return 2;
    case SDL_BUTTON_X1:     return 3;
    case SDL_BUTTON_X2:     return 4;
    default: return 0;
    }
}

static uint32_t sdl_buttons_mask(void)
{
    uint32_t sdl = SDL_GetMouseState(NULL, NULL);
    uint32_t dom = 0;
    if (sdl & SDL_BUTTON(SDL_BUTTON_LEFT))   dom |= 1;
    if (sdl & SDL_BUTTON(SDL_BUTTON_RIGHT))  dom |= 2;
    if (sdl & SDL_BUTTON(SDL_BUTTON_MIDDLE)) dom |= 4;
    return dom;
}

/*
 * A mouse action emits BOTH the mouse event and the matching pointer event, which
 * is what browsers do. Games written against either API then work; supporting only
 * one silently breaks half the corpus.
 */
void jsglq_events_mouse_button(JSContext *ctx, const SDL_MouseButtonEvent *m, bool down)
{
    const int dom_button = sdl_button_to_dom(m->button);
    const uint32_t mask = sdl_buttons_mask();

    JSValue ev = new_event(ctx, down ? "mousedown" : "mouseup");
    add_pointer_props(ctx, ev, m->x, m->y, dom_button, mask);
    dispatch(ctx, ev);

    JSValue pev = new_event(ctx, down ? "pointerdown" : "pointerup");
    add_pointer_props(ctx, pev, m->x, m->y, dom_button, mask);
    JS_SetPropertyStr(ctx, pev, "pointerId", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, pev, "pointerType", JS_NewString(ctx, "mouse"));
    JS_SetPropertyStr(ctx, pev, "isPrimary", JS_TRUE);
    JS_SetPropertyStr(ctx, pev, "pressure", JS_NewFloat64(ctx, down ? 0.5 : 0.0));
    dispatch(ctx, pev);

    /* A full press-and-release also produces `click`, which is what the corpus's
       audio-unlock handlers listen for. */
    if (!down && m->clicks > 0) {
        JSValue cev = new_event(ctx, "click");
        add_pointer_props(ctx, cev, m->x, m->y, dom_button, mask);
        JS_SetPropertyStr(ctx, cev, "detail", JS_NewInt32(ctx, m->clicks));
        dispatch(ctx, cev);
    }
}

void jsglq_events_mouse_motion(JSContext *ctx, const SDL_MouseMotionEvent *m)
{
    const uint32_t mask = sdl_buttons_mask();

    JSValue ev = new_event(ctx, "mousemove");
    add_pointer_props(ctx, ev, m->x, m->y, 0, mask);
    JS_SetPropertyStr(ctx, ev, "movementX", JS_NewInt32(ctx, m->xrel));
    JS_SetPropertyStr(ctx, ev, "movementY", JS_NewInt32(ctx, m->yrel));
    dispatch(ctx, ev);

    JSValue pev = new_event(ctx, "pointermove");
    add_pointer_props(ctx, pev, m->x, m->y, 0, mask);
    JS_SetPropertyStr(ctx, pev, "pointerId", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, pev, "pointerType", JS_NewString(ctx, "mouse"));
    JS_SetPropertyStr(ctx, pev, "isPrimary", JS_TRUE);
    JS_SetPropertyStr(ctx, pev, "movementX", JS_NewInt32(ctx, m->xrel));
    JS_SetPropertyStr(ctx, pev, "movementY", JS_NewInt32(ctx, m->yrel));
    dispatch(ctx, pev);
}

void jsglq_events_wheel(JSContext *ctx, const SDL_MouseWheelEvent *w)
{
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);

    JSValue ev = new_event(ctx, "wheel");
    add_pointer_props(ctx, ev, mx, my, 0, sdl_buttons_mask());
    /* SDL's y is positive-up, the DOM's deltaY is positive-down. */
    double dir = (w->direction == SDL_MOUSEWHEEL_FLIPPED) ? -1.0 : 1.0;
    JS_SetPropertyStr(ctx, ev, "deltaX", JS_NewFloat64(ctx, w->preciseX * 100.0 * dir));
    JS_SetPropertyStr(ctx, ev, "deltaY", JS_NewFloat64(ctx, -w->preciseY * 100.0 * dir));
    JS_SetPropertyStr(ctx, ev, "deltaZ", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, ev, "deltaMode", JS_NewInt32(ctx, 0));  /* DOM_DELTA_PIXEL */
    dispatch(ctx, ev);
}

void jsglq_events_resize(JSContext *ctx, int width, int height)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, width));
    JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, height));
    JS_FreeValue(ctx, global);

    dispatch(ctx, new_event(ctx, "resize"));
}
