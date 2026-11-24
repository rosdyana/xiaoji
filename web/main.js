// Page side of the web build (M4): starts one worker per run and shows its output.
// window.xn holds the state of the last run, for tests/web.mjs. The Boot Android screen (W2) starts a worker
// for the product boot; window.xb holds its state, and window.xb.screen what the screen shows.
//
// Query parameters: ?theme=<name> (web/theme-<name>.json), ?log=1 (the log panel open), ?cases=1 (the ART
// cases panel, for the tests), ?boot=manifest and ?trace, ?nopack (docs/web-preview.md).

import { BOOT, RESTART } from './cases.js';
import { DEFAULT_THEME, loadTheme } from './theme.js';
import {
  newScreen, startRun, onDownload, onBootTree, onMilestone, onRestart, onError, onEnd, onStop, render as renderScreen,
} from './bootscreen.js';
import { createRing, attachInput, ensureScreen } from './input.js';

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

// The theme is set as CSS custom properties (the colours were checked by validateTheme) and as text.
function applyTheme(t) {
  const root = document.documentElement.style;
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

// Draws the screen from its state, and records it in window.xb.screen for the tests.
function render() {
  if (!scr) return;
  const v = renderScreen(scr, performance.now());
  const phone = $('phone');
  phone.dataset.state = v.state;
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
  };
  setControls();
}

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

$('boot').addEventListener('click', async () => {
  await themeReady;
  $('blog').textContent = '';
  $('bfetch').textContent = '0 requests, 0 files, 0 bytes';
  renderMilestoneRows();
  window.xb = { status: 'loading', lines: 0, milestones: {}, fetch: null, done: null, error: null, screen: null };
  setBootStatus('loading');
  startRun(scr);
  bootTimer = setInterval(render, 250);

  // The input ring (web/input.js) is shared with the engine's worker, which drains it (docs/input.md); off without SharedArrayBuffer.
  const inputRing = createRing();
  if (inputRing) detachInput = attachInput({ ring: inputRing, screen: ensureScreen($('phone')), back: $('btn-back'), home: $('btn-home') });
  const worker = new Worker(new URL('./worker.js', location.href), { type: 'module' });
  bootWorker = worker;
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
    if (m.type === 'boot-start') {
      // The machine status stays 'loading' (Stop depends on it); only the display text changes.
      $('bstatus').textContent = `loading ${m.files} entries`;
    } else if (m.type === 'download') {
      onDownload(scr, m);
    } else if (m.type === 'boot-tree') {
      onBootTree(scr, now);
      setBootStatus('running');
    } else if (m.type === 'bootline') {
      appendLog(m.text);
      if (m.text.startsWith('xiaoji: Starting phase 100')) window.xb.statsAt = m.text; // ?statsat (A/B runs)
      if (RESTART.test(m.text)) {
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
      onEnd(scr, m.exitCode, now);
      stopBoot(`exit ${m.exitCode}`);
      window.xb.status = 'done';
    } else if (m.type === 'error') {
      window.xb.error = m.text;
      appendLog(m.text);
      onError(scr);
      stopBoot('error');
    } else if (m.type === 'out') {
      appendLog(m.text);
    }
  };
  worker.onerror = (ev) => {
    window.xb.error = ev.message || 'worker error';
    appendLog(window.xb.error);
    onError(scr);
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
    early: params.get('early'),
  });
  if (mode === 'pack') {
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
    stopBoot('stopped');
  }
});

// The log panel: hidden by default, opened by "Show log" or ?log=1. The ART cases: ?cases=1.
function setLogOpen(open) {
  $('devlog').hidden = !open;
  $('logtoggle').textContent = open ? 'Hide log' : 'Show log';
  $('logtoggle').setAttribute('aria-expanded', String(open));
}
$('logtoggle').addEventListener('click', () => setLogOpen($('devlog').hidden));
setLogOpen(params.has('log'));
$('cases').hidden = !params.has('cases');
renderMilestoneRows();
