// Lazy snapshot pages, the engine's worker's side (docs/snapshot.md, S5).
//
// The snapshot's manifest splits the image: the hot part is written into memory before main (web/worker.js,
// loadSnapshot), and the lazy pages (pool pages: guest and memfd memory) come after. The engine (engine/snaplazy.c)
// keeps a bit per pending page and calls into this object (Module.xjLazy):
//   lo(), hi()   the address range of the lazy pages (0, 0: none), before it allocates its bitmap;
//   mark(bits)   sets the bit of every lazy page; returns how many;
//   fault(addr)  the engine touched a pending page: its chunk is copied in now (from the background loader's ring, by
//                waiting for its pack when that is on its way, or with a synchronous request); returns the pages written;
//   poll()       the scheduler loop, every 2 ms while pages are pending: copies in what the loader has brought;
//   finish()     every pending page in (before the engine writes a snapshot);
//   end(stats)   none is pending any more.
// Only this thread writes the engine's memory (it is not shared), so the background loader (web/snaploader.js) inflates
// into a ring of slots in a SharedArrayBuffer and this side copies them out. A page is written only while its bit is
// set: a page the engine freed before it arrived (snap_lazy_drop) has its bit cleared and keeps its new bytes.
//
// The ring: ctrl[HEAD] is the number of chunks the loader has put, ctrl[TAIL] the number taken; slot i % RING_SLOTS
// holds the chunk ctrl[SLOT0 + slot] (its index in the lazy order) in data[slot * slotBytes, ...). chunkState[k] is
// DELIVERED once chunk k is in memory (the loader skips it then), packState[p] LOADING while pack p is on its way.
import { inflateZlib } from './inflate.js';

export const RING_SLOTS = 64;
export const HEAD = 0;
export const TAIL = 1;
export const LOADER_DONE = 2; // ctrl[LOADER_DONE]: 1 once the background loader has every pack it was given
export const SLOT0 = 4;
export const DELIVERED = 1;
export const PACK_IDLE = 0;
export const PACK_LOADING = 1;
export const PACK_DONE = 2;
export const PACK_FAILED = 3;
export const SNAP_CACHE = 'xiaoji-snap-v1';
const FAULT_WAIT_MS = 2000; // the longest a fault waits for a pack that is on its way before it fetches the chunk itself
const RETRY_MS = [250, 1000, 3000, 6000];
const now = () => performance.now();

// The lazy chunks in their order: [pack index, offset in the pack, zlib bytes, raw bytes, id] each.
export function lazyChunks(lazy) {
  const out = [];
  lazy.packs.forEach((p, pi) => {
    for (const [id, off, zlen, raw] of p.chunks) out.push([pi, off, zlen, raw, id]);
  });
  return out;
}

// The shared buffers of a restore with lazy pages (one set, given to both sides).
export function lazyBuffers(lazy) {
  const nChunks = lazyChunks(lazy).length;
  const slotBytes = lazy.chunk_pages * lazy.page_bytes;
  return {
    ring: new SharedArrayBuffer(RING_SLOTS * slotBytes),
    ctrl: new Int32Array(new SharedArrayBuffer(4 * (SLOT0 + RING_SLOTS))),
    chunkState: new Int32Array(new SharedArrayBuffer(4 * Math.max(1, nChunks))),
    packState: new Int32Array(new SharedArrayBuffer(4 * Math.max(1, lazy.packs.length))),
    slotBytes,
  };
}

const sleeper = typeof SharedArrayBuffer !== 'undefined' ? new Int32Array(new SharedArrayBuffer(4)) : null;
function sleepSync(ms) {
  if (sleeper) Atomics.wait(sleeper, 0, 0, ms);
}

// A synchronous GET of bytes [start, end) of url, with a Range request; a server that sends the whole file (200)
// works too. Retried; throws when every try failed.
export function xhrRange(url, start, end) {
  const xhr = new XMLHttpRequest();
  xhr.open('GET', url, false);
  xhr.responseType = 'arraybuffer';
  xhr.setRequestHeader('Range', `bytes=${start}-${end - 1}`);
  xhr.send(null);
  if (xhr.status === 206) return new Uint8Array(xhr.response);
  if (xhr.status === 200) return new Uint8Array(xhr.response).slice(start, end);
  throw new Error(`HTTP ${xhr.status}`);
}

