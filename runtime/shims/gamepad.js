/*
 * navigator.getGamepads() — the W3C Gamepad API over SDL.
 *
 * The host (src/bind_gamepad.c) loads SDL_GameControllerDB at startup, so nearly
 * every pad arrives already normalized to SDL's standard layout. That layout is
 * a near-exact match for the W3C "standard" mapping, which makes the common path
 * here a straight index translation rather than a device database.
 *
 * Devices SDL still does not recognize fall back to a raw-joystick heuristic.
 * That fallback CANNOT be correct for every unknown device — button order on an
 * unmapped pad is whatever the firmware says — so it reports `mapping: ''`, which
 * is exactly what the spec says to do when the layout is unknown. A game that
 * cares can then read the raw arrays instead of trusting the button indices.
 */

// W3C Standard Gamepad button order. SDL gives these by name; this fixes the order.
const STANDARD_BUTTONS = [
  'a', 'b', 'x', 'y',                       // 0-3   face
  'leftShoulder', 'rightShoulder',          // 4-5
  null, null,                               // 6-7   triggers, from axes below
  'back', 'start',                          // 8-9
  'leftStick', 'rightStick',                // 10-11 stick clicks
  'dpadUp', 'dpadDown', 'dpadLeft', 'dpadRight', // 12-15
  'guide',                                  // 16
];

// A trigger reads as an axis; the spec also exposes it as a button. Below this
// value it is not "pressed" — matches the threshold jsgamelauncher settled on.
const TRIGGER_THRESHOLD = 0.11;

/*
 * Resting position varies by mapping, and assuming one was wrong twice: a pad
 * described with a full-range axis rests at -1, SDL's virtual gamepad rests at
 * 0.5, and a correctly mapped trigger rests at 0. Anything at or below this is
 * treated as released, which covers all three without needing to know which
 * kind of pad is attached. A real pull goes well past it.
 */
const TRIGGER_DEADZONE = 0.6;

function triggerValue(v) {
  const t = v ?? 0;
  if (t <= TRIGGER_DEADZONE) return 0;
  // Rescale so the usable travel still spans 0..1 rather than starting partway.
  return (t - TRIGGER_DEADZONE) / (1 - TRIGGER_DEADZONE);
}

// SDL hat bits (SDL_HAT_UP etc). A hat is how most cheap pads report their dpad.
const HAT_UP = 0x01, HAT_RIGHT = 0x02, HAT_DOWN = 0x04, HAT_LEFT = 0x08;

function makeButton(pressed, value) {
  return { pressed: !!pressed, touched: !!pressed, value: value ?? (pressed ? 1 : 0) };
}

/*
 * SDL's recognized-controller path: everything is already named, so this is a
 * direct translation into standard order.
 */
function fromController(raw) {
  const c = raw.ctrl;
  const buttons = STANDARD_BUTTONS.map((name) =>
    (name === null ? makeButton(false, 0) : makeButton(c[name])));

  // Triggers: SDL reports 0..1 on its own axis. Populate BOTH the button
  // (indices 6/7, analog value) and the axis, as a browser does.
  const lt = triggerValue(c.leftTrigger);
  const rt = triggerValue(c.rightTrigger);
  buttons[6] = makeButton(lt > TRIGGER_THRESHOLD, lt);
  buttons[7] = makeButton(rt > TRIGGER_THRESHOLD, rt);

  return {
    buttons,
    axes: [c.leftStickX ?? 0, c.leftStickY ?? 0, c.rightStickX ?? 0, c.rightStickY ?? 0],
    mapping: 'standard',
  };
}

/*
 * Unrecognized device: guess. Most pads that reach here follow the common
 * "first four buttons are the face buttons" convention, and report the dpad on
 * hat 0. This is a best effort and says so by reporting mapping: ''.
 */
