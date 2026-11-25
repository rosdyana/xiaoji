// Page side of the web build (M4): starts one worker per run and shows its output.
// window.xn holds the state of the last run, for tests/web.mjs. The Boot Android screen (W2) starts a worker
// for the product boot; window.xb holds its state, and window.xb.screen what the screen shows.
//
// Query parameters: ?theme=<name> (web/theme-<name>.json), ?log=1 (the log panel open), ?cases=1 (the ART
// cases panel, for the tests), ?boot=manifest and ?trace, ?nopack (docs/web-preview.md), ?snapshot=off (always boot)
// and ?snapshot=make (boot, and write a snapshot at the home screen: make snapshot; docs/snapshot.md).

import { BOOT, RESTART } from './cases.js';
import { DEFAULT_THEME, loadTheme } from './theme.js';
import { newFrameView } from './frame.js';
import {
  newScreen, startRun, onDownload, onRestore, onBootTree, onMilestone, onRestart, onError, onEnd, onStop, render as renderScreen,
  onShown, onAnimRun, onFrame, animPhase, animTimings, animNextMs,
} from './bootscreen.js';
import { createRing, attachInput, ensureScreen } from './input.js';
import * as session from './session.js';

const $ = (id) => document.getElementById(id);
const base = new URL('./', location.href).href;
const params = new URLSearchParams(location.search);

window.xn = { status: 'idle', stdout: [], stderr: [], done: null, error: null };

// window.xn.status is the machine-readable state: 'running', 'exit N' or 'error'.
function setStatus(s) {
  $('status').textContent = s;
}

function showTiming(t, f) {
  const rows = [
    ['manifest fetch', t.manifestMs, 'ms'],
    ['module import + instantiate', t.importMs + t.instantiateMs, 'ms'],
    ['MEMFS tree build', t.fsBuildMs, 'ms'],
    ['time to first instruction (from callMain)', t.firstInsnMs, 'ms'],
    ['time to first instruction (from Run)', t.firstInsnSincePressMs, 'ms'],
    ['engine run (callMain)', t.callMainMs, 'ms'],
    ['total (from Run)', t.totalMs, 'ms'],
    ['files in manifest', t.files, ''],
    ['fetch requests (chunks)', f.requests, ''],
    ['distinct files fetched', f.files, ''],
    ['bytes fetched', f.bytes, 'B'],
  ];
  const tbody = rows.map(([k, v, u]) => {
    const val = typeof v === 'number' ? (u === 'ms' ? v.toFixed(1) : String(v)) : 'n/a';
    return `<tr><td>${k}</td><td class="n">${val}</td><td>${u}</td></tr>`;
  });
  $('timing').innerHTML = tbody.join('');
}

$('run').addEventListener('click', () => {
  const name = $('case').value;
  $('out').textContent = '';
  $('err').textContent = '';
  $('timing').innerHTML = '';
  window.xn = { status: 'running', stdout: [], stderr: [], done: null, error: null };
  setStatus('running ' + name);
  window.xn.status = 'running';
  $('run').disabled = true;

  const worker = new Worker(new URL('./worker.js', location.href), { type: 'module' });
  const finish = () => {
    worker.terminate();
    $('run').disabled = false;
  };
  worker.onmessage = (ev) => {
    const m = ev.data;
    if (m.type === 'out') {
      window.xn.stdout.push(m.text);
      $('out').textContent += m.text + '\n';
    } else if (m.type === 'err') {
      window.xn.stderr.push(m.text);
      $('err').textContent += m.text + '\n';
    } else if (m.type === 'done') {
      window.xn.done = m;
      window.xn.status = `exit ${m.exitCode}`;
      setStatus(window.xn.status);
      showTiming(m.timing, m.fetch);
      finish();
    } else if (m.type === 'error') {
      window.xn.error = m.text;
      window.xn.status = 'error';
      setStatus('error');
      $('err').textContent += m.text + '\n';
      finish();
    }
  };
  worker.onerror = (ev) => {
    window.xn.error = ev.message || 'worker error';
    window.xn.status = 'error';
    setStatus('error');
    $('err').textContent += window.xn.error + '\n';
    finish();
  };
  worker.postMessage({ type: 'run', name, base });
});

// ---- Boot Android (W1, W2) ----

