// Runs one xiaoji ART case in a Web Worker (M4).
//
// The guest root is /rootfs in MEMFS. It is built from build/web/manifest.json: directories
// with FS.mkdir, symlinks with FS.symlink, and regular files as lazy files that fetch their
// bytes from /rootfs/<path> on first use (emscripten's createLazyFile, synchronous XHR with
// Range requests, which is allowed in workers). Each run uses a fresh module instance.

import { CASES, ENGINE_ENV_ARGS } from './cases.js';

const now = () => performance.now();
const post = (msg) => self.postMessage(msg);

// Counts what the lazy files fetch. Each synchronous GET of a file chunk is one request;
// its response body is the bytes that came over the wire.
const fetched = { requests: 0, bytes: 0, perUrl: new Map() };
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
    fetched.requests++;
    fetched.bytes += buf && buf.byteLength !== undefined ? buf.byteLength : 0;
    const n = buf && buf.byteLength !== undefined ? buf.byteLength : 0;
    fetched.perUrl.set(info.url, (fetched.perUrl.get(info.url) ?? 0) + n);
  }
  return r;
};

function encodeGuestPath(p) {
  return p.split('/').map(encodeURIComponent).join('/');
}

// Builds /rootfs from the manifest. Entries are sorted by path, so parents come first.
function buildTree(FS, manifest, base) {
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
      const url = base + 'rootfs' + encodeGuestPath(e.path);
      FS.createLazyFile(parent, name, url, true, false);
      FS.chmod(host, e.mode);
      files++;
    } else {
      throw new Error(`manifest: unknown type ${e.type} for ${e.path}`);
    }
  }
  return files;
}

async function run(name, base) {
  const c = CASES[name];
  if (!c) throw new Error(`unknown case ${name}`);
  const timing = {};
  const tRun = now();

  let t = now();
  const res = await fetch(base + 'manifest.json', { cache: 'no-store' });
  if (!res.ok) throw new Error(`manifest: HTTP ${res.status}`);
  const text = await res.text();
  const manifest = JSON.parse(text);
  timing.manifestMs = now() - t;
  timing.manifestBytes = text.length;

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
  const files = buildTree(Module.FS, manifest, base);
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
        .map(([u, n]) => [decodeURIComponent(u.slice(u.indexOf('/rootfs/') + 7)), n]),
    },
  });
}

self.onmessage = async (ev) => {
  const msg = ev.data;
  if (!msg || msg.type !== 'run') return;
  try {
    await run(msg.name, msg.base);
  } catch (e) {
    post({ type: 'error', text: String(e && e.stack ? e.stack : e) });
  }
};
