# Booted snapshots (S1 to S4)

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

The file written by `--snapshot-out` (`engine/snap.c`): a 4 KiB header (magic `XJSNAP` version 1, the build id, the
flags, the snapshot's monotonic and real time and instruction count, the image's ranges, the guest root), the writable
trees, the extent table (address and length of each run of non-zero pages), then the extents' bytes. A restore zeroes
what no extent covers.

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
memory before `main` runs, and starts the engine with `--restore`. Eager first: every chunk before the restore. Lazy
guest pages (a page-table bit "in the snapshot, not loaded", filled on first touch from its chunk, as lazy file
mappings are) are the next step if the eager size is too large; they need the page pool's pages to be keyed by chunk,
which the pool makes possible.

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

- **Size.** The snapshot is as big as the boot packs. Most of it is the guest's anonymous memory (401 MiB of guest
  pages not zero in the web census: ART heaps of 49 processes) and 81 MB of texture pixels. Lazy guest pages (a page
  fetched from its chunk on first touch, S1 "Format and delivery"), dropping the textures the composer can redraw, and
  trimming processes the home screen does not need (docs/boot.md B16's lean list) are the next steps.
- **Writing it takes a minute** in the browser (synchronous POSTs of 64 MiB parts and the WebGL read-back).
- **The download is not cached yet:** the packs are fetched with `fetch` but not put into the chunk store's Cache
  Storage as the boot packs are (docs/loading.md).
- After a restore the tree's chunks are read on demand, synchronously (the boot packs are not fetched). On localhost
  that costs nothing; over a network each first read of a chunk is a round trip.
- A snapshot pack that fails to download mid-restore is a hard error (memory is already being replaced); there is no
  fallback to a boot at that point.
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

## S4. Tooling

| Command | What |
|---|---|
| `make snapshot` | the page's snapshot for the current tree and web engine (`tools/snapshot-web.mjs make`): boots in headless Chromium with `?snapshot=make`, the engine writes `build/snap/web.snap` through the dev server, `tools/snappack.py` packs it into `build/web/xj/snapshot.json` and `build/web/xj/packs/`. About 15 minutes since B19 (HOME + 480 s and the quiet window, docs/demo.md: `XN_SNAP_AFTER`, `XN_SNAP_QUIET`, `XN_SNAP_AT`, `XN_XJ`, and the pre-warm, off by default: `XN_SNAP_PREWARM=on`, `XN_PREWARM_DELAY`); it fails when the last system_server started SystemUI more than once, or a pre-warmed app does not start. Playwright as in `tests/web.mjs` (`XN_PLAYWRIGHT`; `PLAYWRIGHT_BROWSERS_PATH=$PWD/build/pw-browsers` when the cache has no Chromium) |
| `make test-snapshot-web` | the page restores it: first frame, drawer, Settings (`tools/snapshot-web.mjs restore`; Settings' place in the drawer is the tree's, `tools/ui-points.mjs`), screenshots `build/web-shots/s1-*.png` and the whole log `s1-restore.log` |
| `node tools/latency.mjs` | the demo's yardstick: press to first frame, the idle load, drawer, Settings, Home, Calculator (docs/demo.md) |
| `make snapshot-native` | the native engine's (null GL) from a fresh `/data` -> `build/snap/native.snap`, for `build/xiaoji --restore` |
| `tools/snappack.py SNAP OUT --tree MANIFEST` | packs any snapshot file: content-addressed zlib chunks of 256 KiB in packs of at most 16 MiB (Cloudflare Pages' 25 MiB file limit), only packs are written (the deployment's file count stays the tree's 10,8xx chunks plus packs); the previous snapshot's packs are removed |

A snapshot is tied to the build: after any engine change, `make snapshot` again (the page boots cold, with a log line
saying why, when the snapshot is another build's or another tree's).

## Verification (final sources)

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
