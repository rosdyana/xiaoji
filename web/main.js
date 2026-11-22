// Page side of the web build (M4): starts one worker per run and shows its output.
// window.xn holds the state of the last run, for tests/web.mjs.

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
