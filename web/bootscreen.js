// State of the Boot Android screen (W2), without the DOM. The page feeds it the worker's events (download
// progress, the boot's milestones, errors, the end of the run) and render() gives what the screen shows.
// main.js draws it, and window.xb.screen records it for tests/web.mjs.
//
// Phases: idle, download (the packs, before the engine starts), boot (the engine runs; the stage is
// theme.stages[stage]), stopped (Stop), done (the engine ended). An error (a system_server fatal exception,
// or the worker's error) is shown over the phase and is sticky; the worker keeps running, and the screen
// never claims progress it did not see.
//
// The bar is one value over the boot: the stages' expected seconds (BOOT.stageSeconds) weight the slices.
// Within a slice the fill is CAP * (1 - exp(-t / expected)), so it eases toward the end of the slice and
// never reaches it. The value never goes back (the largest so far is shown), and it jumps to the start of the
// next slice when that stage is entered.
import { BOOT } from './cases.js';

const CAP = 0.95;

export function newScreen(theme) {
  return {
    theme,
    phase: 'idle',
    stage: -1,
    enteredAt: 0,
    stageLog: [],
    dl: { loaded: 0, total: 0, fromCache: false, cacheable: false },
    classes: null,
    errorKind: null, // null, 'fatal' (system_server) or 'error' (the worker)
    restarted: false, // init restarted the framework (RESTART in cases.js)
    fill: 0,
    exitCode: null,
  };
}

// A new run: the download phase starts.
export function startRun(s) {
  Object.assign(s, {
    phase: 'download', stage: -1, enteredAt: 0, stageLog: [], classes: null, errorKind: null, restarted: false,
    dl: { loaded: 0, total: 0, fromCache: false, cacheable: false }, fill: 0, exitCode: null,
  });
}

// The packs keep downloading after the engine has started (web/packloader.js): the bytes are kept in the boot
// phase too, and shown in its detail line.
export function onDownload(s, p) {
  if (s.phase !== 'download' && s.phase !== 'boot') return;
  s.dl = { loaded: p.loaded, total: p.total, fromCache: !!p.fromCache, cacheable: !!p.cacheable };
}

// The engine starts: the first stage ("boot") begins. The bar starts again at 0: the download's bar was the
// bytes of the packs, and the boot's is its own estimate.
export function onBootTree(s, now) {
  if (s.phase !== 'download') return;
  s.phase = 'boot';
  s.bootAt = now;
  s.fill = 0;
  enter(s, 0, now);
}

function enter(s, k, now) {
  s.stage = k;
  s.enteredAt = now;
  s.stageLog.push({ index: k, label: s.theme.stages[k].label, atMs: Math.round(now) });
}

export function onMilestone(s, id, line, now) {
  if (s.phase !== 'boot') return;
  if (id === 'fatal') {
    if (!s.errorKind) s.errorKind = 'fatal';
    return;
  }
  if (id === 'preload') {
    const m = /preloaded (\d+) classes/.exec(line || '');
    if (m) s.classes = Number(m[1]);
  }
  const k = s.theme.stages.findIndex((st) => st.from === id);
  if (k > s.stage) enter(s, k, now);
}

// A booted snapshot is restored instead of a boot (docs/snapshot.md): the 'restore' phase, with the snapshot's download
// as its progress; it lasts until the first frame (onFrame).
export function onRestore(s, p) {
  if (s.phase !== 'download' && s.phase !== 'restore') return;
  s.phase = 'restore';
  s.dl = { loaded: p.loaded || 0, total: p.total || 0, fromCache: false, cacheable: false };
}

// The framework exited and init restarts it: the screen says Android stopped, and it stays so.
export function onRestart(s) {
  if (s.phase === 'boot') s.restarted = true;
}

export function onError(s) {
  if (!s.errorKind) s.errorKind = 'error';
}

export function onEnd(s, exitCode, now) {
  if (s.phase === 'stopped') return;
  s.phase = 'done';
  s.exitCode = exitCode;
  s.endedAt = now;
}

export function onStop(s) {
  if (s.phase === 'idle' || s.phase === 'done') return;
  s.phase = 'stopped';
}

const mb = (n) => (n / 1048576).toFixed(1);

