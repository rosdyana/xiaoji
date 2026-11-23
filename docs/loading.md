# Loading the Boot Android page (W1 follow-up)

The Boot Android preview (`docs/web-preview.md`) fetched the product tree lazily, one synchronous request per
file and one HEAD per file, and the engine waited for each. This note measures what the boot reads, designs the
delivery layer, and reports what tier 1 gains. Tier 2 is design only.

## 1. Measurements

### Setup

- Product tree `build/rootfs-xiaoji`: 3,510 files, 1,327.2 MiB (1.39 GB). Boot: `xiaoji.rc`, `--logcat`, phase 100.
- Headless Chromium through `tools/webboot-bench.mjs` (Playwright from the xiaonux bench), a fresh profile per
  cold run, the same machine for every row.
- Power: the first two trace runs were on battery with Low Power Mode on (116 s engine, the same as
  `docs/web-preview.md`). Later the machine was on AC with Low Power Mode off; every timing in section 4 is from
  that state, recorded by `pmset -g` at the start and end of the batch. The two states differ by almost 2x, so no
  number here is compared with a run from the other state.
- Access trace: `?trace` (`web/trace.js`) wraps `FS.read`. Every engine read reaches it: `read`, `pread` and
  the mmap fill (`engine/syscall.c` `map_file` -> `fill_from_fd` -> `host_pread` -> libc `pread` -> `FS.read`).
  The trace records 4 KiB pages, 64 KiB chunks, the first-read order of the chunks, and the time spent in
  `FS.read`. It is opt-in; the timing rows come from runs without it.

### Working set

`tools/workingset.py` reads the trace and the tree.

| Type | Files | File bytes (MB) | 4 KiB pages touched (MB) | Touched share | 64 KiB chunks touched (MB) |
|---|---|---|---|---|---|
| .so | 383 | 113.6 | 105.3 | 92.7% | 120.1 |
| .oat/.odex/.vdex/.art | 71 | 95.3 | 92.7 | 97.2% | 96.2 |
| .jar/.apk | 74 | 151.8 | 131.5 | 86.7% | 135.1 |
| other | 434 | 110.1 | 111.2 | 101.0% | 131.4 |
| all | 962 | 470.8 | 440.8 | 93.6% | 441.1 |

The boot read 962 of the 3,510 files. The other 2,548 files (about 920 MB) were never opened. Of the bytes of the
962 files, 93.6% are read at least once. The cause is the engine's mmap: `map_file` fills the whole mapped
range at `mmap` time, so every mapped byte is read, whether or not the guest touches it. The trace therefore
measures what the engine maps and reads, not what the guest touches. The page counter counts a file's last page
in full, so the "other" row shows slightly over 100%; that is a counting artifact, not extra data.

Engine reads in total: 19,976 `FS.read` calls, 772 MB, because the same ranges are read more than once. Time in
`FS.read`: 30.5 s of the 66 s engine time of the same run. That includes the synchronous fetches, which blocked
for 3.3 s (1,300 GETs and 1,027 HEADs in the old path).

