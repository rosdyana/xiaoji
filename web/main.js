// Page side of the web build (M4): starts one worker per run and shows its output.
// window.xn holds the state of the last run, for tests/web.mjs. The Boot Android panel (W1) starts
// a worker for the product boot; window.xb holds its state.

import { BOOT } from './cases.js';

const $ = (id) => document.getElementById(id);
const base = new URL('./', location.href).href;

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

// Boot Android (W1). window.xb is the machine-readable state, for tests/web.mjs: status is 'idle',
// 'loading', 'running', 'stopped', 'done' or 'error'; milestones maps each reached id to its
// wall time (s, from the worker's start), engine ms and instruction count.
const LOG_LINES = 20000;
window.xb = { status: 'idle', lines: 0, milestones: {}, fetch: null, done: null, error: null };
let bootWorker = null;

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
  $('boot').disabled = false;
  $('stop').disabled = true;
  if (status) setBootStatus(status);
}

$('boot').addEventListener('click', () => {
  $('blog').textContent = '';
  $('bfetch').textContent = '0 requests, 0 files, 0 bytes';
  renderMilestoneRows();
  window.xb = { status: 'loading', lines: 0, milestones: {}, fetch: null, done: null, error: null };
  setBootStatus('loading');
  $('boot').disabled = true;
  $('stop').disabled = false;

  const worker = new Worker(new URL('./worker.js', location.href), { type: 'module' });
  bootWorker = worker;
  worker.onmessage = (ev) => {
    const m = ev.data;
    if (m.type === 'boot-start') {
      // The machine status stays 'loading' (Stop depends on it); only the display text changes.
      $('bstatus').textContent = `loading ${m.files} entries`;
    } else if (m.type === 'boot-tree') {
      setBootStatus('running');
    } else if (m.type === 'bootline') {
      appendLog(m.text);
    } else if (m.type === 'milestone') {
      const row = $('ms-' + m.id);
      if (row && m.stamp) {
        row.querySelector('[data-k="wall"]').textContent = (m.wall / 1000).toFixed(2) + ' s';
        row.querySelector('[data-k="ms"]').textContent = (m.stamp.ms / 1000).toFixed(2) + ' s';
        row.querySelector('[data-k="insn"]').textContent = (m.stamp.insn / 1e9).toFixed(3) + ' G';
      }
      window.xb.milestones[m.id] = { label: m.label, wall: m.wall, stamp: m.stamp, line: m.line };
    } else if (m.type === 'fetch') {
      window.xb.fetch = { requests: m.requests, files: m.files, bytes: m.bytes };
      $('bfetch').textContent = `${m.requests} requests, ${m.files} files, ${fmtBytes(m.bytes)}`;
    } else if (m.type === 'done') {
      window.xb.done = m;
      stopBoot(`exit ${m.exitCode}`);
      window.xb.status = 'done';
    } else if (m.type === 'error') {
      window.xb.error = m.text;
      appendLog(m.text);
      stopBoot('error');
    } else if (m.type === 'out') {
      appendLog(m.text);
    }
  };
  worker.onerror = (ev) => {
    window.xb.error = ev.message || 'worker error';
    appendLog(window.xb.error);
    stopBoot('error');
  };
  worker.postMessage({ type: 'boot', base: new URL('./', location.href).href });
});

$('stop').addEventListener('click', () => {
  if (window.xb.status === 'loading' || window.xb.status === 'running') stopBoot('stopped');
});
