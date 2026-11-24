// The page's input to the Android guest (I1, docs/input.md). Pointer and keyboard events on the Android screen become
// evdev events in a ring buffer that the engine drains (engine/evdev.c: XIAOJI_WEB reads it through xiaoji_input_pop in
// engine/jit_lib.js). The engine's worker never runs its event loop while it is in wasm, so the events cannot be posted to
// it: the ring is a SharedArrayBuffer, written here with Atomics and read there.
//
// The ring (an Int32Array over a SharedArrayBuffer): word 0 is the head (events written), word 1 the tail (events read),
// both counted modulo 2^31; then the entries, 4 words each (dev, type, code, value), as many as (length - 2) / 4. One
// writer (this page) and one reader (the engine). A full ring drops the event (ringPush returns false).
//
// The screen is #android-screen (created here, inside #phone, if the page has none yet: the graphics canvas takes this
// element when it lands). Its coordinates map to the display's 720 x 1280 (engine/display.h).
// The Home and Back buttons (#btn-home, #btn-back) send the keys KEY_HOMEPAGE and KEY_BACK.

export const DISPLAY_W = 720; // engine/display.h: XJ_DISPLAY_W
export const DISPLAY_H = 1280; // engine/display.h: XJ_DISPLAY_H
export const RING_CAP = 256;
export const DEV_TOUCH = 0; // /dev/input/event0
export const DEV_KEYBOARD = 1; // /dev/input/event1

// The codes (linux/input-event-codes.h).
export const EV_SYN = 0x00;
export const EV_KEY = 0x01;
export const EV_ABS = 0x03;
export const SYN_REPORT = 0;
export const BTN_TOUCH = 0x14a;
export const ABS_MT_SLOT = 0x2f;
export const ABS_MT_POSITION_X = 0x35;
export const ABS_MT_POSITION_Y = 0x36;
export const ABS_MT_TRACKING_ID = 0x39;
export const ABS_MT_PRESSURE = 0x3a;
export const KEY_BACK = 158;
export const KEY_HOMEPAGE = 172;

// KeyboardEvent.code -> the Linux KEY_ code. The physical key, as the layout of the phone's keyboard expects it:
// Generic.kl turns these into the characters.
const KEY_BY_CODE = {
  Escape: KEY_BACK, // Esc is Back (the phone's back key)
  Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Digit5: 6, Digit6: 7, Digit7: 8, Digit8: 9, Digit9: 10, Digit0: 11,
  Minus: 12, Equal: 13, Backspace: 14, Tab: 15,
  KeyQ: 16, KeyW: 17, KeyE: 18, KeyR: 19, KeyT: 20, KeyY: 21, KeyU: 22, KeyI: 23, KeyO: 24, KeyP: 25,
  BracketLeft: 26, BracketRight: 27, Enter: 28, ControlLeft: 29,
  KeyA: 30, KeyS: 31, KeyD: 32, KeyF: 33, KeyG: 34, KeyH: 35, KeyJ: 36, KeyK: 37, KeyL: 38,
  Semicolon: 39, Quote: 40, Backquote: 41, ShiftLeft: 42, Backslash: 43,
  KeyZ: 44, KeyX: 45, KeyC: 46, KeyV: 47, KeyB: 48, KeyN: 49, KeyM: 50,
  Comma: 51, Period: 52, Slash: 53, ShiftRight: 54, NumpadMultiply: 55, AltLeft: 56, Space: 57, CapsLock: 58,
  ControlRight: 97, AltRight: 100, Home: 102, ArrowUp: 103, PageUp: 104, ArrowLeft: 105, ArrowRight: 106, End: 107,
  ArrowDown: 108, PageDown: 109, Delete: 111, MetaLeft: 125, MetaRight: 126, ContextMenu: 139,
};

// The KEY_ code of a KeyboardEvent.code, or -1 when the key is not sent.
export function keyCode(code) {
  const k = KEY_BY_CODE[code];
  return k === undefined ? -1 : k;
}

// The ring. Returns null where there is no SharedArrayBuffer (a page that is not cross-origin isolated): input is off.
export function createRing() {
  if (typeof SharedArrayBuffer === 'undefined' || !globalThis.crossOriginIsolated) return null;
  return new Int32Array(new SharedArrayBuffer(4 * (2 + 4 * RING_CAP)));
}

