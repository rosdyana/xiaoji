// The boot pack downloader for the Boot Android page (docs/loading.md, L2). A module worker of its own, so the
// engine's worker can block in wasm while the packs arrive. It fetches the v2 manifest, lays the packs out in one
// SharedArrayBuffer (layoutPacks in web/chunks.js), and then, one pack after another in the boot's first-read
// order: loads the pack from the chunk cache or the network, decodes and hashes each chunk, writes it into the
// buffer, and sets the pack's control word to ready (or failed) and wakes the engine's worker.
//
// Messages to the page: 'layout' (the manifest, the buffer and the control words; once), 'layout-error' (no
// manifest: the page tells the engine to fetch it itself), 'download' (the bytes of the packs, for the boot
// screen), 'packstats' (the counters), 'pack-error' (one pack failed; the rest go on).
import { PACK_FAILED, PACK_READY, inflateNative, layoutPacks, openCache, sha256Id } from './chunks.js';

const now = () => performance.now();
const post = (msg) => self.postMessage(msg);

self.onmessage = async (ev) => {
  const msg = ev.data;
  if (msg && msg.type === 'start') await start(msg);
};

async function start({ base, noPacks }) {
  let manifest;
  let manifestBytes;
  let sab;
  let ctrl;
  try {
    const res = await fetch(base + 'xj/manifest-xiaoji-v2.json', { cache: 'no-store' });
    if (!res.ok) throw new Error(`manifest: HTTP ${res.status}`);
    const text = await res.text();
    manifest = JSON.parse(text);
    manifestBytes = text.length;
    const { total } = layoutPacks(manifest.packs);
    sab = new SharedArrayBuffer(Math.max(1, total));
    ctrl = new Int32Array(new SharedArrayBuffer(4 * Math.max(1, manifest.packs.length)));
  } catch (e) {
    post({ type: 'layout-error', text: String(e && e.message ? e.message : e) });
    return;
  }
  post({ type: 'layout', manifest, manifestBytes, sab, ctrl });
  const packs = manifest.packs;
  if (noPacks) {
    // ?nopack: no pack is loaded; the engine reads every chunk synchronously.
    failFrom(ctrl, 0);
    post({ type: 'packstats', stats: { packs: packs.length, packDownloads: 0, packNetBytes: 0, packChunks: 0 } });
    return;
  }
  try {
    await loadAll(base + 'xj/', packs, sab, ctrl);
  } catch (e) {
    // A failure before the packs (the cache, the buffer): every pack not yet ready is a fallback for the engine.
    post({ type: 'pack-error', pack: -1, text: String(e && e.message ? e.message : e) });
    failFrom(ctrl, 0);
  }
  post({ type: 'packs-done' });
}

function failFrom(ctrl, k) {
  for (let j = k; j < ctrl.length; j++) {
    if (Atomics.load(ctrl, j) === 0) {
      Atomics.store(ctrl, j, PACK_FAILED);
      Atomics.notify(ctrl, j);
    }
  }
}

async function loadAll(xj, packs, sab, ctrl) {
  const t0 = now();
  const { where } = layoutPacks(packs);
  const cache = await openCache();
  if (cache) {
    // The cache holds the chunks of this build: entries the build no longer lists are deleted.
    const keep = new Set();
    for (const p of packs) for (const [id] of p.chunks) keep.add(xj + 'chunks/' + id + '.z');
    try {
      for (const req of await cache.keys()) if (!keep.has(req.url)) await cache.delete(req);
    } catch {
      // a stale entry is harmless
    }
  }
  // Probe every pack before the first download: the progress total and the warm decision need it.
  const have = await Promise.all(packs.map((p) => probe(xj, p, cache)));
  const prog = {
    loaded: 0,
    total: packs.reduce((n, p) => n + p.bytes, 0),
    fromCache: !!cache && have.every((h) => h && h.every(Boolean)),
    cacheable: !!cache,
  };
  const stats = {
    packs: packs.length, packDownloads: 0, packBytes: 0, packNetBytes: 0, packChunks: 0,
    chunkCacheHits: 0, chunkCachePuts: 0, packMs: 0,
  };
  const tick = { last: 0 };
  const emit = (force) => {
    const t = now();
    if (!force && t - tick.last < 100) return;
    tick.last = t;
    post({ type: 'download', ...prog });
  };
  emit(true);
  for (let k = 0; k < packs.length; k++) {
    try {
      await loadPack(xj, packs[k], have[k], cache, sab, where, prog, stats, emit);
      Atomics.store(ctrl, k, PACK_READY);
    } catch (e) {
      // This pack's chunks become fallbacks; the others still load.
      post({ type: 'pack-error', pack: k, text: String(e && e.message ? e.message : e) });
      Atomics.store(ctrl, k, PACK_FAILED);
    }
    Atomics.notify(ctrl, k);
    stats.packMs = now() - t0;
    post({ type: 'packstats', stats: { ...stats } });
  }
  emit(true);
}

// The cache entries of a pack's chunks, in order (undefined where missing), or null without a cache.
async function probe(xj, pack, cache) {
  if (!cache) return null;
  return Promise.all(pack.chunks.map(([id]) => cache.match(xj + 'chunks/' + id + '.z').catch(() => undefined)));
}

// Streams one pack's bytes into memory, counting them into the progress as they arrive.
async function download(url, pack, prog, emit) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`pack ${pack.file}: HTTP ${res.status}`);
  const out = new Uint8Array(pack.bytes);
  const reader = res.body.getReader();
  let off = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (off + value.length > pack.bytes) {
      throw new Error(`pack ${pack.file}: more than ${pack.bytes} bytes, the manifest says ${pack.bytes}`);
    }
    out.set(value, off);
    off += value.length;
    prog.loaded += value.length;
    emit(false);
  }
  if (off !== pack.bytes) throw new Error(`pack ${pack.file}: ${off} bytes, the manifest says ${pack.bytes}`);
  return out;
}

async function loadPack(xj, pack, hv, cache, sab, where, prog, stats, emit) {
  const cached = hv ? hv.filter(Boolean).length : 0;
  stats.chunkCacheHits += cached;
  let buf = null;
  if (!hv || cached < pack.chunks.length) {
    buf = await download(xj + pack.file, pack, prog, emit);
    stats.packDownloads++;
    stats.packBytes += buf.length;
    stats.packNetBytes += buf.length;
  } else {
    prog.loaded += pack.bytes;
    emit(false);
  }
  const puts = [];
  for (let i = 0; i < pack.chunks.length; i++) {
    const [id, off, len, raw] = pack.chunks[i];
    let z;
    if (buf) {
      z = buf.subarray(off, off + len);
      if (cache && !(hv && hv[i])) {
        puts.push(cache.put(xj + 'chunks/' + id + '.z', new Response(z.slice())).then(
          () => stats.chunkCachePuts++,
          () => {},
        ));
      }
    } else {
      z = new Uint8Array(await hv[i].arrayBuffer());
    }
    const data = await inflateNative(z, raw);
    if ((await sha256Id(data)) !== id) throw new Error(`pack ${pack.file}: chunk ${id} does not verify`);
    const w = where.get(id);
    new Uint8Array(sab, w.off, w.raw).set(data);
    stats.packChunks++;
  }
  await Promise.all(puts); // the chunks are in the cache before the pack is ready
}
