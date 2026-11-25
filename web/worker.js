// Runs one xiaoji ART case, or the Android boot, in a Web Worker (M4, W1).
//
// The guest root is /rootfs in MEMFS. It is built from a manifest: directories with FS.mkdir, symlinks
// with FS.symlink, and regular files. The ART cases use build/web/manifest.json, whose files are lazy files
// fetched from <tree>/<path> on first use (emscripten's createLazyFile, synchronous XHR with Range requests,
// allowed in workers). The Boot Android page uses the chunk store (web/chunks.js, docs/loading.md) by default:
// boot packs are fetched before the engine starts, and each file's bytes come from them. ?boot=manifest uses
// build/web/manifest-xiaoji.json with lazy files instead.
// Each run uses a fresh module instance.
//
// The engine runs synchronously and never yields, so the boot cannot use timers here. Its log
// lines are posted from the print hook as they are written (postMessage from a busy worker is
// delivered to the page), and the fetch counters are posted from the XHR hook.

import { BOOT, CASES, ENGINE_ENV_ARGS } from './cases.js';
import { openTrace } from './trace.js';
import { ChunkStore, buildChunkTree, earlyPackIndex, inflateNative } from './chunks.js';

// The display the engine presents (engine/display.h, XJ_DISPLAY_W and XJ_DISPLAY_H): the size of the OffscreenCanvas.
const FB_WIDTH = 540;
const FB_HEIGHT = 960;

// The packs' layout from the downloader (web/packloader.js), relayed by the page: the manifest, the shared buffer and
// the control words. The engine's module loads while it is on its way, so the boot waits for it here.
let resolveLayout;
const layoutReady = new Promise((r) => {
  resolveLayout = r;
});

const now = () => performance.now();
const post = (msg) => self.postMessage(msg);