const LOG_LINES = 20000;
window.xb = { status: 'idle', lines: 0, milestones: {}, fetch: null, done: null, error: null, screen: null };
let bootWorker = null;
let bootPacker = null; // the pack downloader (web/packloader.js), pack mode
let detachInput = null; // the page's input to the guest (web/input.js), attached while the boot runs
let bootTimer = null;
let scr = null; // the screen's state (bootscreen.js)
let theme = DEFAULT_THEME;
const themeReady = loadTheme(params.get('theme')).then((t) => {
  theme = t.theme;
  applyTheme(t.theme);
  if (t.note) $('themenote').textContent = t.note;
  scr = newScreen(theme);
  render();
  return t;
});

// The theme is set as CSS custom properties (the colours were checked by validateTheme) and as text. They are set on
// the phone: the theme colours the device's screen, not the page around it.
function applyTheme(t) {
  const root = $('phone').style;
  // The stylesheet reads --acc for the accent (the theme key is accent).
  for (const [k, v] of Object.entries(t.colors)) root.setProperty(k === 'accent' ? '--acc' : '--' + k, v);
  $('name').firstElementChild.textContent = t.name;
  $('name').lastElementChild.textContent = t.accentPart;
  $('sub').textContent = t.subtitle;
  if (t.logo.image) {
    $('logo-img').setAttribute('href', t.logo.image);
    $('logo-img').style.display = '';
    $('glyph').style.display = 'none';
  } else {
    $('glyph').textContent = t.logo.text;
  }
}

function setText(el, t) {
  if (el.textContent !== t) el.textContent = t;
}

// The boot animation (W3): the part (bootscreen.js animPhase) and the motion go on the phone as data-anim and
// data-motion, and the CSS in index.html plays them. prefers-reduced-motion is read on each render, so a change
// takes effect at once. A hidden tab pauses the loop (data-paused). A timer renders again when the intro or the
// outro ends, also when no boot runs (the boot's own timer renders every 250 ms).
const reducedMq = window.matchMedia ? window.matchMedia('(prefers-reduced-motion: reduce)') : null;
let animTimer = null;
let animVars = '';

function renderAnim(now) {
  const phone = $('phone');
  const reduced = !!(reducedMq && reducedMq.matches);
  onShown(scr, now);
  const t = animTimings(scr.theme, reduced);
  const vars = `${t.introMs} ${t.outroMs} ${t.spinMs} ${t.breatheMs}`;
  if (vars !== animVars) {
    animVars = vars;
    const root = document.documentElement.style;
    root.setProperty('--intro', t.introMs + 'ms');
    root.setProperty('--outro', t.outroMs + 'ms');
    root.setProperty('--spin', t.spinMs + 'ms');
    root.setProperty('--breathe', t.breatheMs + 'ms');
  }
  const phase = animPhase(scr, now, reduced);
  if (phone.dataset.motion !== t.motion) phone.dataset.motion = t.motion;
  if (phone.dataset.anim !== phase) phone.dataset.anim = phase;
  phone.dataset.glow = t.glow ? 'on' : 'off';
  const next = animNextMs(scr, now, reduced);
  if (animTimer) clearTimeout(animTimer);
  animTimer = next === null ? null : setTimeout(() => {
    animTimer = null;
    render();
  }, Math.max(0, next) + 20);
  return { anim: phase, motion: t.motion };
}

if (reducedMq && reducedMq.addEventListener) reducedMq.addEventListener('change', () => render());
document.addEventListener('visibilitychange', () => {
  if (document.hidden) $('phone').dataset.paused = '1';
  else delete $('phone').dataset.paused;
});
if (document.hidden) $('phone').dataset.paused = '1';