// Writes one event. false when the ring is full (the event is dropped).
export function ringPush(ring, dev, type, code, value) {
  const cap = (ring.length - 2) >> 2;
  const head = Atomics.load(ring, 0);
  const tail = Atomics.load(ring, 1);
  if (((head - tail) & 0x7fffffff) >= cap) return false;
  const at = 2 + 4 * (head % cap);
  ring[at] = dev;
  ring[at + 1] = type;
  ring[at + 2] = code;
  ring[at + 3] = value;
  Atomics.store(ring, 0, (head + 1) & 0x7fffffff);
  return true;
}

// A point of the screen in display pixels: the element's rect, the client point, clamped to the display.
export function displayPoint(rect, clientX, clientY, w = DISPLAY_W, h = DISPLAY_H) {
  const fx = rect.width > 0 ? (clientX - rect.left) / rect.width : 0;
  const fy = rect.height > 0 ? (clientY - rect.top) / rect.height : 0;
  const x = Math.min(w - 1, Math.max(0, Math.round(fx * w)));
  const y = Math.min(h - 1, Math.max(0, Math.round(fy * h)));
  return { x, y };
}

// The touchscreen's protocol B (the kernel's multitouch.rst): a contact has a slot, a change is sent for its slot, and
// each frame ends with SYN_REPORT. push(dev, type, code, value) writes one event. A pointer (its pointerId) takes a free
// slot while it is down, and BTN_TOUCH is down while any contact is. Points are display pixels (displayPoint).
// The methods return false for a pointer that is not (or no longer) tracked, or when all 10 slots are taken.
export function createTouch(push) {
  const active = new Map(); // pointerId -> { slot, id }
  const free = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
  let nextId = 1;
  const frame = () => push(DEV_TOUCH, EV_SYN, SYN_REPORT, 0);
  const pressureOf = (p) => Math.min(255, Math.max(1, Math.round((p > 0 ? p : 0.5) * 255)));
  const pos = (x, y, pressure) => {
    push(DEV_TOUCH, EV_ABS, ABS_MT_POSITION_X, x);
    push(DEV_TOUCH, EV_ABS, ABS_MT_POSITION_Y, y);
    push(DEV_TOUCH, EV_ABS, ABS_MT_PRESSURE, pressureOf(pressure));
  };
  return {
    get count() {
      return active.size;
    },
    down(pid, x, y, pressure) {
      if (active.has(pid) || free.length === 0) return false;
      const slot = free.shift();
      const id = nextId;
      nextId = nextId >= 65535 ? 1 : nextId + 1;
      active.set(pid, { slot, id });
      push(DEV_TOUCH, EV_ABS, ABS_MT_SLOT, slot);
      push(DEV_TOUCH, EV_ABS, ABS_MT_TRACKING_ID, id);
      pos(x, y, pressure);
      if (active.size === 1) push(DEV_TOUCH, EV_KEY, BTN_TOUCH, 1);
      frame();
      return true;
    },
    move(pid, x, y, pressure) {
      const a = active.get(pid);
      if (!a) return false;
      push(DEV_TOUCH, EV_ABS, ABS_MT_SLOT, a.slot);
      pos(x, y, pressure);
      frame();
      return true;
    },
    up(pid) {
      const a = active.get(pid);
      if (!a) return false;
      push(DEV_TOUCH, EV_ABS, ABS_MT_SLOT, a.slot);
      push(DEV_TOUCH, EV_ABS, ABS_MT_TRACKING_ID, -1);
      active.delete(pid);
      free.push(a.slot);
      free.sort((p, q) => p - q);
      if (active.size === 0) push(DEV_TOUCH, EV_KEY, BTN_TOUCH, 0);
      frame();
      return true;
    },
  };
}