function fmtSec(t) {
  const s = Math.floor(t);
  return s < 60 ? `${s} s` : `${Math.floor(s / 60)} min ${s % 60} s`;
}

function bootFill(s, now) {
  const secs = BOOT.stageSeconds;
  if (s.stage >= secs.length) return s.fill;
  const total = secs.reduce((a, b) => a + b, 0);
  let done = 0;
  for (let j = 0; j < s.stage; j++) done += secs[j];
  const T = secs[s.stage];
  const t = Math.max(0, (now - s.enteredAt) / 1000);
  const cur = CAP * (1 - Math.exp(-t / T)) * T;
  return ((done + cur) / total) * 100;
}

// What the screen shows now. Updates s.fill (the monotonic bar value) as a side effect.
// Returns { state, label, count, detail, fill, steps, errorKind }: steps has one entry per stage, each
// 'done', 'now' or '' (pending).
export function render(s, now) {
  const n = s.theme.stages.length;
  const steps = Array(n).fill('');
  let label = '';
  let count = '';
  let detail = '';
  let state = s.phase;
  let raw = s.fill;

  if (s.phase === 'idle') {
    label = 'Ready';
    detail = 'Press Boot Android to start';
  } else if (s.phase === 'download') {
    const d = s.dl;
    const pct = d.total ? Math.min(100, (d.loaded / d.total) * 100) : 0;
    raw = pct;
    steps[0] = 'now';
    if (d.fromCache) {
      label = 'Loading from cache';
      detail = `${mb(d.total)} MB`;
    } else {
      label = 'Downloading system';
      detail = `${mb(d.loaded)} of ${mb(d.total)} MB` + (d.cacheable ? ' · cached next time' : '');
    }
    count = `${Math.floor(pct)}%`;
  } else if (s.phase === 'restore') {
    const d = s.dl;
    raw = d.total ? Math.min(100, (d.loaded / d.total) * 100) : 0;
    steps[0] = 'now';
    label = 'Restoring Android';
    detail = d.total && d.loaded < d.total ? `${mb(d.loaded)} of ${mb(d.total)} MB` : 'Starting from the home screen';
    count = `${Math.floor(raw)}%`;
  } else if (s.phase === 'boot') {
    if (s.stage >= 0) {
      raw = bootFill(s, now);
      for (let k = 0; k < s.stage; k++) steps[k] = 'done';
      steps[s.stage] = 'now';
      label = s.theme.stages[s.stage].label;
      count = `${s.stage + 1} / ${n}`;
      const parts = [];
      if (s.classes !== null && s.stage === 1) parts.push(`${s.classes.toLocaleString('en-US')} classes`);
      parts.push(fmtSec((now - (s.bootAt ?? s.enteredAt)) / 1000) + ' booting');
      if (s.dl.total && s.dl.loaded < s.dl.total) parts.push(`packs ${mb(s.dl.loaded)} of ${mb(s.dl.total)} MB`);
      detail = parts.join(' · ');
    }
  } else if (s.phase === 'stopped') {
    label = 'Stopped';
    detail = 'Press Boot Android to start again';
    if (s.stage >= 0) {
      count = `${s.stage + 1} / ${n}`;
      for (let k = 0; k < s.stage; k++) steps[k] = 'done';
      steps[s.stage] = 'now';
    }
  } else if (s.phase === 'done') {
    label = 'Boot ended';
    detail = `exit ${s.exitCode}`;
    if (s.stage >= 0) {
      count = `${s.stage + 1} / ${n}`;
      for (let k = 0; k < s.stage; k++) steps[k] = 'done';
      steps[s.stage] = 'now';
    }
  }

  // Calm and honest: the bar stays where the screen last saw it, and nothing is claimed beyond the log.
  const failed = s.restarted || !!s.errorKind;
  if (s.restarted) {
    state = 'error';
    label = 'Android stopped · Restarting…';
    detail = 'The framework exited; init restarts it';
    raw = s.fill;
  } else if (s.errorKind) {
    state = 'error';
    label = 'Something went wrong';
    detail = s.errorKind === 'fatal' ? 'system_server stopped' : 'The engine stopped';
    raw = s.fill;
  }

  // The bar moves only while the run is moving: not in an error, and not after Stop or the end.
  if (!failed && s.phase !== 'stopped' && s.phase !== 'done') {
    s.fill = Math.max(s.fill, Math.min(100, raw));
  }
  return { state, label, count, detail, fill: s.fill, steps, errorKind: s.errorKind };
}