// Draws the screen from its state, and records it in window.xb.screen for the tests.
function render() {
  if (!scr) return;
  const now = performance.now();
  const v = renderScreen(scr, now);
  const phone = $('phone');
  phone.dataset.state = v.state;
  const a = renderAnim(now);
  setText($('stage-label'), v.label);
  setText($('stage-count'), v.count);
  setText($('detail'), v.detail);
  const pct = Math.round(v.fill);
  $('bar-fill').style.width = pct + '%';
  $('bar').setAttribute('aria-valuenow', String(pct));
  [...$('steps').children].forEach((el, k) => {
    el.className = v.steps[k] === 'done' ? 'done' : v.steps[k] === 'now' ? 'now' : '';
  });
  window.xb.screen = {
    state: v.state, phase: scr.phase, stage: scr.stage, label: v.label, count: v.count, detail: v.detail,
    fill: pct, errorKind: v.errorKind, stageLog: scr.stageLog.slice(), fromCache: scr.dl.fromCache,
    anim: a.anim, motion: a.motion,
  };
  noteStages();
  session.tick(sess, now);
  renderSession();
  setControls();
}

// ---- The session record and the "This session" cells (web/session.js) ----

const sess = session.newSession();
window.xb.session = sess;
let shownEvents = 0;
const shownCells = new Set();

function fmtElapsed(ms) {
  const t = ms / 1000;
  if (t < 60) return t.toFixed(t < 10 ? 2 : 1) + ' s';
  const s = Math.round(t);
  return `${Math.floor(s / 60)} min ${String(s % 60).padStart(2, '0')} s`;
}

// Each boot stage the screen enters lands once in the record (bootscreen.js keeps them in stageLog).
function noteStages() {
  if (!scr) return;
  for (const e of scr.stageLog) session.note(sess, 'stage-' + e.index, e.label, e.atMs);
}

function renderSession() {
  const list = $('record');
  if (sess.events.length < shownEvents) {
    list.textContent = '';
    shownEvents = 0;
  }
  if (sess.events.length && shownEvents === 0) list.textContent = '';
  for (; shownEvents < sess.events.length; shownEvents++) {
    const e = sess.events[shownEvents];
    const li = document.createElement('li');
    if (e.extra === 'bad') li.className = 'bad';
    const text = document.createElement('span');
    text.textContent = e.text;
    const t = document.createElement('time');
    t.textContent = fmtElapsed(e.atMs);
    li.append(text, t);
    list.append(li);
  }
  for (const td of document.querySelectorAll('td.cell[data-cell]')) {
    const key = td.dataset.cell;
    const v = sess.cells[key];
    const state = v === undefined ? 'off' : 'on';
    if (td.dataset.drawn === state) continue;
    const bytes = key === 'download';
    const slots = bytes ? 4 : 3;
    let text = '', unit = bytes ? 'MB' : 's';
    if (v !== undefined) {
      const n = bytes ? v / 1048576 : v / 1000;
      text = n < 10 ** (slots - 1) ? n.toFixed(1) : String(Math.round(n));
    }
    td.innerHTML = segDisplay(text, slots, unit) +
      `<span class="sr">${v === undefined ? 'not measured yet' : text + ' ' + unit}</span>`;
    if (td.dataset.drawn === 'off' && state === 'on') {
      td.classList.remove('just-lit');
      void td.offsetWidth;
      td.classList.add('just-lit');
    }
    td.dataset.drawn = state;
  }
  window.xb.session = sess;
}

// A seven-segment readout (SVG) for a "This session" cell: slots digits with a decimal point before the last one. The
// segments a value does not light stay drawn, unlit, so an unmeasured cell reads as a display that is off. text is the
// value ('' for none), right-aligned in the slots.
const SEGS = { 0: 'abcdef', 1: 'bc', 2: 'abged', 3: 'abgcd', 4: 'fgbc', 5: 'afgcd', 6: 'afgedc', 7: 'abc', 8: 'abcdefg', 9: 'abcdfg' };
function segPolys(x0) {
  const h = 0.9;
  const hz = (y) => `${x0 + 1.6},${y} ${x0 + 1.6 + h},${y - h} ${x0 + 8.4 - h},${y - h} ${x0 + 8.4},${y} ${x0 + 8.4 - h},${y + h} ${x0 + 1.6 + h},${y + h}`;
  const vt = (x, y1, y2) => `${x0 + x},${y1} ${x0 + x + h},${y1 + h} ${x0 + x + h},${y2 - h} ${x0 + x},${y2} ${x0 + x - h},${y2 - h} ${x0 + x - h},${y1 + h}`;
  return { a: hz(1), g: hz(9), d: hz(17), f: vt(1, 1.6, 8.4), b: vt(9, 1.6, 8.4), e: vt(1, 9.6, 16.4), c: vt(9, 9.6, 16.4) };
}
function segDisplay(text, slots, unit) {
  const intPart = text.includes('.') ? text.split('.')[0] : text;
  const frac = text.includes('.') ? text.split('.')[1] : '';
  const digits = (intPart + frac).padStart(slots, ' ').slice(-slots).split('');
  const dpAt = frac ? slots - 2 : -1; // the point follows this digit
  const W = 13;
  let body = '';
  digits.forEach((ch, k) => {
    const on = SEGS[ch] || '';
    const p = segPolys(k * W);
    for (const [seg, pts] of Object.entries(p)) body += `<polygon class="${on.includes(seg) ? 'on' : 'off'}" points="${pts}"/>`;
    if (k === slots - 2) body += `<rect class="${k === dpAt ? 'on' : 'off'}" x="${k * W + 10.4}" y="15.8" width="1.8" height="1.8"/>`;
  });
  const w = slots * W - 2;
  return `<span class="seg" aria-hidden="true"><svg viewBox="-1 0 ${w + 1} 18" width="${((w + 1) * 14) / 18}" height="14">` +
    `<g transform="skewX(-7) translate(2 0)">${body}</g></svg><span class="unit${text ? '' : ' off'}">${unit}</span></span>`;
}