// Counts what the lazy files fetch. Each synchronous GET of a file chunk is one request;
// its response body is the bytes that came over the wire.
const fetched = { requests: 0, bytes: 0, perUrl: new Map() };
// Every synchronous request (GET and HEAD), with the time the worker was blocked on it.
const sync = { gets: 0, heads: 0, blockedMs: 0 };
let onFetch = null; // set by the boot: called after each counted GET
const origOpen = XMLHttpRequest.prototype.open;
const origSend = XMLHttpRequest.prototype.send;
XMLHttpRequest.prototype.open = function (method, url, ...rest) {
  this.__xn = { method: String(method).toUpperCase(), url: String(url) };
  return origOpen.call(this, method, url, ...rest);
};
XMLHttpRequest.prototype.send = function (body) {
  const t0 = now();
  const r = origSend.call(this, body);
  const info = this.__xn;
  sync.blockedMs += now() - t0;
  if (info && info.method === 'HEAD') sync.heads++;
  if (info && info.method === 'GET') sync.gets++;
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

// ---- Booted snapshots (docs/snapshot.md, S3) ----
//
// xj/snapshot.json (tools/snappack.py) describes a snapshot the web engine wrote at the home screen: its build, the tree
// it booted, the image's ranges and extents (each a list of chunks), the head (the file's header and writable trees),
// and the packs that hold the chunks. A restore writes the image into the module's memory before main runs, the head
// into /snap.head, and starts the engine with --restore /snap.head (engine/snap.c: the image is marked preloaded).

async function sha256Hex(buf) {
  const d = new Uint8Array(await crypto.subtle.digest('SHA-256', buf));
  let hex = '';
  for (const b of d) hex += b.toString(16).padStart(2, '0');
  return hex;
}

// Checks the snapshot against this engine build and the tree manifest's bytes; returns the reason it cannot be used,
// or null.
function snapshotMismatch(Module, snap, treeHex) {
  const build = Module.UTF8ToString(Module._xj_snap_build());
  if (snap.build !== build) return `built by another engine (${snap.build}; this is ${build})`;
  if (snap.tree !== treeHex) return 'made from another tree';
  return null;
}

// Downloads the snapshot's packs (in parallel) and writes each chunk where the image has it as soon as it is inflated
// (a chunk can appear at many places: equal pages are stored once). progress(loaded, total) after each pack. Returns
// { head, downloadMs, bytes }.
async function loadSnapshot(Module, snap, base, progress) {
  const t0 = now();
  const total = snap.download_bytes;
  let top = 0;
  for (const [, hi] of snap.ranges) top = Math.max(top, hi);
  if (!Module._xj_snap_grow(top)) throw new Error(`snapshot: cannot grow memory to ${top} bytes`);
  const heap = Module.HEAPU8;
  for (const [lo, hi] of snap.ranges) heap.fill(0, lo, hi);
  const head = new Uint8Array(snap.head.bytes);
  const targets = new Map(); // chunk id -> [[array, offset], ...]
  const want = (id, arr, off) => {
    if (!targets.has(id)) targets.set(id, []);
    targets.get(id).push([arr, off]);
  };
  for (const [addr, , ids] of snap.extents) ids.forEach((id, k) => want(id, null, addr + k * snap.chunk_size));
  snap.head.chunks.forEach((id, k) => want(id, head, k * snap.chunk_size));
  let loaded = 0;
  await Promise.all(snap.packs.map(async (p) => {
    const res = await fetch(base + 'xj/' + p.file);
    if (!res.ok) throw new Error(`snapshot ${p.file}: HTTP ${res.status}`);
    const buf = new Uint8Array(await res.arrayBuffer());
    loaded += buf.length;
    progress(loaded, total);
    await Promise.all(p.chunks.map(async ([id, off, zlen, rlen]) => {
      const c = await inflateNative(buf.subarray(off, off + zlen), rlen);
      for (const [arr, at] of targets.get(id) || []) (arr || Module.HEAPU8).set(c, at);
    }));
  }));
  Module._xj_snap_preloaded();
  return { head, downloadMs: now() - t0, bytes: total };
}

// The Android boot (W1): the same command as the native product boot (docs/boot.md, B7), on the
// product tree from build/web/manifest-xiaoji.json. Stops when the page stops the worker, or when
// the engine's --init-timeout (wall clock, in seconds) runs out.
// The guest paths of the manifest's files (under /rootfs, as buildTree mounts them) and their sizes.
function fileSizes(manifest) {
  const sizes = new Map();
  for (const e of manifest.entries) {
    if (e.type === 'file') sizes.set('/rootfs' + e.path, e.size);
  }
  return sizes;
}

async function runBoot(base, opts) {
  const t0 = now();
  const traceOn = !!(opts && opts.trace);
  let trace = null;
  let firstLogWall = null;
  let packsWall = null;
  let tracePosted = false;
  // The run's counters, at phase 100 and at the end (the page keeps the last one; tests/bench reads it).
  const postStats = (when) => {
    postFetch(true); // the page's fetch line is rate-limited; this makes it current at phase 100
    post({
      type: 'stats',
      when,
      wall: now() - t0,
      firstLogWall,
      packsWall,
      sync: { ...sync },
      store: store ? store.stats : null,
    });
  };
  const postTrace = (when) => {
    if (!trace || tracePosted) return;
    tracePosted = true;
    post({
      type: 'trace',
      when,
      wall: now() - t0,
      firstLogWall,
      sync: { ...sync },
      access: trace.summary(),
    });
  };
  // mode 'pack' (?boot=pack): the chunk store and boot packs (web/chunks.js); 'manifest': the lazy-file
  // manifest (the page's default, until the pack mode is proven).
  const packMode = opts && opts.mode === 'pack';
  // ?snapshot=make: the boot writes a snapshot at the home screen to the dev server (web/serve.mjs --snapshot-upload,
  // make snapshot) and stops. Otherwise a snapshot the page found (opts.snap, main.js) is restored when it matches.
  const makeSnap = !!(opts && opts.snapshot === 'make');
  const snap = opts && opts.snap ? opts.snap : null;
  // Manifest mode fetches the manifest here. Pack mode gets it with the layout (see layoutReady above).
  let manifest = null;
  let size = 0;
  if (!packMode) {
    ({ manifest, size } = await fetchManifest(base, BOOT.manifest));
    post({ type: 'boot-start', manifestBytes: size, files: manifest.count });
  }
  const rc = await fetch(base + BOOT.rc, { cache: 'no-store' });
  if (!rc.ok) throw new Error(`${BOOT.rc}: HTTP ${rc.status}`);
  const rcText = await rc.text();
  let store = null;

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
    if (firstLogWall === null) firstLogWall = now() - t0;
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
        if (ms.id === 'phase100') {
          postStats('phase100');
          postTrace('phase100');
        }
      }
    }
  };

  // The GL backend (docs/graphics.md, G2): the engine makes its WebGL2 context on the OffscreenCanvas of this worker
  // (self.xjCanvas, engine/gl_webgl.c). The frame counters are in a shared Int32Array (the page takes the frames).
  const webgl = !!(opts && opts.gl === 'webgl');
  self.xjFrameCtl = opts && opts.frameSab ? new Int32Array(opts.frameSab) : null;
  self.xjCanvas = webgl ? new OffscreenCanvas(FB_WIDTH, FB_HEIGHT) : null;
  const { default: createXiaoji } = await import(base + 'xiaoji.mjs');
  const Module = await createXiaoji({
    print: (line) => post({ type: 'out', text: line }),
    printErr: onLine,
  });
  // The page's input ring (web/input.js): the engine drains it through xiaoji_input_pop (engine/jit_lib.js, docs/input.md).
  if (opts && opts.input) Module.inputRing = new Int32Array(opts.input);
  let files;
  let restored = null;
  if (snap) {
    // A restore: the tree's chunks are read on demand (the boot packs are not waited for), the image is written now.
    const tm = await fetch(base + 'xj/manifest-xiaoji-v2.json', { cache: 'no-store' });
    const tbuf = await tm.arrayBuffer();
    const why = snapshotMismatch(Module, snap, await sha256Hex(tbuf));
    if (why) {
      post({ type: 'restore-refused', text: why });
      throw new Error(`snapshot: ${why}`);
    }
    manifest = JSON.parse(new TextDecoder().decode(tbuf));
    size = tbuf.byteLength;
    store = new ChunkStore(base + 'xj/', manifest.chunk_size);
    post({ type: 'restore-start', total: snap.download_bytes, image: snap.image_bytes });
    restored = await loadSnapshot(Module, snap, base, (loaded, total) => post({ type: 'restore-progress', loaded, total }));
    files = buildChunkTree(Module.FS, manifest, store);
    Module.FS.writeFile('/snap.head', restored.head);
    post({ type: 'restore-loaded', wall: now() - t0, loadMs: restored.downloadMs, bytes: restored.bytes });
  } else if (packMode) {
    // The packs come from the downloader while the module loads. The engine starts once the first pack (or the
    // first opts.early MB) is in; its reads of a later pack wait for that pack. ?nopack, or no layout (the page
    // could not start the downloader): every chunk is read synchronously, with the manifest fetched here.
    const lay = await layoutReady;
    if (lay.error) {
      post({ type: 'pack-error', pack: -1, text: `no packs: ${lay.error}` });
      ({ manifest, size } = await fetchManifest(base, 'xj/manifest-xiaoji-v2.json'));
      store = new ChunkStore(base + 'xj/', manifest.chunk_size);
    } else {
      manifest = lay.manifest;
      size = lay.manifestBytes;
      store = new ChunkStore(base + 'xj/', manifest.chunk_size);
      store.attachPacks(manifest.packs, lay.sab, lay.ctrl);
    }
    post({ type: 'boot-start', manifestBytes: size, files: manifest.count });
    const earlyBytes = Math.max(0, Number(opts && opts.early ? opts.early : 0)) * 1048576;
    await store.waitStart(manifest.packs ? earlyPackIndex(manifest.packs, earlyBytes) : 0);
    packsWall = now() - t0;
    files = buildChunkTree(Module.FS, manifest, store);
  } else {
    files = buildTree(Module.FS, manifest, base, BOOT.tree);
  }
  Module.FS.writeFile('/xiaoji.rc', rcText);
  if (traceOn) trace = openTrace(Module.FS, fileSizes(manifest));
  post({ type: 'boot-tree', files, wall: now() - t0, packsWall, store: store ? store.stats : null });

  // ?timeout=S (debugging, B18): the engine's wall-clock limit in seconds instead of BOOT.timeoutS, for slow machines.
  const timeoutS = opts && Number(opts.timeoutS) > 0 ? Math.floor(Number(opts.timeoutS)) : BOOT.timeoutS;
  const argv = restored ? ['--logcat', '--restore', '/snap.head', '--init-timeout', String(timeoutS)]
    : ['--logcat', '--init', '/xiaoji.rc', '--root', '/rootfs', '--init-timeout', String(timeoutS)];
  if (makeSnap) {
    // The home screen, then a quiet minute (docs/snapshot.md): the launcher's HOME start, 60 s later.
    // ?snapat=log:TEXT: another condition (docs/demo.md: the pre-warm's last app).
    const at = opts.snapAt && /^log:[^\n]{1,200}$/.test(opts.snapAt) ? opts.snapAt : 'log:cmp=com.android.launcher3/';
    argv.push('--snapshot-at', at, '--snapshot-after', String(Number(opts.snapAfter) || 60),
      '--snapshot-out', base + '__snap/web.snap');
    // ?snapquiet=PCT:WINDOW:MAX: then a quiet window (docs/demo.md), so the restored phone is not busy finishing its boot.
    if (opts.snapQuiet && /^\d+:[\d.]+:[\d.]+$/.test(opts.snapQuiet)) argv.push('--snapshot-quiet', opts.snapQuiet);
  }
  if (opts && opts.lazy === false) argv.push('--no-lazy-mmap');
  if (opts && opts.lazyWindow) argv.push('--lazy-window', String(Number(opts.lazyWindow)));
  // ?statsat: the engine prints its instruction count and lazy reads at phase 100 (for the A/B runs).
  if (opts && opts.statsAt) argv.push('--stats-at', 'Starting phase 100');
  if (webgl && !restored) argv.push('--gl', 'webgl'); // a restore keeps the snapshot's backend
  // ?debug=1: the engine's fault reports (XIAOJI_DEBUG_FAULTS, which the page cannot set) go to the log.
  if (opts && opts.debugFaults) argv.push('--debug-faults');
  // ?dumpat=SECONDS: a dump of every guest thread at that time (the engine's --dump-at).
  if (opts && opts.dumpAt) argv.push('--dump-at', String(opts.dumpAt).replace(/[^0-9,]/g, ''));
  // ?load=MS: the engine's load report (docs/demo.md); ?prop=K=V: a property for a boot (not a restore).
  if (opts && Number(opts.loadEvery) > 0) argv.push('--load-every', String(Math.floor(Number(opts.loadEvery))));
  if (opts && opts.props && !restored) {
    for (const p of opts.props) if (/^[A-Za-z0-9._-]+=[^\n]*$/.test(p)) argv.push('--prop', p);
  }
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
  postStats('done');
  postTrace('done');
  post({ type: 'done', name: 'boot', exitCode, wall: now() - t0, sync: { ...sync }, store: store ? store.stats : null });
}

