# Spike 0: ART on a user-mode ARM64 engine

**Question:** can a portable C engine run real Android 13 ARM64 bionic binaries, all the way up to ART running a `.dex` file? How fast is it in wasm?

**Exit criterion:** `dalvikvm64 -cp hello.dex Hello` prints its line under the engine in three places:
1. natively on macOS,
2. as wasm in Node,
3. as wasm in Chrome.

Timings are recorded for each.

## Guest

Google's prebuilt AOSP image `system-images;android-33;default;arm64-v8a` r02 (`tools/fetch-image.sh`).

What extraction found (2026-11-22):
- **`system.img`** is a GPT disk with two partitions: `vbmeta` and `super`.
- **`super`** uses Android's dynamic-partition format (LP metadata v10.0). It holds `system` (799 MiB), `system_ext` (140 MiB), `product` (276 MiB) and `vendor` (87 MiB), all ext4. `tools/lpunpack.py` reads it, since Debian doesn't package lpunpack.
- **APEXes:** 27 modules in `/system/apex`, 16 of them compressed (`.capex`). Every payload is ext4. `tools/apexunpack.py` extracts them, and they're placed at `/apex/<name>`.
- **Where the binaries live:**
  - `dalvikvm64` is at `/apex/com.android.art/bin`.
  - `linker64` is at `/apex/com.android.runtime/bin`; `/system/bin/linker64` is a symlink to it.
  - The boot image (`boot*.art/.oat/.vdex`) is in `/system/framework/arm64`, built for the full framework boot classpath.
- **Size:** the extracted rootfs is 1.4 GB.

## The reference ("oracle")

The guest's own binaries run natively in the arm64 Linux VM, in a chroot of `build/rootfs`, under strace (`tools/oracle.sh`).

| Case | Result |
|---|---|
| `toybox echo hi from bionic` | ✅ |
| `toybox ls /system/bin/linker64` | ✅ |
| `sh -c 'echo $((6*7))'` (mksh) | ✅ `42` |
| `dalvikvm64 -Xint … Hello` | ✅ 1M-iteration loop in 33–45 ms |
| `dalvikvm64 … Hello` (JIT) | ✅ loop in 6–9 ms; whole process 0.39 s |

To get ART running outside a booted Android:

1. **Linker config.** Android generates per-APEX, isolated linker namespaces at boot (`linkerconfig`). That needs system properties, and without them it aborts (`SANITIZER_DEFAULT_VENDOR is not defined`).
   - xiaoji writes its own config instead (`tools/gen-ldconfig.py`): one flat, non-isolated namespace.
   - It also defines the named namespaces ART and nativeloader look up (`system`, and `com_android_<apex>`), all linked to `default`, so libc loads only once.
   - Search order: the ART, i18n and runtime APEXes, then `/system`, then the other APEXes. The VNDK APEX is excluded because its vendor builds of libbinder etc. clash.
2. **Environment** (`tools/guest-env.sh`): the `ANDROID_*` roots from `init.environ.rc`, plus BOOTCLASSPATH, which `derive_classpath` normally builds and which also aborts without properties. The classpath is in boot-image order: ART jars, framework jars, ICU.
3. **`/apex/apex-info-list.xml`**, which apexd normally writes.
4. **Logs:** with no logd, ART's fatal messages disappear. `tools/fakelogd.py` listens on `/dev/socket/logdw` and prints them.

Without system properties (`/dev/__properties__`), ART still runs Hello. A full Android system will need a property service in the engine.

## What the engine must provide (from the JIT run's strace)

