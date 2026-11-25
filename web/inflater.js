// A helper of the snapshot restore (web/worker.js, loadSnapshot; docs/snapshot.md, P1): it inflates a batch of zlib
// chunks with the browser's native DecompressionStream and sends the raw chunks back, their buffers transferred. The
// engine's worker copies them into its memory. DecompressionStream inflates on its caller's thread, so the restore
// runs several of these helpers to inflate in parallel.
//
// In:  { job, z: ArrayBuffer (the compressed bytes; transferred), parts: [[offset, zlen, raw], ...] }
// Out: { job, bufs: [ArrayBuffer, ...] (the raw chunks, in the order of parts; transferred) } or { job, error }
import { inflateNative } from './chunks.js';

self.onmessage = async (ev) => {
  const { job, z, parts } = ev.data;
  try {
    const src = new Uint8Array(z);
    const bufs = [];
    for (const [off, zlen, raw] of parts) bufs.push((await inflateNative(src.subarray(off, off + zlen), raw)).buffer);
    self.postMessage({ job, bufs }, bufs);
  } catch (e) {
    self.postMessage({ job, error: String(e && e.message ? e.message : e) });
  }
};