// The frame path without a guest (tests/web.mjs, XIAOJI_WEB_FRAMES=1; engine/gl_webgl.c, xj_gl_selftest): the engine
// draws a test picture on its WebGL2 color buffer and posts it as a frame. 'done' follows the frame, with the frames posted
// (-1: no backend), the alias path's pixel and the quad's pixel (RGB, packed).
async function glSelfTest(base, frameSab) {
  self.xjFrameCtl = new Int32Array(frameSab);
  self.xjCanvas = new OffscreenCanvas(FB_WIDTH, FB_HEIGHT);
  const { default: createXiaoji } = await import(base + 'xiaoji.mjs');
  const Module = await createXiaoji({
    print: (line) => post({ type: 'out', text: line }),
    printErr: (line) => post({ type: 'err', text: line }),
  });
  const frames = Module._xj_gl_selftest();
  post({
    type: 'done', name: 'glselftest', exitCode: frames, alias: Module._xj_gl_selftest_alias(),
    quad: Module._xj_gl_selftest_quad(), life: Module._xj_gl_selftest_life(), state: Module._xj_gl_selftest_state(),
    compose: Module._xj_gl_selftest_compose(), vao: Module._xj_gl_selftest_vao(), respec: Module._xj_gl_selftest_respec(), wirefloat: Module._xj_gl_selftest_wirefloat(),
  });
}

self.onmessage = async (ev) => {
  const msg = ev.data;
  if (!msg) return;
  try {
    if (msg.type === 'run') await run(msg.name, msg.base);
    else if (msg.type === 'layout') resolveLayout(msg);
    else if (msg.type === 'glselftest') await glSelfTest(msg.base, msg.frameSab);
    else if (msg.type === 'boot') {
      await runBoot(msg.base, {
        trace: msg.trace, mode: msg.mode, lazy: msg.lazy !== false, lazyWindow: msg.lazyWindow,
        statsAt: !!msg.statsAt, early: msg.early, gl: msg.gl, frameSab: msg.frameSab,
        input: msg.input || null, debugFaults: !!msg.debugFaults, dumpAt: msg.dumpAt || 0, timeoutS: msg.timeoutS || 0,
        snapshot: msg.snapshot || null, snap: msg.snap || null, snapAfter: msg.snapAfter || 0,
        loadEvery: msg.loadEvery || 0, props: msg.props || [], snapQuiet: msg.snapQuiet || null, snapAt: msg.snapAt || null,
      });
    }
  } catch (e) {
    post({ type: 'error', text: String(e && e.stack ? e.stack : e) });
  }
};