// The display's scale on the page, in the figure's caption.
function showScale() {
  const w = $('phone').getBoundingClientRect().width;
  const s = Math.round((w / 540) * 1000) / 1000;
  setText($('scale'), w ? `Shown at ${s} scale.` : '');
}
window.addEventListener('resize', showScale);
showScale();

// Start and Stop: Start when no worker runs, Stop while one does.
function setControls() {
  const running = bootWorker !== null;
  const screenEl = $('android-screen'); // the Android screen (web/input.js): shown while the boot runs
  if (screenEl) screenEl.hidden = !running;
  $('btn-back').disabled = !running;
  $('btn-home').disabled = !running;
  $('boot').hidden = running;
  $('stop').hidden = !running;
  $('boot').disabled = running;
  $('stop').disabled = !running;
}

function setBootStatus(s) {
  window.xb.status = s;
  $('bstatus').textContent = s;
}

// What failed, in the visitor's words: the boot screen's label and detail, and the record's line with the recovery.
// The raw error stays in the log (Show log).
let restoreStarted = false;
function describeFailure(text) {
  const net = /NetworkError|Failed to fetch|network|HTTP \d{3}|status \d{3}|ERR_/i.test(String(text || ''));
  const restoring = restoreStarted && !(window.xb.restore && window.xb.restore.state === 'loaded');
  if (net && restoring) {
    return { label: 'Snapshot download failed', detail: 'Network error · press Start Android to retry',
      record: 'The snapshot download failed (network error). Press Start Android to try again, or open the page with ?snapshot=off to boot instead.' };
  }
  if (net) {
    return { label: 'Download failed', detail: 'Network error · press Start Android to retry',
      record: 'A download failed (network error). Press Start Android to try again.' };
  }
  return { label: 'The engine stopped', detail: 'Press Start Android to try again',
    record: 'The engine stopped unexpectedly. Press Start Android to try again; Show log has the details.' };
}
function failed(text, now) {
  const f = describeFailure(text);
  session.note(sess, 'error', f.record, now, 'bad');
  onError(scr, { label: f.label, detail: f.detail });
}

function fmtBytes(n) {
  return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : n >= 1024 ? (n / 1024).toFixed(1) + ' KB' : n + ' B';
}

function renderMilestoneRows() {
  $('bms').innerHTML = '<tr><th>Milestone</th><th>Wall</th><th>Engine time</th><th>Instructions</th></tr>' +
    BOOT.milestones.map((ms) =>
    `<tr id="ms-${ms.id}"><td>${ms.label}</td><td class="n" data-k="wall">-</td>` +
    `<td class="n" data-k="ms">-</td><td class="n" data-k="insn">-</td></tr>`).join('');
}

function appendLog(text) {
  const log = $('blog');
  log.append(document.createTextNode(text + '\n'));
  while (log.childNodes.length > LOG_LINES) log.removeChild(log.firstChild);
  log.scrollTop = log.scrollHeight;
  window.xb.lines++;
}

