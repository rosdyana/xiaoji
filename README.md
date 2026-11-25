# xiaoji

**Android 13 in your browser tab. Nothing to install.** Try it: **[android.xiaonux.dev](https://android.xiaonux.dev)** (Chromium-based browsers, desktop first).

xiaoji (小机, "little machine", like 手机 shǒujī, "phone") is the Android sibling of [xiaonux](https://github.com/rosdyana/xiaonux), which runs Debian in a browser tab. The two take different approaches:

- **xiaonux** emulates a whole RISC-V machine.
- **xiaoji** does not emulate a machine. Its engine, written in C and compiled to WebAssembly, runs Android's own ARM64 user-space programs and provides the Linux kernel interface itself: syscalls, binder, futexes, files, cgroups, seccomp, input devices and the emulator's graphics pipe. Android's OpenGL ES calls go to WebGL2.

There is no guest kernel, MMU or virtual hardware to emulate, which is why xiaoji doesn't build on Cuttlefish or another full-system VM. Its other half is delivery, borrowed from xiaonux: content-addressed chunks fetched on demand and a booted snapshot whose memory loads lazily, so a visitor is on the home screen in seconds instead of downloading a 1 GB image and waiting minutes for a boot.

## Status

The page restores a booted Android 13 (our own trimmed AOSP product, `aosp/device/xiaoji`) straight to the home screen. You can swipe up to the app drawer, open Settings, Files and Calculator (1 + 2 = 3), and type with a physical keyboard.

| Measured (Apple M2, 8 GB, headless Chromium, AC power) | localhost | 50 Mbit/s, 40 ms |
|---|---|---|
| Download before the phone appears | 12 MB | 12 MB |
| Press of Start Android to the home screen | 0.9 s | 3.7 s |
| Rest of the snapshot in (background) | about 3 s | about 35 s |
| First open of Settings / Calculator / Files, settled | 2.2 / 2.0 / 3.5 s | 2.7 / 2.2 / 4.5 s |
| Repeat visit: press to home, download | 0.8 s, 3 MB | 1.6 s, 3 MB |

A boot from scratch (no snapshot) still works: about 270 s natively and about 6 min in the browser.

**Works:** launcher, SystemUI, Settings (including About), Files, Calculator; touch, swipe and keyboard input; the media provider and the software codec service.

**Not yet:** network, audio (the audio HAL is a null device), an on-screen keyboard, Files' storage roots (the engine has no FUSE), installing APKs. Only SwiftShader WebGL2 is measured; real GPUs are untested. The product has no Wi-Fi, Bluetooth, telephony, NFC or UWB.

Recent milestones: seccomp with Linux semantics, so the media codec service runs and the media provider stays up (docs/boot.md, B19); Settings > About fixed (no Wi-Fi rows; the model comes from the build like on a device); lazy snapshot pages (docs/snapshot.md, S5).

## How it was made

xiaoji was built by one developer pair-programming with [Claude Code](https://claude.com/claude-code). The repository keeps the whole trail: the plan for each milestone, what failed and why (docs/), the measurements, and over 330 tests that compare the engine against real Linux on the same binaries. Prior art showed the way: browser OS projects such as [v86](https://github.com/copy/v86) and [JSLinux](https://bellard.org/jslinux/), and Android-in-user-space projects such as Twoyi. The engine and its tooling are written here.

Milestone notes:

| Doc | What |
|---|---|
| [docs/spike-0.md](docs/spike-0.md) | the first question: ART (`dalvikvm64`) on the engine, natively and in wasm |
| [docs/boot.md](docs/boot.md) | the boot, milestone by milestone (B1 to B19): init, binder, zygote, system_server, HALs, seccomp, the product, the launcher |
| [docs/graphics.md](docs/graphics.md) | the goldfish pipe, the gfxstream decoders, the null and WebGL2 backends (G1 to G4) |
| [docs/input.md](docs/input.md) | the touchscreen and keyboard (evdev) and how the page feeds them |
| [docs/loading.md](docs/loading.md) | chunks, boot packs, early start and caching |
| [docs/snapshot.md](docs/snapshot.md) | booted snapshots: the design, `--snapshot-at` and `--restore`, the WebGL objects, the page's restore, lazy guest pages (S1 to S5) |
| [docs/demo.md](docs/demo.md) | the demo product (B19): its apps, what was removed, AOT, the snapshot point, and `tools/latency.mjs`, the yardstick, with the numbers |
| [docs/web-preview.md](docs/web-preview.md) | the browser page and its boot screen |
| [docs/testing.md](docs/testing.md) | the test suite, parallel runs, the remote runner |
| [docs/features.md](docs/features.md) | draft: which Android features are base, add-on, engine-backed or never |

## Quick start (development)

You need macOS (or Linux), clang, Python 3, Node, a JDK (17+), [Emscripten](https://emscripten.org) in `~/emsdk`, and an arm64 Linux VM for the reference runs (an [OrbStack](https://orbstack.dev) VM named `builder`).

```sh
make                 # native engine -> build/xiaoji
make wasm            # browser engine -> build/wasm/
make wasm-node       # Node engine (reads host files directly)
make image           # Google's AOSP 13 arm64 image, extracted -> build/rootfs (for the test suite)
make oracle          # reference runs in the VM -> build/oracle
make guest-tests     # the guest test programs, built in the VM
make test            # the suite: every case natively and in Node, against the oracle
make serve           # the page at http://127.0.0.1:8080/ (needs build/rootfs-xiaoji)
make snapshot        # the page's booted snapshot, its page and touch recordings (headless Chromium, about 25 min; docs/snapshot.md, docs/demo.md)
make help            # every target
```

The browser boot needs the product tree, `build/rootfs-xiaoji`, which is built from `aosp/device/xiaoji` on an x86_64 AOSP build machine (`tools/remote-build.sh`, then `tools/fetch-remote-rootfs.sh`). The test suite can run there too (`tools/remote-test.sh`, see `docs/testing.md`).

Useful test switches: `XIAOJI_JOBS=4` runs cases in parallel, `XIAOJI_BOOT=1` adds the timed Android boot, and `XIAOJI_ROOT=build/rootfs-xiaoji` runs against the product tree. `make test-web` checks the page in headless Chromium; `XIAOJI_WEB_BOOT=1` and `XIAOJI_WEB_FRAMES=1` add the boot and the Android frame checks. On the page, `?debug=1` shows the engine's diagnostics.

Guest files come from Google's AOSP image or from our AOSP build. They are never committed.

## Deploy

The site is static, on Cloudflare Pages:

```sh
tools/deploy.sh --stage   # build/site: the page, the engine and the chunk store pruned to what the manifests name; checks Pages' limits
tools/deploy.sh           # stage, then wrangler pages deploy (project xiaoji; XIAOJI_PAGES_PROJECT overrides)
```

It writes `_headers` for cross-origin isolation (COOP/COEP, needed for SharedArrayBuffer), a Content-Security-Policy, and immutable caching of packs and chunks. Pages answers Range requests with the whole file, so a lazy page fault there downloads its whole pack (up to 1 MiB) instead of one 128 KiB chunk.

## Layout

| Path | What |
|---|---|
| `engine/` | the C engine: ARM64 interpreter and JIT, syscalls, binder, init, procfs and sysfs, cgroups, bpf maps, evdev, goldfish pipe, GL decoders and backends |
| `aosp/device/xiaoji/` | the AOSP product: trimmed device config, Settings overlay, netd stub, null audio HAL |
| `image/` | the init config the engine boots (`xiaoji.rc`) |
| `web/` | the page, its worker, the pack and snapshot loaders, lazy pages, the boot screen and its themes, input |
| `tools/` | image fetch and extraction, AOSP build and fetch, manifests and packs, snapshots, latency yardstick, deploy, GL and kernel-config generators, remote runner |
| `tests/` | guest test programs, init cases, expected outputs, suite and page tests |
| `third_party/gfxstream/` | the GL protocol specs (Apache-2.0) the decoders are generated from |
| `docs/` | milestone notes, plans, measurements |

## License

Apache-2.0. Android components are under their own licenses (AOSP); they are downloaded or built, never committed to this repository (the demo site serves our own AOSP build). See [NOTICE](NOTICE) for the vendored gfxstream specs, dlmalloc and the page's fonts.
