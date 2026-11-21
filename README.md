# xiaoji

Android in your browser. Nothing to install. (Work in progress; will live at android.xiaonux.dev.)

xiaoji (小机, "little phone", like 手机 shǒujī) is the Android sibling of [xiaonux](https://github.com/rosdyana/xiaonux), which runs Debian in a browser tab. The two projects take different approaches:

- **xiaonux** emulates a whole RISC-V machine.
- **xiaoji** does not emulate a machine. Its engine, written in C and compiled to WebAssembly, translates Android's ARM64 user-space code and provides the Linux kernel interface (syscalls, and later binder) itself. Guest threads become Web Workers, and OpenGL ES goes to WebGL2.

That model is what makes real arm64 APKs and a GPU-speed UI possible. xiaoji's distinguishing goal is delivery, borrowed from xiaonux: a booted snapshot, content-addressed chunks fetched on demand, and per-app packs. The aim is a small first download and a start measured in seconds, not a 1 GB download before the first boot.

## Status

This is spike 0: running Android 13's own ART (`dalvikvm64`) on the engine. See [docs/spike-0.md](docs/spike-0.md).

## Quick start (development)

You need macOS (or Linux), clang, Python 3, a JDK (17+), [Emscripten](https://emscripten.org) in `~/emsdk`, and an arm64 Linux VM for the reference runs ([OrbStack](https://orbstack.dev) VM named `builder`).

```sh
make                 # native engine -> build/xiaoji
make wasm            # wasm engine -> build/wasm/
make image           # download Google's AOSP 13 arm64 image, extract -> build/rootfs (~1.4 GB)
make oracle          # run the guest's binaries natively in the VM -> build/oracle (expected outputs + strace)
```

Guest files come from Google's prebuilt AOSP image and are never committed. Later, a trimmed AOSP build will replace them (see `docs/spike-0.md`).

## Layout

| Path | What |
|---|---|
| `engine/` | the C engine (ARM64 → host, Linux syscall layer) |
| `tools/` | image download and extraction, linker config, reference runs |
| `tests/` | guest test programs |
| `web/` | browser harness (later) |
| `docs/` | plans, findings, benchmarks |

## License

Apache-2.0. Android components are under their own licenses (AOSP); they are downloaded, never redistributed from this repository.
