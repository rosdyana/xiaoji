# Web preview: Boot Android in the browser (W1)

The page runs the same engine as the native product boot, in a Web Worker, and shows what the boot prints.
It is a preview: there is no screen yet, nothing leaves localhost, and a reload loses the guest's state.

## Run it

```sh
make serve            # builds the wasm engine and the manifests, serves http://127.0.0.1:8080/
```

Open <http://127.0.0.1:8080/> and click **Boot Android**. The ART cases are behind `?cases=1` (the tests use them). **Stop** ends the run (it terminates the worker). A run otherwise lasts until the
engine's init timeout (900 s on the page), so the page never finishes sooner on its own.

`make serve` needs `build/rootfs-xiaoji`, the product tree (docs/boot.md, B4d and B7). Without it, `make serve`
prints a note and skips the product manifest, and the Boot Android button then fails: its manifest request
returns 404 and the page shows the error. The ART cases still work.

The manifest for the product tree is `build/web/manifest-xiaoji.json`, made by
`tools/mkmanifest.py --product`. It lists every directory and symbolic link, and every regular file as a lazy
file that is fetched from `/rootfs-xiaoji/<path>` on first use. The writable trees (`/data`, `/cache`,
`/metadata`) keep their directories and nothing else, so the browser starts with an empty `/data`. The
manifest has 4,888 entries (3,510 files, 723 symbolic links) and is 453 KB. Delete it to regenerate it.

## The boot screen

The page shows a phone-shaped boot screen (W2): the XiaoJi logo, the wordmark, and at the bottom the stage, a
progress bar, a detail line and a five-step strip. The ART cases are still on the page, behind `?cases=1`.

- **Download** (before the engine starts): "Downloading system" with the real percent and MB of the boot packs
  (`web/chunks.js` reports the bytes as they arrive), and "cached next time". A warm visit (every pack in the chunk
  cache) reads "Loading from cache", and no pack is downloaded.
- **Boot**: five stages, set by `stages` in the theme. 1 "Starting runtime" runs until ART is up, 2 "Loading
  framework" until system_server is forked (the class preload is inside it: the detail line shows the preloaded
  class count from the preload's end), 3 "Starting system server" until phase 100, 4 "Starting services" from phase
  100 on. Stage 5 "Starting home" is reserved and stays pending: the boot does not reach it yet. Stage 4 has no end
  in the log, so its bar only estimates (its expected time is a guess, 90 s).
- **Restart**: the log line `xiaoji: init: zygote exited` (init, when the framework's process exits) shows
  "Android stopped · Restarting…", and the screen keeps that state. In a browser boot without a fatal exception this
  line appeared after system_server was killed (signal 9), and init restarted the zygote.
- **Errors**: a system_server fatal exception (`FATAL EXCEPTION IN SYSTEM PROCESS`) or an error of the worker shows
  "Something went wrong". The worker keeps running, and the bar stays where it was. The fatal path has not been
  seen in a browser boot yet (the probe that looked for it ran 240 s and saw the restart instead). This is not
  verified in the browser.

The bar is one value over the boot: each stage's expected seconds (`BOOT.stageSeconds` in `web/cases.js`, from
one browser boot) weight its slice, and the slice fills toward its end without reaching it. The bar never goes
back. It starts at 0 when the engine starts; the download has its own bar.

**Theme.** `web/theme.json` is the default: the name parts (`name`, `accentPart`), the subtitle, the logo (`text`
or an http(s) `image`), the colours (`bg1`, `bg2`, `ink`, `dim`, `accent`) and the stage labels with the milestone
that starts each stage (`from`, `null` for a stage the boot does not reach). `?theme=pink` loads
`web/theme-pink.json`. A theme that does not validate (`web/theme.js`) is replaced by the built-in default, and
the page says why under the log. The logo and colours are checked before they are used.

**Log.** The logcat pane, the milestone table and the fetch counters are behind **Show log** (or `?log=1`).

## What it shows (the log)

