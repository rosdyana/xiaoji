// Runs one xiaoji ART case, or the Android boot, in a Web Worker (M4, W1).
//
// The guest root is /rootfs in MEMFS. It is built from a manifest (build/web/manifest.json for
// the cases, build/web/manifest-xiaoji.json for the boot): directories with FS.mkdir, symlinks with
// FS.symlink, and regular files as lazy files that fetch their bytes from <tree>/<path> on first use
// (emscripten's createLazyFile, synchronous XHR with Range requests, which is allowed in workers).
// Each run uses a fresh module instance.
//
// The engine runs synchronously and never yields, so the boot cannot use timers here. Its log
// lines are posted from the print hook as they are written (postMessage from a busy worker is
// delivered to the page), and the fetch counters are posted from the XHR hook.

import { BOOT, CASES, ENGINE_ENV_ARGS } from './cases.js';

const now = () => performance.now();
const post = (msg) => self.postMessage(msg);

// Counts what the lazy files fetch. Each synchronous GET of a file chunk is one request;
// its response body is the bytes that came over the wire.
const fetched = { requests: 0, bytes: 0, perUrl: new Map() };
let onFetch = null; // set by the boot: called after each counted GET
const origOpen = XMLHttpRequest.prototype.open;
const origSend = XMLHttpRequest.prototype.send;
XMLHttpRequest.prototype.open = function (method, url, ...rest) {
  this.__xn = { method: String(method).toUpperCase(), url: String(url) };
  return origOpen.call(this, method, url, ...rest);
};
XMLHttpRequest.prototype.send = function (body) {
  const r = origSend.call(this, body);
  const info = this.__xn;
  if (info && info.method === 'GET' && this.status >= 200 && this.status < 300) {
    const buf = this.response;
    const n = buf && buf.byteLength !== undefined ? buf.byteLength : 0;
    fetched.requests++;
    fetched.bytes += n;
    fetched.perUrl.set(info.url, (fetched.perUrl.get(info.url) ?? 0) + n);
    if (onFetch) onFetch();
  }
  return r;
};

function encodeGuestPath(p) {
  return p.split('/').map(encodeURIComponent).join('/');
}

// The guest path of a fetched URL, for the "largest files" list.
function guestPathOf(url) {
  const m = /\/rootfs(?:-xiaoji)?(\/.*)$/.exec(url);
  return m ? decodeURIComponent(m[1]) : url;
}

// Builds /rootfs from the manifest. Entries are sorted by path, so parents come first.
// tree is the URL path of the files under base (rootfs, or rootfs-xiaoji).
function buildTree(FS, manifest, base, tree) {
  FS.mkdir('/rootfs');
  let files = 0;
  for (const e of manifest.entries) {
    const host = '/rootfs' + e.path;
    const slash = host.lastIndexOf('/');
    const parent = host.slice(0, slash) || '/';
    const name = host.slice(slash + 1);
    if (e.type === 'dir') {
      FS.mkdir(host);
      FS.chmod(host, e.mode);
    } else if (e.type === 'symlink') {
      FS.symlink(e.target, host);
    } else if (e.type === 'file') {
      const url = base + tree + encodeGuestPath(e.path);
      FS.createLazyFile(parent, name, url, true, false);
      FS.chmod(host, e.mode);
      files++;
    } else {
      throw new Error(`manifest: unknown type ${e.type} for ${e.path}`);
    }
  }
  return files;
}

async function fetchManifest(base, name) {
  const res = await fetch(base + name, { cache: 'no-store' });
  if (!res.ok) throw new Error(`manifest ${name}: HTTP ${res.status}`);
  const text = await res.text();
  return { manifest: JSON.parse(text), size: text.length };
}