function stopBoot(status) {
  if (bootWorker) bootWorker.terminate();
  bootWorker = null;
  if (detachInput) detachInput();
  detachInput = null;
  if (bootPacker) bootPacker.terminate();
  bootPacker = null;
  if (bootTimer) clearInterval(bootTimer);
  bootTimer = null;
  if (status) setBootStatus(status);
  render();
}

let snapshotRefused = null; // why the snapshot was refused (then this page load boots)
let retrying = false; // the boot that follows a refused snapshot: the same session, from the visitor's press

// The booted snapshot's description (xj/snapshot.json), fetched once when the page loads: its download size goes under
// Start Android, and the press uses it without fetching it again. null when there is none or it is not used.
const glParam = params.get('gl') === 'null' ? 'null' : 'webgl';
const snapWanted = () => !snapshotRefused && !['off', 'make'].includes(params.get('snapshot')) && glParam === 'webgl' &&
  params.get('boot') !== 'manifest' && self.crossOriginIsolated;
const snapInfo = !snapWanted() ? Promise.resolve(null) : fetch(base + 'xj/snapshot.json', { cache: 'no-store' })
  .then((r) => (r.ok ? r.json() : null)).catch(() => null);
snapInfo.then((snap) => {
  const note = $('dlnote');
  if (!self.crossOriginIsolated) {
    note.textContent = 'This page is not cross-origin isolated, so Android starts slowly';
  } else if (snap && Number.isFinite(snap.download_bytes)) {
    note.textContent = `Downloads ${Math.round(snap.download_bytes / 1048576)} MB`;
    if (snap.build) {
      $('buildnote').textContent = `Snapshot for engine build ${snap.build}.`;
      $('buildnote').hidden = false;
    }
  } else {
    note.textContent = 'No snapshot here: Android boots from the start, which takes minutes';
  }
});