// The page's wiring. screen receives the pointers (it gets the focus on a press, so its keys come here) and the keys;
// back and home are buttons that send one press and release. ring is from createRing. Returns detach().
export function attachInput({ ring, screen, back, home }) {
  const push = (dev, type, code, value) => ringPush(ring, dev, type, code, value);
  const touch = createTouch(push);
  const held = new Set(); // KeyboardEvent.code of the keys sent as pressed
  const tap = (code) => {
    push(DEV_KEYBOARD, EV_KEY, code, 1);
    push(DEV_KEYBOARD, EV_SYN, SYN_REPORT, 0);
    push(DEV_KEYBOARD, EV_KEY, code, 0);
    push(DEV_KEYBOARD, EV_SYN, SYN_REPORT, 0);
  };
  const onDown = (e) => {
    if (e.pointerType === 'mouse' && e.button !== 0) return; // a mouse press is its left button
    e.preventDefault();
    screen.focus({ preventScroll: true });
    screen.setPointerCapture(e.pointerId);
    const p = displayPoint(screen.getBoundingClientRect(), e.clientX, e.clientY);
    touch.down(e.pointerId, p.x, p.y, e.pressure);
  };
  const onMove = (e) => {
    if (!touch.count) return;
    const p = displayPoint(screen.getBoundingClientRect(), e.clientX, e.clientY);
    touch.move(e.pointerId, p.x, p.y, e.pressure);
  };
  const onUp = (e) => {
    touch.up(e.pointerId);
  };
  const onKeyDown = (e) => {
    if (e.metaKey || e.ctrlKey) return; // the browser's shortcuts stay with the browser
    const code = keyCode(e.code);
    if (code < 0) return;
    e.preventDefault();
    if (e.repeat || held.has(e.code)) return; // Android repeats keys itself
    held.add(e.code);
    push(DEV_KEYBOARD, EV_KEY, code, 1);
    push(DEV_KEYBOARD, EV_SYN, SYN_REPORT, 0);
  };
  const onKeyUp = (e) => {
    if (!held.has(e.code)) return;
    e.preventDefault();
    held.delete(e.code);
    push(DEV_KEYBOARD, EV_KEY, keyCode(e.code), 0);
    push(DEV_KEYBOARD, EV_SYN, SYN_REPORT, 0);
  };
  const onBlur = () => {
    for (const c of held) {
      push(DEV_KEYBOARD, EV_KEY, keyCode(c), 0);
      push(DEV_KEYBOARD, EV_SYN, SYN_REPORT, 0);
    }
    held.clear();
  };
  const onBack = () => tap(KEY_BACK);
  const onHome = () => tap(KEY_HOMEPAGE);
  const onCtx = (e) => e.preventDefault();

  screen.addEventListener('pointerdown', onDown);
  screen.addEventListener('pointermove', onMove);
  screen.addEventListener('pointerup', onUp);
  screen.addEventListener('pointercancel', onUp);
  screen.addEventListener('keydown', onKeyDown);
  screen.addEventListener('keyup', onKeyUp);
  screen.addEventListener('blur', onBlur);
  screen.addEventListener('contextmenu', onCtx);
  if (back) back.addEventListener('click', onBack);
  if (home) home.addEventListener('click', onHome);
  return function detach() {
    screen.removeEventListener('pointerdown', onDown);
    screen.removeEventListener('pointermove', onMove);
    screen.removeEventListener('pointerup', onUp);
    screen.removeEventListener('pointercancel', onUp);
    screen.removeEventListener('keydown', onKeyDown);
    screen.removeEventListener('keyup', onKeyUp);
    screen.removeEventListener('blur', onBlur);
    screen.removeEventListener('contextmenu', onCtx);
    if (back) back.removeEventListener('click', onBack);
    if (home) home.removeEventListener('click', onHome);
    onBlur();
  };
}

// The Android screen element (#android-screen), created inside the phone frame if the page has none: the placeholder the
// graphics canvas takes over. It stays hidden until the boot runs (main.js).
export function ensureScreen(phone) {
  let el = document.getElementById('android-screen');
  if (!el) {
    el = document.createElement('div');
    el.id = 'android-screen';
    el.className = 'android-screen';
    el.tabIndex = 0;
    el.setAttribute('role', 'application');
    el.setAttribute('aria-label', 'Android screen');
    el.hidden = true;
    phone.appendChild(el);
  }
  return el;
}
