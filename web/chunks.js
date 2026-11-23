// Chunk store for the Boot Android page (docs/loading.md, tier 1). The page fetches the boot pack in
// parallel while the engine module loads, and decodes it with DecompressionStream into memory before the
// engine starts. The engine's reads then come from memory. A chunk that is not in a pack is fetched and
// decoded synchronously when the engine reads it (the fallback), because the engine cannot wait.
//
// Names: chunks/<id>.z is a zlib stream of one raw chunk, <id> the first 32 hex digits of the SHA-256 of
// the raw bytes. packs/<id>.bin concatenates the zlib streams of the boot's working set, in first-read
// order; the manifest lists each chunk as [id, offset, length, rawLength] within its pack. The layout
// and the packer are in tools/pack.py. Every pack chunk is hashed as it is decoded.
import { inflateZlib } from './inflate.js';

export const CHUNK_CACHE = 'xiaoji-chunks-v1';
const now = () => performance.now();

async function sha256Id(bytes) {
  const d = await crypto.subtle.digest('SHA-256', bytes);
  let hex = '';
  for (const b of new Uint8Array(d).subarray(0, 16)) hex += b.toString(16).padStart(2, '0');
  return hex;
}

// Decodes one zlib stream with the browser's own decoder (async; used for the boot pack only).
async function inflateNative(bytes, rawLen) {
  const ds = new DecompressionStream('deflate');
  const out = new Uint8Array(await new Response(new Blob([bytes]).stream().pipeThrough(ds)).arrayBuffer());
  if (out.length !== rawLen) throw new Error(`chunk: ${out.length} bytes, want ${rawLen}`);
  return out;
}

// The cache is an optimization: when Cache Storage is missing or fails, the page fetches as if it were empty.
async function openCache() {
  if (typeof caches === 'undefined') return null;
  try {
    return await caches.open(CHUNK_CACHE);
  } catch {
    return null;
  }
}

export class ChunkStore {
  // base: URL of the chunk store directory (ends in '/'); chunkSize: from the manifest; onNetwork(url, bytes)
  // is called for each response body the page downloads (the worker's fetch counters use it).
  // onProgress(p) is called as the boot packs download: p.loaded and p.total are bytes of the packs
  // (the manifest's sizes), p.fromCache is true when every pack is in the cache, p.cacheable when the
  // cache could be opened (so the next visit can use it).
  constructor(base, chunkSize, onNetwork = () => {}, onProgress = () => {}) {
    this.base = base;
    this.chunkSize = chunkSize;
    this.onNetwork = onNetwork;
    this.onProgress = onProgress;
    this.progress = { loaded: 0, total: 0, fromCache: false, cacheable: false };
    this.mem = new Map(); // id -> raw Uint8Array
    this.stats = {
      packs: 0, packDownloads: 0, packBytes: 0, packNetBytes: 0, packChunks: 0, packMs: 0, packCpuMs: 0,
      chunkCacheHits: 0, chunkCachePuts: 0,
      syncFetches: 0, syncBytes: 0, syncFetchMs: 0, syncDecodeMs: 0,
    };
  }

  chunkUrl(id) {
    return this.base + 'chunks/' + id + '.z';
  }

  emit() {
    this.onProgress({ ...this.progress });
  }

  // Async, before the engine runs. Each chunk is cached under its own hash (the URL of chunks/<id>.z), so a
  // pack is downloaded only when one of its chunks is not cached: when a build changes a chunk, the packs
  // after it shift, but their chunks are still cached, and only the pack with the new chunk is downloaded.
  async loadPacks(packs) {
    const t0 = now();
    const cache = await openCache();
    if (cache) {
      const keep = new Set();
      for (const p of packs) for (const [id] of p.chunks) keep.add(this.chunkUrl(id));
      try {
        for (const req of await cache.keys()) if (!keep.has(req.url)) await cache.delete(req);
      } catch {
        // a stale entry is harmless
      }
    }
    this.stats.packs = packs.length;
    // Probe every pack before the first download, so the progress denominator and the warm/cold decision are known.
    const have = await Promise.all(packs.map((p) => this.probe(p, cache)));
    this.progress = {
      loaded: 0,
      total: packs.reduce((n, p) => n + p.bytes, 0),
      fromCache: !!cache && have.every((h) => h && h.every(Boolean)),
      cacheable: !!cache,
    };
    this.emit();
    await Promise.all(packs.map((p, i) => this.loadPack(p, cache, have[i])));
    this.stats.packMs = now() - t0;
  }

  // The cache entries of a pack's chunks, in order (undefined where missing), or null without a cache.
  async probe(pack, cache) {
    if (!cache) return null;
    return Promise.all(pack.chunks.map(([id]) => cache.match(this.chunkUrl(id)).catch(() => undefined)));
  }

  // Streams one pack's bytes into memory, counting them into the progress as they arrive.
  async download(pack) {
    const url = this.base + pack.file;
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
      this.progress.loaded += value.length;
      this.emit();
    }
    if (off !== pack.bytes) throw new Error(`pack ${pack.file}: ${off} bytes, the manifest says ${pack.bytes}`);
    return out;
  }

  async loadPack(pack, cache, have) {
    const cached = have ? have.filter(Boolean).length : 0;
    this.stats.chunkCacheHits += cached;
    let buf = null;
    if (!have || cached < pack.chunks.length) {
      buf = await this.download(pack);
      this.stats.packDownloads++;
      this.stats.packBytes += buf.length;
      this.stats.packNetBytes += buf.length;
      this.onNetwork(this.base + pack.file, buf.length);
    } else {
      this.progress.loaded += pack.bytes;
      this.emit();
    }
    const puts = [];
    const t0 = now();
    for (let i = 0; i < pack.chunks.length; i++) {
      const [id, off, len, raw] = pack.chunks[i];
      let z;
      if (buf) {
        z = buf.subarray(off, off + len);
        if (cache && !(have && have[i])) {
          puts.push(cache.put(this.chunkUrl(id), new Response(z.slice())).then(
            () => this.stats.chunkCachePuts++,
            () => {},
          ));
        }
      } else {
        z = new Uint8Array(await have[i].arrayBuffer());
      }
      const data = await inflateNative(z, raw);
      if ((await sha256Id(data)) !== id) throw new Error(`pack ${pack.file}: chunk ${id} does not verify`);
      this.mem.set(id, data);
      this.stats.packChunks++;
    }
    this.stats.packCpuMs += now() - t0;
    await Promise.all(puts); // the puts finish before the engine starts: it never yields
  }

  // Synchronous, from the engine's reads: the chunk from memory, else fetched and decoded now.
  get(id, rawLen) {
    const hit = this.mem.get(id);
    if (hit) return hit;
    const t0 = now();
    const xhr = new XMLHttpRequest();
    xhr.open('GET', this.base + 'chunks/' + id + '.z', false);
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