$('boot').addEventListener('click', async () => {
  const pressAt = performance.now();
  const retry = retrying;
  retrying = false;
  session.press(sess, pressAt, retry);
  renderSession();
  await themeReady;
  $('blog').textContent = '';
  $('bfetch').textContent = '0 requests, 0 files, 0 bytes';
  renderMilestoneRows();
  window.xb = { status: 'loading', lines: 0, milestones: {}, fetch: null, done: null, error: null, screen: null };
  setBootStatus('loading');
  // A new boot starts with the boot screen: the Android canvas is hidden until its first frame.
  $('android-screen').hidden = true;
  delete $('phone').dataset.frame;
  startRun(scr);
  restoreStarted = false;
  onAnimRun(scr);
  bootTimer = setInterval(render, 250);

  // The input ring (web/input.js) is shared with the engine's worker, which drains it (docs/input.md); off without SharedArrayBuffer.
  const inputRing = createRing();
  if (inputRing) detachInput = attachInput({ ring: inputRing, screen: ensureScreen($('phone')), back: $('btn-back'), home: $('btn-home') });
  const worker = new Worker(new URL('./worker.js', location.href), { type: 'module' });
  bootWorker = worker;
  // The Android screen (G2): the frame counters are shared with the engine's worker, which needs cross-origin
  // isolation (the pack path needs it too). ?gl=null keeps the null backend (no frames).
  const bootT0 = performance.now();
  const frameSab = self.crossOriginIsolated ? new SharedArrayBuffer(8) : null;
  const frames = newFrameView($('android-screen'), frameSab ? new Int32Array(frameSab) : null);
  const glBackend = params.get('gl') === 'null' ? 'null' : 'webgl';
  // A booted snapshot (docs/snapshot.md): xj/snapshot.json, restored by the worker when it matches the engine build
  // and the tree. Only with the WebGL backend and the chunk store; ?snapshot=off boots, ?snapshot=make makes one.
  const snapParam = params.get('snapshot');
  const snap = snapWanted() ? await snapInfo : null;
  if (snap) session.note(sess, 'snap', `Snapshot found: ${fmtBytes(snap.download_bytes)} to download`, performance.now());
  else if (!snapshotRefused) session.note(sess, 'nosnap', 'No snapshot for this page: booting Android (several minutes)', performance.now());
  window.xb.restore = snap ? { state: 'trying', bytes: snap.download_bytes } : snapshotRefused ? { state: 'refused', text: snapshotRefused } : null;
  setControls();
  render();
  // The packs (pack mode) come from a second worker, so the engine's worker can block on a pack while the rest
  // download (web/packloader.js, docs/loading.md L2). Its messages are the packs' layout (relayed to the engine's
  // worker, which needs it before the boot), the bytes of the packs (the boot screen), and the counters.
  let packCtrl = null;
  let packStats = null; // the downloader's counters, merged into the engine's store stats and the fetch line
  let packNet = { requests: 0, bytes: 0 };
  const failPacks = () => {
    if (!packCtrl) return;
    for (let j = 0; j < packCtrl.length; j++) {
      if (Atomics.load(packCtrl, j) === 0) {
        Atomics.store(packCtrl, j, 2); // failed: the engine reads those chunks synchronously
        Atomics.notify(packCtrl, j);
      }
    }
  };
  const onPacker = (m) => {
    if (m.type === 'layout') {
      packCtrl = m.ctrl;
      worker.postMessage({ type: 'layout', manifest: m.manifest, manifestBytes: m.manifestBytes, sab: m.sab, ctrl: m.ctrl });
    } else if (m.type === 'layout-error') {
      appendLog(`packs: ${m.text}`);
      worker.postMessage({ type: 'layout', error: m.text });
    } else if (m.type === 'download') {
      onDownload(scr, m);
    } else if (m.type === 'packstats') {
      packStats = m.stats;
      packNet = { requests: m.stats.packDownloads, bytes: m.stats.packNetBytes };
      if (window.xb.stats && window.xb.stats.store) window.xb.stats.store = { ...window.xb.stats.store, ...packStats };
    } else if (m.type === 'pack-error') {
      appendLog(`pack ${m.pack}: ${m.text}`);
    } else if (m.type === 'error') {
      appendLog(`packs: ${m.text}`);
      failPacks();
    }
  };
  worker.onmessage = (ev) => {
    const m = ev.data;
    const now = performance.now();
    if (m.type === 'restore-start' || m.type === 'restore-progress') {
      restoreStarted = true;
      onRestore(scr, { loaded: m.loaded || 0, total: m.total });
      render();
    } else if (m.type === 'restore-loaded') {
      window.xb.restore = { ...window.xb.restore, state: 'loaded', loadedWall: m.wall, loadMs: m.loadMs, bytes: m.bytes, timing: m.timing };
      appendLog(`snapshot: ${fmtBytes(m.bytes)} in ${(m.loadMs / 1000).toFixed(2)} s`);
      session.setCell(sess, 'download', m.bytes);
      session.note(sess, 'loaded', `Snapshot read: ${fmtBytes(m.bytes)} in ${(m.loadMs / 1000).toFixed(1)} s; restoring`, now);
      renderSession();
      setBootStatus('running');
    } else if (m.type === 'restore-refused') {
      // The snapshot is another build's or another tree's: boot instead (this page load does not try it again).
      appendLog(`snapshot not used: ${m.text}; booting`);
      snapshotRefused = m.text;
      session.note(sess, 'refused', "The snapshot doesn't match this engine; booting instead (several minutes)", now, 'bad');
      worker.onmessage = null;
      stopBoot('snapshot refused');
      retrying = true;
      setTimeout(() => $('boot').click(), 0);
    } else if (m.type === 'boot-start') {
      // The machine status stays 'loading' (Stop depends on it); only the display text changes.
      $('bstatus').textContent = `loading ${m.files} entries`;
    } else if (m.type === 'download') {
      onDownload(scr, m);
    } else if (m.type === 'boot-tree') {
      onBootTree(scr, now);
      setBootStatus('running');
      if (window.xb.restore && window.xb.restore.state === 'loaded') session.note(sess, 'engine', 'Engine running from the snapshot', now);
    } else if (m.type === 'bootline') {
      appendLog(m.text);
      // Activity starts (a tap that opens an app, tools/snapshot-web.mjs) and the engine's snapshot lines, kept apart from
      // the log panel, which keeps only its last lines.
      if (params.has('keeplog')) (window.xb.all = window.xb.all || []).push(m.text); // the whole log (?keeplog, tests)
      if (window.xbHooks && window.xbHooks.onLine) window.xbHooks.onLine(m.text, now); // measurement (tools/latency.mjs)
      if (/ActivityTaskManager\(\d+\): START u0 |^xiaoji: (snapshot|restore|gl\(webgl\): snapshot)/.test(m.text)) {
        (window.xb.notes = window.xb.notes || []).push(m.text);
        session.onLine(sess, m.text, now);
        if (/category\.HOME\]/.test(m.text) && !(window.xb.restore && window.xb.restore.state === 'loaded')) {
          session.note(sess, 'launcher', 'Home screen started (launcher)', now);
        }
      }
      if (m.text.startsWith('xiaoji: Starting phase 100')) window.xb.statsAt = m.text; // ?statsat (A/B runs)
      if (RESTART.test(m.text)) {
        session.note(sess, 'restart', "Android's framework stopped; init is restarting it", now, 'bad');
        onRestart(scr);
        render();
      }
    } else if (m.type === 'milestone') {
      const row = $('ms-' + m.id);
      if (row && m.stamp) {
        row.querySelector('[data-k="wall"]').textContent = (m.wall / 1000).toFixed(2) + ' s';
        row.querySelector('[data-k="ms"]').textContent = (m.stamp.ms / 1000).toFixed(2) + ' s';
        row.querySelector('[data-k="insn"]').textContent = (m.stamp.insn / 1e9).toFixed(3) + ' G';
      }
      window.xb.milestones[m.id] = { label: m.label, wall: m.wall, stamp: m.stamp, line: m.line };
      onMilestone(scr, m.id, m.line, now);
      render();
    } else if (m.type === 'fetch') {
      const requests = m.requests + packNet.requests;
      const bytes = m.bytes + packNet.bytes;
      window.xb.fetch = { requests, files: m.files, bytes };
      $('bfetch').textContent = `${requests} requests, ${m.files} files, ${fmtBytes(bytes)}`;
    } else if (m.type === 'stats') {
      if (m.store && packStats) m.store = { ...m.store, ...packStats };
      window.xb.stats = m;
    } else if (m.type === 'trace') {
      window.xb.trace = m;
    } else if (m.type === 'done') {
      window.xb.done = m;
      session.note(sess, 'done', `The engine ended (exit ${m.exitCode})`, now, m.exitCode ? 'bad' : '');
      onEnd(scr, m.exitCode, now);
      stopBoot(`exit ${m.exitCode}`);
      window.xb.status = 'done';
    } else if (m.type === 'error') {
      window.xb.error = m.text;
      appendLog(m.text);
      failed(m.text, now);
      stopBoot('error');
    } else if (m.type === 'out') {
      appendLog(m.text);
    } else if (m.type === 'frame') {
      // The engine's composed frame (rcFBPost). window.xb.frames is the count and the first and last frame times.
      window.xb.frames = frames.show(m, now - bootT0);
      session.onFrameSig(sess, session.frameSig(new Uint8Array(m.px), m.w, m.h), now);
      // A measurement hook (tools/latency.mjs; window.xbHooks outlives the boot's new window.xb): the frame's pixels
      // and its arrival time on the page.
      if (window.xbHooks && window.xbHooks.onFrame) window.xbHooks.onFrame(m, now);
      if (!$('phone').dataset.frame) {
        // The first frame: the boot animation's outro (W3) fades the boot screen into the picture.
        $('phone').dataset.frame = '1';
        if (window.xb.restore && window.xb.restore.state === 'loaded') {
          window.xb.restore.firstFrameWall = now - bootT0;
          session.setCell(sess, 'home', now - sess.pressAt);
          session.note(sess, 'frame', 'Home screen: first frame', now);
        } else {
          session.note(sess, 'frame', 'First Android frame', now);
        }
        onFrame(scr, now);
        render();
      }
    }
  };
  worker.onerror = (ev) => {
    window.xb.error = ev.message || 'worker error';
    appendLog(window.xb.error);
    failed(window.xb.error, performance.now());
    stopBoot('error');
  };
  // ?trace records the boot's file access (web/trace.js) for docs/loading.md; off by default.
  // The chunk store and boot packs (web/chunks.js, docs/loading.md) are the default. ?boot=manifest is the
  // lazy-file manifest path, kept until the pack path has run for a while.
  const mode = params.get('boot') === 'manifest' ? 'manifest' : 'pack';
  // ?lazy=0 fills file mappings at mmap time (the A/B baseline, docs/loading.md L2); ?lazywin=BYTES sets the
  // engine's lazy fill window (a power of two from 4096 to 1 MiB). Both are engine arguments.
  worker.postMessage({
    type: 'boot', base: new URL('./', location.href).href, trace: params.has('trace'), mode,
    input: inputRing ? inputRing.buffer : null,
    lazy: params.get('lazy') !== '0', lazyWindow: params.get('lazywin'), statsAt: params.has('statsat'),
    early: params.get('early'), gl: glBackend, frameSab, debugFaults: params.get('debug') === '1', dumpAt: params.get('dumpat'),
    timeoutS: params.get('timeout'),
    snapshot: snapParam, snap, snapAfter: params.get('snapafter'),
    // ?load=MS: the engine's load report every MS ms (--load-every, docs/demo.md); ?prop=K=V (repeatable): a system
    // property for a boot (--prop; a restore keeps the snapshot's), for A/B runs of the product's settings.
    loadEvery: params.get('load'), props: params.getAll('prop'),
    snapQuiet: params.get('snapquiet'), // ?snapshot=make: --snapshot-quiet PCT:WINDOW:MAX (docs/demo.md)
    snapAt: params.get('snapat'), // ?snapshot=make: the condition, log:TEXT (default: the launcher's HOME start)
    // A restore (docs/demo.md, P1): ?prehot=0 leaves out --jit-prehot; ?inflaters=N sets the helpers that inflate the
    // snapshot's chunks (0: the engine's worker inflates them itself).
    prehot: params.get('prehot'), inflaters: params.get('inflaters'), prefetch: params.get('prefetch'),
  });
  if (mode === 'pack' && !snap) {
    // ?early=MB: the engine starts once the first MB of packs is in (default: the first pack).
    if (self.crossOriginIsolated) {
      const packer = new Worker(new URL('./packloader.js', location.href), { type: 'module' });
      bootPacker = packer;
      packer.onmessage = (ev) => onPacker(ev.data);
      packer.onerror = (ev) => {
        appendLog(`packs: ${ev.message || 'the downloader stopped'}`);
        failPacks();
        if (!packCtrl) worker.postMessage({ type: 'layout', error: 'the downloader stopped' });
      };
      packer.postMessage({ type: 'start', base: new URL('./', location.href).href, noPacks: params.has('nopack') });
    } else {
      // Without cross-origin isolation there is no SharedArrayBuffer: the engine reads every chunk synchronously.
      appendLog('packs: the page is not cross-origin isolated (serve with COOP/COEP); reading synchronously');
      worker.postMessage({ type: 'layout', error: 'not cross-origin isolated' });
    }
  }
});