async function run(name, base) {
  const c = CASES[name];
  if (!c) throw new Error(`unknown case ${name}`);
  const timing = {};
  const tRun = now();

  let t = now();
  const { manifest, size } = await fetchManifest(base, 'manifest.json');
  timing.manifestMs = now() - t;
  timing.manifestBytes = size;

  t = now();
  const { default: createXiaoji } = await import(base + 'xiaoji.mjs');
  timing.importMs = now() - t;

  let firstInsnAt = 0;
  t = now();
  const Module = await createXiaoji({
    print: (line) => post({ type: 'out', text: line }),
    printErr: (line) => post({ type: 'err', text: line }),
    onRunStart: () => {
      firstInsnAt = now();
    },
  });
  timing.instantiateMs = now() - t;

  t = now();
  const files = buildTree(Module.FS, manifest, base, 'rootfs');
  timing.fsBuildMs = now() - t;
  timing.files = files;

  const argv = ['--root', '/rootfs', '--stats', ...ENGINE_ENV_ARGS, '--', ...c.argv];
  const tCall = now();
  let exitCode;
  try {
    exitCode = Module.callMain(argv);
  } catch (e) {
    if (e && typeof e.status === 'number') {
      exitCode = e.status;
    } else {
      throw e;
    }
  }
  const tEnd = now();
  timing.callMainMs = tEnd - tCall;
  timing.firstInsnMs = firstInsnAt ? firstInsnAt - tCall : null;
  timing.firstInsnSincePressMs = firstInsnAt ? firstInsnAt - tRun : null;
  timing.totalMs = tEnd - tRun;

  post({
    type: 'done',
    name,
    exitCode,
    timing,
    fetch: {
      requests: fetched.requests,
      bytes: fetched.bytes,
      files: fetched.perUrl.size,
      largest: [...fetched.perUrl].sort((a, b) => b[1] - a[1]).slice(0, 8)
        .map(([u, n]) => [guestPathOf(u), n]),
    },
  });
}

// Matches the stamp logd puts in front of each line: "[<ms> ms <insn> insn] ".
const STAMP = /^\[(\d+) ms (\d+) insn\] /;

// The Android boot (W1): the same command as the native product boot (docs/boot.md, B7), on the
// product tree from build/web/manifest-xiaoji.json. Stops when the page stops the worker, or when
// the engine's --init-timeout (wall clock, in seconds) runs out.
async function runBoot(base) {
  const t0 = now();
  const { manifest, size } = await fetchManifest(base, BOOT.manifest);
  const rc = await fetch(base + BOOT.rc, { cache: 'no-store' });
  if (!rc.ok) throw new Error(`${BOOT.rc}: HTTP ${rc.status}`);
  const rcText = await rc.text();
  post({ type: 'boot-start', manifestBytes: size, files: manifest.count });

  const seen = new Set();
  const fetchTick = { last: 0 };
  const postFetch = (force) => {
    const t = now();
    if (!force && t - fetchTick.last < 200) return;
    fetchTick.last = t;
    post({ type: 'fetch', requests: fetched.requests, files: fetched.perUrl.size, bytes: fetched.bytes, wall: t - t0 });
  };
  onFetch = () => postFetch(false);

  const onLine = (line) => {
    let text = line;
    let stamp = null;
    const m = STAMP.exec(line);
    if (m) {
      stamp = { ms: Number(m[1]), insn: Number(m[2]) };
      text = line.slice(m[0].length);
    }
    post({ type: 'bootline', text: line, stamp });
    for (const ms of BOOT.milestones) {
      if (!seen.has(ms.id) && ms.re.test(text)) {
        seen.add(ms.id);
        post({ type: 'milestone', id: ms.id, label: ms.label, line: text, stamp, wall: now() - t0 });
      }
    }
  };

  const { default: createXiaoji } = await import(base + 'xiaoji.mjs');
  const Module = await createXiaoji({
    print: (line) => post({ type: 'out', text: line }),
    printErr: onLine,
  });
  const files = buildTree(Module.FS, manifest, base, BOOT.tree);
  Module.FS.writeFile('/xiaoji.rc', rcText);
  post({ type: 'boot-tree', files, wall: now() - t0 });

  const argv = ['--logcat', '--init', '/xiaoji.rc', '--root', '/rootfs', '--init-timeout', String(BOOT.timeoutS)];
  let exitCode;
  try {
    exitCode = Module.callMain(argv);
  } catch (e) {
    if (e && typeof e.status === 'number') {
      exitCode = e.status;
    } else {
      throw e;
    }
  }
  postFetch(true);
  post({ type: 'done', name: 'boot', exitCode, wall: now() - t0 });
}

self.onmessage = async (ev) => {
  const msg = ev.data;
  if (!msg) return;
  try {
    if (msg.type === 'run') await run(msg.name, msg.base);
    else if (msg.type === 'boot') await runBoot(msg.base);
  } catch (e) {
    post({ type: 'error', text: String(e && e.stack ? e.stack : e) });
  }
};
