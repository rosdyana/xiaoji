# Loading the Boot Android page (W1 follow-up)

The Boot Android preview (`docs/web-preview.md`) fetched the product tree lazily, one synchronous request per
file and one HEAD per file, and the engine waited for each. This note measures what the boot reads, designs the
delivery layer, and reports what tier 1 gains. Tier 2 is design only.

## 1. Measurements

### Setup

- Product tree `build/rootfs-xiaoji`: 3,510 files, 1,327.2 MiB (1.39 GB). Boot: `xiaoji.rc`, `--logcat`, phase 100.
- Headless Chromium through `tools/webboot-bench.mjs` (Playwright from the xiaonux bench), a fresh profile per
  cold run, the same machine for every row.
- Power: a first trace run on battery with Low Power Mode on took 116 s of engine time, the same as
  `docs/web-preview.md`; its trace file was overwritten by the next run. The working set below is from a second
  trace run, on AC with Low Power Mode off (66 s engine). Every timing in section 4 is from that state, recorded by
  `pmset -g` at the start and end of the batches. The battery run took 116 s against 66 s on AC, so no number here is
  compared with a run from the other state.
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
restored. xiaonux saves the whole VM in `emu/tinyemu/xsnap.h`: a symmetric serializer (the same code saves and
loads, fixed-width little-endian fields, magic `XNXSNAP1`). `docs/phase-2.md` describes what it holds: CPU and CSRs,
the interrupt controllers, the virtio queues, and RAM with zero pages skipped and compressed (a 256 MB guest gave a
16.2 MB snapshot, 68.5 MB raw, in `docs/benchmarks.md`). For xiaoji the state is larger
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
engine change, and it is out of scope for tier 1. Done in L2 (section 6.1).

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

Headless Chromium on the M2 MacBook, AC power with Low Power Mode off (`pmset -g` before and after the batches).
Each cold run has a fresh profile. Warm runs reuse the profile of one cold run on the same port, because Cache
Storage is per origin. Every number is taken at phase 100: the wire tally and the stats are read there, since the
boot runs on until Stop. Medians are over the runs listed in the `n` column; the ranges show the spread. The
throttled rows use the model in section 5.

| Config | n | Wall to phase 100, median (s) | Engine, median (s) | Engine range (s) | First log line, median (s) | Wire to phase 100 (MB) | Requests | Packs before engine, median (s) | Blocked, median (s) | Chunk cache hits |
|---|---|---|---|---|---|---|---|---|---|---|
| manifest (before), localhost | 6 | 74.1 | 67.9 | 63.1 to 87.4 | 0.19 | 452.9 | 1,300 sync GETs, 1,027 HEADs | 0.0 | 3.5 | n/a |
| pack (after), localhost, cold | 9 | 41.0 | 37.4 | 36.9 to 38.8 | 2.74 | 188.1 | 12 pack GETs, 0 sync GETs | 2.7 | 2.7 | 0 of 4,023 |
| pack (after), localhost, warm | 8 | 42.0 | 38.6 | 36.4 to 46.1 | 2.26 | 1.7 | 0 pack GETs, 0 sync GETs | 2.2 | 2.2 | 4,023 of 4,023 |
| manifest (before), 50 Mbit/s, 20 ms RTT | 3 | 200.4 | 172.7 | 171.6 to 174.8 | 2.37 | 452.9 | 1,300 sync GETs, 1,027 HEADs | 0.0 | 124.1 | n/a |
| pack (after), 50 Mbit/s, 20 ms, cold | 3 | 72.2 | 40.1 | 36.3 to 77.4 | 31.19 | 188.1 | 12 pack GETs, 0 sync GETs | 30.9 | 30.9 | 0 of 4,023 |
| pack (after), 50 Mbit/s, 20 ms, warm | 1 | 44.0 | 39.7 | 39.7 | 3.35 | 1.7 | 0 pack GETs, 0 sync GETs | 3.1 | 3.1 | 4,023 of 4,023 |

The localhost manifest runs are three from the first session and three from the batch that also has the byte
tally, so the baseline's spread is wide (63 to 87 s engine). The pack rows pool the runs of both sessions; the
warm row includes one run of a shifted-pack check (below). "Blocked" is the time before the engine starts plus the
time in synchronous requests. "First log line" is the first logcat line, which is the nearest point this build
gives: `onRunStart` is not an emscripten hook in this SDK (it is not in `emsdk/upstream/emscripten/src`), so the
first-instruction stamp is not measured.

