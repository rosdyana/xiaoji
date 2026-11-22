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