export class LazyPages {
  // Module: the engine; lazy: the manifest's lazy section; pages: Uint32Array, the lazy pages' numbers in order;
  // base: the chunk store's URL (xj/); bufs: lazyBuffers(lazy); get: a synchronous range GET (xhrRange);
  // loader: the background loader (a Worker, or null: every chunk on demand); record: keep every first touch
  // (tools/snapshot-web.mjs record), page by page, with no loader.
  // poison: every pending page is filled with 0xA5 when it is marked (?lazypoison=1): a read that skipped the fill reads
  // that, not the zeros of fresh memory, and the guest shows it.
  constructor({ Module, lazy, pages, base, bufs, get = xhrRange, loader = null, record = false, post = () => {}, retryMs = RETRY_MS,
    poison = false }) {
    this.Module = Module;
    this.lazy = lazy;
    this.pages = pages;
    this.base = base;
    this.bufs = bufs;
    this.get = get;
    this.loader = loader;
    this.record = record;
    this.post = post;
    this.retryMs = retryMs;
    this.poison = poison;
    this.cp = lazy.chunk_pages;
    this.chunks = lazyChunks(lazy);
    let lo = Infinity, hi = 0;
    for (const p of pages) {
      lo = Math.min(lo, p);
      hi = Math.max(hi, p + 1);
    }
    this.loPage = pages.length ? lo : 0;
    this.hiPage = pages.length ? hi : 0;
    // page number - loPage -> lazy index + 1 (0: not a lazy page)
    this.index = new Int32Array(Math.max(1, this.hiPage - this.loPage));
    pages.forEach((p, i) => (this.index[p - this.loPage] = i + 1));
    this.bits = 0;
    this.active = false;
    this.ringData = new Uint8Array(bufs.ring);
    this.cache = new Map(); // record mode: inflated chunks, a few
    this.touches = [];
    this.t0 = 0;
    this.lastPost = 0;
    this.stats = {
      pages: pages.length, faults: 0, faultMs: 0, waits: 0, waitMs: 0, syncFetches: 0, syncBytes: 0, syncMs: 0,
      inflateMs: 0, ringChunks: 0, pagesIn: 0, retries: 0, doneMs: null,
    };
  }

  lo() {
    return this.loPage * 4096;
  }

  hi() {
    return this.hiPage * 4096;
  }

  bitOf(page) {
    const i = page - this.loPage;
    return [this.bits + (i >>> 3), 1 << (i & 7)];
  }

  pending(page) {
    const [at, m] = this.bitOf(page);
    return (this.Module.HEAPU8[at] & m) !== 0;
  }

  mark(bits) {
    this.bits = bits >>> 0;
    const heap = this.Module.HEAPU8;
    for (const p of this.pages) {
      const i = p - this.loPage;
      heap[this.bits + (i >>> 3)] |= 1 << (i & 7);
      if (this.poison) heap.fill(0xa5, p * 4096, p * 4096 + 4096);
    }
    this.active = true;
    this.t0 = now();
    if (this.loader) this.loader.postMessage({ type: 'go' });
    return this.pages.length;
  }

  // Writes chunk k's pages that are still pending from raw (its bytes); returns how many.
  put(k, raw, only = -1) {
    const heap = this.Module.HEAPU8;
    const first = k * this.cp;
    const n = Math.min(this.cp, this.pages.length - first);
    let wrote = 0;
    for (let j = 0; j < n; j++) {
      const p = this.pages[first + j];
      if (only >= 0 && p !== only) continue;
      const [at, m] = this.bitOf(p);
      if (!(heap[at] & m)) continue;
      heap.set(raw.subarray(j * 4096, (j + 1) * 4096), p * 4096);
      heap[at] &= ~m;
      wrote++;
    }
    if (only < 0) Atomics.store(this.bufs.chunkState, k, DELIVERED);
    this.stats.pagesIn += wrote;
    return wrote;
  }

  poll() {
    if (!this.active) return 0;
    const { ctrl } = this.bufs;
    let wrote = 0;
    let tail = Atomics.load(ctrl, TAIL);
    const head = Atomics.load(ctrl, HEAD);
    while (tail !== head) {
      const slot = tail % RING_SLOTS;
      const k = Atomics.load(ctrl, SLOT0 + slot);
      if (Atomics.load(this.bufs.chunkState, k) !== DELIVERED) {
        const raw = this.ringData.subarray(slot * this.bufs.slotBytes, slot * this.bufs.slotBytes + this.chunks[k][3]);
        wrote += this.put(k, raw);
        this.stats.ringChunks++;
      }
      tail++;
      Atomics.store(ctrl, TAIL, tail);
    }
    if (wrote) Atomics.notify(ctrl, TAIL);
    const t = now();
    if (t - this.lastPost > 500) {
      this.lastPost = t;
      this.report();
    }
    return wrote;
  }