The JIT run made about 15,600 syscalls of 60 distinct kinds across 11 processes or threads (the shell plus ART's threads). Counts are from strace, so loop timings measured under it are inflated:

- **Memory:** mprotect 3939, mmap 3468, munmap 2321, madvise 572, mremap 267, brk 3, memfd_create 1, ftruncate 1
- **Files:** openat 1047, fstat 384, close 372, pread64 347, readlinkat 313, newfstatat 298, fstatfs 247, read 202, fcntl 85, lseek 72, faccessat 15, writev 14, flock 10, statfs 2, getcwd 2, write 2
- **Processes:** prctl 1287 (mostly naming memory regions), futex 112, clone 10, exit 10, set_tid_address 7, getpriority 8, sched_* 6, tgkill 1, set_robust_list 1, rseq 1, setpriority 1, execve 3 (the shell; not needed when ART is started directly)
- **Signals:** rt_sigprocmask 76, rt_sigaction 50, sigaltstack 22, rt_sigtimedwait 1
- **Misc:** clock_gettime 10 (most calls go through the vDSO; bionic should fall back to the syscall when there is no `AT_SYSINFO_EHDR`, so a vDSO is likely a speed-up rather than a requirement — unverified), getrandom 9, uname 5, getrlimit or prlimit64 4, the get*id family, socket and connect 7 (logd; failures are fine)

Also from the traces:

- **No real faults in Hello.** ART installs SIGSEGV handlers, but none fires in either run. Hello runs without fault delivery; later workloads (implicit null checks, stack overflow) will need it.
- **Synthetic files opened successfully:** `/proc/self/maps` and `/proc/self/stat` (twice each), `/proc/self/cmdline`, `/proc/filesystems`, `/sys/devices/system/cpu/online` and `/dev/tty`. The engine has to generate these.
- **Address space (this decides the memory design).** Even `toybox echo` reserves two 8448 MiB and two 2048 MiB `PROT_NONE` regions (bionic's scudo allocator), and ART adds 512 MiB and 256 MiB heap reservations. Peak reserved address space is about 21–23 GiB. That doesn't fit a 4 GiB wasm memory if guest address equals arena offset. See the design notes below.

## Open questions for the go/no-go

1. **Memory model.**
   - (a) A software page table that translates guest 48-bit addresses to arena pages. Reservations cost nothing until a page is made writable, and the stock image works. Each memory access pays a TLB lookup, as in TinyEMU.
   - (b) Identity mapping below 4 GiB, which needs the trimmed AOSP build to shrink the reservations: scudo's svelte or low-RAM configuration (`MALLOC_SVELTE`) and ART heap sizes.

   M0 starts with (a), measures the per-access cost, and revisits (b) once the trimmed build exists.
2. **fork and zygote.** Android starts apps by forking zygote. Two processes can't share one identity-mapped wasm memory; with (a), each process can have its own page table over a shared page pool. Design this before spike 2.
3. **Instruction count.** The oracle ran with the Mac's full CPU features. Measure Hello's dynamic instruction count under user-mode QEMU with a baseline ARMv8.0 CPU; dividing it by a realistic interpreter rate predicts wasm time before M3.

## Milestones

| | Goal | Check |
|---|---|---|
| M0 | ELF loader, guest page table over an arena, A64 integer interpreter, write/exit, auxv | static test programs match the oracle |
| M1 | linker64, file and memory syscalls, TLS, FP/SIMD load/store | `toybox echo`, `toybox ls` match |
| M2 | shell: signals, ioctls | `sh -c 'echo $((6*7))'` → 42 |
| M3 | ART: green threads, futex, AdvSIMD, synthetic `/proc` files (fault delivery and a vDSO can come later) | Hello with `-Xint`, then with the JIT |
| M4 | wasm in Node and Chrome | Hello in both; timings in `docs/benchmarks.md` |

Engine design notes:
- **Memory:** see open question 1. Whatever the model, address translation must ignore the top byte, because scudo returns tagged pointers (top byte 0xB4).
- **Page protection bits**, because ART relies on faults for null, stack and suspend checks.
- **A minimal `AT_HWCAP` (FP and ASIMD only)**, so bionic and ART take their ARMv8.0 paths.

## Parallel track: trimmed AOSP 13 build

The build runs on the remote machine `build-host`: x86_64, i7-12700 with 20 threads, 64 GB RAM, Ubuntu 20.04, in `~/xiaoji-aosp`. It's a shallow, partial-clone sync of `android13-release`, followed by a baseline `sdk_phone64_arm64` build (the same product as the prebuilt guest, build `TE1A.220922.034`, `sdk_phone64_arm64-userdebug`) and then a trimmed `vendor/xiaoji` product. That product sets `low_ram`, flattens APEXes, and keeps only Launcher3, SystemUI, Settings, PackageInstaller and DocumentsUI.

## M0 results

M0 is the ELF loader, the software guest page table, the A64 integer interpreter, the M0 syscalls and the auxv stack. `make test` runs `tests/run.sh`, which compares each freestanding guest program with a native run in the builder VM, for the engine and for `node build/wasm-node/xiaoji.js`.

| Case | Engine (macOS) | Node (wasm) |
|---|---|---|
| `hello-O0`, `hello-O2` | PASS | PASS |
| `args-O0`, `args-O2` | PASS | PASS |
| `arith-O0`, `arith-O2` | PASS | PASS |
| `control-O0`, `control-O2` | PASS | PASS |
| `memory-O0`, `memory-O2` | PASS | PASS |
| `glibc_hello-O2` (stretch) | XFAIL | XFAIL |

Summary from `make test`: 20 passed, 0 failed, 2 XFAIL. The XFAIL is `unimplemented A64 instruction 0x4e010c20 at pc 0x40ff44`: glibc uses a SIMD instruction, and vector data processing is outside M0.

Instruction rate for `control-O2` (4,577,199 instructions, `XIAOJI_STATS=1`):

| Runner | Time | Rate |
|---|---|---|
| Native (macOS arm64, `build/xiaoji`) | 0.064 s | about 71.5 M instructions/s (3 runs: 71.3 to 71.6) |
| Node (`build/wasm-node/xiaoji.js`) | 0.091 s | about 50 M instructions/s (runs: 49.7 to 50.4) |

Conditions: battery power, Low Power Mode on (`pmset`: `lowpowermode 1`), runs of about 64 ms (native) and 91 ms (Node). Re-measure on AC power with Low Power Mode off before quoting these numbers.

Notes from bringing up M0:
- `args` prints `AT_HWCAP & 3`. The engine reports 0x3, and the native kernel reports 0x2fb3ffff.
- Guest tests are built with `-mno-outline-atomics`. Without it gcc 14 emits calls to `__aarch64_ldadd*` helpers that `-nostdlib` cannot link.
- Test loops are written so gcc emits no NEON code (volatile indices and volatile byte accesses). The engine rejects vector data processing.
- Accesses with address bits 48 to 55 set (after masking the top byte) are a segfault, exit 139.
