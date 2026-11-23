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

export const PACK_CACHE = 'xiaoji-packs-v1';
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

export class ChunkStore {
  // base: URL of the chunk store directory (ends in '/'); chunkSize: from the manifest.
  constructor(base, chunkSize) {
    this.base = base;
    this.chunkSize = chunkSize;
    this.mem = new Map(); // id -> raw Uint8Array
    this.stats = {
      packs: 0, packBytes: 0, packNetBytes: 0, packCacheHits: 0, packChunks: 0, packMs: 0, packCpuMs: 0,
      syncFetches: 0, syncBytes: 0, syncFetchMs: 0, syncDecodeMs: 0,
    };
  }

  // Async, before the engine runs: fetches the packs (in parallel, from Cache Storage when the pack
  // was fetched before), verifies and decodes every chunk into memory.
  async loadPacks(packs) {
    const t0 = now();
    const urls = packs.map((p) => this.base + p.file);
    const cache = typeof caches !== 'undefined' ? await caches.open(PACK_CACHE) : null;
    if (cache) {
      // Packs are named by their hash; the ones this build does not list are stale.
      const keep = new Set(urls);
      for (const req of await cache.keys()) if (!keep.has(req.url)) await cache.delete(req);
    }
    this.stats.packs = packs.length;
    await Promise.all(packs.map((p, i) => this.loadPack(p, urls[i], cache)));
    this.stats.packMs = now() - t0;
  }

  async loadPack(pack, url, cache) {
    let res = cache ? await cache.match(url) : undefined;
    let net = 0;
    if (res) {
      this.stats.packCacheHits++;
    } else {
      res = await fetch(url);
      if (!res.ok) throw new Error(`pack ${pack.file}: HTTP ${res.status}`);
      if (cache) await cache.put(url, res.clone());
      net = 1;
    }
    const buf = new Uint8Array(await res.arrayBuffer());
    if (buf.length !== pack.bytes) {
      throw new Error(`pack ${pack.file}: ${buf.length} bytes, the manifest says ${pack.bytes}`);
    }
    this.stats.packBytes += buf.length;
    if (net) this.stats.packNetBytes += buf.length;
    const t0 = now();
    for (const [id, off, len, raw] of pack.chunks) {
      const data = await inflateNative(buf.subarray(off, off + len), raw);
      if ((await sha256Id(data)) !== id) throw new Error(`pack ${pack.file}: chunk ${id} does not verify`);
      this.mem.set(id, data);
      this.stats.packChunks++;
    }
    this.stats.packCpuMs += now() - t0;
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
