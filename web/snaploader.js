// The background loader of a restore with lazy pages (docs/snapshot.md, S5): a worker of the engine's worker. While
// the engine runs, it downloads the snapshot's lazy packs and the tree's prefetch packs (docs/demo.md, P1), from the
// Cache Storage when it has their chunks and from the network otherwise, in one priority order:
//   1. a lazy pack the engine asked for ('urgent': a fault on one of its pages);
//   2. the lazy packs of the recorded first touches and the prefetch packs, interleaved by their share of bytes (both
//      are what the recorded first minute needed: the pages of the processes, and the files' clean pages);
//   3. the other lazy packs, in address order.
// A lazy chunk is inflated (DecompressionStream), checked against its id (SHA-256) and put into the ring the engine's
// worker copies from (web/lazypages.js); a prefetch chunk goes where the chunk store reads it (web/chunks.js,
// attachPrefetch). A pack is retried; one that fails for good is marked failed, and the engine then reads its chunks
// itself, on demand.
//
// Messages in: {type: 'start', ...} (the buffers and the manifests; it then waits for 'go'), 'go' (the engine started:
// downloads begin), 'urgent' {pack}, 'stop' (every lazy page is in: only prefetch packs are left to load).
import { CHUNK_CACHE, PACK_FAILED as PF_FAILED, PACK_READY as PF_READY, inflateNative, layoutPacks, sha256Id } from './chunks.js';
import {
  DELIVERED, HEAD, LOADER_DONE, PACK_DONE, PACK_FAILED, PACK_LOADING, RING_SLOTS, SLOT0, SNAP_CACHE, TAIL, lazyChunks,
} from './lazypages.js';

const PREFETCH_LOADING = 3; // web/prefetch.js's state for a pack on its way (chunks.js waits on it)
const FETCHES = 2;
const RETRY_MS = [500, 2000, 5000];
const now = () => performance.now();
let st = null;

self.onmessage = (ev) => {
  const m = ev.data;
  if (!m) return;
  if (m.type === 'start') {
    st = { ...m, urgent: [], stopped: false, started: false };
    self.postMessage({ type: 'started' });
  } else if (m.type === 'go' && st && !st.started) {
    st.started = true;
    run().catch((e) => self.postMessage({ type: 'error', text: String(e && e.message ? e.message : e) }));
  } else if (m.type === 'urgent' && st) {
    st.urgent.push(m.pack);
  } else if (m.type === 'stop' && st) {
    st.stopped = true;
  }
};

async function openCache(name) {
  if (typeof caches === 'undefined') return null;
  try {
    return await caches.open(name);
  } catch {
    return null;
  }
}

async function fetchRetried(url, bytes) {
  let err = null;
  for (let t = 0; t <= RETRY_MS.length; t++) {
    if (t) await new Promise((r) => setTimeout(r, RETRY_MS[t - 1]));
    try {
      const res = await fetch(url);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const buf = new Uint8Array(await res.arrayBuffer());
      if (buf.length !== bytes) throw new Error(`${buf.length} bytes, the manifest says ${bytes}`);
      return buf;
    } catch (e) {
      err = e;
    }
  }
  throw new Error(`${url}: ${err && err.message ? err.message : err}`);
}

// The zlib bytes of each chunk of a pack: from the cache when all are there, else the pack is downloaded (and its
// chunks put into the cache).
async function packChunks(cache, keyOf, url, pack, stats) {
  if (cache) {
    const hits = await Promise.all(pack.chunks.map(([id]) => cache.match(keyOf(id)).catch(() => undefined)));
    if (hits.every(Boolean)) {
      stats.cachePacks++;
      return Promise.all(hits.map(async (h) => new Uint8Array(await h.arrayBuffer())));
    }
  }
  const buf = await fetchRetried(url, pack.bytes);
  stats.netBytes += buf.length;
  stats.netPacks++;
  const parts = pack.chunks.map(([, off, zlen]) => buf.subarray(off, off + zlen));
  if (cache) {
    pack.chunks.forEach(([id], i) => cache.put(keyOf(id), new Response(parts[i].slice())).catch(() => {}));
  }
  return parts;
}

async function ringPut(k, data) {
  const { ctrl, ring, slotBytes } = st.bufs;
  for (;;) {
    const head = Atomics.load(ctrl, HEAD);
    const tail = Atomics.load(ctrl, TAIL);
    if (head - tail < RING_SLOTS) {
      const slot = head % RING_SLOTS;
      new Uint8Array(ring, slot * slotBytes, data.length).set(data);
      Atomics.store(ctrl, SLOT0 + slot, k);
      Atomics.store(ctrl, HEAD, head + 1);
      Atomics.notify(ctrl, HEAD);
      return;
    }
    const w = Atomics.waitAsync(ctrl, TAIL, tail, 1000);
    if (w.async) await w.value;
  }
}