What the rows show:

- **Bytes.** The manifest path moved 452.9 MB to phase 100: 1,300 GETs for the engine's files and 12 for the page,
  and 1,027 HEADs that carry no body. The pack path moved 188.1 MB cold: 186.3 MB of boot packs and 1.7 MB of page,
  JavaScript, wasm and the v2 manifest. Warm, it moved 1.7 MB. The v2 manifest is 1.2 MB and the dev server sends
  it on every visit (`no-store`), so it is most of the warm figure. A deployed site can cache it by content.
- **Where the local engine gain comes from.** The engine ran 67.9 s before and 37.4 s after (medians), with about
  the same work: 4.61 to 4.72 G instructions in the pack runs, 4.62 to 4.76 G in the manifest runs. A trace run of
  each path shows the time inside `FS.read`: 30.5 s for the manifest path (the trace run of section 1), 2.3 s for
  the pack path (a trace run of this batch), with the same number of calls (19,976). That is about 28 s of the
  30 s gain. The rest is the synchronous waits and run-to-run spread. So most of the localhost gain is the read path,
  not the prefetch. The cost was emscripten's per-byte copy in the lazy-file read; the chunk store copies whole
  chunks.
- **The cost of waiting for packs.** The engine starts after the packs. On localhost that is 2.7 s. At 50 Mbit/s it is
  31 s (median), against 2.4 s for the manifest path, which starts at once and then waits on each file. The manifest
  path still ends later on the throttled link (200.4 s against 72.2 s wall, medians), because its 1,027 HEADs and
  1,300 GETs each wait a round trip and the 452 MB shares the link with the packs' 186 MB.
- **Warm.** On localhost a warm visit is not faster in the engine (38.6 s median, against 37.4 s cold; the ranges
  overlap). The packs are decoded and hashed again on each visit, and the cache saves the network. The cache's value
  is on a slow link: 44.0 s wall warm against 72.2 s cold at 50 Mbit/s (one warm run). I did not find a warm engine
  gain, and the eight warm runs do not show one.
- **The throttled cold outlier.** One throttled cold run (`run2/thr-pack-cold`) took 77.4 s of engine time; two
  more runs took 40.1 s and 36.3 s. Its milestones have the same instruction counts as a local run
  (4.623 G in both), and every interval took about twice as long as the local one, from the first log line on. The
  network is idle while the engine runs, so a uniform slowdown at fixed work points to the host, not to a guest wait.
  Another process was running on the same Mac during that batch: a native engine test run from the main checkout,
  which I saw in `ps` at the end of the work. I did not start it, and I did not check whether it overlapped that run.
  The median of three is the number to use.
- **Fallback.** With `?nopack`, no pack is loaded and every chunk is read synchronously: 4,023 GETs, 10.6 s blocked,
  of which 7.3 s is the JavaScript inflater, and phase 100 at 76.7 s wall. The fallback works, and it is slower than
  the packs, which is why the packs are loaded first.
- **Shifted packs.** The packer was run with 8 MiB packs instead of 16 MiB, so all 23 packs had new boundaries and
  new names, with the same 10,978 chunks. A warm run on the profile that held the 16 MiB packs' chunks downloaded
  no packs (`packDownloads: 0`), served all 4,023 chunks from the cache, made no synchronous fetch, and moved 1.7 MB.
  The per-chunk cache therefore survives a change of pack boundaries, which is the case a changed chunk causes.
  The packs were then rebuilt at 16 MiB.
- **Order file.** The committed `image/boot-order.json` was first written by a one-line extraction from the trace.
  `tools/workingset.py --order-out` was then run on the same trace, and its output is identical to the committed
  file (7,366 entries).

**Tests.** `make test-web` (the ART cases) passes. `XIAOJI_WEB_BOOT=1 make test-web` passes: the chunk check (all
10,978 chunks hash, all 3,510 files reassemble), the ART cases, and the boot to phase 100 in pack mode with 12 packs
and 0 synchronous fetches. `node tests/chunks.mjs` on a copy with one corrupted chunk fails with the chunk's name
and the file that depends on it.

