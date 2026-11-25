// The restore's prefetch (docs/demo.md, P1): a worker of the engine's worker that reads the chunks a visitor's first
// minute needs, in the order a recorded session first read them (tools/prefetch.py, the snapshot manifest's
// "prefetch" packs), while the engine runs. The engine's worker cannot wait for anything async once the engine
// runs, so the chunks go into a SharedArrayBuffer it laid out before (layoutPacks in web/chunks.js), and each pack's
// control word says when its chunks are there: the chunk store (attachPrefetch) takes a chunk from here when its pack
// is ready, waits a little for a pack in flight, and otherwise reads the chunk itself as before.
//
// In:  { base (the chunk store's URL), packs, sab, ctrl, delayMs }
// Out: { type: 'prefetch-done', ms, bytes, packs, failed }
import { PACK_FAILED, PACK_READY, inflateNative, layoutPacks } from './chunks.js';

export const PACK_LOADING = 3;
const now = () => performance.now();

self.onmessage = async (ev) => {
  const { base, packs, sab, ctrl, delayMs } = ev.data;
  self.postMessage({ type: 'prefetch-started' });
  const t0 = now();
  // The restore's own download and inflate come first; the prefetch starts after them.
  if (delayMs > 0) await new Promise((r) => setTimeout(r, delayMs));
  const { where } = layoutPacks(packs);
  const mem = new Uint8Array(sab);
  let bytes = 0;
  let failed = 0;
  for (let k = 0; k < packs.length; k++) {
    const p = packs[k];
    Atomics.store(ctrl, k, PACK_LOADING);
    try {
      const res = await fetch(base + p.file);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const buf = new Uint8Array(await res.arrayBuffer());
      bytes += buf.length;
      for (const [id, off, zlen, raw] of p.chunks) {
        const c = await inflateNative(buf.subarray(off, off + zlen), raw);
        mem.set(c, where.get(id).off);
      }
      Atomics.store(ctrl, k, PACK_READY);
    } catch {
      failed++;
      Atomics.store(ctrl, k, PACK_FAILED);
    }
    Atomics.notify(ctrl, k);
  }
  self.postMessage({ type: 'prefetch-done', ms: now() - t0, bytes, packs: packs.length, failed });
};
