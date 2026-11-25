# Booted snapshots (S1 to S5)

Goal: a visitor restores Android at the home screen in seconds (plus the download) instead of booting it (about 6.5 min
in the browser, 285 s natively). This note is the design (S1), then what each stage built and measured.

Status: see the end of each section. Measurements are filled in as the stages land.

## S1. Design

### The idea: an image of the engine, not a serialization of Android

The engine is one host thread (`engine/sched.c`): every guest thread is a green thread, a blocked thread is a state in
its `thread_t` (futex key, wait key, deadline, a syscall rewound to its `svc`), never a host stack. So at a slice
boundary of the scheduler loop the engine's whole state is data in its own memory: processes, threads and their
registers, page tables and guest pages, fd tables and the objects behind them, binder, properties, logd, timers, the
GL decoders. A snapshot is an **image of that memory**, plus the few things that are outside it.

The other way, serializing every module's objects (35 engine files, with pointer graphs through binder, sockets, epoll,
memfds), was rejected: each module would need a writer and a reader kept in step with it, and a 5-minute boot is the
only full test. The image covers every module by construction, including ones added later. Its price is that an image
restores only into the **same engine build** (the task's format requirement already ties a snapshot to the build), and
that natively the engine must run at fixed addresses (below).

| Build | What the image is | Fixed addresses |
|---|---|---|
| native (`build/xiaoji`) | the executable's `__DATA` `__data`, `__bss`, `__common` (Linux: `__data_start` to `_end`) and the heap arena | ASLR off (`engine/aslr.c` re-executes the engine: macOS `posix_spawn` SETEXEC with the private DISABLE_ASLR flag, as lldb uses; Linux `personality(ADDR_NO_RANDOMIZE)`); every engine allocation comes from an arena at `0x200000000000` (`engine/arena.c`, dlmalloc on a fixed reservation) |
| Node, web (wasm) | linear memory from 1024 (static data) to the stack, and from the stack's top to the break; not the stack | linear memory has no randomization |

Native details:

- `engine/arena.c` defines `malloc`, `free` and the rest for the executable. dlmalloc (`third_party/dlmalloc`, public
  domain) runs on two reservations: a small-block heap grown by MORECORE, and a large-block region for blocks of 256 KiB
  and more (page tables, page-pool slabs), whose freed blocks become fresh zero pages. dlmalloc's own state is in the
  executable's bss, so it is in the image with the arena.
- The C library's own allocations (`opendir`, `fopen`) stay in the system allocator, so nothing of the C library may
  live across a snapshot point: `vfs.c` drops a directory stream from before a restore (`vfs_dir_gen`) and opens it
  again at its position; exit handlers are a list in engine memory (`snap_atexit`), not the C library's.
- Not saved: the GOT and anything else of the libraries, which differ from boot to boot of the host. A debug check,
  `XIAOJI_SNAP_CHECK=1`, scans every word of the image outside the page pool for pointers into a library, the system
  allocator or the host stack (S2 results below).

### The snapshot point

The top of the scheduler loop (`run_loop`, `snap_point`): no guest instruction is half done, every thread is runnable
(its registers in its `cpu_t`) or in a wait the image holds, no host call is in progress. The conditions
(`--snapshot-at`) only set a flag when they hold: `time:S`, `icount:N`, `log:TEXT` (the first guest log line containing
TEXT, seen by logd whether logcat prints or not), `prop:NAME=VALUE`; `--snapshot-after S` waits S more seconds. For a
booted snapshot the condition is the launcher's HOME start (`log:cmp=com.android.launcher3/`) plus an idle period. The
loop's idle sleeps end at a time condition, so a snapshot can be taken while every thread is blocked. Writing the
snapshot ends the run (exit 0): the run that writes it is not continued, which keeps the "after" side simple (the exit
handlers flush caches to the host files after the image was taken, and the restore puts the trees back anyway).

### What is captured, and how

| State | How |
|---|---|
| Guest memory | In the image. Host pages of guest memory and memfds come from a page pool (`mem_page_alloc`, slabs of 256 pages); free pool pages are zeroed before the image is taken, so they cost nothing. Copy-on-write sharing between zygote children is the share table, which is in the image: shared pages stay shared. Clean file-backed pages: see "Size" below |
| Threads, CPU state | In the image (`thread_t`, `cpu_t`). The TLBs and fetch cache are emptied on restore (`mem_tlb_reset`); the exclusive monitor is cleared at every slice start anyway |
| JIT (wasm) | Translated blocks live in wasm functions of the instance's table, which a new instance does not have: `jit_snap_reset` drops every block, page entry, queued pc and chain entry, and the table base is taken again. Code is retranslated as it runs |
| Processes, fd tables; files, pipes, sockets, epoll, eventfd, timerfd, inotify; binder procs, nodes, refs, transactions; ashmem, memfd, the shared file cache; properties; logd; POSIX and interval timers; cgroups; mount namespaces; evdev queues; goldfish pipe, address space, sync; GL decoders and the null GL backend | In the image, with no code of their own |
| Host descriptors (files the engine opened on the host: guest files, mapped files) | `host.c` keeps a record per descriptor (path, kept current across `rename`, and open flags). At a snapshot each record gets its offset; a file whose path no longer names it (unlinked, replaced) gets its bytes kept in the record (in the image). A restore opens each path at the same descriptor number and offset (`dup2`), or an unlinked copy of the kept bytes. Descriptors 0 to 2 are the new run's |
| Directory streams | `DIR *` of the C library: a stream from before a restore is opened again and moved to its position (`dread`, the entries consumed) |
| Writable trees (`/data`, `/cache`, `/metadata` under `--root`) | Host files, not memory: walked into the snapshot file (records: kind, mode, path, bytes). A restore empties those trees under the root and writes them back, so a second restore of the same snapshot starts from the same files. The guest-visible modes and owners are the metadata table (`meta.c`), which is in the image. Without `--root` nothing is captured or touched |
| tmpfs mounts (`/data_mirror`) | `mntns.c` backs a tmpfs with a host directory under `/tmp`: each live one is recorded by its host path, and put back (emptied first) by a restore |
| Directories and links init's rc made outside the trees (`/mnt/...`) | On the native host they are still there; a page's tree is the manifest's only, so a restore makes the missing ones again from init's boot actions (in the image) in init's mount namespace (`init_snap_replay`); the mounts themselves are in the image |
| Clocks | The engine's monotonic clock is the host's plus an offset (`host_snap_clock`): a restore sets it so the clock goes on from the snapshot's time. Every deadline the image holds (futex and sleep timeouts, timerfds, alarms, init's timers) stays where it was relative to the guest. CLOCK_REALTIME follows the host (a restored device has "slept" since the snapshot). This run's own timers (`--init-timeout`, `--dump-at`, `--stats` start) count from the restore |
| This run's options | Applied again after the image is in (`apply_options` in `main.c`): logcat, stats, lazy window, faults, dump times, the input script (its times count from the restore), a new snapshot condition |
| Signal handlers of the host | Installed again (init's SIGINT, the debug SIGUSR1) |
| GL host state (WebGL backend) | See "GL" below |

### GL

WebGL objects are JavaScript objects; they cannot be in the image. The WebGL backend (`gl_webgl.c`) uses guest names
that are emscripten GL names (`GL.textures[name]` and so on), and its own state (contexts, surfaces, color buffers,
EGLImages, aliases, per-context bindings) is C state in the image. So a snapshot of GL is the set of live WebGL objects
and their contents, and a restore makes them again **under the same names**:

- At the snapshot (JS, in the worker): for each table of emscripten's GL (`textures`, `buffers`, `framebuffers`,
  `renderbuffers`, `programs`, `shaders`, `vaos`, `samplers`), each live object: textures with their level sizes and
  formats (recorded by wrapping the context's `texImage2D`/`texStorage2D`/`copyTexImage2D` when the context is made:
  WebGL2 cannot query them) and their pixels read back through a framebuffer (color-renderable formats); buffers through
  `getBufferSubData`; shaders' sources; programs' attached shaders, the attribute locations of the link, the uniform
  values (`getUniform` for each active uniform), the uniform block bindings, and emscripten's uniform id maps
  (`uniformSizeAndIdsByName`, which depend on the driver's enumeration order, so they are restored as recorded, not
  recomputed); framebuffer attachments; renderbuffer formats and sizes; VAO attribute state and element buffer; sampler
  parameters. The result is a blob in engine memory (malloc'd by the C side), so it is in the image.
- At the restore: a new WebGL2 context on the page's canvas under the same handle, the objects made in an order that
  respects dependencies (shaders, programs and links, textures and their pixels, renderbuffers, framebuffers, buffers,
  VAOs), installed at their old names, and `GL.counter` set past the largest. The per-context state is the backend's
  `gctx_t`, which `apply` binds when a stream switches context; the restore marks no context as applied so the first
  call rebinds everything. Then the last posted color buffer is posted again as the first frame (the launcher at rest
  draws nothing new by itself).
- Depth and stencil contents and transform feedback are not kept (WebGL2 cannot read them back; at a quiet home screen
  they hold nothing the next frame needs). Queries and syncs are not kept (none outlives a frame).
- Null backend: its state is all C (in the image). A null-backend snapshot and a WebGL one share the container format
  but are never interchangeable: they are different builds (native or Node, and web) and a restore refuses another
  build's image. **The shipping snapshot must be written by the web build**, in a browser, because only the WebGL
  backend has the pixels.

### Format and delivery

The file written by `--snapshot-out` (`engine/snap.c`): a 4 KiB header (magic `XJSNAP` version 2, the build id, the
flags, the snapshot's monotonic and real time and instruction count, the image's ranges, the guest root), the writable
trees, the extent table (address and length of each run of non-zero pages), the region table (since S5: the page pool's
slabs and the WebGL blob, so the packer can tell guest pages from the engine's own memory), then the extents' bytes. A
restore zeroes what no extent covers.

The build id is `<flavor>-<hash>-<layout>`: the flavor (native, node, web), a hash of every file the engines are built
from (the Makefile's `XJ_BUILD_ID`), and the address of a static of `snap.c` (a check that the layout is the image's).
A restore refuses any other build's snapshot with a message. The tree: natively (and in Node) the header records a
fingerprint of the guest root (size and modification time of `system/build.prop`, `framework.jar` and `libc.so`), and a
restore under another tree is refused, since dropped clean pages are read again from the root's files. For the page,
the snapshot's manifest records the SHA-256 of the tree manifest: the page restores only when the engine's build id and
the tree it would boot match the snapshot's, and boots otherwise (`restore-refused`, then a normal boot). `tools/pack.py`
removes `snapshot.json` when it repacks the tree.

For the web (S3, S4), the file is packed like the boot packs (docs/loading.md): the image cut into content-addressed,
zlib-compressed chunks named by their hash, listed in a snapshot manifest next to the packs, so they are cached forever
and shared between snapshots that have equal chunks. The page fetches them, inflates them straight into the wasm
memory before `main` runs, and starts the engine with `--restore`. Eager first: every chunk before the restore. Since
S5 the guest pages are lazy: a bit per host page of the page pool says "in the snapshot, not loaded", and the page is
filled on first touch or in the background (S5, below).

### Size

What makes an image big, and what keeps it small:

- Zero pages are not written. Free pool pages are zeroed first.
- Freed heap blocks are not zero (dlmalloc keeps their old bytes). Natively every free chunk is zeroed before the image
  (`arena_zero_free`, dlmalloc's `inspect_all`), and large freed blocks are fresh zero pages anyway. The wasm builds use
  Emscripten's malloc, which has no such walk compiled in; the web census shows 4 MiB free in a 1,960 MiB heap, so it
  does not matter there.
- Clean file-backed pages (library text, the boot image, the dex files, read once) are a copy of a file the page
  already has (the chunk store). Dropping them before the image (the page is read again from its file on next touch, as
  `mem_drop` does) removes them from the snapshot.

## S2. Native engine: `--snapshot-at`, `--restore`

Status: **done.** The native engine writes a snapshot of the product boot at the home screen and restores it in under a
second; Android goes on (the launcher and SystemUI live, logcat continues, an input script opens Settings).

### Options

| Option | Meaning |
|---|---|
| `--snapshot-at COND` | `time:S` (engine seconds, fractions allowed), `icount:N`, `log:TEXT`, `prop:NAME=VALUE` |
| `--snapshot-after S` | S more seconds after COND |
| `--snapshot-out FILE` | the snapshot file; the engine exits 0 after writing it (1 if the write failed) |
| `--restore FILE` | go on from the snapshot; no program and no `--init` (the snapshot has them). This run's `--logcat`, `--stats`, `--debug-faults`, `--dump-at`, `--init-timeout`, `--input-script` (times from the restore) and a new `--snapshot-at` apply |
| `XIAOJI_SNAP_CHECK=1` | (snapshot, macOS) the closure check: words of the image outside the page pool that point into a library, the system allocator or the host stack |
| `XIAOJI_SNAP_VERIFY=1` | (restore, native) every extent is read back from the file and compared with memory |
| `XIAOJI_SNAP_KEEP_CLEAN=1` | (snapshot) keep clean file pages in the image (for comparison) |
| `XJ_NOMINCORE=1` | (snapshot, native) read every page of the arena instead of asking `mincore` which are resident (a debug knob) |

`make snapshot-native` makes `build/snap/native.snap` from a fresh `/data`.

### Tests (`tests/run.sh`, native and Node)

| Case | What |
|---|---|
| `snapwait` (`tests/guest/snapwait.c`), oracle | threads blocked in `read` on a pipe, in `FUTEX_WAIT`, in `epoll_pwait` on an eventfd and a timerfd (armed 1 s), a child process blocked in `read`, the main thread in a 600 ms `nanosleep`; the native Linux run is the expectation |
| `snapwait snapshot` | snapshot at `time:0.3` (every waiter blocked: the loop's idle sleep ends at the snapshot's time), restore with two extra host descriptors open (7 and 8): the two outputs one after the other are the oracle's; the sleep measures 600 ms or a little more on the continued clock, the timer fires once |
| `snapwait snapshot chained` | the restored run takes a second snapshot 0.1 s later; a third run restores that one; the three outputs are the oracle's |
| `snapshot-binder` | under `--root build/rootfs`: servicemanager and a shell; snapshot at `prop:debug.xiaoji.snap=1` + 0.3 s (servicemanager waiting in its binder read, the shell in `sleep`), restore: the client lists the services through binder (`binder-sm.out`) |
| `snapshot-init` | the same under the built-in init (`tests/init/snapshot.rc`): the restore re-enters init's loop, the oneshot finishes and stops init through `xiaoji.exit` (`init-smoke.out`) |

The writable trees of `build/rootfs` are captured and put back by the rootfs cases.

### The product boot (null backend)

`build/xiaoji --logcat --init image/xiaoji.rc --root build/rootfs-xiaoji --snapshot-at log:cmp=com.android.launcher3/
--snapshot-after 60 --snapshot-out ...` after `make reset-data` (AC power, Low Power Mode off). Run 3 is `make
snapshot-native` with the final sources; runs 2 and 3 shared the machine with compiles or a swapping desktop (3.5 to
5.2 GB of swap in use), so their boots were slower.

| | run 1 (no clean-page drop) | run 2 | run 3 (final sources) |
|---|---|---|---|
| launcher HOME `START` | 271.7 s | 374.7 s | 476.2 s |
| snapshot taken | 333.1 s, 34.98 G instructions | 437.8 s, 42.18 G instructions | 539.6 s, 34.89 G instructions |
| processes; threads | 49; 253 futex, 1 sleep, 23 sigwait, 299 restart (0 runnable) | 49; 252 futex, 1 sleep, 23 sigwait, 300 restart | 45; 168 runnable, 213 futex, 1 sleep, 19 sigwait, 106 restart (the slow boot was still busy at HOME + 60 s) |
| page pool | 938 MiB | 941 MiB, of which 477 MiB freed by the clean-page drop | 910 MiB, 469 MiB freed |
| clean file pages dropped | - | 728,291 page-table entries (2,844 MiB of mappings) are the file's bytes; 29,071 differ (kept) | 617,037 dropped, 24,784 kept |
| host descriptors | 1,974 | 1,980; 0 files kept as bytes (none unlinked) | 1,733; none kept as bytes |
| writable trees | 1,304 entries, 4 MiB | 1,311 entries, 4 MiB | 1,393 entries, 3 MiB (with `/data_mirror`) |
| image (non-zero memory) | 1,275 MiB | 803 MiB in 22,151 extents; file 848 MB | 725 MiB in 19,297 extents |
| time to write | - | 2.5 s | 2.8 s |
| packed (`tools/snappack.py`, 256 KiB zlib chunks) | - | 21,622 distinct chunks, **110.8 MiB** in 7 packs | - |

The image is mostly page tables full of "mapped, no page yet" entries (ART's reservations), which compress to almost
nothing, and the anonymous heaps of 49 processes.

Restore (`build/xiaoji --logcat --restore SNAP --input-script tests/input/settings.script --init-timeout 90` or 120):

| | run 2 | run 3 (final sources) |
|---|---|---|
| image read in | **0.43 s** (803 MiB from the page cache) | **0.47 s** (725 MiB) |
| fixups (trees, descriptors, caches) | 0.25 s (1,980 descriptors) | 0.33 s (1,733) |
| peak resident set | 453 MB | 628 MB |
| the input script (a swipe up at 2 s, a tap at display (205, 201) at 22 s) | `START u0 {... cat=[android.intent.category.LAUNCHER] ... cmp=com.android.settings/.Settings bnds=[141,131][270,311]} from uid 10049` 21 s after the restore, then `Displayed com.android.settings/.Settings` | the same `START` 20 s after the restore, then `Displayed` |
| after the restore (90 s, 120 s) | logcat goes on; the launcher (pid 1539) and SystemUI (1378) do not exit; no ANR, no fatal exception, no fatal signal. `com.android.providers.media.module` is killed for "timeout publishing content providers", as in the cold boots (it waits for a codec HAL the product does not have) | the same: launcher (1411) and SystemUI (1250) live, no ANR, fatal exception or fatal signal |

Two bugs the product run found, both fixed with the tests above: an extent could start before its range when two ranges
share a page (the restore then zeroed restored bytes: ranges are now merged and extents clipped), and the clock offset
was set after main applied the options, so the input script's origin was in the old clock (the clock is now set as
soon as the image is in).

### The closure check

`XIAOJI_SNAP_CHECK=1` on the binder case: 16.5 M words scanned, 10 hits: `g_root` (main's pointer to its argv, which a
restore sets again) and 9 heap words whose values are page-table tags (virtual page numbers + 1 of guest addresses),
which happen to fall in the range of the shared libraries. None is a real pointer out of the image.

## S3. The browser: WebGL and the page

Status: **done.** The page restores a snapshot the web engine wrote in headless Chromium: the first frame after the
restore is the launcher's home screen, a swipe up opens the all-apps drawer (with its text: "Search apps", the icons'
labels), a tap on Settings opens Settings.

### How the page restores

- `make snapshot` (S4) writes `build/web/xj/snapshot.json` and its packs into the chunk store.
- The page (`web/main.js`) fetches `xj/snapshot.json` when Boot Android is pressed (only with the WebGL backend, the
  chunk store and cross-origin isolation; `?snapshot=off` always boots). With a snapshot it does not start the boot
  packs' downloader; the worker (`web/worker.js`) checks the snapshot's build against the module's
  (`_xj_snap_build`) and its tree against the SHA-256 of `xj/manifest-xiaoji-v2.json`. A mismatch is reported
  (`restore-refused`) and the page boots instead.
- The worker grows memory to the image's top (`_xj_snap_grow`), zeroes the image's ranges, downloads the packs in
  parallel and inflates each chunk (`DecompressionStream`) straight to every place the image has it (equal chunks are
  stored once), writes the head (header and writable trees) to `/snap.head`, marks the image as loaded
  (`_xj_snap_preloaded`) and calls `main` with `--restore /snap.head`. The tree's files are read on demand from the chunk
  store (no boot pack is waited for).
- The engine then puts back the writable trees and the tmpfs directories, makes the rc's directories again, opens the
  host files again, makes the WebGL objects again (`engine/gl_snap.js`) and posts the last composed frame.
- The boot screen (`web/bootscreen.js`) has a `restore` phase: "Restoring Android", the snapshot's download as the bar,
  until the first frame.

### Making it

`?snapshot=make` boots with `--snapshot-at log:cmp=com.android.launcher3/ --snapshot-after 60` and `--snapshot-out`
the dev server's `/__snap/web.snap` (`web/serve.mjs --snapshot-upload DIR`): the web build writes its snapshot with
synchronous POSTs of each part at its offset. `tools/snapshot-web.mjs make` drives this in headless Chromium and then
runs `tools/snappack.py`. Since the demo product (docs/demo.md, B19) the page also takes `?snapat=log:TEXT` (another
condition), `?snapafter=S` and `?snapquiet=PCT:WINDOW:MAX` (`--snapshot-quiet`), and `tools/snapshot-web.mjs make` takes
the snapshot at HOME + 480 s and then waits for a quiet engine (a pre-warm of the demo apps is there, off by default):
the measurements of this section are of the old point (HOME + 60 s), docs/demo.md has the new one and its numbers.

### Measurements (headless Chromium, SwiftShader WebGL2, M2 8 GB, AC power, localhost)

| | |
|---|---|
Nine make runs and their restores; the ranges cover them, and the last (final sources) is given where it matters. The
machine had 3.5 to 5.2 GB of swap in use from other programs during the later runs, which is why those are slower.

| | |
|---|---|
| boot to the launcher's HOME `START` (engine time) | 233 to 260 s (391 s in the last run, swapping) |
| snapshot taken at | HOME + 60 s (298 to 460 s engine time) |
| writing it (image to the server) | 50 to 84 s (synchronous POSTs; the WebGL read-back included) |
| image | 772 to 852 MiB of non-zero memory (in about 1,965 MiB of linear memory), about 19,000 to 19,600 extents |
| WebGL objects (last run) | 64 textures (levels: 50 RGBA8, 5 R8, 2 RGBA, 1 RGB, 1 ALPHA from its upload copy), 10 buffers, 85 programs, 138 shaders, 54 framebuffers, 10 renderbuffers, 6 vertex arrays; the blob is 81 to 83 MB (texture pixels), part of the image |
| **download** | **112.5 to 131.9 MiB** in 8 packs (about 20,000 distinct 256 KiB chunks; last run 112.5 MiB), against **103 MB of boot packs** for a boot |
| download, inflate and write into memory (localhost) | 5.7 to 5.9 s (unloaded machine), 8 to 10 s (swapping) |
| engine start (`main`) to the first frame | **0.23 to 0.31 s** (0.44 to 1.1 s swapping) |
| press of Boot Android to the first frame (localhost) | **6.2 to 6.4 s** (8.7 to 11.5 s swapping; last run 8.7 s), against about 6.5 min for a boot |
| tap on Settings to its `START` | within 20 s (the check waits up to 40 s) |

The fallback (`tools/snapshot-web.mjs fallback`): with the snapshot manifest's build altered the worker reports "built
by another engine" and the page boots (the engine runs); with no `snapshot.json` (404) the page boots.

Screenshots (`build/web-shots/`, not committed): `s1-restore-first-frame.png` (the first frame: the home screen with
its search bar and status bar), `s1-restore-allapps.png` (after the swipe up: the drawer with Files and Settings),
`s1-restore-settings.png` (Settings).

Five problems the browser runs found, fixed:

- `/data_mirror` is a tmpfs, whose directory the engine keeps on the host under `/tmp` (`mntns.c`); in the page that is
  MEMFS, which a new worker starts without. Zygote's specialization of an app then failed to mount
  `/data_mirror/data_de/null` and the app aborted. Snapshots now carry every live tmpfs directory (by host path), and a
  restore makes the rc's directories and links again where they are missing (`init_snap_replay`).
- Immutable textures (`texStorage2D`) were refilled with RGBA bytes whatever their format; they now get the format their
  internal format takes (R8 gets RED, RGB8 gets RGB...).
- Textures in the unsized ALPHA, LUMINANCE and LUMINANCE_ALPHA formats cannot be attached to a framebuffer, so their
  pixels cannot be read back: `gl_snap.js` keeps a copy of their uploads (from client memory or from a pixel unpack
  buffer). The launcher's glyph atlas is one: without it the drawer's labels were missing after a restore.
- WebGL fixes a buffer's kind at its first binding (an index buffer can never be bound as a vertex buffer, nor the other
  way round). The restore filled every buffer through `COPY_READ_BUFFER`, after which binding an index buffer to a
  vertex array failed (INVALID_OPERATION) and the launcher drew garbage geometry and garbled text. Buffers now note
  their kind at their first binding, and a restore makes each through a binding of its kind (index buffers under a
  scratch vertex array). The restore reports the first GL error it meets (`xiaoji: gl(webgl): restore: ... no GL
  error`).
- A swipe on a machine that is swapping can reach the launcher late enough to read as a long press (its menu opens).
  `tools/snapshot-web.mjs restore` presses Home before the swipe and tries up to three times.

### Open

- **Size.** Done in S5 for what comes before the first frame: the guest's pages are lazy, and 11.7 MiB comes before
  `main` (146.9 MiB before S5). The total is about the same (128.6 MiB of snapshot packs). The texture pixels turned
  out cheap (5.8 MiB compressed, S5). Trimming processes the home screen does not need (docs/boot.md B16's lean list)
  would shrink the total.
- **Writing it takes a minute** in the browser (synchronous POSTs of 64 MiB parts and the WebGL read-back).
- **The download is cached** since S5: the snapshot's chunks go into Cache Storage (`xiaoji-snap-v1`), and a repeat
  visit downloads only the manifests and the page.
- After a restore the tree's chunks are read on demand, synchronously (the boot packs are not fetched). On localhost
  each read cost about 2.5 ms (fetch and inflate; 0.7 s for a drawer's first open); over a network each would be a
  round trip. Since P1 the chunks a recorded first minute reads are prefetched in the background (above); the rest
  are still read this way.
- A snapshot pack that fails to download is retried since S5 (4 tries before `main`, then the restore ends and the
  worker is dropped with its memory: "Snapshot download failed", press again; a lazy page: 5 tries, then the engine
  stops with the same message). There is still no fallback to a boot at that point.
- **Native snapshots skip pages `mincore` reports not resident.** On macOS the runs under heavy swap restored correctly;
  on Linux a swapped-out page reports as absent and would be left out. `XJ_NOMINCORE=1` reads every page; reading
  `/proc/self/pagemap` (which tells swapped from untouched) is the fix.
- The closure check ran on the small cases (`snapwait`, binder), not on a product snapshot; the native product restores
  were all in the host boot session that made them.
- The product restore (native and page) is a command (`make snapshot-native` and a restore; `make test-snapshot-web`),
  not a `tests/run.sh` case: each needs a boot.
- The build id is a hash of the sources and the Makefile plus one data address, not of the binary: a rebuild with
  another compiler would carry the same id.
- The snapshot is made and restored on the same WebGL implementation (SwiftShader in headless Chromium). Emscripten's
  uniform ids are restored as recorded, but limits and extensions the guest cached at boot, and texture formats a GPU
  renders differently, are not checked on other GPUs yet.

## P1. The pre-warmed snapshot and a faster restore (docs/demo.md, P1)

- **Pre-warm.** `make snapshot` opens Files, Calculator and Settings once and sends the phone home before the
  snapshot, so a visitor's first open of each is a hot start. The image is 916 MiB (765 before) and the download
  141.8 MiB (115.8).
- **Images above 2 GB.** The pre-warmed image's top is above 2 GB (0x8155a000). A C pointer reaches JavaScript as a
  signed number, and the web build's writer posted every extent above 2 GB from the wrong place (`xj_snap_post`); the
  restored phone drew blank screens at its first app redraw. Every pointer the engine hands to JavaScript is now taken
  with `>>> 0` (the writer, the GL blob, the JIT, the input ring), and `make test-snapshot-web` checks that Settings is
  drawn after the restore. The census prints the largest WebGL textures and the processes' resident sets.
- **The page's restore** (`web/worker.js`, `loadSnapshot`): only the memory the module had before the grow is zeroed
  (new wasm memory is zero; zeroing the image's whole 2 GB range wrote every page of it); at most 3 packs download at
  once; four helper workers (`web/inflater.js`, `?inflaters=N`) inflate the chunks in batches and the engine's worker
  copies them in. Press to the first frame: 2.6 s (median, AC) for 142 MiB, against 6.4 s for 116 MiB with the old loader (5.0 s against
  12.3 s on battery).
- **The prefetch** (`tools/prefetch.py`, `web/prefetch.js`, the chunk store's `attachPrefetch`): `snapshot.json` may
  list `prefetch` packs, the tree's chunks a recorded first minute read after a restore (`image/restore-order.json`),
  in first-read order. A worker of the engine's worker fills them into a SharedArrayBuffer while the engine runs; the
  chunk store takes a chunk from there when its pack is in (waiting up to 250 ms for a pack in flight) and otherwise
  reads it synchronously as before. `tools/snappack.py` removes an old snapshot's prefetch packs with its own.
- **The pre-hot set**: the snapshot records the guest pcs of the translated blocks (before the clean-page drop), and a
  restore with `--jit-prehot` (the page passes it; `?prehot=0` does not) queues each for translation at its first run.
- **The freezer**: the engine's binder answers `BINDER_GET_FROZEN_INFO` and `BINDER_FREEZE` with EINVAL, and
  ActivityManager runs without the cached-app freezer, which killed cached apps about 10 minutes after they were cached.
- **Lazy guest pages** (S1, "Format and delivery") were not done in P1: the restore was eager. Done in S5.

## S5. Lazy snapshot pages

Goal: a first-time visitor over a real network downloads much less before the home screen appears; the rest comes in
the background, in the order a recorded session first needed it, or on demand; the first opens of the drawer and the
demo apps stay where they were once the background download is done; and the engine never reads a page that has not
arrived as zeros.

Status: **done.** The page downloads 11.7 MiB before Android runs instead of 146.9 MiB; press to the home screen
takes 0.9 s instead of 2.6 s on localhost and 3.7 s instead of 26.4 s at 50 Mbit/s with a 40 ms round trip; the rest
arrives in the background (3 s and 33 s) in the order a recorded session needed it, and app opens afterwards are
unchanged within the spread. A repeat visit downloads 2.8 MiB.

### What is lazy, and why only that

The census of the image by kind (`tools/snappack.py` now prints it, from the snapshot's new region table; the final
snapshot of these sources, zlib level 6 per run of one kind):

| Kind | Raw (MiB) | zlib level 6 (MiB) |
|---|---|---|
| the page pool's pages (guest and memfd pages not zero) | 462.5 | 136.8 |
| the WebGL blob (its pages not zero; textures, buffers, programs) | 129.2 | 5.8 |
| the rest: the engine's own memory (page tables, processes, binder, malloc) | 361.3 | 6.3 |
| the image | 953.0 | 148.9 |

The guest's pages are almost all of the download, and they are the only part of the image whose every access goes
through a few places in the engine: the page table's TLB fill (`page_lookup` in `engine/mem.c`, which the interpreter,
the translator's inline TLB probe, `mem_copy_in`/`out` and every syscall reach on a TLB miss), the copy-on-write copy,
and the memfd functions (`engine/memfd.c`: binder's buffers, ashmem, the shared file cache). The rest of the image (the
engine's own structures: page tables, processes, binder, the GL decoders, malloc's state) is reached by C pointers from
everywhere, so it stays hot: it is in memory before `main`. It is 361 MiB raw but compresses to 6 MiB (page tables full
of "mapped, no page yet" entries).

**Textures** were the other candidate (the task's "drop the textures the compositor can redraw"): the WebGL blob is
169 MiB (66 textures of 1 MiB or more, 166 MiB: the cached apps' windows and the task snapshots of recents), but its
pages that are not zero compress to 5.8 MiB. Dropping or deferring them would save at most that, and a dropped window
buffer of a cached app shows garbage until the app redraws, so they stay in the hot part.

### The engine (`engine/snaplazy.c`, `engine/snaplazy.h`)

- A bit per host page of the page pool's range: "in the snapshot, not in memory yet". `snap_lazy_need(p)` before the
  bytes of a pool page are read, written or copied: in `page_lookup` (after the permission check, before the TLBs or the
  translator see the page's host address), in `cow_break` (`mem_poke` reaches it without `page_lookup`), in
  `memfd_page`, `memfd_pread` and `memfd_truncate` (`memfd_pwrite` goes through `memfd_page`). With nothing pending,
  `snap_lazy_bits` is NULL and the check is one load and a branch on the TLB-miss path; the translator's inline probe
  and every TLB hit are unchanged. The TLBs and the fetch cache are empty after a restore (as before), so no host
  pointer to a pending page can be cached.
- A pending page that is freed (`mem_page_free`: an unmap, a process exit, a memfd's end) is dropped: its bit is cleared
  and its bytes are never fetched; its next owner writes it before reading it. This also keeps a late arrival from
  overwriting a page that was freed and handed out again.
- The scheduler loop calls `snap_lazy_poll` at its top every 2 ms while pages are pending (the page copies in what the
  background loader has brought), and an idle loop sleeps at most 4 ms at a time until the last page is in.
- A snapshot taken by a run that still has pages pending first brings them all in (`snap_lazy_finish`), so the image
  is whole.
- The cost of the check: HEAD's engines against these, alternating, `jitstress-O2` (31 M instructions), 20 pairs:
  Node's interpreter (`XIAOJI_JIT=0`) 0.400 s against 0.398 s (medians; the new one faster in 11 of 20), the native
  engine 0.268 s against 0.269 s; with Node's translator, 10 pairs, about 0.23 s both. No difference beyond the
  run-to-run spread.
- The page supplies the pages through seven JavaScript functions (`Module.xjLazy`: `lo`, `hi`, `mark`, `fault`, `poll`,
  `finish`, `end`; below). Natively and in Node,
  `XIAOJI_SNAP_LAZY=1` makes a restore read the image as always, then poison every pool page of it (0xA5) and mark it
  pending; a fault reads the page from the snapshot file again (2: also a background fill of 64 pages every 2 ms). A
  path that read a pending page without filling it would read the poison, not zeros, and the cases below would fail.
- The snapshot file (version 2) has a region table: the pool's slabs and the WebGL blob.

### The page

- `tools/snappack.py`: the hot part is every byte of the image that is not a pool page, plus the pool pages a recorded
  restore touched before its first frame. It is cut into 256 KiB chunks in packs of at most 4 MiB, as before (S4). The
  lazy pool pages go into chunks of 32 pages (128 KiB), in the recorded first-touch order and then in address order,
  in packs of at most 1 MiB; `packs/<id>.map` lists the pages' numbers in that order (one zlib stream). The manifest is
  version 2 (`lazy`).
- Before `main` (`web/worker.js`, `loadSnapshot`): the hot packs and the map. Each pack is taken from Cache Storage
  (`xiaoji-snap-v1`, one entry per chunk, keyed by its id) when all its chunks are there, else downloaded and its chunks
  put there; a download is tried 4 times (0.5, 2 and 5 s apart). A pack that still fails ends the restore before
  `main`: nothing has run, the worker is dropped with its memory, and the page says "Snapshot download failed · press
  Start Android to retry" (a new press makes a new worker). `?snapcache=0` leaves the cache alone; `?snaplazy=0` loads
  the lazy packs before `main` too (the eager restore, from S5's packs).
- The background loader (`web/snaploader.js`, a worker of the engine's worker, told to start by the engine when it
  marks the pages): two downloads at a time; the lazy packs that hold recorded touches interleaved by bytes with the
  tree's prefetch packs (P1; one queue, so the two streams do not fight for the link), then the other lazy packs; a pack
  a fault needs moves to the front. It inflates each chunk (`DecompressionStream`), checks it against its id (SHA-256),
  and puts it into a ring of 64 slots of 128 KiB in a SharedArrayBuffer: only the engine's thread can write the engine's
  memory (it is not shared), and it gets no messages while `main` runs. A pack is retried 3 times; one that fails for
  good is marked, its pages are then read on demand, and the loader tries it again every 10 s, doubling up to 5 min,
  until the engine has every page (pending pages keep the engine's 2 ms poll going).
- The engine's side (`web/lazypages.js`, `LazyPages`): `mark` sets the bits; `poll` copies what is in the ring, page by
  page, only where the bit is still set; `fault` drains the ring, waits for the page's pack if the loader is on it (at
  most 2 s), and otherwise fetches the page's chunk itself with a synchronous Range request (a server that answers 200
  with the whole pack works too) and inflates it with `web/inflate.js`; 5 tries (0.25, 1, 3 and 6 s apart), then
  `snapshot: the lazy pages of packs/... could not be downloaded`, which stops the engine and shows "Snapshot download
  failed". `finish` and `end` are the engine's last two calls.
- The load report's `load+` line has the lazy counters (faults, chunks fetched on demand, time waiting, pages in), and
  the page logs when every lazy page is in.
- `?lazypoison=1` fills every pending page with 0xA5 when it is marked, so a read that skipped the fill reads that and
  not the zeros of fresh memory (the browser's counterpart of `XIAOJI_SNAP_LAZY`).
- The touch order: `tools/snapshot-web.mjs record` (run by `make snapshot` after the first packing) restores with
  `?snaprecord=1`: no background loader, every page on demand and one at a time, and each first touch kept with its time.
  The order is per snapshot: anonymous pages have no identity across boots, so unlike the tree's `restore-order.json`
  it cannot be committed. A recording in which an app does not start fails the make.
- The recording also runs without the tree's prefetch (`?prefetch=0`), so the server's log has every file chunk the
  session read; `tools/prefetch.py record` turns it into this snapshot's prefetch order (`build/snap/restore-order.json`,
  then the committed `image/restore-order.json`'s other chunks). The first measurements below were made with the
  committed order alone, and the new snapshot's Files open then read 18 to 20 file chunks on demand (0 or 1 with B19's
  snapshot): the clean pages a snapshot drops depend on its own make.

Two makes of this work: the one measured below (`make snapshot`, then `node tools/snapshot-web.mjs record` twice more:
the first recording missed two taps, and the record step now tries a tap three times; then for the prefetch order),
and the final one, made after an unrelated change to `engine/props.c` changed the build id and the product tree was
replaced by a rebuild (`build/rootfs-xiaoji`: a Settings overlay for the About crash, model and manufacturer XiaoJi;
`make snapshot` alone, the second try: the first make's pre-warm did not see Settings drawn in 150 s):

| | measured make | final make |
|---|---|---|
| image | 916 MiB of memory, 108,362 lazy pages (423 MiB) | 953 MiB, 118,392 lazy pages (462 MiB) |
| recording (about 4.5 minutes, every page on demand) | 44,176 pages touched (173 MiB): 26 before the first frame, 17,025 by the end of the 20 s idle, 24,293 by the tap on Settings, 36,702 by the tap on Files | 42,151 (165 MiB), 0 before the first frame |
| before `main` | **11.7 MiB**: 3 hot packs (4.0, 4.0, 3.6 MiB) and the page map (188 KiB) | **12.2 MiB** (4.0, 3.9, 4.0 MiB and a small fourth; map 202 KiB) |
| after `main`: lazy packs | 116.9 MiB in 120 packs (the recorded touches in the first 50, 48.9 MiB) | 135.6 MiB in 139 packs |
| after `main`: the tree's prefetch | 68.1 MiB (1,453 chunks: 1,280 from the recording, 173 more from `image/restore-order.json`) | 67.2 MiB (1,431: 1,258 and 173) |
| `snapshot.json` | 592 KiB (2.6 MiB for B19's eager manifest, which listed every chunk of the image) | 618 KiB |

B19's eager snapshot downloaded **146.9 MiB** before `main`.

The hot pool pages are few because the first frame comes before the guest runs: it is the last composed frame, posted
again by the restore of the WebGL objects (S3). Everything the guest touches after that is in the lazy order.

### Measurements

Headless Chromium (SwiftShader) on the M2 with 8 GB, AC power, Low Power Mode off (`pmset` at every run);
`tools/latency.mjs --runs 1 --calc --files --wait-fill --revisit`, a new browser profile per run; **before** is HEAD's
page, engine and B19's eager snapshot, frozen (`--web`, `--wasm`, `--xj`), **after** this tree's; before and after
alternated, one run at a time (`build/lazy/measure.sh`). Throttled is the dev server's model (`--throttle 50:40`: one
40 ms round trip per response, all bodies sharing 50 Mbit/s; it does not model TCP slow start). App opens are measured
after `--wait-fill`, once every lazy page and prefetch pack is in. Medians (range) of 3 runs; seconds unless marked.

First batch (swap 5.0 to 7.0 GB in use; the after runs used the committed prefetch order, see "The page"):

| | before, localhost | **after, localhost** | before, 50 Mbit/s, 40 ms | **after, 50 Mbit/s, 40 ms** |
|---|---|---|---|---|
| snapshot bytes before `main` | 146.9 MiB | **11.7 MiB** | 146.9 MiB | **11.7 MiB** |
| server bytes, page load to the first frame | 173.4 MiB (169.4 to 173.4) | 22.5 MiB (22.4 to 24.5) | 153.2 MiB (153.1 to 153.5) | **16.3 MiB** (16.0 to 16.8) |
| server bytes, whole run | 218.4 MiB | 199.8 MiB | 219.5 MiB | 201.7 MiB |
| press to the first frame | 2.6 (2.6 to 2.7) | **0.9** (0.9 to 1.0) | 26.4 (26.3 to 26.7) | **3.7** (3.7 to 4.0) |
| every lazy page in, after `main` | - | 3.2 (3.1 to 3.4) | - | 32.6 (32.5 to 32.8) |
| lazy faults in the run (chunks fetched on demand, ms blocked) | - | 0 in each run | - | 3 (1, 52 ms); 1 (0, 7 ms); 1 (0, 11 ms) |
| idle 30 s after the first frame: engine busy | 11% | 12% | 17% | 26% (the fill) |
| idle: the browser's CPU (one core = 100%) | 12% | 13% | 15% | 42% (the fill) |
| drawer: response / settled | 0.6 / 0.9 | 0.5 / 0.9 | 0.5 / 1.0 | 0.6 / 1.0 |
| first Settings: response / settled | 1.5 / 1.8 (1.8 to 1.9) | 1.6 / 2.2 (1.9 to 2.3) | 2.2 / 2.5 (1.7 to 5.8) | 2.2 / 2.7 (2.2 to 2.9) |
| first Calculator: response / settled | 1.1 / 1.7 (1.6 to 1.8) | 1.2 / 2.0 (1.9 to 2.1) | 1.5 / 1.9 (1.6 to 2.7) | 1.3 / 2.2 (2.1 to 2.2) |
| first Files: response / settled | 2.3 / 2.9 (2.9 to 3.0) | 2.8 / 3.5 (3.5 to 3.6) | 3.7 / 4.1 (4.0 to 4.9) | 3.7 / 4.5 (4.3 to 4.5) |
| repeat visit (same profile): press to the first frame | 2.4 | **0.8** | 26.3 | **1.6** |
| repeat visit: server bytes to the first frame / in its first 20 s | 169.4 / 216.2 MiB | **2.8 / 3.7 MiB** | 153.0 / 217.2 MiB | **2.8 / 3.0 MiB** |

Second batch, localhost, with the snapshot's own prefetch order and two controls on the same snapshot: **eager**
(`?snaplazy=0&snapcache=0`: every page before `main`, nothing cached) and **no cache** (`?snapcache=0`). Its second and
third rounds shared the Mac with another session's native boot and a dev server (swap 8.5 to 10 GB): the ranges are
wide, and only the medians and the counts below are worth reading.

| | before | after | after, eager | after, no cache |
|---|---|---|---|---|
| press to the first frame | 2.9 (2.7 to 3.8) | 1.0 (0.9 to 1.4) | 3.0 (2.8 to 7.0) | 0.9 (0.7 to 1.1) |
| idle 20 s after the fill: engine busy / browser CPU | 9% / 10% | 7% / 8% | 8% / 10% | 7% / 7% |
| first Settings settled | 2.4 (1.9 to 8.0) | 2.5 (2.4 to 2.7) | 3.8 (2.1 to 7.9) | 2.6 (1.9 to 3.2) |
| first Calculator settled | 2.4 (2.2 to 12.9) | 2.4 (2.1 to 3.9) | 2.9 (2.1 to 3.6) | 4.5 (2.6 to 5.6) |
| first Files settled | 3.4 (1.4 to 6.8) | 3.4 (3.0 to 5.0) | 4.6 (3.3 to 5.0) | 4.6 (3.9 to 5.3) |
| file chunks read on demand by the Files open | 9, 0, 10 | 0, 0, 1 | 0, 0, 17 | 0, 0, 1 |

What the rows show:

- **Before the first frame** the page now downloads 11.7 MiB of snapshot (16 to 25 MiB from the page load, with the
  page, the engine, the two manifests and, on localhost, what the background loader fetched in the half second after
  `main`), against 147 MiB. Press to the first frame: 0.9 s against 2.6 s on localhost, **3.7 s against 26.4 s** at
  50 Mbit/s. The total over a session is a little lower (200 against 219 MiB: this snapshot is smaller, and its
  manifest is 592 KiB instead of 2.6 MiB).
- **The background fill** takes 3 s on localhost and 33 s at 50 Mbit/s (117 MiB of lazy packs and 64 to 68 MiB of
  prefetch). On the throttled link the engine faulted on 1 to 3 pages in a whole run and fetched at most one chunk on
  demand: the recorded order brought the pages before the phone touched them. While it runs the engine is busier (26%
  against 17% idle, from copying 423 MiB in and the 4 ms wake-ups); after it, the idle phone costs what it did (7% busy
  and 8% of a core against 9% and 10%, second batch).
- **App opens once the fill is done** are within the run-to-run spread of before in the second batch (Settings 2.5
  against 2.4, Calculator 2.4 against 2.4, Files 3.4 against 3.4 settled, medians), and within the task's reference
  (drawer 1.1, Settings 2.0, Calculator 2.1, Files 3.7 s) give or take 0.4 s. In the first batch after was 0.3 to 0.6 s
  slower, with more guest instructions per open (Files 199 to 214 M against 135 to 185 M) and 18 to 20 file chunks read
  on demand by Files: the new snapshot's own state and its missing prefetch, which the recorded prefetch order fixed (0
  or 1 chunk). The eager control of the same snapshot is not faster than the lazy restore, so the lazy path costs the
  opens nothing measurable; the per-access check is measured above (no difference).
- **A repeat visit** downloads 2.8 MiB (the page, the engine and the manifests, which the dev server sends `no-store`)
  and shows the home screen in 0.8 s on localhost and 1.6 s at 50 Mbit/s, against 26 s before (HEAD never cached the
  snapshot).
- The snapshot of these sources is not the one measured: the final engine (with an unrelated change to
  `engine/props.c` from another session) and the rebuilt product tree needed a new `make snapshot`. One run of each
  network on it (localhost, then 50 Mbit/s; swap about 8.5 GB): press to the first frame 0.9 and 3.7 s, every lazy page
  in after 3.4 and 36.4 s, 22 and 37 faults (21 and 34 chunks fetched on demand, 78 ms and 1.8 s blocked in all),
  drawer 0.9 and 1.0 s, Settings 2.7 and 3.3 s, Calculator 4.0 and 2.9 s, Files 4.2 and 3.3 s settled, the idle phone
  after the fill 7% busy, a repeat visit 2.8 MiB. (An intermediate make, on the old tree with the new engine, had one
  run each too; its throttled Files open is under Open.)

### Tests

`tests/run.sh` (native and Node), with every pool page of the image poisoned and pending (`XIAOJI_SNAP_LAZY`); each
output must be the oracle's (or the uninterrupted run's) and a page must have faulted:

| Case | What |
|---|---|
| `snapwait snapshot lazy` | the S2 snapshot restored on demand only: the blocked threads' stacks, the pipe and futex words, the child; one page is freed before it is read |
| `snapwait snapshot lazy chained` | restored with the background fill, then a second snapshot 0.1 s on (it must bring every pending page in first), restored on demand |
| `procs`, `memory`, `jitstress`, `threads` (O2) `lazy restore mid-run` | a snapshot at an instruction count in the middle of the program (fork's copy-on-write pages, mmap'd and mprotect'd memory, translated code in Node, threads' stacks), restored on demand |
| `snapshot-binder lazy`, `snapshot-init lazy` | servicemanager's and the shell's memory, binder's buffers and ashmem (memfd pages), on demand (binder) and with the background fill (init) |

`tests/snapshot.mjs` (`make test-web`), in Node:

- `tools/snappack.py` on a synthetic version 2 snapshot: the pool pages are lazy except the one touched before the first
  frame; the lazy order is the recorded first touches, then address order; the hot extents hold the engine's memory,
  the blob and the hot page and no lazy page; the census by kind.
- `LazyPages` with a fake engine: a fault on a page not yet loaded gets the right bytes (and its chunk's other pages); a
  page freed before its chunk arrives keeps its new bytes; `poll` copies what the loader put in the ring; a failed
  download is retried; one that keeps failing ends with the error and the page still pending; `finish` brings
  everything in.
- `web/snaploader.js` against the dev server: every lazy page arrives through the ring with no fault, and a pack whose
  first download fails (503) is retried.

`make test-snapshot-web` restores the real snapshot (lazily) in the page.

### Open

- **The hot set is the pages touched before the first frame**, which is almost nothing (the first frame comes before
  the guest runs). On a slow link the pages the idle phone touches right after it are read on demand until the
  loader has their packs (the throttled rows above). `snappack.py --hot-ms` moves the cut (for instance to the end of
  the recorded idle window, about 17,000 pages, some 10 MiB more before `main`); not measured.
- **Range requests on Cloudflare Pages** are assumed, not verified: a fault fetches one 128 KiB chunk (about 35 KiB of
  zlib) with a Range header. A host that answers 200 still works, but each such fault then downloads its whole pack
  (up to 1 MiB).
- **The recording is per snapshot** (anonymous pages have no identity across boots) and adds about 6 minutes to
  `make snapshot`; it is made with every page on demand, so its times are stretched and only its order is used. A tap
  the slow recording misses is tried again (three times).
- **Every lazy page is downloaded**, also those no recorded session touched (about 60% of the lazy bytes), so the total
  download is about what it was; a visitor who leaves early stops it. Leaving the untouched ones on demand only would
  save their bytes for every visitor and cost a round trip for each later first touch.
- **A repeat visit still downloads the manifests** (`snapshot.json`, 592 KiB, and the tree's 1.1 MiB manifest; the dev
  server sends them `no-store`) and the page's files. A deployment can cache them by name.
- **Codec.** Each chunk on its own, as the page decodes them (`build/lazy/codec.mjs` over the packs of the make before the final one): the hot part
11.6 MiB with zlib level 9, 10.4 with brotli 9, 9.1 with brotli 11 (every 8th chunk, scaled), 9.9 with zstd 19; the
lazy part 136.6, 123.4, 115.2 and 121.0 MiB. Brotli 11 would save about 2.5 MiB before `main` (0.4 s at 50 Mbit/s) and
21 MiB in the background, but the browser's `DecompressionStream` has no brotli or zstd here, so it would need a wasm
or JavaScript decoder on the fault path and in the loader. Not adopted; the store stays zlib.
- The fault path inflates with the page's JavaScript inflater (1 to 6 ms a 128 KiB chunk); the background path uses
  the browser's.
- The page cannot tell a lazy page a visitor's session needs but the recording did not; such a page is read on demand
  (a round trip) unless the loader has reached its pack.
- In the final snapshot's one throttled check, the Files open started a second DocumentsUI process (275 M guest
  instructions, 29 file chunks read on demand, 1.5 s of fetching; 13.4 s settled); its localhost check settled in 6.1 s.
  One run each, not investigated; it looks like that snapshot's state, not the lazy path (no lazy fault in either run).
- One eager control run (second batch, 10 GB of swap in use) lost system_server (`DeadSystemException` in Files); it
  had no lazy pages. Not investigated.
- The first batch's before runs were measured, then their JSON files deleted with other scratch files when the disk
  filled; their numbers are in the table above as the tool printed them.


## S4. Tooling

| Command | What |
|---|---|
| `make snapshot` | the page's snapshot for the current tree and web engine (`tools/snapshot-web.mjs make`): boots in headless Chromium with `?snapshot=make`, the engine writes `build/snap/web.snap` through the dev server, `tools/snappack.py` packs it into `build/web/xj/snapshot.json` and `build/web/xj/packs/`, a restore records the touch order of the lazy pages (S5, `record` below) and the snapshot is packed again with it (`XN_SNAP_RECORD=off`: no recording). About 20 minutes since B19 (HOME + 480 s and the quiet window, docs/demo.md: `XN_SNAP_AFTER`, `XN_SNAP_QUIET`, `XN_SNAP_AT`, `XN_XJ`, and the pre-warm, off by default: `XN_SNAP_PREWARM=on`, `XN_PREWARM_DELAY`); it fails when the last system_server started SystemUI more than once, or a pre-warmed app does not start. Playwright as in `tests/web.mjs` (`XN_PLAYWRIGHT`; `PLAYWRIGHT_BROWSERS_PATH=$PWD/build/pw-browsers` when the cache has no Chromium) |
| `make test-snapshot-web` | the page restores it: first frame, drawer, Settings (`tools/snapshot-web.mjs restore`; Settings' place in the drawer is the tree's, `tools/ui-points.mjs`), screenshots `build/web-shots/s1-*.png` and the whole log `s1-restore.log` |
| `node tools/latency.mjs` | the demo's yardstick: press to first frame, the idle load, drawer, Settings, Home, Calculator (docs/demo.md) |
| `node tools/snapshot-web.mjs record` | (S5) restores `build/snap/web.snap`'s packs with `?snaprecord=1` (every lazy page on demand, one at a time), idles 20 s (`XN_RECORD_IDLE`), opens Settings, Calculator and Files from the drawer, writes the first touch of each page to `build/snap/touch.json`, and packs the snapshot again with it |
| `node tools/snapshot-web.mjs pack` | packs `build/snap/web.snap` again without a boot: eager with `XN_SNAP_RECORD=off`, or with an order (`XN_SNAP_ORDER=FILE`, `XN_SNAP_HOT_MS=MS`) |
| `make snapshot-native` | the native engine's (null GL) from a fresh `/data` -> `build/snap/native.snap`, for `build/xiaoji --restore` |
| `tools/snappack.py SNAP OUT --tree MANIFEST` | packs any snapshot file: content-addressed zlib chunks of 256 KiB in packs of at most 4 MiB (`--pack-max`; Cloudflare Pages' limit is 25 MiB a file), only packs are written (the deployment's file count stays the tree's 10,8xx chunks plus packs); the previous snapshot's packs are removed. A version 2 snapshot's pool pages go into lazy packs of at most 1 MiB, 32 pages a chunk (S5: `--order`, `--hot-ms`, `--eager`, `--lazy-pages`, `--lazy-pack-max`); it prints the census of the image by kind |
| `XIAOJI_SNAP_LAZY=1` (2, 3) | (restore, native and Node) every pool page of the image poisoned and pending, read again from the file on first touch (2: also a background fill; 3: as 2 without the poison); the tests' way into the lazy page path (S5) |

A snapshot is tied to the build: after any engine change, `make snapshot` again (the page boots cold, with a log line
saying why, when the snapshot is another build's or another tree's).

## Verification (final sources)

S5 (lazy snapshot pages), on the final sources and the final snapshot:

| Command | Result |
|---|---|
| `make && make wasm-node && make wasm` | exit 0, no warning |
| `XIAOJI_JOBS=4 tests/run.sh` | **335 passed, 0 failed, 0 xfail** (315 before S5, plus the 20 lazy cases: `snapshot lazy`, `lazy chained`, `lazy restore mid-run`, `snapshot-binder/init lazy`, native and Node) |
| `make test-web` | exit 0, no FAIL (`tests/snapshot.mjs` with the lazy checks) |
| `node tests/input.mjs` | all passed |
| `make snapshot` (new tree; second try), then `make test-snapshot-web` | snapshot: 12.2 MiB before `main`, 135.6 MiB lazy, recording with all three apps started; restore: first frame 1.1 s after the press, Settings drawn, no ANR, fatal exception or fatal signal, no page errors; fallback for another build and for no snapshot: all PASS |
| `make test-snapshot-web` with `XN_SNAP_QUERY='&lazypoison=1'`, and a throttled `tools/latency.mjs` run with it | all PASS; drawer, Settings, Calculator (1 + 2 = 3) and Files open, no ANR, fatal exception or fatal signal |

Before S5:

| Command | Result |
|---|---|
| `make && make wasm-node && make wasm` | exit 0, no warning (clang, emcc) |
| Linux arm64 (the builder VM, gcc 12; where `arena.c` replaces glibc's malloc for the whole process) | the final sources build with the same six gcc warnings as HEAD (bpf.c, mntns.c, vfs.c; none in new code); 80 of the 82 guest cases without arguments match the oracle; the 2 others (`bpfpin-O0/O2`) fail the same way at HEAD; `snapwait` snapshots and restores. The remote suite (`tools/remote-test.sh`) was not run: `XIAOJI_REMOTE_HOST` is unset |
| `XIAOJI_JOBS=4 tests/run.sh` | **298 passed, 0 failed, 0 xfail** (the 12 snapshot cases included) |
| `make test-web` | exit 0, no FAIL (with `tests/snapshot.mjs`: 11 checks) |
| `node tests/input.mjs` | 31 passed, 0 failed |
| `node tests/chunks.mjs` | 10,806 of 10,806 chunks, 3,453 files reassembled (the snapshot's packs do not disturb the store) |
| `make snapshot`, then `make test-snapshot-web` | snapshot made (112.5 MiB packed); restore: the "Restoring Android" stage (`s1-restoring.png`), first frame, drawer with labels, Settings; fallback for another build and for no snapshot: all PASS |
| `make snapshot-native`, then `build/xiaoji --restore build/snap/native.snap --input-script tests/input/settings.script` | resumed in 0.8 s; Settings started 20 s later; 120 s with no ANR, fatal exception or fatal signal; launcher and SystemUI alive |