**Where the data is.** The raw run JSON, the trace and the working-set report are not committed. They are in the
session scratch directory `/private/tmp/claude-501/-Users-rosdyanakusuma-Ross-projects-xiaonux/02218bd1-8168-4b28-a0fa-30c789f8f133/scratchpad/loading/`
(`before-trace.json`, `workingset.json`, `run2/`, `run3/`), which a reboot can clear. The timing tool is
`tools/webboot-bench.mjs`, not `tests/web.mjs`.

## 5. Known limits

- The trace measures the bytes the engine reads and maps, not the bytes the guest touches (section 1).
- One Mac, headless Chromium, and few runs. The localhost spreads are wide (section 4), and the throttled rows are
  single runs. Treat them as a comparison on one machine, not as benchmark numbers.
- Throttling is a model (`web/serve.mjs`, `throttle`): each response waits one RTT, and all bodies share one
  bandwidth bucket. It does not model TCP slow start, connection reuse or a real route.
- The boot pack is fetched in full before the engine starts. On a slow link that delays the first instruction by
  the pack's download time (33.7 s at 50 Mbit/s here); the manifest path starts at once and waits on each file.
  (Superseded by L2, section 6.4: the engine starts after the first pack.)
- The boot order comes from one boot to phase 100. The boot runs on after phase 100, and in some warm runs one
  1 KB chunk outside the order was read through the fallback after phase 100. Regenerating the order from a longer
  trace would remove it. (Section 6.3 takes the order from four lazy-mmap traces; a few chunks are still read
  synchronously on some runs.)
- A fallback chunk is not cached (the read is synchronous), and the fallback decodes with the JavaScript inflater.
- The chunk cache holds the packed chunks of this build. A changed chunk costs its pack, not the whole boot.
- The default page is the pack path. `?boot=manifest` is the old path, kept until the pack path has run for a
  while. The boot test (`XIAOJI_WEB_BOOT=1`) runs the pack path only.
- Deployment is not written: there is no `_headers` file or deploy script for Cloudflare Pages. The dev server sets
  the immutable cache headers on `/xj/chunks/` and `/xj/packs/`, the layout a deploy would use.
- The v2 manifest is 1.2 MB and is fetched on every visit (section 4).

## 6. L2: lazy mmap, repack, early start

L2 makes three changes: the engine fills a file mapping when the guest touches it (instead of at `mmap`), the boot
packs are rebuilt from what the lazy boot touches, and the page starts the engine after the first pack, while the
rest of the packs download in a second worker. Each is measured on its own below. The numbers are on one Mac, and
most of the runs were disturbed by another agent's engine runs, so section 6.5 gives the counts and what was
excluded.

### 6.1 The switch and the knobs

- Native: `--no-lazy-mmap` (or `XIAOJI_LAZY=0`) fills every file mapping at `mmap` time. The eager arm goes through
  the same fill (`mem_map_lazy` fills every window when the switch is off), so both arms run the same binary.
  `--lazy-window BYTES` (or `XIAOJI_LAZY_WINDOW`) sets the fill window: a power of two from 4 KiB to 1 MiB. The
  default is 64 KiB, aligned in the file.
- Counters: `--stats` prints the lazy file reads and their bytes after the instruction line. `--stats-at TEXT`
  (or `XIAOJI_STATS_AT`) prints the instructions, time and reads at the first log line that contains TEXT; the A/B
  runs stop at `Starting phase 100` this way, because the engine runs on after it.