Old path, by phase 100: 1,299 GETs carrying 451.9 MB (the page's counter), against 440.8 MB of touched pages.
The 1 MB chunks of emscripten's lazy files fetch 2.5% more than the pages the engine reads.

### Chunk size

Bytes in the chunks that cover the touched pages (overfetch against 4 KiB pages):

| Chunk size | Chunks covering touched pages | Bytes in them (MB) | Over the touched pages |
|---|---|---|---|
| 16 KiB | 27,406 | 439.7 | -0.3% (counting artifact) |
| 64 KiB | 7,366 | 441.1 | 0.1% |
| 128 KiB | 4,042 | 442.2 | 0.3% |
| 256 KiB | 2,433 | 444.3 | 0.8% |
| 1 MiB | 1,285 | 451.9 | 2.5% |

The working set is dense, so the chunk size barely changes what the boot downloads. The file count sets the
limit: Cloudflare Pages allows 20,000 files per deployment. At 64 KiB the whole tree needs 19,569 files (section
3), which fits, but leaves 431 for the page and for growth. At 128 KiB it needs 10,991. The cost of 128 KiB is
0.2% more overfetch on the boot (0.3% against 0.1%), and the boot packs are the same size either way. So the store
uses 128 KiB, for headroom under the file limit.

### Compressibility of the working set

The working set is the 64 KiB chunks above, 441.1 MB raw. Solid streams per file type (MB), and per-chunk gzip
for the chunk store:

| Format | .so | .oat/.odex/.vdex/.art | .jar/.apk | other | all (solid) | all, per 64 KiB chunk |
|---|---|---|---|---|---|---|
| gzip-1 | 42.1 | 44.1 | 44.8 | 67.1 | 198.2 | 200.7 |
| gzip-6 | 38.7 | 40.4 | 40.4 | 65.1 | 184.6 | 188.5 |
| gzip-9 | 38.5 | 40.2 | 40.3 | 65.0 | 184.1 | 188.1 |
| zstd-3 | 36.3 | 39.7 | 41.1 | 60.5 | 177.5 | not measured |
| zstd-15 | 32.8 | 36.4 | 38.2 | 55.9 | 163.3 | not measured |
| brotli-5 | 32.4 | 36.5 | 36.7 | 57.4 | 163.0 | not measured |
| brotli-9 | 30.0 | 34.5 | 35.3 | 53.4 | 153.2 | not measured |

gzip-9 per chunk is 42.6% of the raw size. Brotli-9 as one solid stream is 17% smaller than gzip-9 as one solid
stream, which is about 30 MB on this boot. The browser decodes deflate natively (`DecompressionStream('deflate')`)
and brotli only through a wasm decoder. That gain is not large enough for a decoder in the page, so the store
uses deflate.

### Where the 116 s goes

Engine time is the stamp at phase 100. Of the old path's 66 s engine time (AC, Low Power Mode off):

- 30.5 s inside `FS.read`, of which 3.3 s is blocked in the 2,327 synchronous requests (1,300 GETs and 1,027 HEADs).
  The other 27 s is the copy in emscripten's `writeChunks` loop, which reads a lazy file one byte at a time through
  `LazyUint8Array.get`. That attribution is by subtraction; it was not profiled. Section 4 checks it: the pack
  path, which copies whole chunks, spends much less time in `FS.read`.
- The other 35.5 s is the engine's own work (translation, the boot), outside the read path.

So on localhost the network is a small part of the engine time, and most of the time inside reads is the byte copy.
On a real network the synchronous fetches are the larger cost, and the HEADs add a round trip each (section 4 has
the simulated numbers).

## 2. Design

### Tier 1 (implemented)

**(a) Content-addressed chunk store.** `tools/pack.py` splits each file into 128 KiB chunks. A chunk is named by
the first 32 hex digits (128 bits) of the SHA-256 of its raw bytes and stored as `chunks/<id>.z`, a zlib stream
(level 9). Chunks are shared across files and across builds, so identical content is stored once. The manifest
v2 (`manifest-xiaoji-v2.json`) lists, per file, its size and its chunk ids. The browser needs no HEAD request: the
size comes from the manifest.

**(b) Boot pack.** `packs/<id>.bin` concatenates the zlib streams of the boot's working set, in the boot's
first-read order (`image/boot-order.json`, from the trace). Each pack is at most 16 MiB, under the 25 MiB file
limit. The manifest lists each chunk's offset, length and raw length in its pack. The page fetches all packs in
parallel while the engine module is imported and instantiated, decodes them with `DecompressionStream`, checks
each chunk's hash (`crypto.subtle`), and only then builds the tree and starts the engine. Engine reads then come
from memory.

**(c) Persistent cache.** Each chunk is stored in Cache Storage (`xiaoji-chunks-v1`) under the URL of its
hash, `chunks/<id>.z`. Before the packs are downloaded, the page checks every chunk of every pack. A pack is
downloaded only when one of its chunks is missing; the packs of a build are cut by running size, so a changed
chunk shifts the packs after it, but their chunks are still cached and they are not downloaded. The chunks of a
downloaded pack are written to the cache, and the writes are awaited before the engine starts, because the
engine never yields. Entries for chunks the build no longer lists are deleted. Cache Storage failures are
ignored and the page falls back to the network. A chunk fetched during the run (the fallback) is not written to
the cache: the engine's thread is busy and the fallback read is synchronous.

**(d) Compression.** The store uses zlib (deflate), decoded by the browser (`DecompressionStream('deflate')`)
on the async pack path, and by `web/inflate.js` (a small synchronous inflater, after puff.c) on the fallback.
The store does not depend on `Content-Encoding`, so any static host serves the files as they are, with
`application/octet-stream` and no transformation. How Cloudflare Pages treats a precompressed `.br` file is not
determined here: the one Pages page I could fetch did not describe it, so the design does not rely on it. Brotli
would need a wasm decoder (section 1, compressibility), and the gain did not justify one.

**Fallback.** A chunk that is not in memory is fetched with a synchronous request and decoded with `inflate.js`,
then kept in memory. The boot does not stop at phase 100, and a read after phase 100 can miss the boot order (in
the warm runs one 1 KB chunk was read after phase 100 in some runs; the runs count the bytes up to phase 100). A file's MEMFS node reads through the chunk store (`buildChunkTree` in `web/chunks.js`),
with the size from the manifest. The engine's reads, `pread` and the mmap fill alike, pass through `FS.read` and
so through the store.

**Why not the xiaonux design for the engine.** xiaonux's block device suspends the request in C, the page fetches
the chunk asynchronously, and C resumes. xiaoji's engine runs to completion inside `callMain` and has no point
where it can yield to the page, so a read that misses cannot wait for an async fetch. The adaptation is the one
xiaonux's prefetch bundle already uses: everything the boot is known to read is in memory before the engine
starts, and a miss falls back to a synchronous request.

### Tier 2 (design only)

**Post-boot snapshot.** A returning visitor could skip the boot if the engine state at phase 100 were saved and
restored. xiaonux saves the whole VM (RAM, CPU, devices) in `emu/wasm/xsnap*`. For xiaoji the state is larger
and spread over more objects: guest RAM as memfd-backed views (`engine/mem.c`, `engine/memfd.c`), the process
table and per-thread CPU state (`engine/proc.c`, `engine/sched.c`, `engine/cpu_a64.c`), the descriptor table
(`engine/vfs.c`, `engine/fdobj.c`), binder and its proc state (`engine/binder.c`), the property area, the
pipes and sockets, signals, the guest file metadata layer (`engine/meta.c`, which is a no-op in the browser build),
and the translation cache (`engine/jit.c`). A snapshot would need every one of these serialized. File-backed
mappings should be stored as references (path and offset) into the chunk store, not as copies, so the snapshot
stays small. Host state (open host fds, the log sink, the timer) would be reopened on restore. The size is not
measured yet: it needs the guest RAM in use at phase 100, which this work did not record.

**Per-app packs.** Apps opened after the boot would get their own packs, built from a trace of opening each app,
loaded when the app is first opened. The store's format already supports that: a pack is a list of chunk ids.

**Trimming the image.** 2,548 files (about 920 MB, two thirds of the tree) are never opened by the boot. They
are the candidates for removal from the product image, or for moving to an on-demand pack. The image's dex2oat
options also decide the size of the `.oat`, `.odex` and `.vdex` files, which are 95 MB of the files the boot
opens. Both need the product build, not the page.

**Engine: lazy mmap.** The eager fill in `map_file` is why 94% of mapped bytes are read. A demand-filled mapping
would cut the working set toward what the guest touches, which the trace does not measure (section 1). That is an
engine change, and it is out of scope for tier 1.

## 3. Pack output (full product tree)

`tools/pack.py build/rootfs-xiaoji build/web/xj --order image/boot-order.json --chunk-size 131072` (MiB as the
packer prints them):

- 3,510 files; 10,978 distinct chunks (461.0 MiB as zlib level 9); 12 boot packs, 177.7 MiB (186.3 MB) in total.
- 10,991 deployed files (chunks, packs, manifest), under the 20,000 limit.
- At 64 KiB (scratch run, not kept): 19,556 chunks, 12 packs (178.6 MiB), 19,569 deployed files. It fits, with 431
  to spare, which is why 128 KiB was chosen.
- `node tests/chunks.mjs`: all 10,978 chunks decode with `web/inflate.js` and hash to their names; all 3,510 files
  reassemble from their chunks to the original bytes (1,327.2 MiB); all 12 packs equal their chunks.
- A corrupted chunk in a copy makes the check fail, naming the chunk and the file.

## 4. Results

Commit `w1-browser-boot`. Headless Chromium on the M2 MacBook, AC power with Low Power Mode off (`pmset -g`
before and after the batch). Each cold run has a fresh profile. The warm runs reuse the profile of one cold run,
on the same port, because Cache Storage is per origin. Localhost rows are the median of three runs, and each
row's spread is given below. Throttled rows are one run each, with the model in section 5. Every run is taken
at phase 100 (the wire tally and the stats are read there, not at the Stop click, since the boot runs on).

| Config | Wall to phase 100 (s) | Engine (s) | First log line (s) | Wire to phase 100 (MB) | Requests | Packs before engine (s) | Blocked on fetch (s) | Chunk cache hits |
|---|---|---|---|---|---|---|---|---|
| manifest (before), localhost, median of 3 | 81.8 | 75.8 | 0.20 | 452.9 | 1,300 sync GETs, 1,027 HEADs | 0.0 | 7.4 | n/a |
| pack (after), localhost, cold, median of 3 | 41.8 | 38.3 | 2.65 | 188.1 | 12 pack GETs, 0 sync GETs | 2.6 | 2.6 | 0 of 4,023 |
| pack (after), localhost, warm, median of 3 | 49.6 | 45.6 | 2.85 | 1.7 | 0 pack GETs, 0 sync GETs | 2.8 | 2.8 | 4,023 of 4,023 |
| manifest (before), 50 Mbit/s, 20 ms RTT | 201.2 | 174.8 | 2.31 | 452.9 | 1,300 sync GETs, 1,027 HEADs | 0.0 | 122.3 | n/a |
| pack (after), 50 Mbit/s, 20 ms, cold | 113.7 | 77.4 | 33.73 | 188.1 | 12 pack GETs, 0 sync GETs | 33.2 | 33.2 | 0 of 4,023 |
| pack (after), 50 Mbit/s, 20 ms, warm | 44.0 | 39.7 | 3.35 | 1.7 | 0 pack GETs, 0 sync GETs | 3.1 | 3.1 | 4,023 of 4,023 |

Spread of the localhost engine times: manifest 69.7 to 87.4 s (three runs); pack cold 37.4 to 38.8 s; pack warm
39.9 to 45.6 s. Medians are the rows above. The first line of the manifest run's stamp (0.20 s) is the first
logcat line, not the first instruction: `onRunStart` is not an emscripten hook in this SDK (it is not in
`emsdk/upstream/emscripten/src`), so the first-instruction stamp is not measured. The first log line is the nearest
point. "Blocked on fetch" is the
time before the engine starts plus the time in synchronous requests; for the manifest it is the synchronous
requests alone.

What the rows show:

- **Bytes.** The manifest path moved 452.9 MB to phase 100: 1,312 GETs (1,300 for the engine's files and 12 for
  the page), and 1,027 HEADs that carry no body. The pack path moved 188.1 MB cold: 186.3 MB of boot packs and
  1.7 MB of page, JavaScript, wasm and the v2 manifest. Warm, it moved 1.7 MB. The v2 manifest is 1.2 MB, sent on
  every visit by the dev server (`no-store`); a deployed site can cache it by content, which would leave the warm
  visit near zero.
- **Where the local engine gain comes from.** The engine ran 75.8 s before and 38.3 s after, with about the same
  work (4.61 to 4.72 G instructions in the pack runs, 4.62 to 4.75 G in the manifest runs). A trace run of each path
  shows the time inside `FS.read`: 30.5 s for the manifest path (the trace run of section 1), 2.3 s for the pack
  path (the trace run of this batch), with the same number of calls (19,976). That is about 28 s of the 37.5 s gain.
  The rest is the synchronous waits (3 to 7 s in the manifest runs) and run-to-run spread. So most of the localhost
  gain is the read path, not the prefetch. The cost was emscripten's per-byte copy in the lazy-file read, and the
  chunk store copies whole chunks.
- **The cost of waiting for packs.** The engine starts after the packs. On localhost that is 2.6 s; on
  50 Mbit/s it is 33.7 s, against 2.3 s for the manifest path, which starts at once and then waits on each file. The
  manifest path still ends later on the throttled link (201.2 s against 113.7 s wall), because its 1,027 HEADs and
  1,300 GETs each wait a round trip and the 452 MB shares the same link as the packs' 186 MB.
- **Warm.** On localhost a warm visit is not faster than a cold one (45.6 s engine against 38.3 s, the same
  instruction count). The pack is decoded and hashed again on each visit, and the cache saves only the network.
  The cache's value is on a slow link: 44.0 s wall warm against 113.7 s cold at 50 Mbit/s. I did not find the cause
  of the local warm engine time; it is not explained by the work done.
- **Unexplained: the throttled cold engine time.** The throttled cold run's engine time (77.4 s) is about twice
  its local counterpart, with the same instruction count (4.623 G in both), and the throttled warm run's engine time
  (39.7 s) is normal. The network is idle while the engine runs, so the slowdown is not a request in flight. The
  candidates are guest timeouts that run on host time, and host contention; I did not isolate either.
- **Fallback.** With `?nopack`, no pack is loaded and every chunk is read synchronously: 4,023 GETs, 10.6 s blocked
  (7.3 s of that is the JavaScript inflater), phase 100 at 76.7 s wall. The fallback works, and it is slower than
  the packs, which is why the packs are loaded first.

**Tests.** `make test-web` (the ART cases) passes. `XIAOJI_WEB_BOOT=1 make test-web` passes: the chunk check (all
10,978 chunks hash, all 3,510 files reassemble), the ART cases, and the boot to phase 100 in pack mode with 12 packs
and 0 synchronous fetches. `node tests/chunks.mjs` on a copy with one corrupted chunk fails with the chunk's name
and the file that depends on it.

## 5. Known limits

- The trace measures the bytes the engine reads and maps, not the bytes the guest touches (section 1).
- One Mac, headless Chromium, and few runs. The localhost spreads are wide (section 4), and the throttled rows are
  single runs. Treat them as a comparison on one machine, not as benchmark numbers.
- Throttling is a model (`web/serve.mjs`, `throttle`): each response waits one RTT, and all bodies share one
  bandwidth bucket. It does not model TCP slow start, connection reuse or a real route.
- The boot pack is fetched in full before the engine starts. On a slow link that delays the first instruction by
  the pack's download time (33.7 s at 50 Mbit/s here); the manifest path starts at once and waits on each file.
- The boot order comes from one boot to phase 100. The boot runs on after phase 100, and in some warm runs one
  1 KB chunk outside the order was read through the fallback after phase 100. Regenerating the order from a longer
  trace would remove it.
- A fallback chunk is not cached (the read is synchronous), and the fallback decodes with the JavaScript inflater.
- The chunk cache holds the packed chunks of this build. A changed chunk costs its pack, not the whole boot.
- The default page is the pack path. `?boot=manifest` is the old path, kept until the pack path has run for a
  while. The boot test (`XIAOJI_WEB_BOOT=1`) runs the pack path only.
- Deployment is not written: there is no `_headers` file or deploy script for Cloudflare Pages. The dev server sets
  the immutable cache headers on `/xj/chunks/` and `/xj/packs/`, the layout a deploy would use.
- The v2 manifest is 1.2 MB and is fetched on every visit (section 4).