- **logcat**: (behind Show log) the engine's `--logcat` lines, as they are written. Each line has the engine's stamp
  (`[ms insn]`, the time since the first log line and the instructions run so far), which the page uses for the
  milestone strip. The pane keeps the last 20,000 lines.
- **Milestones**: a row for each point of the native boot, from the log text (`BOOT.milestones` in
  `web/cases.js`): ART up, the class preload, the sensors HAL, the health HAL, SurfaceFlinger, the fork of
  system_server, its entry, `Starting phase 100`, the BatteryService health update, and the first fatal
  exception of system_server (the B7 blocker; the Node run logged it, the 90 s native runs did not). Each row has the page's wall time
  (from the start of the worker), the engine's time, and the instruction count.
- **Fetched**: the requests, the files and the bytes that have come over the wire so far. In the default boot
  these are the boot packs (one request each) and any synchronous chunk fallbacks; with `?boot=manifest`, the lazy
  files.

## Timings

Commit `054b086`, the product tree, and the same `--logcat --init image/xiaoji.rc` boot as B7. Each run starts
from a cleared `data/` (native and Node; the browser never reads the host `data/`). Engine times are the
engine's stamp, so they are the same clock in all three rows. Browser wall times are from the start of the
worker, and include the manifest fetch and the engine's start-up.

Machine state (`pmset -g`, recorded before and after the runs): `lowpowermode 1`, drawing from battery. B7's
native numbers were taken on AC with Low Power Mode off, and reached phase 100 at 47 s; these native runs took
77 s. The power state is the likely cause, but it was not measured separately. Compare the rows with each
other, not with B7 or with the benchmark power rule (AC, Low Power Mode off).

| Milestone | Native, interpreter (3 runs, median) | Node, translator (1 run) | Browser, translator (3 runs, median) |
|---|---|---|---|
| ART up (zygote runs ZygoteInit) | 6.74 s, 0.428 G | 6.07 s, 0.428 G | wall 22.51 s; engine 12.64 s, 0.428 G |
| class preload done | 55.02 s, 3.292 G | 49.38 s, 3.308 G | wall 87.27 s; engine 77.63 s, 3.311 G |
| sensors HAL (mask 6f) | 0.99 s, 0.096 G | 1.06 s, 0.096 G | wall 13.15 s; engine 3.21 s, 0.099 G |
| health HAL registered | 3.35 s, 0.223 G | 3.62 s, 0.224 G | wall 19.36 s; engine 9.54 s, 0.227 G |
| SurfaceFlinger main thread ready | 3.90 s, 0.257 G | 4.16 s, 0.258 G | wall 19.93 s; engine 10.08 s, 0.262 G |
| system_server forked | 70.38 s, 4.319 G | 56.38 s, 4.243 G | wall 95.25 s; engine 85.64 s, 4.247 G |
| system_server entered | 71.30 s, 4.370 G | 57.19 s, 4.294 G | wall 102.73 s; engine 93.12 s, 4.298 G |
| Starting phase 100 | 76.90 s, 4.697 G | 62.63 s, 4.620 G | wall 115.88 s; engine 106.27 s, 4.648 G |

The native runs reached phase 100 at 79.27, 76.90 and 76.75 s (4.692, 4.697 and 4.697 G). The browser runs
reached it at wall 115.66, 116.62 and 115.88 s. The Node run went on to BatteryService's health update at
85.98 s (6.417 G); the browser runs were stopped at phase 100, so there is no browser number for that line.

The native build is the interpreter only (`engine/jit.c` is wasm-only, see the Makefile), so the native rows
compare the interpreter with the translator. Node and the browser run the same wasm with the translator.

