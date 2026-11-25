// The visitor's own session, without the DOM: the record of what happened since Start Android (each event lands once,
// with its time from the press, and is never rewritten) and the "This session" cells of the page's Characteristics
// table. main.js feeds it the events it already sees (the press, the snapshot's load, frames, log lines, input) and
// draws what it returns.
//
// Cells (filled once, from real events only):
//   home      Start Android to the first Android frame of a restore (the snapshot is taken at the home screen)
//   download  the bytes of the snapshot the restore read
//   settings  the first open of Settings, calculator the first open of Calculator: "settled" as tools/latency.mjs
//             measures it (docs/demo.md): from the tap's release to the last frame that changed the picture (more than
//             0.2% of the sampled pixels) before 5 s without a change. The open is the tap released at most 3 s
//             before the log's START line for that app; any other input before it settles drops the measurement.
// The drawer has no event the page can tie to it, so it is not measured here.

export const APPS = [
  { key: 'settings', pkg: 'com.android.settings/.Settings', name: 'Settings' },
  { key: 'calculator', pkg: 'com.android.calculator2/', name: 'Calculator' },
];
const TAP_TO_START_MS = 3000;
const QUIET_MS = 5000;
const GIVE_UP_MS = 30000;
const CHANGED = 0.002;

export function newSession() {
  return { pressAt: null, events: [], keys: new Set(), cells: {}, sig: null, tapAt: null, watch: null };
}

// Start Android: a new session (a retry after a refused snapshot keeps the press and the record: retry true).
export function press(s, now, retry = false) {
  if (retry && s.pressAt !== null) return;
  Object.assign(s, { pressAt: now, events: [], keys: new Set(), cells: {}, sig: null, tapAt: null, watch: null });
  note(s, 'press', 'Start Android pressed', now);
}

// An event of the record: lands once per key. Returns true when it was added.
export function note(s, key, text, now, extra = '') {
  if (s.pressAt === null || s.keys.has(key)) return false;
  s.keys.add(key);
  s.events.push({ key, text, atMs: Math.max(0, now - s.pressAt), extra });
  return true;
}

export function setCell(s, key, value) {
  if (s.cells[key] !== undefined) return false;
  s.cells[key] = value;
  return true;
}

// A frame's signature: a 15-bit colour of every 6th pixel of every 6th row (as tools/latency.mjs keeps them).
export function frameSig(px, w, h) {
  const cols = Math.ceil(w / 6), rows = Math.ceil(h / 6);
  const sig = new Uint16Array(cols * rows);
  let k = 0;
  for (let y = 0; y < h; y += 6) {
    for (let x = 0; x < w; x += 6) {
      const o = (y * w + x) * 4;
      sig[k++] = ((px[o] >> 3) << 10) | ((px[o + 1] >> 3) << 5) | (px[o + 2] >> 3);
    }
  }
  return sig;
}

export function sigChange(a, b) {
  if (!a || !b || a.length !== b.length) return 1;
  let n = 0;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) n++;
  return n / a.length;
}

// A frame arrived: the open being measured moves its last change.
export function onFrameSig(s, sig, now) {
  const change = sigChange(s.sig, sig);
  s.sig = sig;
  if (s.watch && change > CHANGED) s.watch.lastChange = now;
}

// Input sent to the phone. kind 'tap' is a pointer released on the screen; anything else (a press, a key, Back, Home)
// only counts as input that spoils an open being measured.
export function onInput(s, kind, now) {
  if (s.watch && now > s.watch.startAt) s.watch = null;
  if (kind === 'tap') s.tapAt = now;
}

// A log line: an app's START after a tap begins the measurement of its first open.
export function onLine(s, text, now) {
  if (s.watch || s.tapAt === null || !/ActivityTaskManager\(\d+\): START u0 /.test(text)) return;
  const app = APPS.find((a) => text.includes('cmp=' + a.pkg));
  if (!app || s.cells[app.key] !== undefined || now - s.tapAt > TAP_TO_START_MS) return;
  s.watch = { app, t0: s.tapAt, startAt: now, lastChange: null };
}

// The clock: an open settles after QUIET_MS without a change. Returns the app that settled, or null.
export function tick(s, now) {
  const w = s.watch;
  if (!w) return null;
  if (w.lastChange !== null && now - w.lastChange >= QUIET_MS) {
    s.watch = null;
    const ms = w.lastChange - w.t0;
    if (setCell(s, w.app.key, ms)) {
      note(s, w.app.key, `${w.app.name} opened, settled in ${(ms / 1000).toFixed(1)} s`, w.lastChange);
      return w.app;
    }
    return null;
  }
  if (now - w.startAt > GIVE_UP_MS) s.watch = null;
  return null;
}