- Page: `?lazy=0` (the eager arm), `?lazywin=BYTES`, `?statsat` (the engine's phase 100 line in `window.xb.statsAt`),
  and `?early=MB` (section 6.4).
- Lazy mmap is on by default: `build/xiaoji` and the wasm build both have it.

### 6.2 Does lazy mmap slow the engine?

Interleaved A/B to phase 100, after one discarded warm-up run per platform. The native arms are 3 runs each, and a run
that overlapped another engine run is excluded (the runs were checked during each run). The browser arms are 3 runs
each, checked before each run only, so their overlap is unverified (see 6.5). Power was AC with Low Power Mode off for
every timed run of both.

| Platform | Lazy: engine to phase 100 (s) | Eager: engine to phase 100 (s) | Engine reads to phase 100, lazy | Engine reads, eager | Instructions (G) |
|---|---|---|---|---|---|
| Native (`build/xiaoji`) | 45.58, 45.71, 45.87 (median 45.71) | 45.37, 45.86, 46.07 (median 45.86) | 375.6 MB (11,995 reads) | 732.8 MB (18,466 reads) | 4.745 to 4.750 (lazy), 4.722 to 4.749 (eager) |
| Headless Chrome 157 (wasm, the page) | 38.60, 40.38, 40.63 (median 40.38) | 39.61, 40.88, 43.30 (median 40.88) | 375.8 to 376.2 MB | 732.8 MB | 4.67 to 4.68 (both) |

The ranges overlap on both platforms, and the medians differ by 0.15 s (native) and 0.5 s (browser), with lazy the
lower of the two. Neither arm is slower by a margin that separates the ranges, so no profiling and no window tuning
were done. Lazy reads 49% fewer bytes through the engine's reads (375.6 MB against 732.8 MB, native).

The first browser batch ran while the Playwright browser bundle was missing from the machine. Those runs were
discarded. The batch used for the table above ran Google Chrome 157 through `XN_CHROME` (see 6.5), and it checked
for foreign benches before each run but not during a run.

### 6.3 Repack

The boot order is now the union of four lazy-mmap traces (`tools/workingset.py` takes several traces and writes the
union of their first-read orders; the report is for the first trace). Three traces missed five 64 KiB blocks that a
fourth boot read (libpdfium, libpcre2, liblzma and libprofile), so the union has four. This is one more trace than the
two or three in the plan.

| | Before (`image/boot-order.json`, eager) | After (lazy, 4-trace union) |
|---|---|---|
| Order entries (64 KiB blocks) | 7,366 | 4,322 |
| Boot packs | 12 | 7 |
| Chunks in packs | 4,023 | 2,709 |
| Pack bytes, compressed | 186.33 MB (177.7 MiB) | 102.75 MB (97.99 MiB) |
| Pack bytes, raw (chunk sizes summed) | 440.25 MB (419.9 MiB) | 273.79 MB (261.1 MiB) |

The packs are built with `tools/pack.py` at 128 KiB chunks, as before. `node tests/chunks.mjs` passes (all 10,978
chunks hash to their names, all 3,510 files reassemble to 1,327.2 MiB). The deployed file count is 10,986, under the
20,000 limit.

Synchronous GETs at phase 100 in the boots made with the new packs: 2, 0 and 3 with the three-trace union, and 1,
0, 5 and 1 with the four-trace union (the last of them in the web boot test). So the criterion of zero in two boots
was not met. The chunks that are fetched synchronously are in no pack; the boot reads them on some runs and not on
others, and the fallback path serves them. The clean measured runs show 0 to 3 synchronous GETs per boot.

### 6.4 Early start

- `web/packloader.js` is a module worker. It fetches the v2 manifest, computes the layout of the packs
  (`layoutPacks` in `web/chunks.js`, the same function in both workers), and creates one `SharedArrayBuffer` for
  the raw bytes of all pack chunks, and an `Int32Array` with one control word per pack (0 pending, 1 ready, 2 failed).
  It then downloads the packs in order, from the chunk cache when it can, decodes and hashes each chunk, writes it
  to the buffer, and sets the pack's control word and calls `Atomics.notify`.
- The page creates the downloader only when `crossOriginIsolated` is true, and relays the layout to the engine's
  worker. The engine's worker starts once pack 0 (or the packs covering `?early=MB`) is ready (`Atomics.waitAsync`).
  A read of a chunk in a later pack blocks in `Atomics.wait` until that pack is ready, not until the chunk is
  requested again. A chunk in no pack, or in a failed pack, is a synchronous request (the fallback).
- The page server sends COOP and COEP by default (`web/serve.mjs`, `--no-coop` turns them off). The plan assumed the
  page was already cross-origin isolated; it was not, because the engine did not use `SharedArrayBuffer` before this
  change. Without isolation the page reads every chunk synchronously (`?nopack` behaves the same way).
- The boot screen keeps the packs' progress in its detail line during the boot (`packs X of Y MB`). The download
  phase now ends after the first pack, so the test that photographed a download at 25 to 75% now photographs the
  detail line while packs are still arriving.

Tests: `XIAOJI_WEB_BOOT=1 make test-web` passes, including the boot to phase 100 in pack mode, the warm visit with no
pack downloads, and the download screen check (updated as above).

### 6.5 Measurements and what was excluded

Setup: `tools/webboot-bench.mjs` with headless Google Chrome 157 (`XN_CHROME`), a fresh profile for every cold run,
and the warm run on the same port and profile as its cold run. "Before" is `main` at c4951f8, built in a
`git archive` copy: its default page, the pack path with eager engine reads, the 7,366-entry order and 12 packs. It
ran with `--no-coop`, which is the server it was built for. "After" is this branch, with COOP and COEP on (the
default). Throttled rows use the model in `web/serve.mjs` (50 Mbit/s, one 20 ms RTT per response, one shared
bandwidth bucket).

Power. The `pmset -g` record at the start of the batch was taken on battery with Low Power Mode on (`lowpowermode 1`),
and the end record was taken on AC with Low Power Mode off, so the per-run records are the evidence: each timed run
wrote `pmset -g batt` and `lowpowermode` at its start, and every run that is used here shows AC power and Low Power
Mode off.

Other load. The other agent's engine ran for much of the batch. A background sampler logged every engine and bench
process whose working directory was not this worktree, every 2 s, from 00:44 on. A run whose window overlaps any sample
is excluded. Boots before 00:44 were checked only before they started, not during. One boot (`after-thr-1-cold`) is
also excluded: another engine run started just after it ended, and its engine time less its waits is an outlier.
Of 40 boots, 15 are used and 25 are excluded. The used runs per cell are: before, localhost, cold 2 and warm 2;
before, 50 Mbit/s, cold 2 and warm 1; after, localhost, cold 2 and warm 2; after, 50 Mbit/s, cold 2 and warm 2. The
plan asked for 3 per cell; the machine was not quiet enough to get them.

Used runs only, medians with the ranges in brackets:

| Build | Network | Cache | n | Wall to phase 100 (s) | Engine to phase 100 (s) | First log line (s) | Wire to phase 100 (MB) | Requests | Before engine start (s) | Waits in run (s) | Pack GETs | Cache hits | Sync GETs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| before | localhost | cold | 2 | 43.36 (41.72 to 44.99) | 39.04 (37.96 to 40.13) | 2.95 (2.45 to 3.45) | 190.4 (190.4 to 190.4) | 92 (92 to 93) | 2.83 (2.41 to 3.26) | 0.15 (0.12 to 0.18) | 12 (12 to 12) | 0 (0 to 0) | 65 (65 to 65) |
| before | localhost | warm | 2 | 42.31 (41.87 to 42.75) | 38.06 (37.69 to 38.42) | 2.99 (2.95 to 3.04) | 1.8 (1.8 to 1.8) | 15 (15 to 15) | 2.94 (2.90 to 2.97) | 0.07 (0.06 to 0.08) | 0 (0 to 0) | 4023 (4023 to 4023) | 65 (65 to 65) |
| before | 50 Mbit/s, 20 ms | cold | 2 | 73.66 (73.16 to 74.15) | 39.76 (39.20 to 40.32) | 31.07 (31.03 to 31.11) | 190.4 (190.4 to 190.4) | 92 (92 to 92) | 31.00 (30.97 to 31.03) | 1.64 (1.56 to 1.73) | 12 (12 to 12) | 0 (0 to 0) | 65 (65 to 65) |
| before | 50 Mbit/s, 20 ms | warm | 1 | 43.13 (n=1) | 38.70 (n=1) | 3.17 (n=1) | 1.8 (n=1) | 16 (n=1) | 3.13 (n=1) | 0.06 (n=1) | 0 (n=1) | 4023 (n=1) | 65 (n=1) |
| after | localhost | cold | 2 | 40.70 (39.64 to 41.76) | 39.02 (37.97 to 40.07) | 0.45 (0.45 to 0.45) | 105.1 (104.6 to 105.7) | 37 (27 to 47) | 0.39 (0.39 to 0.39) | 0.01 (0.00 to 0.01) | 7 (7 to 7) | 0 (0 to 0) | 2 (1 to 3) |
| after | localhost | warm | 2 | 44.16 (40.57 to 47.76) | 41.75 (38.14 to 45.37) | 1.17 (1.16 to 1.19) | 1.8 (1.7 to 1.9) | 19 (18 to 20) | 1.11 (1.09 to 1.14) | 0.01 (0.00 to 0.01) | 0 (0 to 0) | 2709 (2709 to 2709) | 1 (0 to 2) |
| after | 50 Mbit/s, 20 ms | cold | 2 | 58.51 (57.43 to 59.59) | 53.68 (52.59 to 54.77) | 3.72 (3.72 to 3.73) | 104.7 (104.6 to 104.7) | 28 (28 to 29) | 3.68 (3.67 to 3.69) | 15.05 (14.36 to 15.73) | 7 (7 to 7) | 0 (0 to 0) | 0 (0 to 1) |
| after | 50 Mbit/s, 20 ms | warm | 2 | 40.92 (40.71 to 41.13) | 38.33 (38.07 to 38.59) | 1.37 (1.36 to 1.38) | 1.9 (1.9 to 1.9) | 21 (21 to 21) | 1.32 (1.32 to 1.33) | 0.04 (0.00 to 0.08) | 0 (0 to 0) | 2709 (2709 to 2709) | 2 (0 to 3) |

What the used runs show:

- **Bytes and requests.** Cold, the wire moved 190.4 MB (before) and 104.7 to 105.1 MB (after) to phase 100. The
  difference is the packs: 186.3 MB before and 102.7 MB after. Warm, both moved about 1.8 MB, most of it the 1.2 MB
  v2 manifest, which the dev server sends with `no-store`.
- **First log line.** Cold on the throttled link it fell from 31.1 s to 3.7 s. On localhost it fell from 2.9 s to 0.45 s.
- **Time to phase 100, wall.** Cold on the throttled link: 73.7 s before, 58.5 s after (a 21% cut). Localhost cold is
  about the same (43.4 s, 40.7 s). Warm is about the same (42.3 s, 44.2 s).
- **Engine time.** On the throttled link the engine's own time rose, from 39.8 s to 53.7 s cold, because the engine now
  waits for later packs inside its run (15.1 s in the median). The wall clock still falls, because the engine starts
  while the packs arrive. On localhost the cold engine time is the same (39.0 s on both), and the warm engine time is
  higher after (41.8 s against 38.1 s, with overlapping ranges and one 45.4 s run). I did not find the cause of the
  warm difference. The candidates are the copy from the shared buffer into the wasm heap (`TypedArray.set` from a
  `SharedArrayBuffer` view, which may take a slower path) and the 2,709 cache hits on each warm visit. Neither is
  tested. With two used warm runs per arm this is a lead, not a result.
- **Blocked time.** The engine's blocked time is the time before its start (the module or the first pack, whichever
  is later) plus its waits (in-run waits on packs and synchronous requests). The cold throttled runs start the engine
  at 3.7 s, and then wait 15.1 s in the median inside the run.