// ---- The boot animation (W3) ----
//
// The logo animates like a phone's boot animation, in three parts: an intro that plays once when the screen is
// first shown (the ring, the arc, the glyph and the wordmark come in), a loop while the run works (the arc spins,
// the glow breathes), and an outro when Android's first frame arrives (the boot screen fades into the picture).
// animPhase gives the part to show now; main.js sets it as the phone's data-anim, and the CSS in index.html draws
// it with transform and opacity only.
//
// Phases: 'intro' (once, theme.animation.introMs after the screen is shown), 'loop' (download, boot or any other
// working phase), 'rest'
// (idle, stopped, done: the arc stands still), 'calm' (an error or a restart before any frame: the arc stops and
// dims, nothing reads as success), 'outro' (outroMs after the first frame), 'off' (the boot screen is hidden).
//
// Motion: 'full', 'reduced' (prefers-reduced-motion: no intro and no loop, a short fade for the outro) or 'none'
// (theme animation style "none": nothing moves, the outro is a cut). The phases are the same in each; the CSS
// only animates with 'full'. The state is kept in s.anim, apart from the screen's own fields.

export const ANIM_DEFAULT = { style: 'default', introMs: 2000, outroMs: 600, spinMs: 1600, breatheMs: 3600, glow: true };
export const REDUCED_OUTRO_MS = 200;

function animState(s) {
  if (!s.anim) s.anim = { shownAt: null, frameAt: null };
  return s.anim;
}

export function animMotion(theme, reducedMotion) {
  const a = (theme && theme.animation) || ANIM_DEFAULT;
  if (a.style === 'none') return 'none';
  return reducedMotion ? 'reduced' : 'full';
}

// The timings in effect: the theme's, with the intro off and the outro short or cut when motion is not full.
export function animTimings(theme, reducedMotion) {
  const a = { ...ANIM_DEFAULT, ...((theme && theme.animation) || {}) };
  const motion = animMotion(theme, reducedMotion);
  return {
    motion,
    introMs: motion === 'full' ? a.introMs : 0,
    outroMs: motion === 'full' ? a.outroMs : motion === 'reduced' ? Math.min(a.outroMs, REDUCED_OUTRO_MS) : 0,
    spinMs: a.spinMs,
    breatheMs: a.breatheMs,
    glow: a.glow,
  };
}

// The screen is first drawn: the intro starts (only the first call counts).
export function onShown(s, now) {
  const a = animState(s);
  if (a.shownAt === null) a.shownAt = now;
}

// A new run: the boot screen is back, so the outro of the last run is forgotten.
export function onAnimRun(s) {
  animState(s).frameAt = null;
}

// Android's first frame: the outro starts (later frames do not restart it).
export function onFrame(s, now) {
  const a = animState(s);
  if (a.frameAt === null) a.frameAt = now;
}

export function animPhase(s, now, reducedMotion) {
  const a = animState(s);
  const t = animTimings(s.theme, reducedMotion);
  if (a.frameAt !== null) return now - a.frameAt < t.outroMs ? 'outro' : 'off';
  if (s.restarted || s.errorKind) return 'calm';
  if (a.shownAt !== null && now - a.shownAt < t.introMs) return 'intro';
  // Every phase that is not at rest is work (download, boot, and any phase added later), so the arc turns.
  return s.phase === 'idle' || s.phase === 'stopped' || s.phase === 'done' ? 'rest' : 'loop';
}

// Milliseconds until animPhase changes by time alone (the end of the intro or of the outro), or null.
export function animNextMs(s, now, reducedMotion) {
  const a = animState(s);
  const t = animTimings(s.theme, reducedMotion);
  const p = animPhase(s, now, reducedMotion);
  if (p === 'outro') return a.frameAt + t.outroMs - now;
  if (p === 'intro') return a.shownAt + t.introMs - now;
  return null;
}
