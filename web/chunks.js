// Chunk store for the Boot Android page (docs/loading.md, tier 1 and L2). The boot packs are downloaded by a
// second worker (web/packloader.js) into one SharedArrayBuffer, in the boot's first-read order. The engine's
// worker starts once the first pack (or the first N MB) is in, and its reads of a chunk in a pack wait
// (Atomics.wait) for that pack, not for a new request. A chunk in no pack, or in a pack that failed, is fetched
// with a synchronous request and decoded when the engine reads it (the fallback).
//
// Names: chunks/<id>.z is a zlib stream of one raw chunk, <id> the first 32 hex digits of the SHA-256 of
// the raw bytes. packs/<id>.bin concatenates the zlib streams of the boot's working set, in first-read
// order; the manifest lists each chunk as [id, offset, length, rawLength] within its pack. The layout
// and the packer are in tools/pack.py.
import { inflateZlib } from './inflate.js';

export const CHUNK_CACHE = 'xiaoji-chunks-v1';
export const PACK_PENDING = 0;
export const PACK_READY = 1;
export const PACK_FAILED = 2;
const PREFETCH_WAIT_MS = 250;
const now = () => performance.now();

export async function sha256Id(bytes) {
  const d = await crypto.subtle.digest('SHA-256', bytes);
  let hex = '';
  for (const b of new Uint8Array(d).subarray(0, 16)) hex += b.toString(16).padStart(2, '0');
  return hex;
}

// Decodes one zlib stream with the browser's own decoder (async; used for the boot packs only).
export async function inflateNative(bytes, rawLen) {
  const ds = new DecompressionStream('deflate');
  const out = new Uint8Array(await new Response(new Blob([bytes]).stream().pipeThrough(ds)).arrayBuffer());
  if (out.length !== rawLen) throw new Error(`chunk: ${out.length} bytes, want ${rawLen}`);
  return out;
}

// The cache is an optimization: when Cache Storage is missing or fails, the page fetches as if it were empty.
export async function openCache() {
  if (typeof caches === 'undefined') return null;
  try {
    return await caches.open(CHUNK_CACHE);
  } catch {
    return null;
  }
}

// The place of every pack chunk in the shared buffer. Both workers compute it from the manifest's packs, so
// neither needs a message to find a chunk: where.get(id) is { k (pack index), off (byte offset), raw }.
export function layoutPacks(packs) {
  const where = new Map();
  let total = 0;
  packs.forEach((p, k) => {
    for (const [id, , , raw] of p.chunks) {
      if (where.has(id)) throw new Error(`manifest: chunk ${id} is in two packs`);
      where.set(id, { k, off: total, raw });
      total += raw;
    }
  });
  return { where, total };
}

// The first pack index that must be ready before the engine starts: the first pack whose end reaches
// earlyBytes of pack data (0 means the first pack).
export function earlyPackIndex(packs, earlyBytes) {
  let cum = 0;
  for (let k = 0; k < packs.length; k++) {
    cum += packs[k].bytes;
    if (cum >= earlyBytes) return k;
  }
  return Math.max(0, packs.length - 1);
}

export class ChunkStore {
  // base: URL of the chunk store directory (ends in '/'); chunkSize: from the manifest. The synchronous requests
  // are counted by the worker's XHR hook, not here.
  constructor(base, chunkSize) {
    this.base = base;
    this.chunkSize = chunkSize;
    this.mem = new Map(); // id -> raw Uint8Array (a view of the shared buffer, or a decoded fallback chunk)
    this.where = null; // id -> { k, off, raw } for the chunks of the packs (attachPacks)
    this.sab = null;
    this.ctrl = null;
    this.stats = {
      packs: 0, packChunks: 0, packWaits: 0, packWaitMs: 0, packStartMs: 0, packFailedFallbacks: 0,
      syncFetches: 0, syncBytes: 0, syncFetchMs: 0, syncDecodeMs: 0,
    };
  }

  chunkUrl(id) {
    return this.base + 'chunks/' + id + '.z';
  }

  // The packs' shared buffer and control words (from the downloader). Packs not attached: every read is a fallback.
  attachPacks(packs, sab, ctrl) {
    this.where = layoutPacks(packs).where;
    this.sab = sab;
    this.ctrl = ctrl;
    this.stats.packs = packs.length;
  }

  // The restore's prefetch (web/prefetch.js, docs/demo.md P1): packs a second worker fills while the engine runs.
  // Their control words are PACK_PENDING (not started), 3 (in flight), PACK_READY or PACK_FAILED.
  attachPrefetch(packs, sab, ctrl) {
    this.pwhere = layoutPacks(packs).where;
    this.psab = sab;
    this.pctrl = ctrl;
    Object.assign(this.stats, { prefetchHits: 0, prefetchWaits: 0, prefetchWaitMs: 0, prefetchMisses: 0 });
  }

