# xiaoji

Android in your browser. Nothing to install. (Work in progress; will live at android.xiaonux.dev.)

xiaoji (小机, "little phone", like 手机 shǒujī) is the Android sibling of [xiaonux](https://github.com/rosdyana/xiaonux), which runs Debian in a browser tab. The two projects take different approaches:

- **xiaonux** emulates a whole RISC-V machine.
- **xiaoji** does not emulate a machine. Its engine, written in C and compiled to WebAssembly, runs Android's ARM64 user-space code and provides the Linux kernel interface itself: syscalls, binder, futexes, files, cgroups, input devices and the emulator's graphics pipe. Android's OpenGL ES calls go to WebGL2.

There is no guest kernel, MMU or virtual hardware to emulate, which is why xiaoji doesn't build on Cuttlefish or another full-system VM. xiaoji's distinguishing goal is delivery, borrowed from xiaonux: content-addressed chunks fetched on demand, boot packs, a booted snapshot and per-app packs (planned). The aim is a small first download and a start measured in seconds, not a 1 GB download before the first boot.

## Status

Android 13 (our own trimmed AOSP product, `aosp/device/xiaoji`, with Wi-Fi, Bluetooth, telephony, NFC and UWB off) boots in a browser tab to the launcher. You can open the app drawer, start Settings, Files and Calculator (1 + 2 = 3), and type into the launcher's search. The demo apps are compiled ahead of time (docs/demo.md).

| | Native engine (M2) | Browser (headless Chromium, M2) |
|---|---|---|
| `sys.boot_completed` | about 270 s | about 6 min |
| Launcher on screen | about 285 s | about 6.5 min |

The browser numbers were measured on an 8 GB machine that was swapping, on battery.

A booted snapshot skips the boot (docs/snapshot.md): the page restores Android at the home screen from a snapshot the web engine wrote, with the demo apps already opened once (docs/demo.md, P1). On localhost, Boot Android shows the launcher about 2.6 s after the press on AC (a 142 MiB download, inflated by four workers into the engine's memory), against about 6.5 min for a boot. `make snapshot` makes it for the current tree and engine. The last measurement (headless Chromium on the M2, on battery with Low Power Mode on, a swapping 8 GB machine; medians of 3 runs against L1's snapshot in the same conditions): the first open of Settings settles in 3.5 s (23.6 s before P1), Calculator in 4.1 s (12.8 s), the drawer in 2.4 s (2.4 s), and the idle phone costs the browser 16% of a core (100%); the file pages the first minute needs are prefetched in the background. Since S5 the snapshot's guest pages are lazy (docs/snapshot.md, S5): the page downloads about 12 MiB before Android runs (146.9 MiB before), and the rest in the background in the order a recorded session first needed it, or on first touch. Press to the home screen: 0.9 s on localhost (2.6 s before) and 3.7 s at a simulated 50 Mbit/s with a 40 ms round trip (26.4 s before); everything is in after about 33 s on that link, and app opens afterwards are where they were. The snapshot is kept in Cache Storage, so a repeat visit downloads about 3 MB. Natively, `build/xiaoji --restore` resumes a snapshot in under a second.

Milestone notes:

| Doc | What |
|---|---|
| [docs/spike-0.md](docs/spike-0.md) | the first question: ART (`dalvikvm64`) on the engine, natively and in wasm |
| [docs/boot.md](docs/boot.md) | the boot, milestone by milestone (B1 to B19): init, binder, zygote, system_server, HALs, the product, the launcher |
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
make snapshot        # the page's booted snapshot and its touch recording (headless Chromium, about 20 min; docs/snapshot.md, docs/demo.md)
make help            # every target
```

The browser boot needs the product tree, `build/rootfs-xiaoji`, which is built from `aosp/device/xiaoji` on an x86_64 AOSP build machine (`tools/remote-build.sh`, then `tools/fetch-remote-rootfs.sh`). The test suite can run there too (`tools/remote-test.sh`, see `docs/testing.md`).

Useful test switches: `XIAOJI_JOBS=4` runs cases in parallel, `XIAOJI_BOOT=1` adds the timed Android boot, and `XIAOJI_ROOT=build/rootfs-xiaoji` runs against the product tree. `make test-web` checks the page in headless Chromium; `XIAOJI_WEB_BOOT=1` and `XIAOJI_WEB_FRAMES=1` add the boot and the Android frame checks. On the page, `?debug=1` shows the engine's diagnostics.

Guest files come from Google's AOSP image or from our AOSP build. They are never committed.

## Layout

| Path | What |
|---|---|
| `engine/` | the C engine: ARM64 interpreter and JIT, syscalls, binder, init, procfs and sysfs, cgroups, bpf maps, evdev, goldfish pipe, GL decoders and backends |
| `aosp/device/xiaoji/` | the AOSP product: trimmed device config, netd stub, null audio HAL |
| `image/` | the init config the engine boots (`xiaoji.rc`) |
| `web/` | the page, its worker, the pack loader, the boot screen and its themes, input |
| `tools/` | image fetch and extraction, AOSP build and fetch, manifests and packs, GL and kernel-config generators, remote runner |
| `tests/` | guest test programs, init cases, expected outputs, suite and page tests |
| `third_party/gfxstream/` | the GL protocol specs (Apache-2.0) the decoders are generated from |
| `docs/` | milestone notes, plans, measurements |

## License

Apache-2.0. Android components are under their own licenses (AOSP); they are downloaded or built, never redistributed from this repository. See [NOTICE](NOTICE) for the vendored gfxstream specs.