function fromJoystick(raw) {
  const buttons = STANDARD_BUTTONS.map(() => makeButton(false, 0));
  const rawBtns = raw.buttons || [];

  // Pass through positionally as far as the pad provides them.
  for (let i = 0; i < rawBtns.length && i < buttons.length; i++) {
    buttons[i] = makeButton(rawBtns[i]);
  }

  // Hat 0 -> dpad (buttons 12..15). Without this the dpad is invisible on the
  // many pads that report it as a hat rather than as buttons.
  const hat = (raw.hats && raw.hats[0]) || 0;
  if (raw.hats && raw.hats.length) {
    buttons[12] = makeButton(hat & HAT_UP);
    buttons[13] = makeButton(hat & HAT_DOWN);
    buttons[14] = makeButton(hat & HAT_LEFT);
    buttons[15] = makeButton(hat & HAT_RIGHT);
  }

  const axes = (raw.axes || []).slice(0, 4);
  while (axes.length < 4) axes.push(0);

  // Axes 4/5 are triggers on most layouts, resting at -1 and travelling to +1.
  const rawAxes = raw.axes || [];
  if (rawAxes.length > 4) {
    const lt = (rawAxes[4] + 1) / 2;
    buttons[6] = makeButton(lt > TRIGGER_THRESHOLD, lt);
  }
  if (rawAxes.length > 5) {
    const rt = (rawAxes[5] + 1) / 2;
    buttons[7] = makeButton(rt > TRIGGER_THRESHOLD, rt);
  }

  return { buttons, axes, mapping: '' };
}

/* Exported for tests. The unrecognized-device path cannot be reached with SDL's
   virtual joystick (SDL always maps that one), so the only way to keep the hat
   and trigger fallbacks honest is to call this directly with a synthetic device. */
export const __testFromJoystick = fromJoystick;
export const __testFromController = fromController;

export function installGamepads(g) {
  const host = g.__jsglq_gamepad;
  if (!host) return;   // built without gamepad support

  // Gamepad objects are rebuilt each poll, but `index` must stay stable for a
  // connected device, and disconnects must leave a hole rather than reshuffling
  // later pads — a game holding index 1 should not silently start reading pad 2.
  const nav = g.navigator;

  const getGamepads = () => {
    const raw = host.poll();
    const out = [];
    for (const r of raw) {
      const mapped = r.ctrl ? fromController(r) : fromJoystick(r);
      const pad = {
        id: r.name,
        index: r.index,
        connected: true,
        mapping: mapped.mapping,
        buttons: mapped.buttons,
        axes: mapped.axes,
        timestamp: g.performance ? g.performance.now() : 0,
        // Non-standard but universally present in practice, and games use them
        // to identify a pad without string-matching the name.
        guid: r.guid,
        vibrationActuator: r.hasRumble
          ? {
              type: 'dual-rumble',
              playEffect: (type, params) => {
                const p = params || {};
                host.rumble(r.index,
                  p.weakMagnitude ?? 0, p.strongMagnitude ?? 0, p.duration ?? 0);
                return Promise.resolve('complete');
              },
              reset: () => { host.rumble(r.index, 0, 0, 0); return Promise.resolve('complete'); },
            }
          : null,
      };
      // Sparse by index, as the browser API is: getGamepads()[2] is pad 2 even
      // when pads 0 and 1 are unplugged.
      out[r.index] = pad;
    }
    // Fill holes with null rather than leaving them `undefined`, which is what
    // the spec returns and what `for (const p of pads) if (p)` expects.
    for (let i = 0; i < out.length; i++) if (out[i] === undefined) out[i] = null;
    return out;
  };

  if (nav) {
    try {
      nav.getGamepads = getGamepads;
    } catch {
      Object.defineProperty(nav, 'getGamepads', { value: getGamepads, configurable: true });
    }
  }

  /*
   * gamepadconnected / gamepaddisconnected.
   *
   * The events fire from a poll rather than from SDL's hotplug event, because
   * the JS layer only learns about devices when it polls. Diffing the connected
   * set each frame is what turns that into the two events games listen for.
   */
  let known = new Set();
  g.__jsglq_pumpGamepadEvents = () => {
    const pads = getGamepads();
    const now = new Set();
    for (const p of pads) {
      if (!p) continue;
      now.add(p.index);
      if (!known.has(p.index)) {
        const ev = new g.Event('gamepadconnected');
        ev.gamepad = p;
        g.dispatchEvent(ev);
      }
    }
    for (const idx of known) {
      if (!now.has(idx)) {
        const ev = new g.Event('gamepaddisconnected');
        ev.gamepad = { index: idx, connected: false, buttons: [], axes: [], id: '' };
        g.dispatchEvent(ev);
      }
    }
    known = now;
  };
}