  // A chunk of the prefetch: its bytes when its pack is in (waiting up to PREFETCH_WAIT_MS for a pack in flight, which
  // is faster than a request and the synchronous decoder), else null (not started yet, or failed).
  prefetched(id, rawLen) {
    const w = this.pwhere && this.pwhere.get(id);
    if (!w || w.raw !== rawLen) return null;
    let v = Atomics.load(this.pctrl, w.k);
    if (v === 3) {
      const t0 = now();
      Atomics.wait(this.pctrl, w.k, 3, PREFETCH_WAIT_MS);
      v = Atomics.load(this.pctrl, w.k);
      this.stats.prefetchWaits++;
      this.stats.prefetchWaitMs += now() - t0;
    }
    if (v !== PACK_READY) {
      this.stats.prefetchMisses++;
      return null;
    }
    this.stats.prefetchHits++;
    return new Uint8Array(this.psab, w.off, w.raw);
  }

  // Async, before the engine starts: resolves when packs 0..upto are ready or failed. A failed pack is not an
  // error here: its chunks fall back to synchronous reads.
  async waitStart(upto) {
    if (!this.ctrl) return;
    const t0 = now();
    for (let k = 0; k <= upto; k++) {
      while (Atomics.load(this.ctrl, k) === PACK_PENDING) {
        await Atomics.waitAsync(this.ctrl, k, PACK_PENDING).value;
      }
    }
    this.stats.packStartMs = now() - t0;
  }

  // Synchronous, from the engine's reads: blocks until pack k is ready or failed. True if it is ready.
  waitPack(k) {
    let v = Atomics.load(this.ctrl, k);
    if (v !== PACK_PENDING) return v === PACK_READY;
    const t0 = now();
    while ((v = Atomics.load(this.ctrl, k)) === PACK_PENDING) Atomics.wait(this.ctrl, k, PACK_PENDING);
    this.stats.packWaits++;
    this.stats.packWaitMs += now() - t0;
    return v === PACK_READY;
  }

  // Synchronous, from the engine's reads: the chunk from memory, else from its pack (waiting for the pack), else
  // fetched and decoded now (the fallback).
  get(id, rawLen) {
    const hit = this.mem.get(id);
    if (hit) return hit;
    const w = this.where && this.where.get(id);
    if (w) {
      if (w.raw !== rawLen) throw new Error(`chunk ${id}: ${rawLen} bytes, the pack says ${w.raw}`);
      if (this.waitPack(w.k)) {
        const data = new Uint8Array(this.sab, w.off, w.raw);
        this.mem.set(id, data);
        this.stats.packChunks++;
        return data;
      }
      this.stats.packFailedFallbacks++;
    }
    if (this.pwhere) {
      const data = this.prefetched(id, rawLen);
      if (data) {
        this.mem.set(id, data);
        return data;
      }
    }
    const t0 = now();
    const xhr = new XMLHttpRequest();
    xhr.open('GET', this.chunkUrl(id), false);
    xhr.responseType = 'arraybuffer';
    xhr.send(null);
    if (xhr.status !== 200) throw new Error(`chunk ${id}: HTTP ${xhr.status}`);
    const z = new Uint8Array(xhr.response);
    const t1 = now();
    const data = inflateZlib(z, rawLen);
    this.stats.syncFetches++;
    this.stats.syncBytes += z.length;
    this.stats.syncFetchMs += t1 - t0;
    this.stats.syncDecodeMs += now() - t1;
    this.mem.set(id, data);
    return data;
  }
}

// Creates the guest tree in MEMFS like buildTree (worker.js), but a regular file's bytes come from the chunk
// store, chunk by chunk, when the engine reads them. The size is the manifest's, so stat and lseek need no
// request. The reads go through FS.read, which is where the access trace hooks in (web/trace.js).
export function buildChunkTree(FS, manifest, store) {
  FS.mkdir('/rootfs');
  const cs = store.chunkSize;
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
      const node = FS.createFile(parent, name, null, true, false);
      const size = e.size;
      const ids = e.chunks;
      Object.defineProperty(node, 'usedBytes', { get: () => size });
      const read = (stream, buffer, offset, length, position) => {
        if (position >= size) return 0;
        const n = Math.min(size - position, length);
        let done = 0;
        while (done < n) {
          const pos = position + done;
          const ci = Math.floor(pos / cs);
          const within = pos - ci * cs;
          const c = store.get(ids[ci], Math.min(cs, size - ci * cs));
          const take = Math.min(c.length - within, n - done);
          buffer.set(c.subarray(within, within + take), offset + done);
          done += take;
        }
        return n;
      };
      node.stream_ops = { ...node.stream_ops, read };
      FS.chmod(host, e.mode);
      files++;
    } else {
      throw new Error(`manifest: unknown type ${e.type} for ${e.path}`);
    }
  }
  return files;
}