$('stop').addEventListener('click', () => {
  if (window.xb.status === 'loading' || window.xb.status === 'running') {
    onStop(scr);
    session.note(sess, 'stop', 'Stopped', performance.now());
    stopBoot('stopped');
  }
});

// Input to the phone, for the session's app-open timing (web/session.js): a tap's release starts it, other input spoils it.
{
  const scrEl = $('android-screen');
  const input = (kind) => () => session.onInput(sess, kind, performance.now());
  scrEl.addEventListener('pointerdown', input('down'), true);
  scrEl.addEventListener('pointerup', input('tap'), true);
  scrEl.addEventListener('keydown', input('key'), true);
  $('btn-back').addEventListener('click', input('back'), true);
  $('btn-home').addEventListener('click', input('home'), true);
}

// The log panel: hidden by default, opened by "Show log" or ?log=1. The ART cases: ?cases=1.
function setLogOpen(open) {
  $('devlog').hidden = !open;
  $('logtoggle').textContent = open ? 'Hide log' : 'Show log';
  $('logtoggle').setAttribute('aria-expanded', String(open));
}
$('logtoggle').addEventListener('click', () => {
  setLogOpen($('devlog').hidden);
  if (!$('devlog').hidden) $('devlog').scrollIntoView({ block: 'start' });
});
setLogOpen(params.has('log'));
$('cases').hidden = !params.has('cases');
renderMilestoneRows();
renderSession();
