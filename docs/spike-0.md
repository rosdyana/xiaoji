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

The build runs on a remote x86_64 machine: i7-12700 with 20 threads, 64 GB RAM, Ubuntu 20.04, in `~/xiaoji-aosp`. It's a shallow, partial-clone sync of `android13-release`, followed by a baseline `sdk_phone64_arm64` build (the same product as the prebuilt guest, build `TE1A.220922.034`, `sdk_phone64_arm64-userdebug`) and then a trimmed `vendor/xiaoji` product. That product sets `low_ram`, flattens APEXes, and keeps only Launcher3, SystemUI, Settings, PackageInstaller and DocumentsUI.

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

## M1 results

M1 adds the guest filesystem (`engine/vfs.c`), the fd table, PT_INTERP loading,
file mappings, the M1 syscalls (`engine/sysfs.c`, `engine/syscall.c`) and the
AdvSIMD and FP instructions that bionic and toybox execute. The exit check passes:
`build/xiaoji --root build/rootfs -- /system/bin/toybox echo hi from bionic`
prints `hi from bionic` and exits 0.

Conditions for the timings: `pmset -g` showed `lowpowermode 1`, and the Mac was on
"Battery Power" (about 52%, discharging).

### Test summary (`make test`: 40 passed, 0 failed, 0 xfail)

| Case | Engine (macOS) | Node (wasm) |
|---|---|---|
| M0 programs: `args`, `arith`, `control`, `hello`, `memory` (O0, O2) | PASS | PASS |
| `glibc_hello-O2` (XFAIL in M0; the SIMD it needed is now implemented) | PASS | PASS |
| `simd-O0`, `simd-O2` (787 result lines each, identical to native) | PASS | PASS |
| `toybox-echo`, `-ls`, `-uname`, `-wc`, `-sha`, `-lsdir`, `-cat` | PASS | PASS |

Per runner: 20 passed, 0 failed. The 20 are 10 M0 programs, `glibc_hello-O2`,
the two SIMD programs and the seven rootfs cases. `sh-arith` (M2) and the `hello`
cases (M3) are not in the pass list.

The `-Wall -Wextra` check: `clang` and `emcc` both build every engine file with no
warnings (`-std=c11`). The Makefile's emcc lines do not pass `-Wall -Wextra`, so the
check was run by hand per file.

### `toybox sha256sum` (`XIAOJI_STATS=1`, linker64, 3 runs each)

| Runner | Instructions | Time | Rate |
|---|---|---|---|
| Native (macOS arm64, `build/xiaoji`) | 67,592,270 to 67,592,781 | 0.758 to 0.759 s | 89.0 to 89.1 M instructions/s |
| Node (`build/wasm-node/xiaoji.js`) | 67,592,310 to 67,594,690 | 1.270 to 1.295 s | 52.2 to 53.2 M instructions/s |

The output matches the oracle hash `8b01da90...`. The instruction counts vary by a
few hundred because of `getrandom` and the allocator's randomized placement.

### Syscall trace against the oracle

Both `toybox echo` and `toybox sha256sum` were traced with `--trace` and compared
with the oracle straces, by name and by the sign of the return value, after the
toybox `execve`:

- The file-related syscalls (`openat`, `close`, `fstat`, `newfstatat`, `readlinkat`,
  `read`, `pread64`, `write`) match the oracle, except as listed below.
- `prctl(PR_SET_VMA)` returns 0 in the engine and -EINVAL in the oracle. The plan
  specifies 0. It does not change output.
- `prctl(PR_GET_DUMPABLE)` (10 calls) and `prctl(PR_SET_TAGGED_ADDR_CTRL)` (1 call)
  return -EINVAL, which is the plan's rule for other options. The oracle returns 1 and
  0. Output does not change.
- The oracle opens `/proc/filesystems`, which the engine answers with ENOENT (the
  plan says every `/proc`, `/sys` and `/dev` path other than the listed synthetic
  files returns ENOENT). Output does not change.
- Two anonymous `mmap` calls fall in a different order, and one `munmap(addr, 0)`
  is EINVAL in the oracle while the engine's matching call has a non-zero length.
  This is allocator ordering (scudo), not a wrong result, and was not chased
  further.

### AdvSIMD and FP families implemented

Each family follows the Arm ARM encoding fields and has a case in
`tests/guest/simd.c`, which `make test` compares with native.

Listed in the plan:
- LD1/ST1 multiple structures (one to four registers, immediate and register
  post-index).
- DUP (element and general), INS (element and general), UMOV, SMOV.
- MOVI, MVNI, ORR and BIC (vector immediate). FMOV (vector immediate, cmode 1111)
  is not implemented.
- AND, BIC, ORR, ORN, EOR, BSL, BIT, BIF.
- ADD and SUB (vector).
- CMEQ, CMGT, CMGE, CMHI, CMHS, CMTST (register); CMEQ, CMGE, CMGT, CMLE, CMLT
  (zero).
- ADDP, UMAXP, UMINP.
- ADDV, UMAXV, UMINV, UADDLV.
- SHRN and SHRN2, USHR, SSHR, SHL.
- XTN and XTN2.
- UZP1/2, TRN1/2, ZIP1/2, EXT, TBL and TBX (one or two table registers).
- REV64, CNT, NOT.
- Scalar FMOV between general and SIMD registers (S, D, and the D[1] lanes).

Added on demand, beyond the plan's expected list (each hit by toybox, the linker or
libc, then checked):
- Scalar floating point, default FPCR only: FADD, FSUB, FMUL, FDIV, FCMP and FCMPE
  (register and zero), SCVTF and UCVTF (from W or X to S or D), FCVTNS, FCVTNU,
  FCVTPS, FCVTPU, FCVTMS, FCVTMU, FCVTZS and FCVTZU (to W or X from S or D). NaN
  operands propagate (the first NaN, quieted) and generated NaNs use the default
  NaN. Saturation and NaN-to-zero follow the Arm ARM. A non-default FPCR (rounding
  mode, FZ, DN or AHP) returns -1.
- SSHLL, USHLL, SXTL, UXTL and their 2 forms.
- MUL, MLA and MLS (vector).
- UMLAL and UMLAL2 (widening multiply-accumulate).
- PMULL and PMULL2, 8-bit polynomial form only. The 64-bit form (crypto extension)
  is not implemented.
- SADDLP and UADDLP.
- SMAX, SMIN, UMAX and UMIN (vector).
- LD1 and ST1 of one lane (B, H, S, D, with or without post-index), and LD1R.
- CRC32 and CRC32C (eight forms, integer). They are not in the plan, and
  AT_HWCAP does not advertise them, but bionic's allocator (scudo) uses CRC32C
  unconditionally.

### Notable findings

- **AT_HWCAP does not advertise CRC32.** AT_HWCAP has only FP and ASIMD. CRC32C is
  not baseline (it is FEAT_CRC32), but scudo's allocator uses CRC32C unconditionally,
  so it was implemented as hit. The 8-bit PMULL is baseline AdvSIMD; the 64-bit form
  needs FEAT_PMULL and is not implemented. The 8-bit PMULL was hit in GHASH code in
  `libcrypto`, which runs while the library loads (toybox loads it).