- **Instructions.** Used runs show 4.666 to 4.676 G to phase 100 in every cell except one before run at 4.765 G, so
  the work is the same.
- **Functional checks, not timed.** `?nopack` and `--no-coop` each reached phase 100 once, with the synchronous path:
  2,698 synchronous GETs and no pack download, and 2,697 synchronous GETs with no pack store. On localhost the two
  took 43.8 s to phase 100 each. Their cost on a throttled link is not measured. Before this change, a host without
  cross-origin isolation still preloaded packs (main has no `SharedArrayBuffer` path), so such a host now reads every
  chunk synchronously. That is a regression for it, and the page does not warn about the cost beyond one log line.

Limits: 2 to 3 used runs per cell; one Mac; headless Chrome 157 (the earlier sections used Playwright's headless
shell, which was missing from the machine when these runs were made, so section 4 and this section are not directly
comparable); the throttle is a model. The warm engine difference is open.

Browser lazy/eager A/B (section 6.2). The table in 6.2 uses the batch made with the packs of main's order (the
eager-trace packs), where both arms have the chunks they read. It ran with a busy check before each run and no
sampler, so it is unverified for overlap. A later batch with the lazy-trace packs (this branch's order) is not a
valid comparison: the eager arm reads chunks that those packs do not hold, so it makes about 1,400 synchronous GETs
and its engine time includes them. That batch is reported only as a limit of the repack.

### 6.6 Open items

- The boot still makes 0 to 4 synchronous fetches per boot. A fuller trace set (more boots, with the contaminated ones
  removed) would shrink that, and the fallback path is what covers it.
- The warm engine difference on localhost (section 6.5) is unexplained.
- Cross-origin isolation is needed for the pack path, and no deployment writes the headers yet (section 5). Without
  them the page reads synchronously.