async function run() {
  const t0 = now();
  const { base, lazy, prefetch, bufs, prefetchBufs } = st;
  const stats = { netBytes: 0, netPacks: 0, cachePacks: 0, failed: 0, lazyPacks: 0, prefetchPacks: 0 };
  const snapCache = st.noCache ? null : await openCache(SNAP_CACHE);
  const chunkCache = st.noCache ? null : await openCache(CHUNK_CACHE);
  const chunks = lazyChunks(lazy);
  // The order: the lazy packs that hold touched pages, interleaved with the prefetch packs by bytes; then the rest.
  const touchedChunks = Math.ceil((lazy.touched || 0) / lazy.chunk_pages);
  const touchedPacks = touchedChunks ? chunks[touchedChunks - 1][0] + 1 : 0;
  const jobs = [];
  const lazyBytes = lazy.packs.slice(0, touchedPacks).reduce((n, p) => n + p.bytes, 0) || 1;
  const pfPacks = prefetch ? prefetch.packs : [];
  const pfBytes = pfPacks.reduce((n, p) => n + p.bytes, 0) || 1;
  let li = 0, pi = 0, lb = 0, pb = 0;
  while (li < touchedPacks || pi < pfPacks.length) {
    if (pi >= pfPacks.length || (li < touchedPacks && lb / lazyBytes <= pb / pfBytes)) {
      jobs.push({ kind: 'lazy', k: li });
      lb += lazy.packs[li++].bytes;
    } else {
      jobs.push({ kind: 'prefetch', k: pi });
      pb += pfPacks[pi++].bytes;
    }
  }
  for (let k = touchedPacks; k < lazy.packs.length; k++) jobs.push({ kind: 'lazy', k });
  const pfWhere = prefetch ? layoutPacks(pfPacks).where : null;
  const lazyDone = new Set();

  const next = () => {
    while (st.urgent.length) {
      const k = st.urgent.shift();
      if (!lazyDone.has(k) && Atomics.load(bufs.packState, k) === 0) return { kind: 'lazy', k };
    }
    while (jobs.length) {
      const j = jobs.shift();
      if (j.kind === 'prefetch' || (!st.stopped && !lazyDone.has(j.k) && Atomics.load(bufs.packState, j.k) === 0)) return j;
    }
    return null;
  };

  const doLazy = async (k) => {
    lazyDone.add(k);
    const pack = lazy.packs[k];
    // A pack whose chunks are all in memory already (the engine fetched them on demand) is skipped.
    const first = chunks.findIndex((c) => c[0] === k);
    const ks = pack.chunks.map((_, i) => first + i);
    if (ks.every((c) => Atomics.load(bufs.chunkState, c) === DELIVERED)) {
      Atomics.store(bufs.packState, k, PACK_DONE);
      return;
    }
    Atomics.store(bufs.packState, k, PACK_LOADING);
    try {
      const parts = await packChunks(snapCache, (id) => base + 'snapchunks/' + id, base + pack.file, pack, stats);
      for (let i = 0; i < pack.chunks.length; i++) {
        if (st.stopped) return;
        if (Atomics.load(bufs.chunkState, ks[i]) === DELIVERED) continue;
        const [id, , , raw] = pack.chunks[i];
        const data = await inflateNative(parts[i], raw);
        if ((await sha256Id(data)) !== id) throw new Error(`lazy chunk ${id} does not verify`);
        await ringPut(ks[i], data);
      }
      Atomics.store(bufs.packState, k, PACK_DONE);
      stats.lazyPacks++;
    } catch (e) {
      stats.failed++;
      Atomics.store(bufs.packState, k, PACK_FAILED);
      self.postMessage({ type: 'pack-error', text: String(e && e.message ? e.message : e) });
    }
    Atomics.notify(bufs.packState, k);
    Atomics.notify(bufs.ctrl, HEAD);
  };

  const doPrefetch = async (k) => {
    const pack = pfPacks[k];
    const { sab, ctrl } = prefetchBufs;
    Atomics.store(ctrl, k, PREFETCH_LOADING);
    try {
      const parts = await packChunks(chunkCache, (id) => base + 'chunks/' + id + '.z', base + pack.file, pack, stats);
      const mem = new Uint8Array(sab);
      for (let i = 0; i < pack.chunks.length; i++) {
        const [id, , , raw] = pack.chunks[i];
        mem.set(await inflateNative(parts[i], raw), pfWhere.get(id).off);
      }
      Atomics.store(ctrl, k, PF_READY);
      stats.prefetchPacks++;
    } catch (e) {
      stats.failed++;
      Atomics.store(ctrl, k, PF_FAILED);
      self.postMessage({ type: 'pack-error', text: String(e && e.message ? e.message : e) });
    }
    Atomics.notify(ctrl, k);
  };

  const worker = async () => {
    for (let j = next(); j; j = next()) {
      if (j.kind === 'lazy') await doLazy(j.k);
      else await doPrefetch(j.k);
    }
  };
  await Promise.all(Array.from({ length: FETCHES }, worker));
  // A lazy pack that failed for good is tried again, ever more slowly, until the engine has every page (a fault reads
  // one of its pages on demand meanwhile): pending pages keep the engine's 2 ms poll going.
  for (let wait = 10000; !st.stopped; wait = Math.min(wait * 2, 300000)) {
    const failed = lazy.packs.map((_, k) => k).filter((k) => Atomics.load(bufs.packState, k) === PACK_FAILED);
    if (!failed.length) break;
    await new Promise((r) => setTimeout(r, wait));
    for (const k of failed) {
      if (st.stopped) break;
      lazyDone.delete(k);
      Atomics.store(bufs.packState, k, 0);
      await doLazy(k);
    }
  }
  Atomics.store(bufs.ctrl, LOADER_DONE, 1);
  // The snapshot cache keeps this snapshot's chunks only.
  if (snapCache) {
    const keep = new Set([...(st.hotIds || []), ...chunks.map((c) => c[4])].map((id) => base + 'snapchunks/' + id));
    keep.add(base + lazy.map.file);
    try {
      for (const req of await snapCache.keys()) if (!keep.has(req.url)) await snapCache.delete(req);
    } catch {
      // a stale entry is harmless
    }
  }
  self.postMessage({ type: 'done', ms: now() - t0, stats });
}