- **FPSR cumulative flags are not updated** by the scalar FP ops. I did not check
  whether any path reads FPSR. FPCR is honoured only in its default configuration;
  any other setting returns -1.
- **The decoder accepted some encodings from neighbouring groups.** A review found
  that scalar AdvSIMD (under the scalar FP prefix), vector-by-element (under the shift
  group) and copies with non-zero size bits reached the wrong handlers. Reproduced
  with `.inst` in a scratch guest (`sqxtun b0, h1` ran as FADD), then fixed: those
  now return -1.
- **Some paths are not exercised by any test case.** The seven rootfs cases never
  dispatch `madvise`, `statx`, `faccessat2`, `readv`, `lseek`, `getcwd`, `chdir`,
  `fchdir`, `dup3`, `kill`, `tgkill`, `tkill`, `msync`, `mlock`, `munlock`,
  `sched_yield`, `getpriority`, `setpriority`, `getpgid`, `getppid` or `set_robust_list`.
  The synthetic `/dev` files, `/proc/self/cmdline` and the `seekdir` resume in
  getdents64 are also untested. They are implemented and reviewed but not run.
- **uid mismatch.** `getuid` (and `getpid` and the other ids) return 1000 as in M0,
  while stat reports uid and gid 0 as the plan asks. The sign-only trace comparison
  cannot see this.
- **Host calls outside the plan's list.** `vfs.c` uses `telldir`, `seekdir` and
  `rewinddir` (and `_XOPEN_SOURCE 700` for them). They work on macOS and Emscripten
  but are not in the plan's list and sit outside `host.c`.
- The linker's `std::unordered_map` rehash computes a float load factor. That is
  why scalar FP was needed before `toybox echo` could run at all.
- Under emscripten, `ENOTSUP` and `EOPNOTSUPP` are the same value, so the errno
  table guards the second label.
- The region list (`engine/mem.c`) replaces the page walk in `mem_find_free`.
  Without it, each `mmap(NULL)` would scan the allocator's multi-GiB reservations.

## M2+M3 results

Stages, as in the M2+M3 plan:

| Stage | Status |
|---|---|
| 1. Deterministic Hello, sh-arith, gettimeofday, root uids | done: `sh-arith` PASS natively and in Node |
| 2. Green threads, clone, futex, sleeps, `tests/guest/threads.c` | done: `threads-O0/O2` PASS natively and in Node; deadlock path checked by hand (exit 134) |
| 3. Signals and synchronous faults | done (commit `5ff89b8`): `signals-O0/O2` PASS natively and in Node |
| 4. ART syscalls, /proc, instructions for `-Xint` | done (commit `45a9f28`): `hello-int` PASS natively and in Node |
| 5. JIT run, memfd dual mapping | done: `hello-jit` PASS natively and in Node; `memfd-O0/O2` PASS |

`make test` (`tests/run.sh`) after stage 5: **58 passed, 0 failed, 0 xfail**. Before stage 5 it was 52 passed and 2 failed (`hello-jit`, native and Node).

Stage 5 changes:
- `engine/memfd.c`, `engine/memfd.h`: a memfd object. Its pages are host pages, materialized on first use (a 64 MiB memfd costs only the pages that are touched). The object is refcounted: one reference per open guest fd and one per mapped region that names it. The pages are freed only when the last reference goes, so `close()` with live mappings is safe.
- `memfd_create` [279] and `ftruncate` [46] (`engine/sysfs.c`). memfd fds support read, write, pread, lseek, fstat, dup and close (`engine/vfs.c`, new `VFS_MEMFD` kind). MFD_CLOEXEC is accepted; sealing is not modelled.
- `mmap` of a memfd (`engine/syscall.c`, `mmap_memfd`): MAP_SHARED maps the memfd's own pages. MAP_PRIVATE copies the contents. `mremap` growth of a shared view continues the same memfd at the next offset (`map_tail`).
- `engine/mem.c`: shared page-table entries (`PTE_SHARED`) point at the memfd's host pages without owning them. The first touch of a shared entry with no host page yet takes the page from the memfd at the region's offset (`page_lookup`). `munmap` clears the entry and leaves the page to the memfd. `mprotect` keeps the shared bit. `madvise(MADV_DONTNEED)` (`mem_drop`) skips shared pages, so contents survive. Regions carry the memfd reference (`region_t.shm`), and `mem_move` (mremap) carries it too.
- Cache maintenance (`engine/cpu_a64.c`, system-instruction space): DC ZVA is implemented (it zeroes the 64-byte block). DC and IC maintenance (CRn 7, op1 0 or 3) and TLBI (CRn 8) are NOPs. There is no decode cache, so freshly written code runs at once. `tests/guest/memfd.c` checks this with the dual-view pattern: write code through the RW view, maintain the caches, run through the RX view, rewrite, run again.
- Refcounting is per memfd, not per host page. The memfd owns its pages and is refcounted by open fds plus mapped regions. Each page-table entry points at a page without owning it. The brief asked for refcounted host pages that several entries point at; this is the same sharing with one owner, which is simpler and frees everything in one place.
- `madvise(MADV_DONTNEED)` on a memfd view: the region path is a display name (`/memfd:<name> (deleted)`), not a host file, so the refill step is skipped for memfd regions. Before this, the refill would have tried to open that name. The current hello-jit trace does not reach this case (all 578 madvise calls returned 0). `tests/guest/memfd.c` checks it against native.
- `DCZID_EL0` now reports `0x4` (64-byte blocks, DZP 0), as the native arm64 VM does. It reported `0x10` (DC ZVA prohibited) before, which contradicted the DC ZVA implementation. `CTR_EL0` still differs from native in bit 28 (IDC) and was left as it was, since the plan said it already works.
- `PR_SET_VMA` returns `-EINVAL`, as the oracle does, so the engine does not keep anonymous VMA names. The plan asked for those names in `/proc/self/maps`, which conflicts with the oracle kernel; this was left for the parent to decide.
- Instruction family added: `ADDP Dd, Vn.2D` (`0x5E`, size 11, scalar pairwise). It is the 64-bit integer pairwise add, not a floating-point add. The first version treated it as FP, and the native probe showed the mismatch: `addp(1.0, 2.0)` gives `+Inf` on hardware because the bit patterns are added. Checked in `tests/guest/simd.c` (`addp-d-int`, `addp-d-int-v2`).
- `XIAOJI_STATS=1` prints from an `atexit` handler (`engine/main.c`), so it also covers `exit()` calls (segfault without a handler, out-of-memory). Checked on these paths, natively and in Node: exit_group from a non-main thread (exit code 3, as native); a guest SIGILL handler that calls exit_group, and one that calls SYS_EXIT; normal exit of hello-int and hello-jit. Not verified: the path described under Known gaps (a crash-dump thread that ends silently), which the committed engine also fails to report.
- Syscalls from the oracle trace: no `unimplemented syscall` message appeared in the four measured runs. The ART trace uses memfd_create, ftruncate, socket (`-EACCES`), flock, getrlimit, prctl PR_SET_VMA (`-EINVAL`, as the oracle returns), and others already handled. FP and SIMD instructions beyond ADDP were not needed by hello-jit.

