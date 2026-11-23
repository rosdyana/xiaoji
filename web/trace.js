// Opt-in access trace for the Boot Android page (docs/loading.md). Wraps FS.read, which is where the
// engine's reads and its mmap fills end up (host_pread -> libc pread -> FS.read), so one hook covers
// both. The trace records, per file, the 4 KiB pages and 64 KiB chunks that were read, and the order in
// which the 64 KiB chunks were first read. It is off unless the page asks for it (?trace).

export const PAGE = 4096;
export const CHUNK = 65536;

// Runs of set bits in a bitmap, as [start, count] pairs.
function runs(bits, n) {
  const out = [];
  let start = -1;
  for (let i = 0; i <= n; i++) {
    const on = i < n && bits[i] === 1;
    if (on && start < 0) start = i;
    if (!on && start >= 0) {
      out.push([start, i - start]);
      start = -1;
    }
  }
  return out;
}

// sizes: Map from guest path to file size. Only paths in sizes are traced.
export function openTrace(FS, sizes) {
  const files = new Map(); // path -> { pages, chunks } bitmaps
  const order = []; // [path, chunk index] in first-read order
  let calls = 0;
  let bytes = 0;
  let readMs = 0; // all FS.read time, traced files or not (includes any synchronous fetch in it)
  const origRead = FS.read;
  FS.read = function (stream, buffer, offset, length, position) {
    const pos = position === undefined ? stream.position : position;
    const t0 = performance.now();
    const n = origRead.call(this, stream, buffer, offset, length, position);
    readMs += performance.now() - t0;
    const size = n > 0 ? sizes.get(stream.path) : undefined;
    if (size !== undefined) {
      calls++;
      bytes += n;
      let f = files.get(stream.path);
      if (!f) {
        f = { pages: new Uint8Array(Math.ceil(size / PAGE)), chunks: new Uint8Array(Math.ceil(size / CHUNK)) };
        files.set(stream.path, f);
      }
      const p0 = Math.floor(pos / PAGE);
      const p1 = Math.floor((pos + n - 1) / PAGE);
      for (let p = p0; p <= p1; p++) f.pages[p] = 1;
      const c0 = Math.floor(pos / CHUNK);
      const c1 = Math.floor((pos + n - 1) / CHUNK);
      for (let c = c0; c <= c1; c++) {
        if (!f.chunks[c]) {
          f.chunks[c] = 1;
          order.push([stream.path, c]);
        }
      }
    }
    return n;
  };
  return {
    summary() {
      const out = { calls, bytes, readMs, chunkSize: CHUNK, pageSize: PAGE, files: {}, order };
      for (const [path, f] of files) {
        out.files[path] = {
          size: sizes.get(path),
          pages: runs(f.pages, f.pages.length),
          chunks: runs(f.chunks, f.chunks.length),
        };
      }
      return out;
    },
  };
}