  report(done = false) {
    this.stats.loaderDone = Atomics.load(this.bufs.ctrl, LOADER_DONE) === 1;
    const msg = { type: 'lazy', stats: { ...this.stats }, done };
    if (this.record && this.touches.length) {
      msg.touches = this.touches;
      this.touches = [];
    }
    this.post(msg);
  }

  // Chunk k's bytes, fetched now (a range of its pack), retried.
  fetchChunk(k) {
    const [pi, off, zlen, raw] = this.chunks[k];
    const url = this.base + this.lazy.packs[pi].file;
    let err = null;
    for (let t = 0; t <= this.retryMs.length; t++) {
      if (t) {
        this.stats.retries++;
        sleepSync(this.retryMs[t - 1]);
      }
      try {
        const t0 = now();
        const z = this.get(url, off, off + zlen);
        const t1 = now();
        const data = inflateZlib(z, raw);
        this.stats.syncFetches++;
        this.stats.syncBytes += z.length;
        this.stats.syncMs += t1 - t0;
        this.stats.inflateMs += now() - t1;
        return data;
      } catch (e) {
        err = e;
      }
    }
    throw new Error(`snapshot: the lazy pages of ${this.lazy.packs[pi].file} could not be downloaded ` +
      `(${this.retryMs.length + 1} tries: ${err && err.message ? err.message : err})`);
  }

  fault(addr) {
    const t0 = now();
    const page = Math.floor(addr / 4096);
    const li = page >= this.loPage && page < this.hiPage ? this.index[page - this.loPage] - 1 : -1;
    if (li < 0) throw new Error(`snapshot: page 0x${addr.toString(16)} is pending but not a lazy page`);
    const k = Math.floor(li / this.cp);
    this.stats.faults++;
    let wrote = 0;
    if (this.record) {
      // One page at a time, so every first touch is seen; the chunk is kept for its neighbours.
      let raw = this.cache.get(k);
      if (!raw) {
        raw = this.fetchChunk(k);
        if (this.cache.size >= 512) this.cache.delete(this.cache.keys().next().value); // 64 MiB of chunks
        this.cache.set(k, raw);
      }
      wrote = this.put(k, raw, page);
      this.touches.push([page * 4096, Math.round(t0 - this.t0)]);
    } else {
      wrote += this.poll();
      if (this.pending(page)) {
        const pi = this.chunks[k][0];
        if (this.loader && Atomics.load(this.bufs.packState, pi) !== PACK_DONE) {
          this.loader.postMessage({ type: 'urgent', pack: pi });
        }
        // Its pack is on its way: wait for the loader (copying in whatever it puts meanwhile).
        const tw = now();
        while (this.pending(page) && this.loader && Atomics.load(this.bufs.packState, pi) === PACK_LOADING &&
          now() - tw < FAULT_WAIT_MS) {
          Atomics.wait(this.bufs.ctrl, HEAD, Atomics.load(this.bufs.ctrl, HEAD), 20);
          wrote += this.poll();
        }
        if (now() - tw > 0.05) {
          this.stats.waits++;
          this.stats.waitMs += now() - tw;
        }
        if (this.pending(page)) wrote += this.put(k, this.fetchChunk(k));
      }
    }
    this.stats.faultMs += now() - t0;
    return wrote;
  }

  finish() {
    let wrote = this.poll();
    for (let k = 0; k < this.chunks.length; k++) {
      const first = k * this.cp;
      const n = Math.min(this.cp, this.pages.length - first);
      let any = false;
      for (let j = 0; j < n && !any; j++) any = this.pending(this.pages[first + j]);
      if (any) wrote += this.put(k, this.fetchChunk(k));
    }
    return wrote;
  }

  end(engineStats) {
    this.active = false;
    this.stats.doneMs = now() - this.t0;
    Object.assign(this.stats, { engineFaults: engineStats.faults, engineFaultMs: engineStats.faultMs, dropped: engineStats.dropped });
    if (this.loader) this.loader.postMessage({ type: 'stop' });
    this.report(true);
  }
}