Measurements (`XIAOJI_STATS=1`, argv as in `tests/run.sh`, `pmset -g` at the time: AC Power, lowpowermode 0):

| Case | Runner | Instructions | Time | Rate | Hello loop (stderr) |
|---|---|---|---|---|---|
| hello-int | native | 7,113,715,042 | 12.825 s | 555 M/s | 3.691 s |
| hello-int | Node | 7,113,752,770 | 55.462 s | 128 M/s | 17.394 s |
| hello-jit | native | 7,665,306,893 | 9.385 s | 817 M/s | 0.359 s |
| hello-jit | Node | 7,665,237,414 | 38.391 s | 200 M/s | 1.514 s |

These are from the final build (`make test`: 58 of 58). Earlier builds differed by less than 0.003% in instruction count. Counts differ slightly between runs because some timers and futex timeouts depend on the wall clock. Node is slower than native for both cases. The JIT run executes more guest instructions in total, but its timed loop is much shorter, which shows the JIT working.

Known gaps:
- memfd: no seals (`F_ADD_SEALS`), no `SIGBUS` past the end of the file (pages past `size` read as they were), no hugetlb.
- File-backed `MAP_SHARED` of non-memfd files is still a copy. It is only read-only, as before.
- FP/SIMD still uses the default FPCR only (round to nearest, no flush-to-zero, DN=0), and FPSR exception flags and QC are not tracked (from M1).
- `fork` and `execve` are not implemented, and ART does not need them for Hello.
- **Open: a silent exit in bionic's crash-dump flow.** In the committed engine (`45a9f28`), hello-jit's first failure (an unimplemented instruction, delivered as SIGILL to bionic's handler) ends with status 1 and no `XIAOJI_STATS` line. The trace shows bionic print `libc: Fatal signal 4`, create a thread (tid 1001) that waits on a futex, and then the main thread resumes at pc `0x7ff0000f2420`. The process ends there without any engine message, and neither the `exit`/`_exit` interposer nor the scheduler paths printed. The exact exit point is not found. The current build no longer takes this path for hello-jit, so this is the one path where the stats line is still missing, and it is not verified.

Stage 1 and 2 changes:
- `tests/hello/Hello.java` prints the deterministic line to stdout and the loop time to stderr. `tools/oracle.sh` writes stderr to `build/oracle/<case>.err`, so `.out` is stdout only.
- `tests/run.sh` rootfs cases use arrays for argv and pass the `tools/guest-env.sh` variables as `--env`. New cases: `sh-arith`, `hello-int`, `hello-jit`.
- `Makefile`: `make test` depends on `wasm-node` (so a stale Node build cannot pass) and on `build/rootfs/data/hello.dex`.
- `engine/sched.c`, `engine/sched.h`: one `cpu_t` per guest thread, 200,000-instruction slices, round-robin, futex wait/wake/requeue/cmp-requeue with timeouts, nanosleep and clock_nanosleep, deadlock report (exit 134), per-thread names and clear_child_tid.
- `engine/syscall.c`: clone [220] (threads only; other clone returns -ENOSYS with a notice), futex [98], exit [93] per thread, exit_group [94] per process, set_tid_address, gettid, sched_yield, nanosleep [101], clock_nanosleep [115], gettimeofday [169], prctl PR_SET_NAME per thread. uid, gid and auxv ids are 0.
- `engine/host.c`: `host_sleep_ns` (nanosleep). It works under Node.

History: after stage 2, `make test` was 46 passed and 4 failed (the Hello cases stopped at FMOV, scalar immediate, `0x1e6c1009`). Stage 4 (commit `45a9f28`) brought `hello-int` to passing. Before stage 5 the `hello-jit` case stopped at `unimplemented syscall 279` (memfd_create) and then at `unimplemented A64 instruction 0x5ef1b800` (ADDP, scalar). Stage 5 fixed both, and then the JIT's DC CVAU, which the cache-maintenance NOPs cover.

## Profiling and corrected numbers (2026-11-22)

`XIAOJI_PROFILE=1` prints per-thread instruction and syscall counts, the top sampled hot spots (with the mapping's guest file and offset), and spin-like events.

What it found:

- **Earlier instruction counts and rates in this document are wrong.** A new thread copied its parent's instruction counter at clone time, so each of ART's seven threads restarted from about 950 M. The real count for hello-jit was 0.998 G, not 7.7 G, and the real rate is about 105 M instructions/s natively. Fixed in `sched.c`.
- **No spinning.** The main thread runs 99.8% of instructions. There were zero WFE, YIELD or failed STXR events, and all futex waits blocked properly. The syscall counts match the native oracle within 3–5%.
- **Most of the work was ART verifying framework DEX files.** 75% of samples were in `libdexfile.so` (`DexFileVerifier::CheckIntraStringDataItem`, `IsValidMemberName`). The cause was `-Ximage:/system/framework/boot.art`, which loads only the ART-module part of the boot image, so the framework jars on BOOTCLASSPATH had no compiled image and were verified from scratch. Booted Android loads the whole chain, so the harness now passes `-Ximage:/system/framework/boot.art:/system/framework/boot-framework.art`. The oracle was regenerated, and its output is unchanged.
- **Under Emscripten, `cpu_run`'s `setjmp` slowed the inlined interpreter loop about 4×.** `run_loop` is now `noinline`; native code is unchanged.
- **Exclusive monitor:** it is now cleared at every context switch, as on hardware.

Hello timings after these fixes, on AC with Low Power Mode off:

| Case | Runner | Instructions | Total | Java loop (1M iterations) |
|---|---|---|---|---|
| hello-jit | native engine | 297.5 M | 2.87 s | 342 ms |
| hello-jit | Node | 297.6 M | 2.90 s | 309 ms |
| hello-int | native engine | 652.4 M | 6.31 s | 3.71 s |
| hello-int | Node | 652.4 M | 5.64 s | 3.07 s |
| hello-jit | native ARM64 (oracle, VM) | — | 0.39 s | 6–9 ms |

The engine interprets about 105 M guest instructions/s. The remaining 7× gap to native on startup, and about 40× on JIT-compiled loops, is what the spike-1 translator targets. For hello-int, the profile shows ART's switch interpreter (`ExecuteSwitchImpl`) as the largest hot spot. The dynamic linker's symbol lookup costs about 130 M instructions on every start; a snapshot would remove it.


## Spike 1: block translator, wasm build (T0 and T1)

The wasm build translates hot A64 blocks to WebAssembly. Files: `engine/jit.c` (translator, wasm builds only), `engine/jit.h`, `engine/jit_lib.js` (`--js-library`: compiles each batch and installs its functions in the table). Hooks in `engine/cpu_a64.c`, `engine/mem.c`, `engine/memfd.c` and `engine/main.c` are `#ifdef __EMSCRIPTEN__`, and the native binary has no translator symbols. The Makefile leaves `jit.c` out of the native build and adds the JS library and `-sALLOW_TABLE_GROWTH` to both wasm targets. Tests: `tests/guest/jitstress.c`.

Design as built:

- **Blocks.** A block starts at a pc reached by a non-sequential step (a branch, a block exit, or the instruction after one the translator does not handle). It runs up to 64 instructions, never crosses a 4 KiB page, and ends at a branch, at an untranslated instruction (SVC, system, SIMD&FP, exclusive and ordered accesses, unknown encodings), or at the limit. Entries are counted in a lossy 4096-entry table; at 1000 entries the block is queued, and queued blocks are compiled in batches of up to 64 per module (or after 8192 later misses). A pc whose first instruction is untranslated is recorded as such, so it is not retried.
- **Keys.** A block is keyed by the host address of its first instruction and the guest VA page. A host page mapped at two VAs (ART's memfd dual mapping) therefore gives two blocks. Each block is on its host page's list, for invalidation.
- **Generated code.** One wasm function `(i32 cpu) -> i32` per block. It loads the guest registers it touches into locals on entry, keeps NZCV in a local (flags are computed eagerly), and stores the written registers back on exit. The return value packs the instruction count (bits 0 to 7) and an exit kind (bits 8 and up): NEXT (the pc is the next instruction to run: an untranslated one, or the length limit), JUMP (a branch, taken or not, or an indirect branch), SMC (a store changed translated code, and the block leaves after that store). The pc is in `cpu_t`.
- **Memory.** Loads and stores probe the interpreter's own TLB inline (the same tag and page fields, read or write array), for accesses within one page, with the address untagged first. A miss, a page crossing, or a write to a page holding code goes to `jit_ld`/`jit_st`, which set the faulting pc and call the interpreter's `mem_*` functions. Code pages are never inserted in the write TLB, and the write TLB is flushed when a page first gets code.
- **Faults.** A fault inside a block longjmps through the generated frames to `cpu_run`, which sets the pc from the helper's argument, as the interpreter does. The registers are stored before each helper call, so the state seen by a signal handler is exact. `jit_fault` charges the block's instructions up to and including the faulting one.
- **Invalidation.** Every write path into a code page (the TLB slow path, `mem_poke`, memfd writes and truncation) and every unmap, drop, protect, move and memfd free calls `jit_page_changed` on the host page first. Invalidated blocks are marked and never reused, and no table slot is reclaimed.
- **Dispatch.** The interpreter loop asks for a block at each non-sequential pc. A block that ran returns to the loop at its exit pc. If the instruction at the pc is untranslated, the interpreter runs it and the next pc is also asked about. A block translated during a dispatch runs at that dispatch.
- **Budget and signals.** A block charges `icount` and the slice budget on exit. Pending signals are checked before each dispatch, so they are delivered at block boundaries.
- **Switches.** `XIAOJI_JIT=0` disables dispatch. `XIAOJI_STATS=1` prints the translated fraction, blocks, modules, refused and untranslatable starts, compile failures, block runs, faults and self-modifying exits in translated code, and invalidations.

Not in T0/T1: block chaining (T2), SIMD&FP and exclusives (interpreted; they end blocks), lazy flags, and register allocation beyond the entry and exit loads. Translation stops at 16384 blocks or 6144 host pages (further hot blocks are not translated), and there is no reclamation of invalidated blocks.

Longjmp: this emcc (7.0.0) defaults to `SUPPORT_LONGJMP` in its `emscripten` mode when `-fwasm-exceptions` is not given, which this build does not use, so no flag was changed. The fault tests below, with faults inside translated code, show that the longjmp unwinds through the generated frames and `cpu_run` recovers the right pc.

Verification (2026-11-22, AC power, Low Power Mode off):

- `make && make wasm && make wasm-node && make test` from clean builds: **62 passed, 0 failed, 0 xfail** (the 58 cases plus jitstress at O0 and O2 on both runners).
- `XIAOJI_JIT=0 tests/run.sh`: 62 passed.
- Threshold-1 build (`-DJIT_HOT=1`): 62 passed. Every entry queues its block, and the batch compiles when 64 are queued (the periodic flush never fires in this build).
- Instruction counts are identical with the JIT on, off, and at threshold 1 for jitstress, control, arith, memory, signals, simd, glibc_hello and hello. `threads` varies between runs even with the JIT off (1982409 to 1982427; the cause was not investigated), so its count is not a check.
- `jitstress` covers: a hot inline-asm kernel of ALU ops, flags, shifts, bitfields, multiplies, divides and every integer load and store form, including logical forms with Rd = 31 (which write the zero register, not SP); hot branches and calls; rewrites of a hot function in an RWX page, after mprotect RW and RX, and after munmap and mmap; faults in the middle of hot blocks, where the handler checks the registers written before the fault, the saved NZCV, the pc offset, and the fault address (for a load and a store); a fault in a hot leaf; and a translated block that stores into its own page and then runs the new instruction. Output matches native. In the default build it records 3 faults in translated code, 1 self-modifying exit, and 5 pages and 5 blocks invalidated.
- The native binary has no translator symbols; clang with `-Wall -Wextra` reports no warnings on the native build, and `emcc -c -Wall -Wextra` reports none for each engine file.

Hello (in-engine time from `XIAOJI_STATS=1`; hello-int three runs per mode, hello-jit six per mode; instruction counts are the same with and without the JIT):

| Case | JIT | Instructions | Engine time, runs in order | Median | Translated instructions | Blocks / modules |
|---|---|---|---|---|---|---|
| hello-int | on | 652.5 M | 3.94, 3.83, 3.66 s | 3.83 s | 91.5% | 1960–1973 / 354–356 |
| hello-int | off | 652.4 M | 5.93, 6.55, 5.79 s | 5.93 s | 0% | — |
| hello-jit | on | 297.5 M | 2.20 to 2.58 s (six runs) | 2.27 s | 84.0% | 1655–1658 / 323–327 |
| hello-jit | off | 297.5 M | 2.88 to 3.58 s (six runs) | 3.15 s | 0% | — |

Speedup, median against median: hello-int 1.55×, hello-jit 1.39×. Against the Node row of the profiling section above (5.64 s for hello-int and 2.90 s for hello-jit, measured before the translator), the gains are about 1.5× and 1.3×. Run-to-run spread on this machine is large, so treat these as a range of roughly 1.3× to 1.6×.

Power state (`pmset -g batt`): "Now drawing from 'AC Power'". `pmset -g` shows lowpowermode 0 and displaysleep 120.

Notes: at about 5.5 blocks per module, JS compilation is a visible cost (about 355 modules per hello-int run); larger batches and chaining are the next steps, not tuned here. Hello leaves about 150 to 190 starts untranslatable per run; most are HINT-space NOPs and barriers, which end a block, as the plan specifies.

Not exercised by any test: `mremap` (it takes the same invalidation path through `mem_move`, but no test runs it on its own), the `XIAOJI_PROFILE` path (dispatch is skipped when the profiler is on), and the web build (`make wasm` compiles, but it has not been run in a browser).

Left for T2: block chaining, translating the HINT-space NOPs and barriers instead of ending blocks there, a larger batch policy, lazy flags, SIMD&FP translation, register caching across blocks, and reclamation of invalidated blocks and table slots.

### T2 (part 1): block chaining

Done: plan item 1, block chaining with wasm tail calls. Not done: items 2 (tried and reverted, below), 3 (FP and SIMD), 4 (lazy flags), 5 (reclamation of invalidated blocks and table slots, and the cap stop).

What it does. Each block charges its `icount` and a slice-budget word (`jit_budget`, a static at a fixed address) as it exits. An exit with a known or runtime target (direct, conditional, fall-through, BR/BLR/RET) tail-calls the module's `chain_fn` (a first function of every module, with a table import `e.t`). `chain_fn` returns to C with no work unless the budget is positive, `stop` is clear, no signal is pending, and `fast[]` has the next pc. On a hit it tail-calls the block's table index with `return_call_indirect`. Otherwise it returns `EXIT_BAIL`, and the dispatcher runs as before. SMC exits never chain. Module layout and the chain code are emitted only when the browser validates a `return_call` module (the same probe as xiaonux's `js_tail_ok`), so older browsers get the T1 code. `XIAOJI_CHAIN=0` turns chaining off, for A/B runs.

Cache design. `fast[]` has 4096 entries keyed by the guest pc (entries hold pc + 1, so zeroed memory is empty), and only untagged pcs are cached. The dispatcher keys blocks by host address and VA page; the cache keys by guest pc, so its validity rests on two rules: `tlb_flush` (called on every change of the mapping, including unmap of shared memfd pages, which do not reach `jit_page_changed`) clears the whole cache, and `jit_page_changed` clears the entries of the blocks it drops. Blocks that are invalid are never entered, because their entries are gone.

Jitstress additions (`tests/guest/jitstress.c`), all checked against native output:
- `hint_loop`: NOP, DMB, DSB, BTI and ISB in a hot loop (passes on the interpreter path; the translator still ends blocks there, see below).
- `chain_loop`: a hot loop that leaves each block by a conditional branch, a direct branch, an indirect call through a table, and a return.
- `smc_chain`: a loop that rewrites a function it calls, in the middle of the loop; the sum is exact.
- `fault_chained`: a leaf (`jitstress_chain_load`: three ALU ops, then the faulting load) that the loop calls twice as often as the warm-up needs; the fault is on the last pass. The handler's pc and address and the exact sum are checked, and the icount matches the JIT-off run.
- `thread_chained`: two threads, each running a long chained loop; the child's result must equal the parent's.

Open items in this part, found while checking the tests (not fixed):
- The per-hop `run_start` store in `chain_fn` is not yet shown to matter. With the store removed, the icount of `fault_chained` did not change. The trace shows why for the current loop: the faulting block is entered from the C dispatcher (which sets `run_start`) rather than from a hop, because early in the loop, blocks L and R of the call site are not yet translated or chained. A bite test that proves the store needs a call site that is chained from its first pass.
- Early in the `fault_chained` loop, the call site dispatches back to C about once per iteration until its blocks are translated and cached. Block translation is batched (64 queued blocks, or 8192 misses), so a hot block can wait for a long time. This is existing T1 batching, but it dominates the chain misses in this test (about 3700 C dispatches to the leaf per run). Chain hits are still 3.5M in this test.
- Native self-modifying code in `smc_chain` was flaky (SIGILL on a fresh RWX page, 50% of runs) until the initial writes were followed by the same I-cache sync as the rewrite. The engine was not the cause.

Measurements (Node, 5 runs each, medians; AC power; Low Power Mode off; `pmset -g` shows `lowpowermode 0`, `displaysleep 120`). Engine time is the `instructions in X s` line of `XIAOJI_STATS=1`.

| Build | hello-jit | hello-int |
|---|---|---|
| T1 start (before T2) | JIT on 2.582 s, off 3.087 s | JIT on 3.741 s, off 6.031 s |
| After item 1, chaining on | JIT on **1.944 s** | JIT on **2.889 s** |
| After item 1, `XIAOJI_CHAIN=0` (same session) | JIT on 2.258 s, off 2.978 s | JIT on 3.848 s, off 5.993 s |

Item 1 against its own A/B control: hello-jit 2.258 to 1.944 s (about 14% faster), hello-int 3.848 to 2.889 s (about 25% faster). Chain hits: 106.4 M in hello-int (10.6 M block runs of 28.7 M dispatches) and 43.5 M in hello-jit. Translated share: 91.5% (hello-int) and 84% (hello-jit), 1949 blocks in 354 modules (hello-int) and about 1650 blocks in about 325 modules (hello-jit). Run-to-run spread on this machine is about 10%.

Item 2 (HINT-space NOPs, barriers other than CLREX, PRFM): tried, no measurable gain (hello-jit 1.944 to 1.957 s, hello-int 2.889 to 2.889 s, translated share 83.9% to 84.1%), so it was reverted. PRFM was already translated in T1. The hot paths of Hello rarely reach these encodings; the untranslatable starts are cold.

Instruction counts are identical with the JIT on and off for jitstress, control, memory, simd and glibc_hello.

Gates at this state: `make test`: 62 passed, 0 failed, 0 xfail; `XIAOJI_JIT=0 tests/run.sh`: 62 passed; threshold-1 build (`-DJIT_HOT=1`): 62 passed. emcc `-c -Wall -Wextra` for `jit.c` and `mem.c`: no warnings; clang native `mem.c`: no warnings.


### T2 (part 2): FP/SIMD, reclamation, and what did not help

Done: item 3 (FP and SIMD), item 5 (reclamation), and both open issues. Item 4 (lazy flags) was built, measured and reverted, since it did not help. Item 2 stays reverted. HEAD for these numbers is `b2b71cd` (item 1).

Deviations from the plan:
- Item 4 is reverted, as the plan allows when it does not help.
- The jitstress thread section moved to a new file, `tests/guest/jitthreads.c` (reason below).
- `tests/run.sh` has a new instruction-count check: the deterministic cases must count the same with the JIT on and off.
- The batch and capacity constants are `-D` overridable, with the same defaults.
- FP arithmetic is not translated to wasm f64. It goes through the interpreter's own code (see item 3).

**Bite test for chained-fault pc correctness (issue 1).** `fault_chained` now calls the leaf through `jitstress_chain_run`, an asm loop that selects the pointer with `csel`. Every pass runs the same blocks, and the last pass enters the faulting block through a chain hop. The old C ternary put the bad pointer in a separate block that first ran on the last pass, and the dispatcher entered it, which set `run_start` and hid the bug. `run_start` is read only by `jit_fault`, so the test sees it through the instruction count. The count check is new in `tests/run.sh`. Evidence on the final program (store removed, then restored):

| jitstress | JIT on | JIT off | run.sh |
|---|---|---|---|
| O0, store removed | 115922199 | 115922202 | FAIL |
| O2, store removed | 30967366 | 30967369 | FAIL |
| O0, restored | 115922202 | 115922202 | PASS |
| O2, restored | 30967369 | 30967369 | PASS |

With the store removed, `run.sh` gives 78 passed and 2 failed (both jitstress icount lines). Restored, it gives 80 passed.

**Thread counts and the count check.** The jitstress thread section waited with `sched_yield` in a loop. Its JIT-on count gave 157 in one run and 190 in another, while the JIT-off count stayed at 190 across three runs. The section moved to `jitthreads.c`, and the main thread waits on a futex. The first version of that wait was `while (!child_done) futex_wait`, which is stable in five runs per mode. The version that always makes the call, `do { futex_wait } while (!child_done)`, gives 25625244 at O0 and 10517263 at O2 in every run, with the JIT on and off (five runs each). `jitthreads` is in the count check. The working explanation is the race over whether the child has finished when the main thread checks, which the always-call form removes. I did not isolate the jitstress variation fully. The `threads` case still varies between runs (4809066 and 4809122 with the JIT on), is not checked, and was not investigated.

**Batch policy (issue 2): measured, not changed.** Hello medians, 5 interleaved runs, on the pre-item-3 build:

| Variant | hello-jit, JIT on | hello-int, JIT on | jitstress C dispatches |
|---|---|---|---|
| default (batch 64, tick 8192) | 2.123 s | 2.932 s | 410,757 |
| tick 512 | 2.171 s | 3.102 s | 397,108 |
| batch 16, tick 512 | 2.111 s | 3.082 s | 397,108 |
| batch 1 | 2.268 s | 3.238 s | 387,869 |

Compiling sooner removes only about 6% of the jitstress dispatches, and no Hello median improves, so the default stays. Hello's C dispatches are not mainly batch waits.

**Item 3: FP and SIMD in translated code.**
- The x111 group (scalar FP and AdvSIMD data processing) and LD1/ST1 of structures (multiple and single) run through `cpu_exec_simd` (`cpu_a64.c`, wasm builds only). It calls the simd.c entry points (`simd_exec`, `simd_ldst_multi`, `simd_ldst_single`) directly, so the results are the interpreter's, bit for bit, by construction.
- Translated inline: LDR and STR of B, H, S, D and Q (unsigned offset, pre-index, post-index, register offset), SIMD&FP LDP and STP of S, D and Q (all index modes), and SIMD literal loads. The interpreter's V branches (`ldst_single`, `ldst_pair`, `ldst_literal`) were read and mirrored: the same scales, the same zeroing of the high word of a scalar load, and the same access order. A Q access writes its low word before it reads or writes the high word, as `mem_read128` and `mem_write128` do, so a fault in the high half leaves the same register state.
- Not translated: the unallocated encodings, which end the block, and the interpreter raises SIGILL for them.
- Synchronisation: the GPRs and NZCV written so far are stored before each helper call. The GPRs, SP and NZCV the call may have written are reloaded after it. `v[]`, FPCR and FPSR are never cached. A store that changes translated code exits the block (SMC). An encoding the engine does not implement takes the interpreter's SIGILL path from inside the block: the block charges the instruction, the handler runs, and the block leaves.
- jitstress coverage (`tests/guest/jitstress.c`): `jitstress_fpsimd` runs 24000 passes of scalar FP (arithmetic, FMA, compare and select, conversions both ways, FP and GPR moves, FRINTZ), SIMD (DUP, ADD, CMEQ, ADDP, UMAXP, AND, ORR, EOR, MOVI, INS, UMOV, LD1 and ST1, Q loads and stores) and NZCV. NaN inputs are excluded (see below); zeros and infinities are included, and NaN results (0/0, infinity minus infinity) still occur. `vec_mem_hot` covers every scalar size and index form, the pairs, and the literal. `fault_vec_hot` faults inside a hot SIMD load (inline path), and `fault_ld1_hot` faults inside a hot LD1 (the helper path, through the longjmp). `unimpl_hot` runs an unallocated encoding in a hot block (native SIGILL, 12000 times). All outputs match the native oracle on the native engine and on the Node interpreter (`XIAOJI_JIT=0`), and on the translator.
- NaN check (the gate's NaN clause): `fpdiag`, the NaN-inclusive generator of the 24000 passes, gives identical output under Node with the JIT on and off (91.0% of its instructions translated). Against native it differs only in the FMSUB case below.
- Measured: the first version of `cpu_exec_simd` called `exec`. It gave hello-jit 1.999 s to 1.946 s (5 runs), hello-int 2.987 s to 2.985 s (8 runs), with C dispatches down 65% (hello-jit, 19.8M to 6.4M) and 47% (hello-int, 28.8M to 15.2M). That version is discarded: it slowed the JIT-off interpreter by 27 to 35% (see the performance finding), so its timings are only evidence for the mechanism.

Interpreter findings, reported and not fixed (the test avoids them):
1. `simd.c` implements FP only with the default FPCR. A non-default RMode, FZ, DN or AHP makes the instruction unimplemented (SIGILL), and the cumulative FPSR flags are not updated (simd.c, line 89). So rounding modes, flush-to-zero, default-NaN mode and FPSR cannot be compared with native. The jitstress FP kernel runs only the default FPCR.
2. FMSUB with a NaN in `Dn` flips the NaN's sign. Example from the scratch diagnostic: `d0 = 0x7ff8000000000001`, `fmsub d18, d0, d1, d4` gives `0x7ff8000000000001` on native and `0xfff8000000000001` in the engine (interpreter and translator alike). The jitstress FP inputs exclude NaN operands.

Performance finding: the first version of `cpu_exec_simd` called the static `exec`. Making it `always_inline`, or calling it through a `noinline` wrapper, still slowed the JIT-off interpreter by 25 to 35% (hello-int off, 6.2 s to 8.5 s). Any second call site of a static dispatch function in `cpu_a64.c` changes how `run_loop` is compiled. A helper that calls only the simd.c entry points, with `exec` untouched, does not slow it (the stubbed variant matches the final build).

**Item 4: lazy flags (tried, reverted).** NZCV was deferred into dedicated locals (the operands for SUB and ADD, the result for logic), computed at block exits and slow paths, and compared directly for the conditions that allow it. Interleaved, 8 runs: hello-jit 2.115 s to 2.128 s, hello-int 3.175 s to 3.252 s. No gain: a block that sets flags still computes NZCV at its exit, so the only saving is for flag sets overwritten inside the same block.

**Item 5: reclamation.** Invalidated block records go on a free list, after being unlinked from the hash table. A record's table slot is `tbase + record index`, so the slot is reused with the record. Table entries are cleared only at a full flush. Pages are not removed one at a time. When a batch would pass the block or page cap, `translate_batch` drops everything (chain cache, hash, pages, blocks, table slots) and retranslates. The flush never runs from `jit_page_changed`, which can run under a live block. The tiny-cap build (`-DJIT_MAX_BLOCKS=256 -DJIT_PAGE_LIMIT=64 -DJIT_BATCH=16 -DJIT_HOT=1`) passes the whole suite, and hello-jit does 5034 full flushes there with correct output. The default build does none in these tests. Item 5 was not timed on its own: the final-against-HEAD delta below is items 3 and 5 combined, with the inline SIMD forms.

**Final numbers.** Node, engine time from `XIAOJI_STATS=1`, 8 interleaved runs each, medians (AC power, Low Power Mode off, load average 3 to 4.5):

| Case | HEAD `b2b71cd` | final | Range (HEAD / final) |
|---|---|---|---|
| hello-jit, JIT on | 2.096 s | 1.938 s (-7.5%) | 2.07 to 2.93 / 1.89 to 3.34 |
| hello-jit, JIT off | 3.045 s | 3.194 s (+4.9%) | 3.02 to 3.36 / 3.05 to 3.47 |
| hello-int, JIT on | 2.957 s | 2.850 s (-3.6%) | 2.94 to 3.06 / 2.77 to 2.93 |
| hello-int, JIT off | 6.264 s | 6.518 s (+4.1%) | 6.00 to 7.10 / 6.24 to 8.53 |

The JIT-off cost is real across sessions: +0.7% and +1.9% in an earlier session (3.052 s to 3.074 s and 6.220 s to 6.340 s), and +2.4% (6.131 s to 6.280 s) with a stubbed helper, which rules out the helper calls. Its source is not isolated. The likely cause is the code layout of the larger translator, but I did not verify that. The JIT-on gains are the same order in both sessions.

Counters, one run each (HEAD, then final):
- hello-jit: translated 83.9% then 87.3%; blocks 1677 in 321 modules then 1672 in 311; block runs 6.11M then 2.33M; C dispatches 19.75M then 6.36M; chain hits 43.5M then 45.7M.
- hello-int: translated 91.5% then 93.1%; blocks 1959 in 357 modules then 1957 in 343; block runs 10.61M then 6.76M; C dispatches 28.76M then 15.24M; chain hits 106.4M then 108.5M.

Hello's instruction count varies by about 0.1M between runs, with the JIT on and off (as noted above), so it is not used as a check.

Gates (final tree):
- `make`, `make wasm`, `make wasm-node`: exit 0. `make test`: 80 passed, 0 failed, 0 xfail. (Was 62 before: the count now includes jitthreads and the count checks.)
- `XIAOJI_JIT=0 tests/run.sh`: 80 passed, 0 failed.
- `-DJIT_HOT=1` build, full suite: 80 passed, 0 failed.
- Tiny caps (`-DJIT_MAX_BLOCKS=256 -DJIT_PAGE_LIMIT=64 -DJIT_BATCH=16 -DJIT_HOT=1`), full suite: 80 passed, 0 failed.
- Instruction counts, JIT on equal to JIT off, at O0 and O2: arith, control, jitstress, jitthreads, memory, signals, simd (`run.sh` icount lines).
- Warnings: clang `-Wall -Wextra` on the native sources (all but `jit.c`): none. `emcc -O3 -c -Wall -Wextra` on every engine file, `jit.c` included: none.
- The native build is unchanged apart from the wasm-only code, which is inside `#ifdef __EMSCRIPTEN__`. The native suite passes.

Power state (`pmset -g`): "Now drawing from 'AC Power'", `lowpowermode 0`, `displaysleep 120`. Timing caveat: during parts of the session, desktop apps (a Chrome helper at about 200% CPU) and a separate job of the same user (a `chunk.py` build with eight workers) loaded the machine. Timings taken in those windows were discarded, and every comparison is interleaved.

What remains:
- Hello still makes 6.4M C dispatches in the JIT-on run (45.7M chain hits). Batch timing was not the cause, and the rest was not analysed.
- FP arithmetic still goes through the helper. Direct wasm would need NaN, rounding and flush handling, and the interpreter is exact only for the default FPCR anyway.
- Register caching across blocks (the plan's optional item) was not started.
- Partial state for SIMD accesses that fault in the high half matches the interpreter by construction, but no test covers it: a test needs an unmapped page after a mapped one.
- The interpreter findings above (non-default FPCR and FPSR, FMSUB NaN sign) are open.
- The `threads` count variation is not investigated.
- The JIT-off cost of +2 to +5% is not isolated.
- The web build has not been run in a browser.

## M4 results: Hello in Chrome (web build)

**The exit criterion is met.** `dalvikvm64 -Ximage:… -cp /data/hello.dex Hello` (hello-jit, and hello-int with `-Xint`) prints `Hello from xiaoji ART (sum=2999997)` with exit 0 in all three places:
1. natively on macOS: `make test`, the `engine` rows for hello-jit and hello-int (PASS);
2. as wasm in Node: `make test`, the `node` rows (PASS);
3. as wasm in a headless Chromium page, run in a Web Worker: `make test-web` (PASS).

The web test compares stdout with `build/oracle/<case>.out` and the exit code with `.rc`, in a fresh browser process per case.

### How it is built

- **Web build** (`make wasm`, `build/wasm/xiaoji.mjs` and `.wasm`): `-sENVIRONMENT=web,worker -sMODULARIZE -sEXPORT_ES6 -sINVOKE_RUN=0 -sFORCE_FILESYSTEM -sEXPORTED_RUNTIME_METHODS=FS,callMain -sEXIT_RUNTIME=1`, plus the same memory, table and `jit_lib.js` flags as before. `-sEXIT_RUNTIME=1` lets the `XIAOJI_STATS` atexit handler run when `callMain` returns. `make wasm-node` is unchanged.
- **Manifest** (`tools/mkmanifest.py build/rootfs build/web/manifest.json`, a Make target): 4,671 entries, 3,520 regular files, 1.54 GB. Each entry has its type (dir, file, symlink), mode, size or symlink target, sorted by path. `/proc`, `/sys` and `/dev` keep their contents out, but the three directories stay as empty entries (see the first problem below). `/data/hello.dex`, `/linkerconfig` and `/apex/apex-info-list.xml` are required and checked. Files are not copied: the server serves them from `build/rootfs`.
- **Worker filesystem** (`web/worker.js`): fetches the manifest, imports `xiaoji.mjs`, and creates `/rootfs` in MEMFS. Directories use `FS.mkdir` and `FS.chmod`, symlinks use `FS.symlink` with the guest-absolute target, and regular files use `FS.createLazyFile(parent, name, url, true, false)` followed by `FS.chmod` with the manifest mode. Lazy files load with emscripten's synchronous XHR (1 MB Range chunks) from `/rootfs/<path>`. Then `callMain(['--root', '/rootfs', '--stats', --env …, '--', <dalvikvm64 argv>])`. Each run uses a fresh module instance. stdout and stderr go to the page through `Module.print` and `Module.printErr`.
- **Argv and environment** (`web/cases.js`): the same argv and the same six `--env` variables as `tests/run.sh` and `tools/guest-env.sh`. `tests/web.mjs` checks the env values against the script on every run.
- **Server** (`web/serve.mjs`, `make serve`): Node, no dependencies. Serves `web/`, `build/wasm/`, `build/web/manifest.json` and `/rootfs/` from `build/rootfs`. Handles HEAD and single `Range` requests (206 with `Content-Range`, 416 when unsatisfiable), refuses paths that leave their root, sends `Content-Type` for `.mjs`, `.wasm` and `.json`, and sends `Cache-Control: no-store`. `--coop` adds COOP and COEP headers and is off by default; the engine does not use SharedArrayBuffer yet, so it is not needed.
- **Page** (`web/index.html`, `web/main.js`): a case selector, Run, stdout and stderr panes, and a timing table. Each Run starts a new worker, which is terminated when the run ends. No framework, no build step.
- **Test** (`tests/web.mjs`, `make test-web`): starts the server on a free port, runs the cases given on the command line (`make test-web` runs hello-jit and hello-int), and reads `window.xn`. Playwright is imported from the xiaonux bench install (`XN_PLAYWRIGHT` overrides the path), with the cached Chromium. Not part of `make test`.

### Engine changes

- **No adapter in `host.c` or `vfs.c`.** The engine's own FS calls (`newfstatat`, `readlinkat`, `openat`, `pread64`, `fstatfs`, as seen in the trace) worked on MEMFS, lazy files and symlinks as they are. `getdents` is not exercised by Hello.
- `engine/main.c`, `EM_JS xn_run_start`: before `sched_run`, calls `Module.onRunStart` if the page set it. Node and native builds have no such hook, so nothing changes there. It is how the worker measures time to first instruction.
- `engine/main.c`, `--stats`: the same as `XIAOJI_STATS=1`. Added because emscripten reads the `ENV` for `getenv` from a module-local variable, so `ENV` in the module config has no effect. Native and Node behaviour is unchanged; the option is only new.

### Problems found on the way

1. `/proc/self/exe` failed with ENOENT. The engine resolves guest paths one component at a time in the host tree, so `/rootfs/proc` has to exist. The first manifest left out `proc`, `sys` and `dev` entirely. The fix keeps those three directories as empty entries; their contents are still synthesized by the engine. After that, hello-jit exits 0.
2. The `ENV` key in the module config is ignored (see above). Fixed with `--stats`.
3. `XIAOJI_STATS` is printed from `atexit`, which does not run when `callMain` returns without `exit`. Fixed with `-sEXIT_RUNTIME=1` in the web build only.

### Timings

`make test-web`, final run, headless Chromium (the Playwright build, `headless: true`), a fresh browser per case, on AC power with `lowpowermode 0`. Time to first instruction is measured from `callMain`, when the engine starts the ELF loads, to the call just before `sched_run`. "From Run" adds the manifest fetch, module instantiation and tree build.

| | hello-jit | hello-int |
|---|---|---|
| total, Run to exit | 13.30 s | 13.40 s |
| engine run (`callMain`) | 13.25 s | 13.34 s |
| time to first instruction, from `callMain` | 35.7 ms | 36.4 ms |
| time to first instruction, from Run | 89 ms | 87 ms |
| manifest fetch (420,992 bytes) | 3.5 ms | 2.8 ms |
| MEMFS tree build (3,520 lazy files) | 40.5 ms | 39.7 ms |
| fetches (chunk requests) | 419 | 413 |
| distinct files fetched | 293 | 292 |
| bytes fetched | 185,192,698 | 179,786,114 |
| `XIAOJI_STATS` | 297,524,445 instructions in 13.209 s (22.52 M/s), 87.3% in translated code | 652,386,505 instructions in 13.307 s (49.02 M/s), 93.1% in translated code |

Earlier runs during the work gave hello-jit totals of 12.0 to 13.3 s and hello-int totals of 12.7 to 13.4 s, with the same instruction counts to within 0.01%. The instruction count varies a little between runs (Hello's threads); the 87% and 93% translated shares did not change. The `-Xint` case still shows 93% in translated code because the translator is the engine's, and `-Xint` only selects ART's interpreter.

Largest fetches in the hello-jit run: `framework.jar` 37.2 MB, `icudt70l.dat` 26.5 MB, `libart.so` 9.8 MB, `boot-framework.oat` 9.6 MB, `libhwui.so` 9.1 MB, `libart-compiler.so` 5.4 MB, `core-oj.jar` 5.3 MB, `boot.oat` 4.0 MB. Each file is fetched whole in 1 MB chunks as the engine reads it, so 185 MB is far more than Hello's own reads. The boot images are small (about 5.9 MB in total).

**Power state** (`pmset -g ps`, `pmset -g`): "Now drawing from 'AC Power'", battery 100%, charged, `lowpowermode 0`. `displaysleep 120` with "display sleep prevented by Godot", which is a running app, not part of this test. Other load on the machine was not checked during the final run.

### Checks

- `make && make wasm && make wasm-node && make test`: 80 passed, 0 failed, 0 xfail.
- `make test-web`: hello-jit and hello-int PASS. Stdout matches the oracle, exit 0, no page errors, and the guest env matches `tools/guest-env.sh`.
- `engine/main.c` compiles with no warnings under `clang -Wall -Wextra` and `emcc -Wall -Wextra` (both without the Makefile's `-Wno-unused-parameter`).
- `make serve`: starts. The server was also checked directly: HEAD, Range 206 and 416, 404, 405, and refusal of a path that leaves the root.

### What remains

- **Delivery is the simplest form.** Whole-file lazy fetch with 1 MB chunks. A Hello run fetches 185 MB, including all of `framework.jar` (37 MB) and `icudt70l.dat` (26 MB). There is no compression, no chunk cache, and no snapshot. The server sends `no-store`, so the browser cache is not used. Snapshots and chunking are the next delivery step.
- **Time to first instruction** is the engine's setup (ELF loads of dalvikvm64 and linker64, which fetch their files), not the first guest instruction after a snapshot.
- **Browsers.** Only Playwright's headless Chromium was run, not headed Chrome or other browsers. Memory use under the 4 GB limit was not measured.
- **COOP/COEP** is off. Turn it on when the engine uses SharedArrayBuffer (threads in the worker).
- **Env in the page** is a copy of `tools/guest-env.sh` in `web/cases.js`. `tests/web.mjs` checks it, but it is still a second copy.
- Nothing is committed. `make test-web` needs the Playwright path (`XN_PLAYWRIGHT`); there is no network install.