**Bytes and requests** (browser, by phase 100, identical in all three runs): 1,299 synchronous GET requests for
976 distinct lazy files, 451,888,886 bytes (431 MiB), out of the 1.39 GB product tree. The page counts the
response bodies of the successful GETs in the worker. emscripten's `createLazyFile` also sends one synchronous
HEAD per file, when the engine first needs its length (`cacheLength` in emscripten's `libfs.js`). The page does
not count those, and they are not in the figures above.

Reading the browser's engine times: the early milestones are 2 to 6 s later than native (the sensors HAL at
3.21 s against 0.99 s, ART at 12.64 s against 6.74 s), and the gap grows during the class preload (77.63 s
against 55.02 s). Every lazy file is a synchronous request, and the worker waits for each one. The instruction
counts at the milestones agree with native to about 2% (phase 100: 4.648 G in the browser, 4.697 G native).

**What these runs do not cover:** the Node figures are one run (with a 300 s init timeout). The browser runs
were stopped at phase 100, so the page's `done` path (the engine's init timeout) was not exercised in the
browser. Stop was. The native and Node runs were not stopped early.

**Measuring:** the browser runs use Playwright from the xiaonux bench (`XN_PLAYWRIGHT`), headless Chromium. Native
runs use `./build/xiaoji --logcat --init image/xiaoji.rc --root build/rootfs-xiaoji --init-timeout 90`
with `XIAOJI_STAMP=1`. Node uses `build/wasm-node/xiaoji.js` with `--init-timeout 300`, because the browser
and Node are slower than the native run's 90 s.

## Test

```sh
XIAOJI_WEB_BOOT=1 make test-web          # the ART cases, then the Boot Android check
# (it also writes build/web-shots/boot-download.png and boot-stage4.png, not committed)
node tests/web.mjs --boot-only           # the Boot Android check alone
```

The check clicks **Boot Android** in headless Chromium and waits (up to `XN_BOOT_TIMEOUT_MS`, default 900000) for
`Starting phase 100`. It then checks that each earlier milestone was reached (ART, preload, the sensors, health
and SurfaceFlinger, the fork, the entry), and that the log was still streaming when the milestone arrived (the
worker was still running). Then it presses **Stop**, reloads the page (a warm visit, the same profile) and checks that the download reads
"Loading from cache" with no pack downloaded, and stops that run too. Without `XIAOJI_WEB_BOOT=1` the check is not run.

## Engine changes for the browser build

- `-DXIAOJI_WEB=1` (Makefile, `build/wasm/xiaoji.mjs` only; the Node build is unchanged):
  - `engine/meta.c`: the metadata sidecar is a no-op. Nothing is read from or written to the page's file system at
    start or exit, and the records live in memory for the run only. The Node build still reads and writes
    `.xiaoji-meta` in its root, as before.
  - `engine/logd.c`: the logcat lines are always stamped (`[ms insn]`). The page reads the stamps for the
    milestone strip. The engine reads `XIAOJI_STAMP` with `getenv`, and I did not find a way for the page to set
    it: `Module` has no `ENV` option in this emscripten, and I did not try an exported-`ENV` route. This is the
    one engine change the browser build needs that the native and Node builds do not have.

## Known limits

- **No screen.** Graphics use the null backend (`engine/gl_null.c`).
- **Everything is fetched over localhost**, from `build/rootfs-xiaoji` through `web/serve.mjs`. The default page
  now loads the tree from a chunk store and boot packs, with a per-chunk cache (`docs/loading.md`); `?boot=manifest`
  is the old lazy-file path, which has no caching (`Cache-Control: no-store`).
- **State is lost on reload.** `/data`, `/cache`, `/metadata` and the rest of the writable state are in MEMFS, and
  the metadata sidecar is not kept. A new run always starts from an empty `/data`.
- **No input.** The page sends nothing to the guest. It can start and stop a boot.
- **The engine runs synchronously** in the worker (docs/graphics.md, open items). The page sees the log lines as
  they are written, because `postMessage` from a busy worker is delivered to the page (the check above verifies
  this), but the page cannot pause or step the engine.
- **The boot stops where the native boot stops** (B7): the owner check in installd and the missing record locks
  (SQLite). The browser runs were not taken past phase 100.
- **Timing is not a bench number.** The machine was on battery with Low Power Mode on for these runs (see above).
  The wall times are a comparison on one machine, not a benchmark.
