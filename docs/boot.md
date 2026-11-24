# Boot milestones

Status of the path to a booted Android guest: init, servicemanager with binder,
zygote and system_server. Android is multi-process, so the engine needs real
guest processes first (B1).

## B1: multiple guest processes

Status: **steps 0 to 4 done (uncommitted); `sh-pipe`, `sh-pipeline`, `sh-subshell`,
`sh-loop` and `sh-bg` pass. `sh-script` still fails, because `chmod` needs
`fchmodat` (53), which the engine does not implement (see the step 4 gaps).**

### Steps

0. Oracle cases and the test harness. Done.
1. **Per-process state** (`d5b59ba`). A process object (`engine/proc.c`) owns the
   address space, fd table, signal table, ids and family. `proc_switch()` installs
   them; `mem_mm_switch()` flushes the TLBs and the JIT chain cache.
2. **Open-file descriptions and pipes.** Done, see below.
3. **fork, vfork, copy-on-write, wait, signals and process groups.** Done, see below,
   with the gaps listed there.
4. `execve`, `execveat`, `#!` scripts, `getrusage`, and the `nanosleep` range check.
   Done, see below.
5. `sh-pipe` passes (first checkpoint). Done.
6. Remaining: `/proc/<pid>/maps`, `clone3`, job-control stop states, `fchmodat`
   (for `sh-script`).

### Step 0 (done): the oracle cases and the harness

- `tools/oracle.sh` has six new cases, all run by the guest's
  `/system/bin/sh` in the builder VM chroot: `sh-pipe`, `sh-pipeline`,
  `sh-subshell`, `sh-bg`, `sh-loop`, `sh-script`. Their reference outputs are
  in `build/oracle/`, and they match the spec:
  - `sh-pipe`: `HELLO`, rc 0
  - `sh-pipeline`: `423`, rc 0
  - `sh-subshell`: `arch=aarch64` / `rc=3`, rc 0
  - `sh-bg`: `done`, rc 0
  - `sh-loop`: `3` / `2` / `1`, rc 0
  - `sh-script`: `script ran arg1`, rc 0
- `tests/run.sh` runs the six cases natively and in Node. They are listed in
  `ROOTFS_PENDING`, so a failure is reported as XFAIL. Remove a name from that
  list when the engine passes it.
- The engine today fails them: `can't create pipe` (syscall 59 is
  unimplemented) and `can't fork` (clone without CLONE_THREAD returns
  -ENOSYS).
- The cases call `/system/bin/toybox` by full path, not bare `toybox`. The
  guest env (`tools/guest-env.sh`) sets no PATH, and bare `toybox` fails in
  the engine with `execve` (221) unimplemented. The oracle needs the same
  path, so the cases match.
- Syscalls the six cases need beyond what the engine has (from
  `build/oracle/sh-*.strace`): `clone` (fork style: CLONE_CHILD_CLEARTID,
  CLONE_CHILD_SETTID, SIGCHLD, no CLONE_VM), `pipe2`, `dup3`, `wait4` (with
  WNOHANG|WSTOPPED|WCONTINUED, pid -1), `execve` (221, a second binary),
  plus `getppid`, `getpgid` and `geteuid`, which may already be handled.


### Step 2 as built

- **Descriptions and slots** (`engine/vfs.h`, `engine/vfs.c`). `vfs_fd` is now an
  open-file description: `refs`, the kind, the offset, the status flags
  (`O_NONBLOCK`, `O_APPEND`), and the object. A fd is a `vfs_slot`: a pointer to a
  description and its own `FD_CLOEXEC` flag. `dup`, `dup2` and `dup3` add a
  reference to the same description (no host `dup`), so offset and flags are shared.
  Closing a slot drops a reference; the last one closes the host fd or the pipe end.
- **Pipes** (`engine/pipe.c`, `engine/pipe.h`). A 64 KiB ring. Each description
  counts as a reader or a writer. Read returns EOF when no writer is left, and
  `-EAGAIN` when the ring is empty and a writer exists. Write returns `-EPIPE` when
  no reader is left (`SIGPIPE` is raised too, through `sig_raise_current`). Writes of
  at most `PIPE_BUF` (4096) bytes wait until they fit whole, as POSIX asks.
- **Blocking is a restart** (`engine/sched.c`). A pipe call that would block calls
  `sched_block_restart(pipe)`. It moves the thread's pc back to its `svc`, parks the
  thread in `TS_RESTART`, and leaves x0 alone. Any change to the pipe
  (`sched_wake_key`) makes the thread runnable again, and the syscall runs again with
  the same registers. The interpreter sets pc to `svc+4` before the syscall, so `pc -= 4`
  is right. The JIT path also ends blocks at SVC, so the interpreter always runs it.
- **Partial writes** keep their progress in the thread (`cpu_t.io_done`). A blocked
  `write` or `writev` restarts from that byte. A signal that wakes a part-done write
  does not restart it: the call returns the bytes written, as Linux does, and
  `io_done` is cleared before any handler runs.
- **readv** on a pipe returns the count once it has read something, and blocks only
  when it has read nothing, so a restart never replays data.
- **Also** `pipe2` (59, `O_CLOEXEC` and `O_NONBLOCK`), `fcntl` `F_GETFD`/`F_SETFD`
  (per slot), `F_GETFL`/`F_SETFL` (per description), `lseek` on a pipe (`ESPIPE`),
  and `fstat` (FIFO mode).

### Step 3 as built

- **Copy-on-write** (`engine/mem.c`). `PTE_COW` marks a private page that has a host
  page and is shared after fork. The entry keeps `PTE_W` (the logical permission);
  a write (`page_lookup` with write intent, and `mem_poke`) copies the page while its
  owner count is above one. Owner counts beyond one live in a small hash table of
  shared host pages; a page with one owner is not in it. `page_release` frees a host
  page only with its last owner, and calls `jit_page_changed` before the free. A break
  calls `host_page_changed` on the old page (translations on it are dropped), then
  clears the TLB and fetch entries of that page. `mem_mm_fork` clones the page table
  and the region list through explicit pointers and flushes the TLBs, so the parent's
  write entries cannot bypass the copy.
- **Address spaces** have a reference count (`mem_mm_ref`/`mem_mm_unref`). vfork
  shares one; the last user frees it and clears `cur_mm` (with a TLB flush), so a
  reused address cannot skip the switch.
- **Processes** (`engine/proc.c`, `engine/sched.c`). `fork` (clone without
  `CLONE_THREAD`) copies the address space (COW), the fd table (each descriptor shares
  its description), and the signal handlers; the child's pid is the next number from
  the tid counter, its x0 is 0, and the parent gets the pid. `CLONE_PARENT_SETTID`
  writes the parent's word; `CLONE_CHILD_SETTID` is written in the child's memory at
  its first slice; `CLONE_CHILD_CLEARTID` clears it at the child's exit. `vfork`
  (`CLONE_VM`) shares the address space, and with `CLONE_VFORK` the parent blocks
  (`TS_VFORK`) until the child ends; the parent then gets the child's pid.
- **Ending a process** (`finish_process`, one path, between slices). When the last
  thread goes (or on exit_group, or a fatal signal), the remaining threads are
  reaped, the process becomes a zombie, its children move to the initial process,
  and its fd table and address space are released at once (so pipes see end of
  file). The parent gets SIGCHLD (with `si_pid`, `si_code` CLD_EXITED or CLD_KILLED,
  and `si_status`), and its `wait4`/`waitid` waiters and a vfork parent are woken.
  The initial process's exit still ends the engine with its status.
- **Waiting**. `wait4` (260) and `waitid` (95) choose children by pid, group, or any
  (`P_ALL`, `P_PID`, `P_PGID`), honour `WNOHANG` and `WNOWAIT`, and reap zombies. The
  wait status is `exit << 8`, or the signal number for a death by signal. A blocked
  wait is a restart wait on the parent, woken when a child ends. `WUNTRACED` and
  `WCONTINUED` are accepted and never report anything (no stop states).
- **Signals** (`engine/signals.c`). Handler tables are looked up in the target
  process. `kill` takes `pid > 0`, `0` (own group), `-1` (all but the initial process)
  and `< -1` (a group). SIGKILL to another process is pending on each of its threads
  and wakes them to die; SIGKILL to the running thread ends it at once. `rt_sigsuspend`
  (133) is implemented: the mask is saved and the handler frame restores it.
- **Process groups and sessions**: `getpgid`, `setpgid` (self or a child, not a
  session leader), `getsid`, `setsid`.
- **Futex keys** are `(word, object)`: a private word is keyed on its address space,
  a word in a memfd mapping on the memfd and offset (`mem_futex_key`). A
  `MAP_SHARED|MAP_ANONYMOUS` mapping is a hidden memfd, so fork children share it.
- **/proc**. `/proc/self` follows the running process. `/proc/<pid>/{stat,status,
  cmdline,exe}` work for a live process. A host without `/proc` (macOS) gets a
  synthetic `/proc` directory.
- **Program path.** For host-root runs the program path is made absolute with
  `realpath` (so `/proc/self/exe` is absolute, as glibc requires).

### Step 3 test results

- `make && make wasm && make wasm-node && make test`: **90 passed, 0 failed, 10 xfail.**
  Old 80, plus `pipes-O0`, `pipes-O2`, `procs-O0`, `procs-O2` in engine and Node, plus
  `sh-bg` in engine and Node (moved out of `ROOTFS_PENDING`). The 10 xfail are
  `sh-pipe`, `sh-pipeline`, `sh-subshell`, `sh-loop` and `sh-script` in engine and
  Node, all waiting on `execve`.
- `XIAOJI_JIT=0 tests/run.sh`: 90 passed, 0 failed, 10 xfail.
- `make test-web`: all checks passed.
- Instruction counts of the deterministic tests (`-O2`) equal the counts before
  step 2 (`control-O2` 4577199, `jitstress-O2` 30967369, and the rest). `glibc_hello-O2`
  (a stretch case, not in the checked set) went from 95688 to 96029, because glibc
  now gets past its origin check.
- Warnings: `emcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -c` on each of the
  18 engine files: 0 warnings. Native `clang -Wall -Wextra`: 0 warnings. The guest
  tests build with `gcc -Wall -Wextra` without warnings.
- `tests/guest/procs.c` (one program, output compared with native, both `-O0` and
  `-O2`): copy-on-write (parent keeps its data), exit status 7, death by SIGKILL,
  a pipe between parent and child with end of file on exit, vfork with the child's
  exit status, a SIGCHLD handler (`si_pid`, `si_code`, status, and the restarted wait),
  reparenting (the grandchild's parent changes, and its pipe report reads `R`),
  session and group (`setsid`, `getpgid`, `getsid`, group gone after reap), `waitid`
  fields, `CLONE_CHILD_SETTID`/`CLEARTID`, a cross-process futex in a shared
  mapping, `/proc/<pid>/stat` open and ENOENT after reap, a kill to a process group,
  and default SIGPIPE death. The `/proc/<pid>` checks compare the `stat` name (it
  contains `(procs-`) and the `cmdline` argv (it contains `procs-`) with native.
  A hot loop after fork rewrites all of `big[]` in both processes, each with its own
  values, and checks them. In Node the translator runs 77% of the instructions of
  this test (`XIAOJI_STATS=1`).

### Gaps (step 3, as of step 3; see step 4 for what is now resolved)

- `execve` was missing (done in step 4). `sh-bg` passed in step 3 only because its
  background `sleep` failed to exec; step 4 made the sleep run.
- A call restarted after a pipe block looks its fd up again. If another thread closed
  that fd meanwhile, the call fails with EBADF. Linux keeps the file reference until
  the call returns.
- Signal interruption is restart-only (SA_RESTART semantics). No call returns
  EINTR, except `rt_sigsuspend`. A part-done pipe write that a signal wakes returns the
  bytes it wrote (as Linux does); that path is not tested.
- Stop states (`WUNTRACED`, SIGSTOP, job control) are not modelled.
- `/proc/<pid>/maps` is not provided. For a process other than the caller,
  `vsize`, `rss` and `startstack` in `/proc/<pid>/stat` are zero, because its
  address space is not the one the region list describes.
- Pipe capacity is fixed at 64 KiB. `poll` and `select` on pipes are not implemented.
- `clone3` is not implemented. `kill(-1)` and `kill(0)` are implemented but only the
  group form is tested.
- A copy-on-write break drops the translations of the old host page for every owner.
  That is only a cost, not a correctness problem.
- SIGCHLD set to `SIG_IGN` does not auto-reap children (Linux does).
- Fork walks every populated second-level table (2^18 entries) of the address
  space, so forks of large address spaces are slow.
- (Resolved in step 4: `nanosleep` and `clock_nanosleep` now return EINVAL for a
  `tv_nsec` out of range, or a negative field.)

### Step 4 as built: execve, execveat, `#!`, getrusage

- **Loading** (`engine/loader.c`, `engine/loader.h`). `loader_load(img, len, path, argc,
  argv, envc, envp, &entry, &sp)` is the one loader. It maps the program and its
  PT_INTERP interpreter (read through the VFS), sets the brk base, and builds the stack
  (argv, envp, auxv). `main` uses it for the first program, with the same mapping order,
  stack layout and exit codes as before (127 for a missing interpreter, 126 for the
  rest). Its VLAs are heap arrays now, and a stack that overflows is `-E2BIG`.
- **execve** (`engine/exec.c`, `engine/exec.h`). `exec_run(c, dirfd, path, argv, envp,
  flags)` serves both 221 and 281 (`AT_FDCWD` or a dirfd; `AT_EMPTY_PATH` names the
  file the dirfd refers to; `AT_SYMLINK_NOFOLLOW`, under which a final symlink is ELOOP,
  as on Linux). Order:
  1. Copy the path, argv and envp to the host heap (EFAULT, ENAMETOOLONG, E2BIG: one
     string over 128 KiB, or more than 2 MiB of strings and pointers in total).
  2. Resolve the path through the VFS, check that it is a regular file with an exec
     bit (EACCES otherwise, which includes a directory), and read it (ENOENT).
  3. Follow `#!` lines: the interpreter, plus at most one argument (the rest of the
     line, trimmed), and the script name as passed. A file shorter than 256 bytes may end
     its `#!` line without a newline (Linux runs it); a longer one with no newline in its
     first 256 bytes is ENOEXEC. At most 4 levels (`-ELOOP` beyond). An image that is
     neither ELF nor `#!` is ENOEXEC.
  4. Build a new address space (`mem_mm_new` and `mem_mm_switch`), and load into it. On a
     failure, switch back to the old one, drop the new one, and restore the brk.
  5. Commit: `p->mm = new`, then `mem_mm_unref(old)`. The switch flushes the TLBs and the
     JIT chain cache; the unref frees the old pages through `page_release`, which calls
     `jit_page_changed` for each freed page.
  6. Close the FD_CLOEXEC slots (`vfs_close_cloexec`). Reset every caught signal to
     SIG_DFL, keep the ignored ones, keep the mask and the pending set, and disable the
     alternate stack (`sig_exec_reset`). Forget the signal trampoline, which lives in the
     old space.
  7. Reap the other threads of the process and make the caller the leader's tid
     (`sched_exec_threads`).
  8. Names: `/proc/self/exe` is the final program (the interpreter, for a script). The
     argv bytes are the new argv. The thread name is the basename of the path passed to
     exec (the script, for `#!`), as Linux does.
  9. If the process is a vfork child, clear its `vfork` flag and wake the parent
     (`sched_wake_key` on the child's process object).
  10. The thread gets fresh registers: x0 to x30, the SIMD registers, NZCV, FPCR and FPSR,
      tpidr_el0, the exclusive monitor, and the wait and suspend state. pc is the entry
      and sp is the new stack. The thread's signal mask, pending signals and tid stay.
- **Dispatch** (`engine/syscall.c`). 221 and 281 call `exec_run`. A successful exec
  returns 0 through the normal write-back of x0.
- **getrusage** (165). `who` is `RUSAGE_SELF`, `RUSAGE_CHILDREN` or `RUSAGE_THREAD`; any
  other value is EINVAL. The times are derived from the counts, so they are
  deterministic: an instruction is 1 ns of user time, and a syscall is 10 us of system
  time. `maxrss` is the peak resident set in KiB, as seen at exec, at exit, or in the
  call. Children's totals are added to the parent when a child is reaped (`proc_free`),
  including the child's own children.
- **Accounting** (`engine/proc.c`, `engine/sched.c`). Each process has its instruction
  count of reaped threads (`dead_icount`), its syscall count, and its child totals.
  `sched_icount_of` gives the live count of a process.
- **nanosleep and clock_nanosleep**: a `tv_nsec` of 10^9 or more, or a negative
  `tv_sec` or `tv_nsec`, is EINVAL.

### Test results (step 4)

- `make && make wasm && make wasm-node && make test`: **102 passed, 0 failed, 2 xfail.**
  The 90 from step 3, plus `sh-pipe`, `sh-pipeline`, `sh-subshell`, `sh-loop` (engine
  and Node: 8), plus `exec-O0` and `exec-O2` (engine and Node: 4). The 2 xfail are
  `sh-script` in engine and Node.
- `XIAOJI_JIT=0 tests/run.sh`: 102 passed, 0 failed, 2 xfail.
- `make test-web`: all checks passed (`hello-int` and `hello-jit`).
- Instruction counts of the deterministic cases are unchanged: `arith-O0` 39365,
  `control-O0` 10061838, `control-O2` 4577199, `jitstress-O0` 115922202, `jitstress-O2`
  30967369, `jitthreads-O0` 25625244, `memory-O0` 38017899, `signals-O0` 13055,
  `simd-O0` 6384406, with the JIT on and off.
- `tests/guest/exec.c` (built `-O0` and `-O2`, output compared with native in the builder
  VM, byte for byte). It covers:
  - failed execs leave the caller running: `ENOENT` (execve and execveat), `EACCES`
    (a directory, a file without an exec bit), `ENOEXEC` (text with no `#!`), `E2BIG`
    (one argument over 128 KiB);
  - fork plus execve of the sibling `args` binary, with argv, an environment variable
    and the wait status (0);
  - execveat with `AT_EMPTY_PATH` through a descriptor (0);
  - a `#!` script (the rest of the line is one argument; the script name is argv[2]);
  - a vfork child that execs; the parent resumes after the exec (0);
  - `FD_CLOEXEC`: the close-on-exec write end is closed in the new image, the read
    end is open;
  - signals: a caught signal is SIG_DFL after exec, an ignored one stays ignored, the
    blocked mask stays, and the alternate stack is disabled;
  - `/proc/self/exe`, the thread name (`comm`), and `/proc/self/cmdline` in the new
    image;
  - a `#!` line with no trailing newline runs;
  - a non-leader thread execs the probe: the probe sees `gettid == getpid`, and the
    status is the probe's (7);
  - `nanosleep` EINVAL for `tv_nsec` 10^9 and -1 (and 0 for 1 us), and `getrusage` (self
    and children set `maxrss`; `who=2` is EINVAL).
  The probe output prints no pid and no fd number.
- `sh-bg` was rechecked with `--trace`: the background `toybox sleep` is exec'd (execve
  returns 0) and its `nanosleep` runs (returns 0).
- A `#!/system/bin/sh` script created on the host with an exec bit (a new name, not
  `sh-script`'s own file) runs through the guest's dynamic `/system/bin/sh -c` in the engine
  and in Node: `t1 ran arg1`, rc 0. So the `#!` path works through a dynamic interpreter; the
  only blocker of `sh-script` is `chmod`.
- Warnings: `clang -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -c` on each engine file
  (except `jit.c`, which is wasm-only and needs emscripten's header): 0 warnings.
  `emcc` with the same flags on every engine file: 0 warnings. The guest test builds with
  `gcc -Wall -Wextra`: 0 warnings.

### Gaps (step 4)

- **`sh-script` fails, and the plan does not cover why.** `chmod 755 /data/local/tmp/s.sh`
  runs `/system/bin/chmod` (toybox). Toybox calls `fchmodat` (53), which the engine does not
  implement (the oracle's strace shows one `fchmodat`). Without it the file never gets
  its exec bit, and the script's own `faccessat(X_OK)` returns EACCES. `fchmodat` (and
  `fchmod`) need a host `chmod` through the VFS, which the plan does not list. They are not
  added here. This is the only XFAIL left, and the "no XFAIL" gate is not met until it is
  decided.
- Nesting of `#!` beyond 4 levels returns ELOOP, as Linux does. The task's error list did
  not name it.
- `wait4` and `waitid` ignore their `rusage` argument (it is not filled).
- `unlinkat` (35) is not implemented, so the exec test leaves three fixed `/tmp` files
  (`xiaoji-exec-script`, `xiaoji-exec-text`, `xiaoji-exec-nox`). Each run rewrites them.
- The times in getrusage are model values (instructions and syscalls), not host time.
  `ru_maxrss` is sampled at exec, at exit and at the call, so a peak between those is
  missed.
- A non-leader thread that execs takes the process's tid (the probe checks gettid ==
  getpid). The other threads are reaped, but nothing in the test observes that they are
  gone (the probe exits the process first). Only the tid takeover is checked.
- `mem_mm_rss_kib` walks the whole page table at every exit and exec (for maxrss). That
  is a fixed cost of about 2^18 entries per walk.
- `AT_SYMLINK_NOFOLLOW` on a final symlink is ELOOP, as on Linux. This is not in the
  test: `/bin` is a symlink on the build VM (native Linux checked by hand: -40), but a
  directory on macOS, where the engine runs. A temporary guest program checked the engine
  (macOS) on `/tmp`, which is a symlink there: -40.
- `execve` of a memfd through an AT_EMPTY_PATH descriptor is not tested. The path used is
  the descriptor's guest name, which may not be a file.
- The exec test checks the probe's `/proc/self/exe` and `comm`, but not the interpreter's
  `comm` for a `#!` script (the test's interpreter is `args`, which prints nothing about
  itself).
- `/proc/<pid>/maps` is still missing, and so are `clone3` and stop states (as before).

## B2: system properties (property area and property service)

Status: **done (uncommitted).** `getprop` and `setprop` work in the engine, with the
property area and the service, and `prop-get`, `prop-set`, `prop-ro`, `prop-list` and
`prop-wait` pass in the engine and in Node, also with `XIAOJI_JIT=0`.

### Backend chosen and why

Single file, bionic's "pre-split" backend. The area is one file at
`/dev/__properties__`, with no `property_info` and no SELinux contexts.

Evidence, from AOSP `android13-release` (`bionic/libc/system_properties/`, read over ssh
on the build machine, not modified):
- `system_properties.cpp`, `SystemProperties::Init`: `is_dir(filename)` decides. A
  directory with `property_info` picks `ContextsSerialized`, a directory without it
  `ContextsSplit`. Anything else (the case here: `stat` fails or the path is a regular
  file) takes `ContextsPreSplit`.
- `include/system_properties/contexts_pre_split.h`: `ContextsPreSplit::Initialize` calls
  `prop_area::map_prop_area(filename)`, and the one area serves every name.
- `prop_area.cpp`, `map_fd_ro`: the file must be owned by uid 0 and gid 0, have no
  group or other write bit, and be at least `sizeof(prop_area)` (128) bytes. The size
  is `st_size` (so the engine's area size is what readers use). The map is
  `PROT_READ, MAP_SHARED`.
- The guest libc (`build/rootfs/apex/com.android.runtime/lib64/bionic/libc.so`) contains
  both `/dev/__properties__` and `/dev/__properties__/property_info` strings, so both
  paths exist in the build. The Android 13 build has `ro.build.version.release=13`.
- Verified in the engine (`--trace` of `getprop ro.hardware`): `newfstatat` on
  `/dev/__properties__` returns 0 (a regular file), `openat` returns an fd, `fstat` 0,
  `mmap(0x40000, PROT_READ, MAP_SHARED)`. No `property_info` lookup follows.

Not disassembled: the check is from source plus the engine trace (which is the
behaviour the guest actually follows). The guest's libc is the Android 13 build, so
the source matches it.

### Format facts (verified against AOSP android13-release)

- Header (`prop_area`, 128 bytes): `bytes_used` (0), `serial` (4), `magic` 0x504f5250
  (8), `version` 0xfc6ed0ab (12), `reserved[28]`. Data starts at 128.
- `prop_bt` is 20 bytes (`namelen`, `prop`, `left`, `right`, `children`), then the
  segment name and a NUL. The root is at data offset 0. The dirty backup area is at data
  offset 20 (92 bytes). The first allocation is at data offset 112.
- `prop_info` is 96 bytes: `serial` (4), `value[92]` (or, for a long value,
  `error_message[56]` and an `offset`, at 60), then the full name and a NUL. The
  serial's top byte is the value length; bit 0 is the dirty bit; bit 16 (`kLongFlag`)
  marks a long record. A long value sits after the record, at `offset` from the record.
- Offsets in the trie are relative to data offset 0. Sibling order is by (length, then
  bytes), not plain lexicographic. Bionic's `foreach` walks left, self, children, right.
- Update (`SystemProperties::Update`): copy the old value to the backup, set the dirty
  bit, write the value, store the new serial (`len << 24 | (serial+1) & 0xffffff`), wake
  the record's serial, bump the area serial, wake the area serial. Add
  (`SystemProperties::Add`): insert, bump the area serial, wake it.
- `Wait` (bionic) futex-waits on the area serial (for any property) or on a record's
  serial. Both are words of the shared area.

### The service protocol (verified against bionic and init)

- Client (`bionic/libc/bionic/system_property_set.cpp`): it first reads
  `ro.property_service.version` and uses protocol 2 only at 2 or more. Otherwise it uses
  the legacy 128-byte `prop_msg` with a 250 ms poll. **Difference from the plan**: the
  engine sets `ro.property_service.version=2`, because the plan did not list it.
- SETPROP2 (`0x00020001`): `socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC)`, `connect` to
  `/dev/socket/property_service` (a sockaddr_un with NUL-terminated path), one `writev`
  of `cmd, u32 len, name, u32 len, value`, then `recv(MSG_WAITALL)` of a u32 result.
- Init's rules (`property_service.cpp`, `util.cpp`): `IsLegalPropertyName` (alphanumerics
  and `._-@:`, no leading or trailing dot, no empty segment). `IsLegalPropertyValue`:
  a non-`ro.` value of 92 bytes or more is `PROP_ERROR_INVALID_VALUE` (0x14). `PropertySet`:
  an existing `ro.` is `PROP_ERROR_READ_ONLY_PROPERTY` (0x0b). A failed add is
  `PROP_ERROR_SET_FAILED` (0x24). **Difference from the plan**: init has no
  `PROP_NAME_MAX` (32) limit for SETPROP2; the 32-byte limit belongs to the legacy
  message only, and the engine follows init.
- Init's `ctl.*` names are handled before `PropertySet`. The engine accepts them and
  logs them to stderr; it does not act on them (B4).

### What is in the engine

- `engine/props.c`, `engine/props.h`: the area builder (a port of bionic's trie code and
  update protocol, with a 256 KiB shadow written to one memfd after each change), the
  build.prop loader, the engine values, `--prop`, the service's `props_set`, and the
  endpoint for `/dev/socket/property_service` (SETPROP2 and the legacy message).
- The area is a memfd (`engine/memfd.c`). Every process that opens `/dev/__properties__`
  gets a read-only fd on it (`ro_area`): `O_RDWR` and `O_WRONLY` opens are EACCES, a write
  is EBADF, `ftruncate` on it is EINVAL (the fd is read-only), `mprotect` to a writable
  protection on the area is EACCES, and `mmap` with `PROT_WRITE` and `MAP_SHARED` is
  EACCES (a private writable map is allowed, as on Linux). `pwrite` and `fallocate` are
  not implemented at all (ENOSYS), so there is no other path to the memfd. `stat` reports a
  regular file, mode 0444, uid and gid 0, and the area's size.
- Wakes: `sched_futex_wake(offset, memfd, ...)` on the area serial (offset 4) and on each
  changed record (offset 128 + data offset). The key and object are the ones
  `mem_futex_key` computes for a word in a shared mapping of the memfd, so a guest
  futex wait on the mapped word is woken by the engine.
- Startup (`engine/main.c`): `props_init` runs after `vfs_init`. It loads, in order, the
  build.prop files that exist (`/system/etc/prop.default`, `/system/build.prop`,
  `/system_ext/etc/build.prop`, `/vendor/default.prop`, `/vendor/build.prop`,
  `/odm/etc/build.prop`, `/product/etc/build.prop`). Later files override, except
  `ro.*`, where the first value wins. Then the engine's values, then the `--prop K=V`
  overrides (repeatable, `--prop` in the usage text).
- **Difference from the plan**: the engine's values are overrides, applied after the
  files, so `ro.secure=0`, `ro.zygote=zygote64` and `ro.adb.secure=0` replace the
  guest's `ro.secure=1` and `ro.zygote=zygote64_32`. The plan's "first wins" applies to
  the files only. Engine values: `ro.hardware`, `ro.boot.hardware`, `ro.kernel.qemu=0`,
  `ro.debuggable=1`, `ro.secure=0`, `ro.adb.secure=0`, `ro.zygote=zygote64`,
  `ro.xiaoji.engine=b2`, `ro.property_service.version=2`.
- Sockets (`engine/sock.c`, `engine/sock.h`): a socket is a VFS fd kind (`VFS_SOCK`)
  holding a `sock_t`, which connects through an ops table (connect, write, read, close).
  The property service is one ops table. B4 adds a listener table and a second ops
  implementation; the fd and VFS hooks stay.
- Syscalls (`engine/sysfs.c`): `socket` (198) for AF_UNIX SOCK_STREAM (SOCK_CLOEXEC
  honoured, SOCK_NONBLOCK ignored because the endpoint never blocks), `connect` (203, the
  path must be `/dev/socket/property_service`, else ENOENT), `sendto` (206, with a
  destination address EISCONN) and `recvfrom` (207) as `write` and `read`. `writev`
  and `read`/`write` go through the VFS as for pipes.
- `engine/linux.h` gained the errno values `ENOTSOCK`, `EPROTONOSUPPORT`,
  `EAFNOSUPPORT`, `EISCONN`, `ENOTCONN`, `ECONNREFUSED`.

### Tools

- `getprop` and `setprop` in the guest are **toolbox** (symlinks to
  `/system/bin/toolbox`), not toybox. The plan assumed toybox. Call them as
  `/system/bin/getprop` and `/system/bin/setprop`, since the guest has no PATH.
- `getprop` with no arguments lists every property, sorted by name (byte order). The
  check is on the names only: `prop-list` extracts them with `toybox sed`, sorts them
  with `toybox sort`, and compares. (An earlier check of whole `[name]: [value]` lines
  flagged a disorder only because `]` sorts after `-`; that was a bad test, not a bad
  order.)
- `getprop -T` and `-Z` both exit 255 with `Unable to load property info file`. They
  need `property_info` (and SELinux contexts), which the single-file backend does not
  have. **Gap.**

### Tests

- `tests/run.sh`, section "Property cases": `prop-get`, `prop-set`, `prop-ro`,
  `prop-list` (each a single `sh -c`, because the store lives in the engine process,
  so setprop and getprop must share one run) and `prop-wait`. Stdout must equal
  `tests/expect/<name>.out`, and the exit code must be 0. Each runs in the engine and
  in Node, so 10 checks.
- `prop-wait` is `tests/guest/prop-wait.c`, built by `make guest-tests` and copied to
  `build/rootfs/data/local/tmp/prop-wait` by `tests/run.sh`. It implements the protocol
  itself: it sets `debug.xiaoji.wait=start` through the socket, maps the area read-only,
  forks, and the child waits with `FUTEX_WAIT` on the record's serial until the value
  is `done`. The parent sleeps 50 ms, sets `done`, and reaps the child. Then it checks
  the read-only guarantee: a writable shared `mmap`, an `mprotect` to writable and an
  `ftruncate` of the area fd are refused (-13, -13, -22), and a child that writes into the
  mapping dies of SIGSEGV (signal 11). The `prop-list` case also checks that the names
  are sorted. The `prop-*`
  guest binaries are not oracle cases (the loop in `tests/run.sh` skips them), because
  native runs have no property service.
- Strace-style check: `futex(0x...3f8, FUTEX_WAIT, 0x5000000)` returns 0 in the
  `--trace` run, so the child was woken (not EAGAIN), by the engine's wake.

### Results (B2)

- `make test`: **114 passed, 0 failed, 0 xfail.** The 104 from before, plus 10 property
  checks (5 cases, engine and Node).
- `XIAOJI_JIT=0 tests/run.sh`: **114 passed, 0 failed, 0 xfail.**
- `make test-web`: all checks passed (`hello-int` and `hello-jit`, after the change).
- Warnings: `clang -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -c` on every
  `engine/*.c` except `jit.c` (wasm-only): 0 warnings. `emcc` with the same flags on every
  `engine/*.c`, `jit.c` included: 0 warnings. `gcc -Wall -Wextra` on
  `tests/guest/prop-wait.c` in the builder VM: 0 warnings.
- `prop-wait` also checks the read-only guarantee (see Tests above).
- Guest instruction counts, re-measured with `XIAOJI_STATS=1` on the freestanding
  deterministic cases, equal the B1 counts exactly: `arith-O0` 39365, `control-O0`
  10061838, `control-O2` 4577199, `jitstress-O0` 115922202, `jitstress-O2` 30967369,
  `jitthreads-O0` 25625244, `memory-O0` 38017899, `signals-O0` 13055, `simd-O0` 6384406.
  Those programs do not use bionic, so they are the unchanged control. Bionic guests
  now do read properties, which their counts reflect; those were not compared with B1
  (B1 did not record them).

### Gaps (B2)

- `ctl.*` properties are accepted and logged, not acted on (B4).
- Each engine run rebuilds the store from the build.prop files and the engine's values.
  Properties set in one run are gone in the next (init's `persist.*` is not modelled).
- The legacy `PROP_MSG_SETPROP` message is implemented but not tested. A legacy client
  waits for the service to hang up with `ppoll` (73), which the engine does not implement,
  so that path fails. Bionic uses protocol 2 when `ro.property_service.version` is 2, and
  the engine sets it.
- Only the record-serial futex wake is tested (`prop-wait`). The area serial (offset 4)
  is woken by the same code, but no test waits on it.
- `ro.xiaoji.engine=b2` is a placeholder: the engine has no version constant.
- `getprop` and `setprop` were not exercised in the web build; `make test-web` runs only
  its hello cases.
- `mprotect` checks the region of the first page only. A range that spans the area and
  other mappings is not split.
- `recv` on the socket returns what is queued and never blocks (no `MSG_WAITALL`
  semantics beyond the reply size, which is always complete in one write).
- `stat` of `/dev/socket/property_service` is ENOENT (the socket is not a path entry).
- No UTF-8 check on values (init checks with `mbstowcs`).
- Values with long lengths are written as long records only for `ro.` names. An
  existing long record cannot be updated, so a `--prop` override of one is refused.
- The area is fixed at 256 KiB. Filling it makes a set fail with
  `PROP_ERROR_SET_FAILED`. At boot it holds 41792 of its 262016 data bytes (the header's
  `bytes_used`, read in a guest with `toybox od`; the serial then was 243), so there is
  room for runtime properties.
- `getprop -T`/`-Z` (needs `property_info`) and SELinux are not modelled.

## B3: the binder driver, servicemanager, epoll, eventfd and timerfd

Status: **B3 steps 1 to 4 are in the engine, uncommitted.** Step 1 is the logd sink
(Android log lines reach a stderr sink). Step 2 fixes the binderLibTest TearDown hang (its
cause was a driver bug in buffer release, below). Step 3 fixes FreedBinder, OnewayQueueing,
GotSid, SchedPolicySet and InheritRt, and adds the scheduler syscalls. SetRpcClientDebug
and SetRpcClientDebugTwice still fail (they need AF_INET, see Gaps). Step 4 ran
binderSafeInterfaceTest and binderThroughputTest once each.

The real `/system/bin/servicemanager` runs as the binder context manager. `service list`
shows `manager`, and `service check manager` finds it. `binderDriverInterfaceTest` passes all
17 cases with servicemanager running, in the engine and in Node.

### Design as built

- **Files.** `engine/binder.c` and `binder.h` (the driver), `engine/poll.c` and `poll.h`
  (readiness, ppoll, epoll), `engine/fdobj.c` and `fdobj.h` (eventfd and timerfd),
  `engine/selinuxfs.c` and `selinuxfs.h` (the synthetic selinuxfs), `engine/logd.c` and
  `logd.h` (the logd sink). Hooks: `engine/vfs.c` and `vfs.h` (fd kinds, synthetic directories
  and files, `vfs_resolve`, `vfs_socket(cloexec, dgram)`), `engine/sysfs.c` (ioctl, statfs, the
  new syscalls, datagram sends), `engine/sock.c` and `sock.h` (stream and datagram sockets),
  `engine/syscall.c`, `engine/signals.c` and `signals.h`, `engine/procfs.c` (`attr/current`),
  `engine/sched.c` and `sched.h` (scheduling policy per thread), `engine/main.c` (`--logcat`),
  `engine/pipe.c`.
- **Devices.** `/dev/binder`, `/dev/hwbinder` and `/dev/vndbinder` are synthetic
  (`SYN_BINDER`). Each is a separate context with its own context manager. Each `open` is a
  `bproc` (one per open description), and the last close of a description is the proc's
  death. `fork` shares descriptions, as Linux does for any fd.
- **Receive buffer.** `mmap` of a binder fd creates a memfd of the mapping's size (at most
  4 MiB) and maps it as a shared view. Only one mapping per proc. `PROT_WRITE` is EPERM.
  A read-only `MAP_PRIVATE` mapping is served as the shared view, as libbinder maps it.
  Buffers come from a first-fit allocator; `BC_FREE_BUFFER` releases them.
- **ioctl.** `BINDER_WRITE_READ`, `VERSION` (protocol 8), `SET_MAX_THREADS`,
  `SET_CONTEXT_MGR` and `SET_CONTEXT_MGR_EXT`, `THREAD_EXIT`, `GET_NODE_INFO_FOR_REF`,
  `GET_NODE_DEBUG_INFO`, `ENABLE_ONEWAY_SPAM_DETECTION` (accepted), `GET_FROZEN_INFO`
  (zeros). `SET_IDLE_TIMEOUT` and `SET_IDLE_PRIORITY` are EINVAL, as the test expects.
  `FREEZE` is EINVAL (not modelled). The ioctl numbers match
  `bionic/libc/kernel/uapi/linux/android/binder.h` on the remote AOSP tree.
- **Commands and returns.** The `BC_` and `BR_` commands in the plan. `BR_NOOP` leads each
  read. A read with no work waits with `sched_block_restart`. A synchronous caller's read
  waits on its own key, and an idle looper's read waits on the proc's key.
- **Nodes, refs, counts.** A node belongs to the proc that created it. A ref maps a handle
  in one proc to a node. Handle 0 is the context manager. Strong and weak counts follow the
  driver's model: a ref's counts, the node's internal counts, and the owner's `has_*` and
  `pend_*` acknowledgements, which produce `BR_ACQUIRE`, `BR_INCREFS`, `BR_RELEASE` and
  `BR_DECREFS` to the owner.
- **Transactions.** Data and offsets are copied into the receiver's buffer, and each object
  is translated: BINDER and WEAK_BINDER become handles in the receiver (or the binder back,
  when the receiver owns the node); HANDLE and WEAK_HANDLE become binders in the owner and
  handles elsewhere; FD holds the description, which the receiver installs (cloexec); PTR
  copies its buffer; FDA's fds are held the same way. A synchronous call waits for its
  reply: `BR_TRANSACTION_COMPLETE` first, then `BR_REPLY`. A nested call goes to the thread
  the caller is serving in the target proc. A oneway call queues on its node.
- **Death.** A proc that closes marks its nodes dead. Refs holding a death notification get
  `BR_DEAD_BINDER`. Requests it held fail for their senders with `BR_DEAD_REPLY`.
- **Thread pool.** `BR_SPAWN_LOOPER` is sent when a looper's read returns items, no other
  looper is idle (blocked for proc work), the proc has fewer started loopers than
  `SET_MAX_THREADS`, and no spawn is pending. The read starts with a `BR_NOOP` word; the
  spawn overwrites that word, so it arrives before the returns. A read that already returned
  items (it does not start the buffer) sends none. `BC_REGISTER_LOOPER` clears the pending
  spawn and counts a started looper. A spawn was observed once in the `binderLibTest`
  PtrSize run (the spawn log line, checked with a temporary print that has been removed).
- **Which work a read takes.** A read takes its own queued work first. It takes proc work
  only if the thread is not waiting for a reply and had no own work when the read started
  (`may_take_proc`, as the kernel's `wait_for_proc_work`, decided once per read).
- **ppoll, epoll, eventfd, timerfd** (`fdobj.c`, `poll.c`). One key serves every poll
  waiter, and each readiness change calls `poll_notify`. epoll is level-triggered, with a
  16-byte `epoll_event`. eventfd2 supports EFD_SEMAPHORE, EFD_NONBLOCK and EFD_CLOEXEC.
  timerfd supports CLOCK_MONOTONIC and CLOCK_BOOTTIME, relative and absolute, and intervals.
  Both clocks are the host's monotonic clock.

### The logd sink (B3 step 1)

liblog writes its text log lines to `/dev/socket/logdw`, an AF_UNIX SOCK_DGRAM socket
(`system/logging/liblog/logd_writer.cpp` on the remote tree). The engine now serves that path
as an endpoint, as it does the property service (`engine/logd.c`).

- **Socket layer.** `socket(AF_UNIX, SOCK_DGRAM)` is accepted. `connect` to `/dev/socket/logdw`
  succeeds on a datagram socket, and any other path is ENOENT. A datagram write (`write`,
  `sendto`, `writev`) is one message: the iovecs are gathered into one buffer (at most 64 KiB,
  EMSGSIZE beyond it) and written with one call to the endpoint.
- **Format.** liblog's datagram is `struct android_log_header_t` (id, tid, realtime sec and
  nsec: 11 bytes packed), then the priority (1 byte), the tag and the message, each
  NUL-terminated. Text buffers are decoded: MAIN (0), RADIO (1), SYSTEM (3) and CRASH (4).
  The binary buffers (EVENTS 2, STATS 5, SECURITY 6, KERNEL 7) are not printed.
- **Output.** `<letter>/<tag>(<pid>): <message>` on stderr, one line per message. The letter
  is the Android priority (V D I W E F, `?` otherwise). The pid is the guest process that made
  the write (the datagram carries no pid, so the engine uses the calling process).
- **Switch.** Off by default: messages are dropped silently. `--logcat` or `XIAOJI_LOGCAT=1`
  turns the output on. Default test output does not change (the 124 cases and their stdout
  are as before).
- **Check.** `xiaoji --logcat --root build/rootfs -- /system/bin/log -p w -t mytag "hello"`
  prints `W/mytag(1000): hello`. A `sh -c` that prints `$$` and then runs `log` shows the
  `log` child's pid.
- **Not modelled.** No logd buffers, no `SCM_CREDENTIALS` (the pid is the engine's own), no
  pmsg. A message with an embedded NUL is cut at the NUL.

### The SELinux decision (permissive, with a synthetic selinuxfs)

servicemanager's `Access::Access` (`frameworks/native/cmds/servicemanager/Access.cpp:91`)
does `CHECK(selinux_status_open(true) >= 0)`, and its checks go through `selinux_check_access`.
The plan asked for "disabled" if libselinux would short-circuit on it. It would not:

- `external/selinux/libselinux/src/enabled.c`: on Android, `is_selinux_enabled()` is
  `selinux_mnt ? 1 : 0`.
- `sestatus.c`: `selinux_status_open()` returns -1 with ENOENT when `selinux_mnt` is NULL, so
  the CHECK fails unless the mount exists. Creating the mount makes `is_selinux_enabled()`
  return 1.
- `checkAccess.c`: `selinux_check_access` returns 0 (allow) only when
  `is_selinux_enabled() != 1`. With the mount present, the check goes through the AVC.

So SELinux is presented as **enabled, permissive (enforce 0), and allow-all**, through
a synthetic selinuxfs at `/sys/fs/selinux`, with:

- `status`: the `struct selinux_kernel_status` page, a shared memfd: version 1, sequence 2
  (even, stable), enforcing 0, policyload 0, deny_unknown 1.
- `enforce` "0", `deny_unknown` "1", `reject_unknown` "0", `policyvers` "30",
  `checkreqprot` "0", `mls` "0".
- `access`: a write of `scon tcon kclass perm` is answered by a read of
  `allowed decided auditallow auditdeny seqno flags`, with allowed = the requested bits.
- `class/service_manager/index` "60", and `class/service_manager/perms/{add,find,list}` =
  1, 2, 3 (libselinux maps value v to bit v-1). Its `discover_class` reads `readdir` entries,
  including `.`, and opens `perms/.`, which must resolve to a directory.
- Every process's context is `u:r:kernel:s0`: `/proc/<pid>/attr/current`,
  `/proc/self/attr/current` and `/proc/thread-self/attr/current` report it (no newline), and
  the binder secctx carries it (it was an empty string, which made every caller's context
  empty).
- The mount reports `SELINUX_MAGIC` from `statfs` on its path. verify_selinuxmnt needs the
  magic and a mount that is not `ST_RDONLY`, so the mount is **read-write**, not read-only as
  the plan said. Only `access` accepts writes; it is a request and reply file.

**Evidence:** servicemanager stays up under `epoll_pwait` and registers `manager`. With the
mount missing, the trace showed the two `statfs` calls for the SELinux mount returning ENOENT,
which is why `/sys/fs` is a synthetic parent (the fix was in `vfs_resolve`). The
`perms/.` case returned ENOENT before the trailing `.` fix. Guest `cat` and `ls -l` show the
files with modes and owners. The `binder-sm` expectation shows `manager` listed and found.

Known gaps in this choice: deny_unknown is 1, so an unknown class or permission is denied
(only `service_manager` is defined). `fstatfs` on an fd of the mount does not report the
SELinux magic (only the path form does). There is no policy: every check is allowed.

### Binder fixes found while running servicemanager and binderLibTest

- **Buffer release dropped the wrong count (the TearDown hang).** `buf_release_refs` read
  8 bytes of each object and took the handle from offset 4, which is the object's `flags`
  word. The handle is at offset 8 (`xl_object` reads it there). So when a transaction buffer
  was freed (`BC_FREE_BUFFER`), the strong count that the translation had given the
  receiver's ref was never dropped, or the wrong ref was dropped. The owner's node then kept
  `int_strong` above zero forever: no `BR_RELEASE` went to the owner, its `BBinder` was not
  destroyed, and `exit(EXIT_SUCCESS)` in `BinderLibTestService::~BinderLibTestService` never
  ran. The main server's `EXIT` handler waits for its children, so every TearDown after an
  `addServer` hung. The fix reads 24 bytes (one `flat_binder_object`) and uses the handle at
  offset 8. The per-node count trace (incs and decs by proc, removed) showed the test
  process's ref to the child's node with no decrement at its buffer free.
- **A strong handle needs a strong ref** (`binder_get_ref_olocked` with need_strong). A
  sender's strong HANDLE whose ref holds no strong count is rejected (`FAILED_TRANSACTION`).
  This is what FreedBinder expects once the count above is released. (FreedBinder)
- **Requests carry the sender's security context when the node asks for it.** A node whose
  flags have `FLAT_BINDER_FLAG_TXN_SECURITY_CTX` (0x1000, from `setRequestingSid`) gets
  `BR_TRANSACTION_SEC_CTX` with the context. Before, only the context manager with
  `SET_CONTEXT_MGR_EXT` got it, and it was sent with the wrong command code. (GotSid)
- **A read takes one proc-work decision** (see Design). A oneway `BC_TRANSACTION` makes
  libbinder wait for `BR_TRANSACTION_COMPLETE`. The driver's read then also took the next
  proc request, which libbinder ran inside the callback send, so the second oneway callback
  was delayed past the test's timeout. (OnewayQueueing)
- **The context manager's counts are acknowledged at creation** (`mgr_counts_acked`). The
  driver's `binder_become_context_manager` sets `has_strong_ref` and `has_weak_ref`. Without
  this, a reference to the manager's node sent `BR_INCREFS` with ptr and cookie 0, and
  servicemanager's libbinder incremented a null weakref (`IPCThreadState::executeCommand`,
  then `RefBase::weakref_type::incWeak` at `this` = 0). This fixed the crash that blocked
  `service list`.
- **A count a transaction gives to its sender's node goes to the sending thread**
  (`xl_sender`, set around the send and the reply translation). The driver queues these to the
  sender's `todo` list (`binder_inc_node` with the sender's list). The proc's list is not read
  by a thread waiting for its reply, so `addService` of a server's local `BBinder` never gave
  it a strong ref. The server's `sp` was then the last one, and its destructor
  (`BinderLibTestService::~BinderLibTestService`, which calls `exit(0)`) ran at the end of
  `run_server`.
- **Replies carry their flags** (`TF_STATUS_CODE`). `SetError` expects the status in
  the reply.
- **Handles** are written as the whole 64-bit field, with the cookie zero, as
  `CheckHandleZeroBinderHighBitsZeroCookie` expects.
- **Real-time priority** (`sched_raise`, SchedPolicySet and InheritRt). A sync request to a
  node whose flags give a minimum real-time policy (2 bits at 9, priority in the low byte),
  or that has inherit-RT (0x800) from a real-time sender, runs its receiving thread at that
  policy and priority. The thread's own policy is restored when the request ends
  (`txn_free`, which runs at the reply). Oneway requests are not raised.

### Scheduling (sched_setscheduler 119, sched_getscheduler 120, sched_getparam 121)

- The policy and priority are stored per thread (`thread_t.policy` and `prio`, in
  `engine/sched.c`), keyed by tid. pid 0 is the calling thread. A new thread or process copies
  its creator's values, as Linux does on clone and fork.
- `sched_setscheduler`: validated as in Linux (OTHER, BATCH and IDLE need priority 0; FIFO and
  RR need 1..99; the SCHED_RESET_ON_FORK flag is accepted and ignored). Policy -1 keeps the
  policy (sched_setparam). EINVAL for a bad policy or priority, ESRCH for an unknown tid.
- `sched_getscheduler` and `sched_getparam` report what is stored.
- The engine does not schedule by these values. They are reported to the guest and used by
  binder's `sched_raise`. There are no permission checks (the guest is root).

### Signals: rt_sigqueueinfo (138) and rt_tgsigqueueinfo (240)

Both deliver the signal with the guest's `si_code` and `si_value` (offset 24 of the
siginfo), and the caller as the sender. The per-signal value is stored and written in the
handler frame and in `rt_sigtimedwait`'s siginfo, where it was `si_status` for SIGCHLD
(the same offset). For `rt_sigqueueinfo`, a non-negative `si_code`, or `SI_TKILL`, to another
process is EPERM, as I recall the rule from `do_rt_sigqueueinfo` in `kernel/signal.c`. No
kernel source is on the remote machine to check it against, so this rule is unverified. The
path exercised is `abort()` (a `rt_tgsigqueueinfo` with `SI_QUEUE`, which is delivered).

### Conformance and tests

- `binderDriverInterfaceTest`: **17 of 17**, with servicemanager running. In
  `tests/run.sh` as `binder-driver 17 cases`, in the engine and in Node.
- `binder-sm` (`tests/run.sh`, `PROP_CASES`, expected output `tests/expect/binder-sm.out`):
  `servicemanager & wait-until-ready; service list; service check manager; kill; wait`.
  Expected: `Found 1 services:`, `0<TAB>manager: [android.os.IServiceManager]`,
  `Service manager: found`. The exit status is 0. Engine and Node.
- The wait for readiness (`SM_WAIT` in `tests/run.sh`) polls `service check manager` for up
  to about 10 s. servicemanager is stopped with SIGTERM (it has no handler) and `wait`ed. The
  engine prints `killed by signal 15` on stderr for it; stdout is unaffected.
- `binderLibTest`: `tests/binderlib.sh` runs each case in its own guest process under a
  watchdog (LIMIT seconds, then SIGKILL), with servicemanager running. Its per-case lines go
  to `build/tests/out/binderlib.txt`. The run is **report only** and off by default:
  `XIAOJI_BINDERLIB=1 tests/run.sh` turns it on. It never changes the fail count.
- `binderSafeInterfaceTest` and `binderThroughputTest` were run once each by hand (below), not
  in `tests/run.sh`.

### Results (B3)

- `make test`: **124 passed, 0 failed**, with and without `XIAOJI_BINDERLIB=1` (122 before this
  pass, minus the 14-case binder entry, plus the 17-case entry and `binder-sm`, each in engine
  and Node).
- `XIAOJI_JIT=0 tests/run.sh`: **124 passed, 0 failed**.
- `make test-web`: all web checks passed.
- binderLibTest (`XIAOJI_BINDERLIB=1`, engine, one guest process per case): **41 pass**, 1
  skipped, 2 fail, 0 hang. Before this pass it was 26 pass, 10 hang, 7 fail.
  - **Pass (41):** CannotUseBinderAfterFork, WasParceled, NopTransaction, NopTransactionOneway,
    NopTransactionClear, SetError, GetId, PtrSize, IndirectGetId2, IndirectGetId3, CallBack,
    BinderCallContextGuard, AddServer, DeathNotificationStrongRef, DeathNotificationMultiple,
    DeathNotificationThread, PassFile, PassParcelFileDescriptor, PromoteLocal,
    LocalGetExtension, RemoteGetExtension, CheckHandleZeroBinderHighBitsZeroCookie, FreedBinder,
    CheckNoHeaderMappedInUser, OnewayQueueing, WorkSourceUnsetByDefault, WorkSourceSet,
    WorkSourceSetWithoutPropagation, WorkSourceCleared, WorkSourceRestored, PropagateFlagSet,
    PropagateFlagCleared, PropagateFlagRestored, WorkSourcePropagatedForAllFollowingBinderCalls,
    SchedPolicySet, InheritRt, VectorSent, BufRejected, WeakRejected, GotSid,
    ServiceNotifications.Unregister.
  - **Skipped (1):** Freeze (the cgroup freezer file is absent, so the test skips itself).
  - **Fail (2):** BinderLibRpcTest.SetRpcClientDebug and SetRpcClientDebugTwice. The test
    starts an inet server on 127.0.0.1 (`setupInetServer`), and `socket(AF_INET)` returns
    EAFNOSUPPORT (logged by the test's `RpcServer`: "Could not create socket: Address family
    not supported by protocol"). An AF_UNIX socketpair would not change this.
  - **Matches no test (4):** the `BinderLibTest/BinderLibRpcTestP.*` names. They run 0 tests
    and the report counts them as passed. They are not real passes.
- binderSafeInterfaceTest (run once, `/data/nativetest64/binderSafeInterfaceTest`): **9 pass,
  1 fail, 1 crash, 6 not run** (17 in all). Failure: `TestIncrementNativeHandle`
  (`fdsAreEquivalent` compares `st_dev` and `st_ino`; the engine's synthetic inode is
  `0x4000 + fd`, so a received fd, with a new number, does not match). Crash: `TestCallMeBack`
  takes SIGSEGV at address 0x4 in the test process, but only in the full run: alone it passes.
  The six cases after it did not run.
- binderThroughputTest (run once): exit 0. `iterations per sec: 11074.3`, average 0.165 ms,
  worst 2.008 ms.
- Warnings: `clang -Wall -Wextra -Wno-unused-parameter -c` and `emcc -O2 -std=c11 -Wall -Wextra
  -Wno-unused-parameter -c`, per file, on `binder.c`, `logd.c`, `sock.c`, `sysfs.c`,
  `syscall.c`, `sched.c`, `main.c` and `vfs.c`: 0 warnings, 0 errors.

### Gaps (B3)

- **RPC (SetRpcClientDebug, SetRpcClientDebugTwice)** need `AF_INET` stream sockets on
  loopback (the inet server), which the engine does not provide. Not attempted: AF_UNIX
  socketpair and listen/accept do not cover this test.
- **Guest-to-guest AF_UNIX stream sockets** (socketpair, bind, listen, accept, blocking
  connect) are not implemented. The `sock.c` table serves only engine endpoints (the property
  service, logd). B4 (zygote) needs them: the work is a listener table, blocking reads,
  poll integration and fd passing, so it is a step of its own.
- **binderSafeInterfaceTest** has the two failures above. `TestIncrementNativeHandle` needs
  the fstat inode of a file description, not of the fd number (`vfs.c`'s `out->ino`). Not
  changed here. `TestCallMeBack` crashes only after the earlier cases in the same guest
  process. Not diagnosed. What the code shows: `node_maybe_free` frees only dead nodes, and
  `xl_object` reuses the node that `node_by_ptr` finds for a pointer without comparing its
  cookie. So a node of a live owner is never freed, and a new object at a reused address can
  be given that old node. The plan's rule (node death once strong reaches zero and the owner
  has acknowledged) is not implemented for live owners. Whether this is the TestCallMeBack
  crash is unverified. The count paths were not changed for this.
- **DeathNotificationMultiple** failed once in the first of about 16 runs (its callbacks
  timed out after 5 s). It passed 10 of 10 later runs. Not diagnosed.
- **Not in default `make test`**: binderLibTest stays report-only. One full pass (48 cases,
  each a guest process) took about 3 minutes here, so the passing subset is far over the 30 s
  budget. One run in about 16 also failed (DeathNotificationMultiple).
- **Scheduling is not enforced.** The policies are stored and used only by binder's
  `sched_raise`. There is no CFS or RT effect, and no permission checks.
- **Not verified by any run:** oneway ordering beyond the OnewayQueueing case, fd passing (FD and FDA
  through binder), PTR scatter-gather (`BC_TRANSACTION_SG`), `BR_TRANSACTION_SEC_CTX` beyond the
  fixed context, death notifications beyond the request and clear pair, the count
  acknowledgements (`BC_ACQUIRE_DONE`, `BC_INCREFS_DONE`) in general, and the sigqueue EPERM
  path.
- **Reply path counts.** Only the sender's thread is used for a transaction's counts. The
  reply path uses the same rule, but no test here checks a reply that carries a binder.
- `BC_ATTEMPT_ACQUIRE` and `BC_ACQUIRE_RESULT` are accepted and ignored.
- `eventfd` writes that would overflow return EAGAIN even on a blocking fd. `ppoll` ignores
  its signal mask. epoll is level-triggered only.
- Threads leave the proc when `BINDER_THREAD_EXIT` runs or when the proc closes.
- The clock is the host's monotonic clock. `CLOCK_REALTIME` timers are EINVAL.
- logd output is stderr only, and it is off by default. The pid printed is the engine's guest
  pid, not a kernel credential.
- The engine's `SYS_SOCKET` case in `syscall.c` (EACCES) is dead code: `sysfs_call` handles
  socket(2) first. Left as it was.

## B4a: AF_UNIX sockets between guest processes

Status: **B4a sockets, init and zygote (to the blocker below) are in the engine, uncommitted.** The socket
section is step 1 (`tests/guest/unixsock.c`, 35 checks, the same output in the native engine, in Node and
natively in the builder VM). The init section follows it, and the zygote section comes last.

### Design as built

- **Files.** `engine/sock.c` and `engine/sock.h` (the socket objects, names, queues, connect and
  accept, blocking, options), `engine/sockcall.c` and `engine/sockcall.h` (the syscalls 198 to 212
  and 242: marshalling of guest memory, descriptors and `msghdr`), and the hooks in `engine/vfs.c`
  (`vfs_socket`, `vfs_socket_adopt`, `vfs_desc_ref`, the `S_IFSOCK` stat override), `engine/poll.c`
  (readiness), and `engine/sysfs.c` (`read`, `readv`, `write`, `writev` on a socket go to
  `sockcall`). The endpoints (property service, logd) keep their ops table.
- **Objects.** A `sock_t` is owned by open file descriptions (one reference each, `refs`) and by
  two internal holds (`hold`): a pending server-side stream socket is held by its listener's accept
  queue, and a connected datagram socket holds its destination. The last description runs the
  close effects (`sock_unref` then `sock_release`): the name is unbound, the peer gets `peer_gone`
  (a hangup) and its `peer` pointer is cleared, queued messages are dropped (their descriptions are
  released), and pending connections are abandoned. The object is freed when no description and no
  hold is left.
- **Names.** Abstract names (a leading NUL) are kept in a list of live bound sockets. A path is a
  guest path: `bind` makes a placeholder file (`O_CREAT|O_EXCL`) and records the canonical path in
  `fs_list`, which `stat` reports as `S_IFSOCK`. The name as given (`disp`) is what `getsockname`,
  `getpeername` and a message's sender report, so a path under a symlink (macOS `/tmp`) keeps its
  spelling. A missing parent directory makes the path its own name (this is how
  `/dev/socket/property_service` reaches the endpoints).
- **Connect (as native).** A stream `connect` queues a server-side socket on the listener and
  returns 0 at once, without waiting for `accept`. It fails with EAGAIN (nonblocking) or blocks
  (`sock_block` on the listener) when the backlog is full: a listener takes `backlog + 1` pending
  connections, as Linux does. ECONNREFUSED when a socket node exists with no listener, ENOENT when
  the path does not exist. The plan's "EINPROGRESS for nonblocking" is **not** used: native
  connect on a local socket never returns it.
- **Blocking.** A blocked call parks with `sched_block_restart` on the socket's key (its address).
  Every change (queue, accept queue, state) wakes the key and `poll_notify`. A call changes nothing
  before it parks, so a restart is safe. Timeouts (`SO_RCVTIMEO`, `SO_SNDTIMEO`) keep a per-thread
  deadline (`sock_block`), so a rerun does not extend it; a success or another error clears it.
- **Stream sends.** A blocking stream send sends in 64 KiB chunks. Its progress is kept in
  `cpu_t.io_done` (as pipe writes do), and the descriptions of `SCM_RIGHTS` go only with the first
  send that succeeds. A restart never sends twice. SIGPIPE is raised only when nothing was sent (the
  kernel's `unix_stream_sendmsg` rule); the partial-then-EPIPE case is not tested.
- **Messages.** A datagram or seqpacket send is one message, and it is sent whole (EMSGSIZE beyond
  the receiver's buffer). A stream read stops at a message that carries descriptions, so the
  descriptions arrive with the data that was sent with them. `MSG_PEEK` holds the descriptions
  instead of moving them. `MSG_TRUNC` returns the full length of a truncated message. `MSG_WAITALL`
  on a stream waits for the buffer's length (or EOF), without consuming anything until then.
- **Descriptors.** `SCM_RIGHTS` holds the sender's descriptions (`vfs_desc_hold`) and the receiver
  installs them (`vfs_desc_install`, cloexec from `MSG_CMSG_CLOEXEC`). A control buffer that is too
  small drops the rest with `MSG_CTRUNC`. `SCM_CREDENTIALS` is delivered when the receiver has
  `SO_PASSCRED`.
- **Credentials.** `SO_PEERCRED` is the peer's pid at connect time (uid and gid are 0, as the
  engine runs as root). Pids are the guest pids.
- **Options.** `SO_TYPE`, `SO_ERROR` (always 0: there is no pending connect error), `SO_ACCEPTCONN`,
  `SO_DOMAIN`, `SO_PROTOCOL`, `SO_PASSCRED`, `SO_PEERCRED` (read only), `SO_SNDBUF` and `SO_RCVBUF`
  (set to twice the request, capped as Linux caps `rmem_max`, minimum 4608; default 425984), and
  `SO_SNDTIMEO` and `SO_RCVTIMEO`. Other options are ENOPROTOOPT.
- **Readiness.** `poll_revents` asks `sock_state`: readable (queued data, a pending connection, or
  end of file), writable (room in the peer's queue; always for a datagram), and HUP (the peer
  closed). The old "always readable" stub is gone for real sockets. Endpoints are still always
  ready.

### Test

- `tests/guest/unixsock.c` (freestanding, built by `make guest-tests`; `make test` runs it as
  `unixsock-O0` and `unixsock-O2`, in the engine and in Node, against the native output): 35 lines.
  It covers socketpair (stream, seqpacket, datagram), message boundaries, truncation, `MSG_PEEK`,
  end of file on peer close, `shutdown(SHUT_WR)` with EPIPE, `SO_TYPE`, `SO_RCVBUF`,
  `SO_RCVTIMEO` (EAGAIN), `ppoll` readiness, a bound path (`S_IFSOCK`, `getsockname`), a
  nonblocking accept (EAGAIN), `SO_ACCEPTCONN`, the backlog (two queued, then EAGAIN), ECONNREFUSED
  after the listener closes, ENOENT, an abstract name with data, a stream server with a forked
  client, `SCM_RIGHTS` (a pipe's write end passed to the child), `SO_PEERCRED` (the child's pid),
  a seqpacket server with a forked client, a blocking write of 1 MiB to a slow reader across fork,
  a blocked `ppoll` (a pending accept) and a blocked `epoll_pwait` (data, then the hangup),
  `SCM_CREDENTIALS` with `SO_PASSCRED`, and `getpeername` (unnamed, and ENOTCONN).
- Output identical in the native engine, in Node, and natively in the builder VM.
- `logd`: `xiaoji --logcat --root build/rootfs -- /system/bin/log -p w -t mytag hello` prints
  `W/mytag(1000): hello`.

### Gaps (B4a sockets)

- Autobind (`bind` with a length of 2) is EINVAL; there is no autobound abstract name.
- `connect` never returns EINPROGRESS (the native behaviour for local sockets).
- The placeholder file is a regular file on the host. If the path is unlinked and a regular file
  is made at it later, `stat` still reports `S_IFSOCK` (the name stays in `fs_list`).
- `SCM_CREDENTIALS` on send uses the sending process's pid; a credential the sender sets is not
  checked. Uid and gid are always 0.
- `SO_ERROR` is always 0. `SO_REUSEADDR`, `SO_KEEPALIVE` and the other socket options are ENOPROTOOPT.
- A control buffer is capped at 64 KiB, and a message's descriptions are capped at 253.
- The receiver's buffer (`SO_RCVBUF`) limits the bytes queued per socket. A seqpacket or datagram
  sender waits when the receiver's queue is full, with no message count limit.
- The SIGPIPE rule for a send that is partly done and then hits EPIPE follows the kernel source; it
  is not checked natively.

### B4a: the built-in init (`--init CONFIG`)

Status: **done (uncommitted), tested in the engine and in Node.** Files: `engine/init.c`,
`engine/init.h`. Hooks: `engine/sched.c` (`run_loop`, `sched_run_init`, `sched_proc_new_child`,
`sched_start_thread`), `engine/main.c` (`--init`, `--init-timeout`), `engine/props.c` (`ctl.*` and
`xiaoji.exit`), `engine/sock.c` (`sock_fsnode_del`), `engine/sockcall.c` (registry-only nodes under
`/dev/socket`), `engine/sysfs.c` (`unlinkat` drops a socket node).

- **Run.** `xiaoji --init image/xiaoji.rc --root build/rootfs [--init-timeout SECS]`. `--root` is
  required. The program argument is not used. Logs (`xiaoji: init: ...`) go to stderr; the services'
  stdin, stdout and stderr are `/dev/null` (B4b; `--init-stdio host` keeps the engine's).
- **No thread for init.** The initial process has no guest thread. `sched.c` calls init's tick
  between slices and whenever nothing is runnable. Every spawn, restart and stop happens there, never
  inside a guest syscall. When nothing is runnable, the loop sleeps at most 10 ms (or until init's next
  restart), so a SIGINT or the timeout is seen. After 5 s with nothing runnable, the thread table is
  printed once (`report_deadlock`), so a hang is not mistaken for slow work.
- **Stopping.** init stops when `xiaoji.exit` is set (any value, through the property service), on
  SIGINT, or at `--init-timeout`. It kills the services with SIGKILL and removes their socket nodes.
  The engine exits with status 0.
- **Spawning.** A service is a fork of the initial process (its address space and descriptors are
  copies, so it inherits the sockets init made for it, with cloexec off), then the program is loaded
  with the same loader as `main`. Its thread starts at the entry point. init closes its own copies of
  the sockets after the fork. A failed load releases the child.

#### Config format (a subset of Android's `.rc`)

```
export NAME VALUE              # global environment (top level or in an on-block)
property NAME VALUE            # a property set at boot (top level)
on early-init                  # an on-block: export, setprop and mkdir lines, in order
    mkdir /data/dalvik-cache 0771
service NAME PATH ARGS...      # ARGS may be "double quoted" (the quotes group words)
    socket NAME stream|dgram|seqpacket MODE [user group [context]]   # /dev/socket/NAME
    oneshot                    # not restarted after it exits
    disabled                   # not started at boot (ctl.start starts it)
    setenv K V                 # service environment
    restart_delay_ms N         # engine option: delay before a restart (default 1000)
```

Other `.rc` options (`class`, `user`, `group`, `critical`, `onrestart`, `task_profiles`, `priority`,
and so on) are accepted and ignored. `import` is not followed. Unknown directives are reported on
stderr.

- **Start order.** The services that are not `disabled`, in file order.
- **Restarts and crash loops.** A service that is not `oneshot` and was not stopped is restarted
  after its delay. Four exits within 60 s stop it for good (`crash-looping; not restarting`).
- **Properties.** init sets `init.svc.<name>` to `running`, `restarting` or `stopped`.
- **`ctl.start`, `ctl.stop`, `ctl.restart`** (value: the service name) are queued by the property
  service and acted on between slices. A restart kills the process and starts a new one at once.
- **Global environment and classpaths.** The `export` lines, then BOOTCLASSPATH,
  DEX2OATBOOTCLASSPATH, SYSTEMSERVERCLASSPATH and STANDALONE_SYSTEMSERVER_JARS, derived at start.
  Derivation follows `derive_classpath.cpp` (the remote tree): the fragments
  `/apex/com.android.art/etc/classpaths/bootclasspath.pb`, then `/system/etc/classpaths/...`, then the
  `/apex/*` ones in name order, deduplicated (for the system-server list, the system fragment comes
  first). Jars are filtered by `min_sdk_version` and `max_sdk_version` against `ro.build.version.sdk`
  (codenames S, T, U, V map to 31 to 35). The `.pb` files are parsed as protobuf by hand.
  **Check:** `tests/init/classpath.rc` prints the four values, and they equal the output of the guest's
  own `/apex/com.android.sdkext/bin/derive_classpath` run in the engine (`tests/expect/init-classpath.out`).
  They are longer than the hand-written list in `tools/guest-env.sh` (31 jars against 12), which is
  left unchanged, since the ART cases depend on it.

#### Sockets made by init

- `socket NAME TYPE MODE` makes an AF_UNIX socket bound to `/dev/socket/NAME` and passes it as
  `ANDROID_SOCKET_NAME=<fd>`. init does not `listen`: Android's `LocalServerSocket` does (it calls
  `impl.listen` in its constructor, checked on the remote tree).
- **Deviation from the plan.** `/dev/socket` is not a real directory. `/dev`, `/proc` and `/sys` are
  served by the engine and never read from the host, so a socket node under `/dev/socket` is a name
  in the engine's registry, with no file. `stat` of such a node is ENOENT (a `stat` of a node under
  `/tmp` is `S_IFSOCK`). `unlinkat` of a bound path drops its name. Connecting to a name that is not
  bound and has no socket node is ENOENT, and to a name whose socket has closed is ECONNREFUSED.
- **Test.** `tests/init/sockets.rc` runs `tests/guest/initsock.c` twice. The server checks that
  `getsockname` reports `/dev/socket/xiaoji`, listens, accepts and answers; the client connects by
  path (retrying until the server listens) and then stops init. Output: `server: name ok`,
  `server: got ping`, `client: got pong`.

#### Tests (`make test`, each in the engine and in Node)

- `init-smoke`: `tests/init/smoke.rc`. servicemanager, and a oneshot that runs `service list` after
  `service check manager` succeeds, then sets `xiaoji.exit`. Expected: `tests/expect/init-smoke.out`.
- `init-classpath`: the four classpath variables (above).
- `init-sockets`: the init-made socket (above).
- `init-ctl`: `tests/init/ctl.rc`. `ctl.stop`, `ctl.start` and `ctl.restart` on a service, observed
  through `getprop init.svc.looper`. A crash-looping service ends `stopped`. The driver polls with
  `until`, never with a fixed sleep for the states it checks.
- SIGINT was checked by hand: the engine exits and a sleeping service is killed, with no process left.

#### Gaps (init)

- A service's child processes are not killed when the service stops (they are reparented to init and
  keep the service's sockets, so a restarted service can see EADDRINUSE). Android's init kills the
  process group.
- Services run with the engine's ids (root). `user` and `group` are not applied.
- `ro.product.cpu.abilist` and `abilist64` are set in `image/xiaoji.rc`, not derived. Android's init
  derives them from `ro.<partition>.product.cpu.abilist*` (`property_initialize_ro_cpu_abilist`); the
  values are what that derivation gives this image.
- `init.environ.rc` is copied into `image/xiaoji.rc` (its imports are not followed).
- No SELinux labels, no property contexts, no `persist.*`, no uevents, no cgroups.
### B4a: zygote

Status: **zygote preloads its classes and resources (milestone b) and forks system_server (milestone c).
The descriptor allowlist refusal of B4a is fixed (B4b). system_server then stops at its storage bind
mount, which needs mount namespaces (B4b, the blocker).** The config is `image/xiaoji.rc`: the `early-init` exports of `init.environ.rc`, the
servicemanager block and the zygote64 block of the Android 13 rc files (verbatim, options the engine
does not model ignored), plus the properties and `mkdir` lines below. Run it with `--logcat`,
`XIAOJI_STAMP=1` (each line then starts with `[ms insn]`: milliseconds and instructions since the first
log line) and `--init-timeout`.

#### Milestones

One run of the native engine on the Mac (AC power, Low Power Mode off), `XIAOJI_STAMP=1`:

| Milestone | Logcat line (source: `ZygoteInit.java`, `AndroidRuntime`) | Elapsed | Instructions |
|---|---|---|---|
| (a) app_process starts ART in zygote mode | `I/AndroidRuntime: Using default boot image` | 2.05 s | 140.8 M |
| (b) classes preloaded | `I/Zygote: ...preloaded 16252 classes in 29078ms.` | 38.1 s | 2.31 G |
| (b) resources preloaded | `I/Zygote: ...preloaded 41 resources in 59ms.` | 38.5 s | 2.33 G |
| (c) zygote calls forkSystemServer | `Zygote.forkSystemServer` in the stack below | 50.2 s | 3.15 G |
| (c) system_server forked | not reached (B4a: refused by the allowlist; see B4b) | | |
| (d) system_server | not reached | | |

The class preload is 29 s of guest time (the guest clock), about 2.2 G instructions after (a): about
60 M instructions per second overall on this machine. The second zygote (pid 1010) is a restart after the fork
failure, not a second boot; its preload takes the same instructions.

#### The B4a blocker (fixed in B4b)

`F/zygote64: JNI FatalError called: (system_server) Not allowlisted (37): /dev/stdin`. The fd walk of the
fork (`FileDescriptorTable::Create`, `fd_utils.cpp`) skips 0 to 2 and reads the target of every other
descriptor. Descriptor 37 shared the description of a stdio descriptor, and the engine named that
description `/dev/stdin`. The fix is in B4b.

#### Engine changes made for zygote

Real implementations unless marked as a stub.

- **Instructions.** FCVTAS and FCVTAU (scalar, rounding ties away); vector by-element FMLA, FMLS, FMUL
  and FMULX (single and double), and MUL, MLA and MLS (16- and 32-bit lanes); vector floating-point
  three-same (FADD, FSUB, FMUL, FDIV, FMAX, FMIN, FMAXNM, FMINNM, FMLA, FMLS, FMULX, FABD, FRECPS,
  FRSQRTS, FCMEQ, FCMGE, FCMGT, FACGE, FACGT, and the pairwise forms); vector floating-point
  two-register miscellaneous (FABS, FNEG, FSQRT, SCVTF, UCVTF, FRINT*, FCVT* to integer); scalar DUP
  (element); scalar FP pairwise (FADDP, FMAXNMP, FMAXP, FMINNMP, FMINP). FMAX, FMIN and the NM forms
  give +0 for the maximum and -0 for the minimum of two zeros, and propagate or ignore NaN as the
  architecture says (`fp_maxmin`). FPCR non-default modes are refused, as before.
- **Oracle.** `tests/guest/fpvec.c` runs 71 of these forms on fixed inputs (signed zeros, infinities,
  NaN, ties, negatives) through inline assembly and prints the raw results. Native and engine output are
  byte-identical, in the engine and in Node (`make test`).
- **Syscalls.** `process_vm_readv` (270) for the caller's own pid: copies between the caller's ranges and
  returns the bytes copied before an unreadable range (EFAULT when nothing was copied). ART's `SafeCopy`
  calls it on its own pid. `/proc/self/task` and `/proc/<pid>/task` list the threads (ZygoteHooks reads
  the list), and `/proc/self/fd` lists the open descriptors (the fd walk of ART's fork). `clock_gettime`
  accepts CLOCK_BOOTTIME, MONOTONIC_COARSE and REALTIME_COARSE.
- **Credentials** (`engine/cred.c`): each process has real, effective, saved and filesystem uids and gids,
  and its supplementary groups; a fork copies them. The set*id family, setgroups and getgroups follow
  `kernel/sys.c`. A process with effective uid 0 is privileged. Socket credentials (`SO_PEERCRED` and
  `SCM_CREDENTIALS`) use the effective uid and gid. The ids are not used for access checks (stat reports
  uid and gid 0 for every file).
- **Named stubs** (where the guest asks for something the engine does not model):
  - `unshare(CLONE_NEWNS)` succeeds with no effect (one mount table is shared). Other namespace flags,
    and CLONE_FILES, are EINVAL.
  - `mount` with a propagation change only (MS_SHARED, MS_SLAVE, MS_PRIVATE, MS_UNBINDABLE) succeeds on
    an existing target with no effect. Any other mount is EPERM.
  - `membarrier`: QUERY reports the global and private expedited commands. Guest threads run one at a time
    on one host thread, so every barrier already holds.
  - `fchownat` and `fchown` succeed on an existing path or descriptor with no effect (no ownership model).
  - `capget` (version 3) reports the full set for a root process; `capset` is accepted and ignored.
- **fcntl**: F_SETPIPE_SZ and F_GETPIPE_SZ on a pipe report the fixed 64 KiB capacity.

#### Properties and directories set in the config (and why)

- `ro.product.cpu.abilist` and `ro.product.cpu.abilist64` = `arm64-v8a`. Without them ART aborts with
  "Unable to determine ABI list". init derives them from `ro.<partition>.product.cpu.abilist*`
  (`property_initialize_ro_cpu_abilist`); the engine does not derive them yet.
- `ro.zygote.disable_gl_preload` = `true`. There is no OpenGL ES driver, and libEGL aborts the preload
  without one. ZygoteInit skips its GL preload when this is set (`PROPERTY_DISABLE_OPENGL_PRELOADING`).
- `mkdir /data/dalvik-cache` and `mkdir /data/dalvik-cache/arm64` (ART's cache). They persist in
  `build/rootfs/data` between runs (the `mkdir` is a no-op when they exist).

#### Known log lines (not fatal, recorded so they are not mistaken for the blocker)

- `memfd_create failed: Invalid argument, no memfd support` then `Failed to create JIT Code Cache` and
  `Missing code cache, cannot create JIT`: the JIT is off in this boot, and zygote carries on without it.
  The engine's `memfd_create` accepts only MFD_CLOEXEC; ART asks for more (sealing), so the JIT is
  disabled rather than made to work.
- `libprocessgroup`: `RC file was not initialized properly`, `controller cpuset is not found`. There are
  no cgroups, so task profiles do nothing.
- `Error opening /proc/mounts` (no `/proc/mounts`), `Unable to find pattern file` (no `/proc/sys/...`
  boot_id), `JKS KeyStore not available` (a Java security provider, from a missing keystore file).

#### Missing daemons, and their effect so far

- **hwservicemanager**: disabled in the rc file and not started. zygote's preload does not need it.
- **logd**: not needed. liblog writes to `/dev/socket/logdw`, which the engine serves (its sink), so
  logcat lines go to stderr.
- **lmkd, vold, netd, installd, apexd, statsd, and the rest**: not started. Effects: no cgroups (above),
  no storage mounts (vold: `/proc/mounts` is missing and there is no mount table), no GPU (EGL is off),
  and `/apex` is the extracted rootfs, not images mounted by apexd.
- **Crash dump**: debuggerd's helper cannot read `/proc/<pid>` of a crashed process, so a fatal signal ends
  in SIGABRT with no tombstone.

#### Repeatability

- A boot writes only the `dalvik-cache` directories under `build/rootfs/data`, and `/mnt/user/0` under
  `build/rootfs/mnt` (B4b; the test harness copies its own test binaries to `data` too).
- `XIAOJI_BOOT=1 tests/run.sh` runs the boot and passes when the log has `...preloaded N classes`. It takes
  about 60 s more than the default run, so it is opt-in. It stops with `--init-timeout 90`.

## B4b: system_server

Status: **the descriptor allowlist blocker of B4a is fixed, and zygote forks system_server (milestone c).
The storage bind mount that stopped B4b is resolved by per-process mount namespaces (B4c). Read "The
blocker (now)" below as the B4b state; the current state is in B4c.**

### Files

- `engine/vfs.c`, `engine/vfs.h`: the Linux-style readlink text of `/proc/self/fd/N` for sockets and pipes
  (`fd_link_text`), and the inode of a pipe or socket, numbered at creation (`vfs_fd.ino`), used by fstat
  and by the link.
- `engine/init.c`, `engine/init.h`: `svc_stdio_null` (the services' stdio), `init_set_stdio_host`.
- `engine/main.c`: `--init-stdio null|host`.
- `engine/syscall.c`, `engine/linux.h`, `engine/proc.h`, `engine/proc.c`: `prctl` (the subset below),
  with per-process state that a fork copies.
- `image/xiaoji.rc`: `mkdir /mnt/user` and `/mnt/user/0` (the on-init directories of `init.rc`).
- `tests/guest/fdnames.c`, `tests/guest/prctl.c`: new deterministic guest tests (`make test`, native
  oracle, engine and Node).
- `tests/init/stdio.rc`, `tests/expect/init-stdio.out`: the services' stdio (below).
- `tests/run.sh`: `--init-stdio host` for the four init cases that read a service's stdout; the new
  `init-stdio` case; the `XIAOJI_BOOT=1` check now also requires the system_server fork line.

### Services' stdio (fixes the allowlist blocker)

- **Default `--init-stdio null`.** Each service's fds 0, 1 and 2 are `/dev/null` (the engine's
  synthetic `SYN_NULL`), opened once O_RDWR and dup'd onto 0, 1 and 2. This is Android's `SetupStdio`
  (`system/core/init/service_utils.cpp` on the remote tree). The engine's own root process keeps its host
  stdio, which the tests rely on.
- **Why the blocker happened.** ART's table (`fd_utils.cpp`, `FileDescriptorTable::Create`) skips 0
  to 2 and reads the target of every other descriptor. Descriptor 37 shared the description of a stdio
  descriptor, and the engine names a description by its name, so the link read `/dev/stdin`, which is not
  allowlisted. With `/dev/null` on the stdio descriptors, the same shared description reads `/dev/null`,
  which is allowlisted.
- **`--init-stdio host`** keeps the engine's own stdio for the services, for debugging. The names are
  then `/dev/stdin`, `/dev/stdout` and `/dev/stderr`, and zygote fails the allowlist again. The four init
  cases that read service stdout (`init-smoke`, `init-classpath`, `init-sockets`, `init-ctl`) run with it.
- **Test.** `tests/init/stdio.rc` runs `fdnames --stdio --out ...` as a oneshot under the default
  stdio. The report goes to a file, since stdout is `/dev/null`. It must equal `tests/expect/init-stdio.out`
  (fds 0 to 2 read `/dev/null`), with an empty stdout and exit 0, in the engine and in Node.

### Linux-style fd names and inodes

`readlink /proc/self/fd/N` (the last component, not followed) reports what Linux reports:

| Kind | Before | Now | fstat |
|---|---|---|---|
| pipe (both ends and dups) | `pipe:[serial]` | `pipe:[ino]` | S_IFIFO, ino per pipe |
| socket | no name (NULL, a crash on readlink) | `socket:[ino]` | S_IFSOCK, ino per socket |
| eventfd, timerfd, epoll | `anon_inode:[...]` | unchanged | unchanged |
| memfd | `/memfd:NAME (deleted)` | unchanged | S_IFREG |
| files, devices | the guest path | unchanged | unchanged |

- The inode of a pipe or socket was `0x4000 + slot`, so a dup had a different inode from the original.
  It is now numbered when the object is made, from a counter (`vfs_fd.ino`): a pipe's both ends share
  the pipe's serial (`0x40000000 + serial`), a socket gets `0x41000000 + serial` from `vfs_socket_adopt`,
  and a dup shares its description. The digits in the link are the same number that `fstat` reports.
  The numbers do not depend on addresses, so they are the same on every run.
- Following `/proc/self/fd/N` (a `stat` through the link) still names the link text, so it is ENOENT for
  a pipe or socket, as it was for a pipe. This is a gap, not a fidelity claim.
- **Test:** `tests/guest/fdnames.c` opens each kind in a fixed order and prints the normalised link and
  whether the inode matches. Its output is byte-identical natively (builder VM) and in the engine.

### `prctl` (the zygote and system_server subset)

Implemented in `sys_prctl`, with the Linux answers: dumpable (default 1, as Linux starts a process; was
0), set dumpable (0 or 1), `PR_SET/GET_KEEPCAPS`, `PR_CAPBSET_READ` (all of bits 0 to 40, which is the
oracle's `CapBnd` 0x1ffffffffff) and `PR_CAPBSET_DROP` (uid 0 only, else EPERM), `PR_SET/GET_SECUREBITS`,
`PR_SET/GET_NO_NEW_PRIVS` (the setting to 1 only), `PR_SET/GET_PDEATHSIG` (recorded only; no signal is
sent), `PR_SET/GET_TIMERSLACK` (default 50000), `PR_SET/GET_CHILD_SUBREAPER`, the tagged-address control
(none: 0), `PR_SET_PTRACER` (accepted with no effect, as the oracle's Yama does), and the existing name
and VMA options. The state is per process and a fork copies it. **Test:** `tests/guest/prctl.c` (as an
ordinary user, so the bounding-set drop is not in it), byte-identical natively and in the engine.

### Milestones (B4b)

One run of the native engine on the Mac (AC power), `XIAOJI_STAMP=1`, the log of the stop at the blocker:

| Milestone | Logcat line | Elapsed | Instructions |
|---|---|---|---|
| (a) app_process starts ART | `I/AndroidRuntime: Using default boot image` | 2.05 s | 140.8 M |
| (b) classes preloaded | `I/Zygote: ...preloaded 16252 classes in 28782ms.` | 37.7 s | 2.31 G |
| (b) resources preloaded | `I/Zygote: ...preloaded 41 resources in 59ms.` | 38.2 s | 2.33 G |
| (c) system_server forked | `I/Zygote: System server process 1008 has been created` | 49.9 s | 3.15 G |
| (d1) system_server starts | `SystemServer: Entered the Android system server!` | not reached | |
| (d2) startBootstrapServices done | not reached | | |
| (d3) startCoreServices done | not reached | | |
| (d4) startOtherServices | not reached | | |
| (d5) System now ready / Boot completed | not reached | | |

The fd blocker of B4a was at 50.1 s and 3.15 G, so (c) moved only by the fork itself.

### The blocker (now): the storage bind mount

```
[49934 ms 3153317007 insn] I/Zygote(1002): System server process 1008 has been created
[49937 ms 3153374140 insn] F/zygote64(1008): jni_internal.cc:825] JNI FatalError called: (system_server) frameworks/base/core/jni/com_android_internal_os_Zygote.cpp:792: Failed to mount /mnt/user/0 to /storage: Operation not permitted
```

- `nativeForkSystemServer` calls `SpecializeCommon` with `MOUNT_EXTERNAL_DEFAULT` (Zygote.cpp line
  2202, hard-coded), so every system_server runs `ensureInAppMountNamespace` (`unshare(CLONE_NEWNS)`)
  and then `BindMount("/mnt/user/0", "/storage")`.
- The engine has one mount table: `unshare(CLONE_NEWNS)` is a no-op, and any bind mount is EPERM (the
  propagation-only `mount` stub is the one thing that succeeds).
- **Why this is a stop, not a patch.** A bind mount is per-process state that every later path lookup
  must honour. A global alias would give every process the same `/storage` view, and apps need
  different per-user and per-app binds (`BindMountStorageToLowerFs`), so one table is wrong for them too.
  The emulation that is correct is a mount table per process (copied by `unshare(CLONE_NEWNS)`; a plain fork shares it, as on Linux),
  with bind aliases applied in `vfs_resolve`. That changes path resolution for every syscall, so it needs
  a decision, not a local fix. Options, for the decision: (1) per-process mount tables in the VFS, the
  correct one; (2) a global `/storage` alias, which is quick but wrong for apps; (3) a different
  `/storage` source per process, set by the engine, which only hides the problem for system_server.
  The mount-namespace rule the engine must follow: a plain `fork` shares the parent's mount namespace on
  Linux, and only `CLONE_NEWNS` (by `clone` or `unshare`) makes a copy (`mount_namespaces(7)`).
- After the abort, init restarts zygote (`started zygote (pid 1015)` in this run, after `zygote exited
  (status 137, by a signal)`), and the new zygote repeats the 50 s preload and the same failure. The
  boot test stops at `--init-timeout`.

### Component choices

| Component | Choice | Why |
|---|---|---|
| Services' stdin, stdout, stderr | `/dev/null` per service, as Android's `SetupStdio` (fixed) | ART's fd allowlist; matches a device |
| Seccomp filter (zygote's `SetUpSeccompFilter`) | not reached: it runs after the storage mount (Zygote.cpp lines 1646 and 1578). By the code it is skipped when SELinux is not enforcing (`gIsSecurityEnforced`) | The engine's SELinux is permissive (B3). To confirm when the boot gets past the mount |
| `PR_SET_PTRACER` (Yama) | accepted, no effect | The oracle kernel has Yama and accepts it |
| `/mnt/user/0` tree | created by config (`mkdir`, as the on-init block of `init.rc`) | zygote's `PrepareDir` needs it |
| `/storage` and the per-user bind mounts | **blocked** (the design question above) | Needs per-process mount namespaces |
| hwservicemanager, HALs (power, health, light, sensors, ...), lmkd, installd, netd, vold, statsd, storaged | not decided yet | system_server has not reached the code that starts them |
| Telephony, NFC, Bluetooth | not decided yet | Decide from docs/features.md when system_server reaches them |

### Profile (observation only, not optimised)

`XIAOJI_PROFILE=1`, the window from boot start to the end of the zygote class preload. The run passed the
preload line (`...preloaded 16252 classes`) at 70 s of profiled wall time, and the init timeout stopped it
at 75 s, before the fork. Unprofiled, the same preload line comes at 37.7 s, so profiling roughly doubles
the wall time. The counts below are from that run.

- 7 threads, 2.43 G instructions. The zygote's main thread has 2.26 G of them.
- Samples by mapping (592727 samples, one per 4096 instructions per thread): `libart.so` 61.0%,
  `boot-core-icu4j.oat` 8.1%, `[anon]` 7.5%, `libdexfile.so` 5.4%, bionic `libc.so` 4.9%, `boot.oat` 4.8%,
  `libicuuc.so` 2.3%, `liblz4.so` 1.8%. These are the complete figures.
- Top 40 hot PCs: 42388 samples, which is 7.2% of all samples. Symbolized from `libart.so`'s `.symtab`
  with `nm` (its text segment has file offset equal to virtual address). Shares are of the top 40, so
  each is about 1 to 1.4% of all samples: `nterp_op_move_result` 19.9%, `ClassLinker::InitializeClass`
  15.0%, `nterp_op_iget_object` 14.1%, `NterpCommonInvokeInstance` 7.8%, `nterp_to_nterp_instance_non_range`
  3.6%. The other hot PCs are 14 instructions of one loop in an anonymous mapping at guest
  0x7ff00004d880 to 0x7ff00004d9ac; the profiler does not name that mapping, so it is not symbolized.
- What this supports: most samples are in `libart.so`, and the hottest single PCs are in ART's nterp
  handlers and `InitializeClass`. A guest-PC profile cannot say how the rest splits between the
  interpreter and class initialization, or what the engine's own dispatch costs. Nothing is optimised.

### Tests

- `make test`: 150 passed, 0 failed (was 140). The new cases are `fdnames-O0/O2` and `prctl-O0/O2`
  (engine and Node, against the native oracle), and `init-stdio` (engine and Node). The four init cases
  that read service stdout now pass `--init-stdio host`.
- `XIAOJI_BOOT=1 make test`: 151 passed, 0 failed. The extra case is the boot check, which requires the
  class preload and the system_server fork line.
- `XIAOJI_JIT=0 make test`: 150 passed, 0 failed.
- `make test-web`: all web checks passed (headless Chromium), exit 0.
- Output was read unfiltered (`rtk proxy make test`) for the two plain runs.

### Gaps (B4b)

- Bind mounts and mount namespaces (the blocker). `unshare(CLONE_NEWNS)` is still a no-op.
- Not exercised by any test: binder and `/dev/__properties__` descriptors, which zygote also holds. They
  are not in `fdnames`. Zygote's check passed for them in the boot (the child reached the mount), so the
  evidence is the boot, not a test.
- The fork's descriptor reopen (`ReopenOrDetach`, Zygote.cpp line 2100) runs in the child before
  `SpecializeCommon`, and the boot reached the storage mount after it. So every descriptor zygote held at
  the fork passed the reopen.
- `/dev/stdin`, `/dev/stdout`, `/dev/stderr` and `/dev/fd` are not links in the engine's `/dev`
  (Android's init creates them). Nothing has needed them yet.
- A stat through `/proc/self/fd/N` of a pipe or socket is ENOENT (B4b, Linux-style names section).
- `PR_SET_PDEATHSIG` is recorded only, and `PR_SET_SECCOMP` is not implemented (zygote's filter is not
  reached yet; see the component table).
- Only pipes and sockets have object inodes. eventfd, timerfd, epoll and memfd keep their old inodes.

## B4c: mount namespaces, system_server's entry (milestone d1)

Status: **per-process mount namespaces work (bind with and without MS_REC, read-only remount, tmpfs, stacked
mounts, umount2 with and without MNT_DETACH, unshare and clone with CLONE_NEWNS), with a guest test that matches
native. Zygote's storage mount passes, and system_server enters "Entered the Android system server!" (d1). The
HIDL service manager now runs. The boot stops in `startBootstrapServices` (d2 begun, not done): servicemanager
and system_server wait for `apexservice`, init refuses the lazy AIDL start that would provide it, and the
engine's deadlock check ends the run (see "The current blocker"). B4c part 2 (below) supersedes that blocker section; its state is the current one.** The timings below were taken on battery with
Low Power Mode on (see "Conditions"); they are not the AC, Low Power Mode off figures the benchmark rule needs.

### The namespace design

Files: `engine/mntns.h`, `engine/mntns.c` (the table and its rules), `engine/vfs.c` and `engine/vfs.h` (the
lookup; the fd table holds the namespace), `engine/sysfs.c` (`mount`, `umount2`, `unshare`), `engine/sched.c`
(`clone` with CLONE_NEWNS; the initial process's exit), `engine/procfs.c` (`/proc/mounts`, `mountinfo`),
`tests/guest/mountns.c`.

- **Ownership.** Each fd table (`fdtab_t`, one per process) holds a refcounted `mntns_t`. A fork shares it
  (`vfs_fdtab_fork`). `unshare(CLONE_NEWNS)` and `clone(CLONE_NEWNS)` give the child a copy
  (`vfs_fdtab_copy_ns`: the mounts are copied, the tmpfs contents are shared). A namespace is freed with its last
  process: a non-initial process releases it when it ends (`proc_release`), and the initial process releases it
  at its exit (`vfs_fdtab_exit_ns`, which leaves an empty namespace). Out of memory in a copy is fatal, as it is
  for the other fd-table allocations.
- **The table.** An ordered list of mounts. Each has an id, a parent id (the mount that covered its target when it
  was made, 0 for the root), a kind (`bind` of a host directory, or `tmpfs` with its own host directory), the
  read-only, nosuid, nodev and noexec flags, and the propagation (shared, or not). The root (the host root that
  `/` maps to) is not in the list: an empty list is the root alone.
- **Liveness.** A mount is live when no later mount has its target (a newer mount on the same path hides it) and
  its parent is visible: the parent's target is not covered by another mount, and the parent is itself visible.
  A mount stacked on its parent is live (its own stacking is ignored). Unmounting the top mount brings the one
  under it back. `MNT_DETACH` removes the mount and every mount under it (Linux's `umount_tree`); without it, a
  mount with children is EBUSY.
- **Lookup.** `vfs_resolve` walks visible guest paths, as before, so `..`, symlinks and `getcwd` stay in guest
  terms. Only the guest-to-host step changed: when a live mount covers the path (longest target prefix,
  component-wise), the host path is the mount's backing directory plus the rest. A covered path skips the
  synthetic `/proc`, `/sys` and `/dev` names. With no mounts the lookup is the old `host_path_of`, byte for byte
  (`mntns_map` returns 0 with an empty table; `make test` is unchanged). `vfs_res.ro` carries the read-only flag.
- **Read-only.** A write, create or truncate open (`vfs_open`), `mkdirat`, `unlinkat` and `fchmodat` under a
  read-only mount give EROFS. Mutations through an fd opened before a remount are not checked (a gap, below).
- **Flags, as Linux's `do_mount` orders them.** `MS_REMOUNT` (with MS_RDONLY and the other flags, on the live
  mount at the target); then `MS_BIND` (with or without MS_REC; a bind onto `/` is EBUSY, and a mount on `/`
  covers nothing in this model); then the propagation flags (recorded on the live mount; accepted with no effect
  on `/`); then `MS_MOVE` (EINVAL, not modelled); then the tmpfs type. A bind copies the flags of the mount that
  covers its source, and ignores the flags of the call (Linux ignores MS_RDONLY on the bind itself; a remount sets
  it). The guest test checks these cases against the native kernel (the MS_PRIVATE bind and the MS_RDONLY bind
  lines); the kernel source was not read.
- **MS_REC.** The live submounts of the source are copied under the new target, with the same backing and
  parent structure. The source's own mount point is the one being bound, so it is not copied again.
- **Syscalls.** `mount`, `umount2` and `unshare(CLONE_NEWNS)` need an effective uid of 0. `clone(CLONE_NEWNS)` is
  not checked for privilege (a gap: Linux needs CAP_SYS_ADMIN there too).
- **Not modelled** (named, so they are not mistaken for support): MS_MOVE (EINVAL), `pivot_root` and `chroot`
  (not hit), `setns` (not hit), propagation between namespaces (a copy is a snapshot), binds of synthetic paths
  such as `/proc` or `/dev` (EINVAL), mounts over synthetic paths (EINVAL), and a bind or tmpfs onto `/` (EBUSY).
- **Procfs.** `/proc/mounts`, `/proc/self/mounts`, `/proc/self/mountinfo`, and `/proc/<pid>/mounts` and
  `mountinfo` of a live process show that process's namespace, in the kernel's layout: the root line, then the live
  mounts in mount order, with octal escapes. Differences from Linux: the major:minor is `0:<id>`, the fs type and
  source of a bind are `rootfs`, the root column of a bind is the source's guest path, and `/proc`, `/sys` and
  `/dev` are not listed (the engine serves them synthetically). Zygote and vold parse these files; whether either
  needs the missing entries was not checked.
- **Tmpfs.** A host directory under `/tmp` named `xiaoji-tmpfs-<pid>-<n>`, removed (recursively, symlinks not
  followed) when the last namespace that holds it goes away, including at the initial process's exit.

### The guest test: `tests/guest/mountns.c`

A freestanding program (built like the other guest tests). It runs in a private namespace (`unshare`, then
`mount(MS_REC|MS_PRIVATE)` on `/`), in a base directory under the repository, given as an absolute path
(`$ROOT/build/tests/mountns-work`, because a guest process starts in `/`). The steps and what each checks:

- a bind and its read and write; the read-only remount (EROFS on create, mkdir and unlink; reads still work; a
  create under the source is still allowed); `umount2` (EINVAL for a non-mount; ENOENT after the unmount);
- a tmpfs stacked on a tmpfs (the top hides the file; `umount2` brings the lower one back);
- `..` out of a tmpfs into the source directory (it leaves the mount);
- rbind (MS_REC) shows the submount; a plain bind does not; `umount2` of a parent with a child is EBUSY; the child
  first, then the parent; `MNT_DETACH` of a parent takes its child with it (the child's path is then ENOENT);
- `MS_BIND|MS_PRIVATE` is a bind (the file is read through it);
- `MS_BIND|MS_RDONLY` on the call is not applied (the create succeeds); a remount sets it (EROFS);
- a child made with `clone(CLONE_NEWNS|SIGCHLD)` sees the parent's mount at clone time, its own unmount of that
  mount does not affect the parent, and its own tmpfs is invisible to the parent;
- a child made with `unshare(CLONE_NEWNS)` and its tmpfs, invisible to the parent;
- the mountinfo lines under the base, normalised: the mount point, `ro` or `rw`, and `tmpfs` (only for a tmpfs with
  its own root) or `bind`.

- The native oracle runs under `sudo -n` (`tests/run.sh` does this for `mountns-*`): the builder VM runs as uid 501
  with passwordless sudo, not as root in a chroot.
- Native and engine output are byte-identical (`make test`: `mountns-O0` and `mountns-O2`, engine and Node).
- The differences from Linux that the normalised output does not show: the fs type of a bind (Linux shows the
  source's fs, here `rootfs`), the root column, and the `0:<id>` device numbers.

### Milestones (B4c)

One run of the native engine, `XIAOJI_STAMP=1 ./build/xiaoji --logcat --init image/xiaoji.rc --root build/rootfs
--init-timeout 120` (the run with hwservicemanager started; `build/tests/boot/run3.err`):

| Milestone | Logcat line | Elapsed | Instructions |
|---|---|---|---|
| (a) app_process starts ART | `I/AndroidRuntime: Using default boot image` | 2.21 s | 158.5 M |
| (b) classes preloaded | `I/Zygote: ...preloaded 16252 classes in 29104ms.` | 38.4 s | 2.33 G |
| (b) resources preloaded | `I/Zygote: ...preloaded 41 resources in 59ms.` | 38.8 s | 2.35 G |
| (c) system_server forked | `I/Zygote: System server process 1009 has been created` | 50.6 s | 3.17 G |
| (d1) system_server starts | `I/SystemServer: Entered the Android system server!` | 51.5 s | 3.22 G |
| (d2) startBootstrapServices begins | `D/SystemServerTiming: startBootstrapServices` | 55.4 s | 3.46 G |
| (d2) startBootstrapServices done | not reached (see the blocker) | | |
| (d3) startCoreServices done | not reached | | |
| (d4) startOtherServices | not reached | | |
| (d5) System now ready / Boot completed | not reached | | |

The storage mount that stopped B4b is past (system_server's `ensureInAppMountNamespace` and the bind of
`/mnt/user/0` to `/storage` succeed, and so does the SELinux setcontext, below). The fork line is at about the same
point as in B4b (49.9 s then, 50.6 s here), with the hwservicemanager process also running.

### Conditions

`pmset -g` on the Mac while these runs were made: `lowpowermode 1` and "Now drawing from 'Battery Power'". The
task said the Mac is on AC, and the benchmark rule (memory: AC and Low Power Mode off) is not met, so the timings
above are not benchmark figures. They are a single run each, and they should be remeasured on AC with Low Power
Mode off before they are compared with B4b.

### Engine changes made for system_server

Real implementations unless marked.

- **Mount namespaces**, as above.
- **`setrlimit` (164) and `prlimit64` (261)**, per process: the table of soft and hard limits is copied by a fork
  (defaults as before: infinity, and 8 MiB soft for the stack). The new limit is checked (soft at most hard;
  raising the hard limit needs euid 0). `getrlimit` (163) reads the same table. A pid other than the caller's is
  ESRCH, or EPERM for a process that exists (the engine does not change another process's limits). This was the
  RLIMIT_CORE failure of zygote's specialize.
- **SELinux context, form only.** `/sys/fs/selinux/context` accepts a write when the context has 4 or 5
  colon-separated non-empty fields (user, role, type, sensitivity, optional categories), and reads are empty. No
  policy is compiled, so the check does not test the context against the policy (the kernel does that). This is a
  named stub; it is what libselinux's `security_check_context` needs, and the failure it fixes was that missing
  file (ENOENT, logged as "Error setting context for system server").
- **`/proc/<pid>/attr/current`** (the caller's, and a live process's) is per process: a write (setcon's) stores the
  context (the NUL dropped; at most 127 bytes) and a read returns it. The copy of the parent's context is made at
  fork. The binder driver still reports `u:r:kernel:s0` as every sender's context (a gap, below).
  `/proc/thread-self/attr/exec` and the other attr files are ENOENT.
- **`hwservicemanager` is started** by `image/xiaoji.rc` (the Android 13 block, verbatim, without `disabled`). This
  is the real daemon. It is what sets `hwservicemanager.ready`, which system_server's HIDL lookups wait for (in
  `system/libhidl/transport/ServiceManagement.cpp`, gated on `/dev/hwbinder` being accessible, which the engine
  serves). With it running, the "Waited for hwservicemanager.ready" loop is gone.

### Component choices

| Component | Choice | Why |
|---|---|---|
| Services' stdin, stdout, stderr | `/dev/null` per service (B4b) | ART's fd allowlist; matches a device |
| Zygote's storage and per-app bind mounts | real, through the engine's mount namespaces (this section) | `BindMountStorageToLowerFs`, `/storage`; apps need per-process views |
| SELinux | permissive; the context is checked for form only; attr/current is per process | The engine's SELinux is permissive (B3); no compiled policy |
| rlimits | real, per process | zygote and system_server set them; no daemon involved |
| `/proc/mounts` and mountinfo | real, from the namespace | `/proc`, `/sys` and `/dev` are not listed (gap) |
| `hwservicemanager` | **real, started** from `image/xiaoji.rc` | system_server's HIDL lookups wait for the property it sets; `/dev/hwbinder` is served by the engine's binder driver. Its HIDL transactions fail (below) |
| `apexservice` (apexd) | **not started; the lazy start is refused** | servicemanager and system_server wait for it. Decide: the real apexd (it mounts APEX images, which the engine's rootfs does not have as images: `/apex` is extracted), or an engine stub. docs/features.md must decide first |
| init's `ctl.interface_start` (lazy AIDL start) | **refused: "is not a ctl action"** | servicemanager asks for it once a second for `apexservice`. Decide with apexd: either init supports it for services declared in the rc files, or the lazy start stays off and the waiters must not block |
| Seccomp filter (zygote's `SetUpSeccompFilter`) | not reached: it is skipped when SELinux is not enforcing | Confirm when the boot reaches it |
| `userfaultfd` (syscall 282) | ENOSYS (the engine's "unimplemented syscall" line) | ART probes it and falls back; not a blocker |
| `/proc/sys/kernel/random/boot_id` | missing (non-fatal: ashmem logs it) | Record, not a blocker |
| cgroups (`/dev/cgroup_info/cgroup.rc`) | missing (non-fatal: libprocessgroup logs it) | No cgroups; task profiles do nothing |
| Other HALs (power, health, light, sensors, ...), lmkd, installd, netd, vold, statsd, storaged | not decided yet | system_server has not reached the code that starts them |
| Telephony, NFC, Bluetooth | not decided yet | Decide from docs/features.md when system_server reaches them |

### The current blocker (d2): apexservice, the lazy start, and the deadlock check

```
[50634 ms 3170995621 insn] I/Zygote(1009): System server process 1009 has been created
xiaoji: unimplemented syscall 282
[51534 ms 3221649710 insn] I/SystemServer(1009): Entered the Android system server!
[55449 ms 3458848694 insn] D/SystemServerTiming(1009): startBootstrapServices
...
[58771 ms 3506550409 insn] W/ServiceManager(1009): Waited one second for apexservice (is service started? ...)
[58771 ms 3506550409 insn] I/servicemanager(1001): Since 'apexservice' could not be found, trying to start it as a lazy AIDL service
xiaoji: init: ctl.interface_start is not a ctl action
...
xiaoji: init: nothing has been runnable for 5 s
xiaoji: deadlock: every thread is blocked and none can wake
[85501 ms 3597846663 insn] E/hw-Parcel(1002): Buffer in parent 0x6ffd6fece2d0 differs from embedded buffer 0x6ffd64c1c100
[85502 ms 3598048833 insn] E/HidlServiceManagement(1009): getService: defaultServiceManager()->getTransport returns Status(EX_TRANSACTION_FAILED): 'BAD_VALUE: '
[85505 ms 3598048833 insn] E/AndroidRuntime(1009): *** FATAL EXCEPTION IN SYSTEM PROCESS: watchdog
java.util.NoSuchElementException
	at android.os.HwBinder.getService(Native Method)
	at com.android.server.Watchdog.addInterestingHidlPids(Watchdog.java:671)
	...
```

- system_server waits in `startBootstrapServices` for `apexservice`; servicemanager asks init to start it lazily,
  and init refuses. The engine then has no runnable thread, and its deadlock check ends the processes (`killed by
  signal 9`). The Watchdog's own HIDL lookup, at 85 s, is the second failure: hwservicemanager's reply carries a
  buffer whose embedded pointer does not match the parent (`hw-Parcel`), so `HwBinder.getService` gets a bad value.
  The binder driver's scatter-gather fix-up for hwbinder (`BINDER_TYPE_PTR` with parent offsets) is the likely
  cause. It is binder work (B3), not a boot-config change, and it was not investigated here.
- Nothing here is a missing file. The two decisions are apexd and `ctl.interface_start` (the table), and the
  hwbinder buffer fix-up is an engine fix in the binder driver.

### Tests

- `make test`: 154 passed, 0 failed, 0 xfail (was 150). The new cases are `mountns-O0` and `mountns-O2`, each under
  the engine and Node. The `mountns` oracle runs under `sudo -n`.
- `XIAOJI_JIT=0 make test`: 154 passed, 0 failed, 0 xfail.
- `XIAOJI_BOOT=1 make test`: 155 passed, 0 failed. The boot check requires the class preload, the system_server fork
  and the d1 line, `Entered the Android system server!`.
- `make test-web`: all web checks passed, exit 0.
- Each run was read unfiltered (`rtk proxy make test`), and `-Wall -Wextra` is clean for the native build and for each
  changed file under emcc.

### Gaps (B4c)

- Mutations through an fd opened before a remount, and `fchmod` and `ftruncate` on an fd, are not checked against the
  read-only flag.
- Dirfd and the cwd are guest strings re-resolved through the current namespace, not pinned to a mount.
- Propagation is recorded, not applied. A copy is a snapshot: a later mount in the init namespace does not reach an
  unshared process. `MS_MOVE`, `pivot_root`, `chroot` and `setns` are not modelled.
- AF_UNIX socket lookup is by guest path, so a bind alias to a socket's directory is not checked against the socket's
  inode (`sock.c`, not audited in this section).
- `/proc/mounts` has no `/proc`, `/sys` or `/dev` entries; the fs type and source of a bind are `rootfs`.
- The binder driver reports `u:r:kernel:s0` as every sender's context, though attr/current is now per process.
- `/proc/thread-self/attr/exec`, `sockcreate`, `fscreate` and `keycreate` are ENOENT.
- A non-initial process's namespace is released by `proc_release` when it ends. The deadlock check's kill path was
  not tested for this.
- hwbinder: the scatter-gather buffer fix-up (the `hw-Parcel` error above), which blocks HIDL lookups.
- init: `ctl.interface_start` is not supported, so lazy AIDL services do not start.

## B4c part 2: lazy services, APEX, the hwbinder fix, and the bootstrap services (d2 begun)

Status: **the three decisions are implemented and tested. APEX: system_server uses the flattened APEX manager and no
longer waits for `apexservice`. Lazy services: `interface <kind> <name>` and `ctl.interface_start|stop|restart` work
(init test). hwbinder: the scatter-gather parent fixup is implemented, and a guest test matches the Linux driver's
answers on the builder VM's kernel. The boot gets through the zygote, system_server's entry (d1),
`startBootstrapServices` and `StartPowerManager` (d2 begun). It stops at the next wait, and the cause is a linker
decision, not a missing binary: the vendor HALs need VNDK-only NDK libraries, which the flat linker config does not
search. The experiment is in "The blocker". d2 is not done; d3, d4 and d5 are not reached.**

### Files

- `engine/binder.c`: the scatter-gather area from `buffers_size`, the parent fixup and its ordering rules, the FDA
  ordering, and the security-context rule (the node flag only).
- `engine/init.c`: `interface <kind> <name>` in a service (up to 16 per service), and `ctl.interface_start|stop|restart`
  (mapped to the declaring service when the request is made).
- `engine/props.c`: `ro.apex.updatable=false` (an engine value, so it wins over `vendor/build.prop`).
- `engine/selinuxfs.c`: the `hwservice_manager` class (the access decision is unchanged: allow-all).
- `engine/procfs.c`: `/proc/meminfo`, a fixed stub (see "Other engine changes").
- `engine/sysfs.c`, `engine/host.c`, `engine/host.h`: `renameat` (38), `renameat2` (276, no flags), `fsync` (82) and
  `fdatasync` (83).
- `image/xiaoji.rc`: the `installd` and `system_suspend` services, the power stats HAL, the power HAL (disabled, started
  by its lazy interface), and the `installd` directories.
- `tests/guest/binder-sg.c` (new): the scatter-gather test. `tests/init/interface.rc` and
  `tests/expect/init-interface.out` (new): the init test. `tests/run.sh`: the `binder-sg` oracle runs under `sudo -n`,
  the `init-interface` case is added, and the `XIAOJI_BOOT` check now also requires `StartPowerManager`.

### Decision 1: APEX (`ro.apex.updatable=false`)

- `frameworks/base/services/core/java/com/android/server/pm/ApexManager.java` (AOSP, read on the remote): the singleton
  is `ApexManagerImpl` only when `ApexProperties.updatable().orElse(false)` is true, and otherwise
  `ApexManagerFlattenedApex`. The only `waitForService("apexservice")` is in `ApexManagerImpl.waitForApexService`.
- `build/rootfs/vendor/build.prop` sets `ro.apex.updatable=true` (line 57). The engine's own values override the build.prop
  files (`engine_props` in `engine/props.c`), so the engine value is the one that counts.
- Checked in the guest: a oneshot's `getprop ro.apex.updatable` prints `false`.
- Result: no `apexservice` line in any boot since the change (the B4c run had "Waited one second for apexservice" and
  the lazy-start request). `apexd` is not started, and the flattened manager does not call `apexservice`.

### Decision 2: lazy services (`interface`, and `ctl.interface_*`)

- AOSP (read on the remote): `ServiceParser::ParseInterface` (`interface <kind> <name>`, with `aidl` or a fully
  qualified HIDL name; a second definition of the same `kind/name` is an error), and `init.cpp`'s `HandleControlMessage`.
  There a `ctl.interface_<action>` value is looked up with `FindInterface` (the full `aidl/<name>` or
  `<fqname>/<instance>`), and `ctl.<action>` with `FindService`.
- The engine: `interface` is parsed into the service (up to 16 per service, each up to 127 bytes), and a second
  definition is an error at parse time. `ctl.interface_start|stop|restart` maps the value to the declaring service when
  the request is made, so the queue holds the short service name. An unknown interface logs Android's text,
  `Could not find '<name>' for ctl.interface_start`, and is ignored. A dropped request is logged too (the queue is full,
  or the name is too long), where it used to be silent.
- The servicemanager and hwservicemanager requests are `ctl.interface_start` with `aidl/<name>` and `<fqname>/<instance>`
  (`frameworks/native/cmds/servicemanager/ServiceManager.cpp:548`, `system/hwservicemanager/ServiceManager.cpp:258`).
  In the boots these reach init and get the expected answer.
- **Test:** `tests/init/interface.rc` declares a disabled `lazy` service with `interface aidl lazytest`. The driver starts
  it with `ctl.interface_start aidl/lazytest`, stops it with `ctl.interface_stop`, restarts it with `ctl.interface_restart`,
  and sends an unknown interface. Expected stdout, in `tests/expect/init-interface.out`: `lazy running`,
  `after stop stopped`, `after restart running`, `unknown running`. Engine and Node: PASS.
- The lazy path is also exercised by the real power HAL (run10, below): servicemanager finds `IPower/default` in the device
  VINTF manifest, asks init for `aidl/android.hardware.power.IPower/default`, and init starts the service.
- Gaps: no fully-qualified-name validation (Android's `FQName` checks), so a malformed `interface` name is accepted. A
  disabled service's `init.svc.<name>` property is not set until it starts.

### Decision 3: the hwbinder scatter-gather fix

**The driver's rules.** The builder VM has the Linux 6.12 source of `drivers/android/binder.c`
(`/home/rosdyanakusuma/xbuild/linux-6.12.117`, in the VM). The running kernel is 7.2 (OrbStack), and these rules were
not compared against the 7.2 source. The rules followed:

- A BINDER_TYPE_PTR object's bytes are copied into the target buffer's scatter-gather area, which starts after the data
  and the offsets and is `buffers_size` long (`BC_TRANSACTION_SG`, `BC_REPLY_SG`). Each copy is 8-byte aligned, and the
  object's `buffer` becomes the target address. A buffer longer than the area left is `BR_FAILED_REPLY` (`too large buffer`).
- A child with `BINDER_BUFFER_FLAG_HAS_PARENT` names its parent by index in the offsets array. The parent must be an
  earlier PTR object of the same transaction (`binder_validate_ptr`). The child's address goes into the parent's bytes at
  `parent_offset`, which must leave room for 8 bytes in the parent (`binder_fixup_parent`).
- Fixups must be in order (`binder_validate_fixup`). The last object fixed up is kept, with the minimum offset for a fixup
  in it. A fixup is allowed only when the parent is that object or one of its parents, and it must not be below the minimum.
  Every PTR object resets the state to itself at minimum 0. A BINDER_TYPE_FDA sets it to its parent, at the end of its fd
  array. Offset 0 is the sentinel for "no object", so an object at data offset 0 cannot be fixed up (this is why the test
  puts a pad word first).
- BINDER_TYPE_FDA: the parent is validated the same way, and the fd array must lie inside the parent's bytes (only when the
  array is not empty).

**What the engine does** (`engine/binder.c`). `btd_parse` reads `buffers_size` for the SG commands. The area is
`buffers_size` bytes after `ALIGN8(data)` and the offsets, and the buffer is allocated at that full size. A PTR advances
the area cursor by `ALIGN8(length)`. The child's address is recorded (`xl_pfix`) and written into the parent's copied bytes
just before the blobs are written (the address is the target address). The ordering check is `xl_fixup_ok`, which follows
`binder_validate_fixup`. The FDA branch follows the driver's order.

**The test** (`tests/guest/binder-sg.c`). A forked server is the context manager (set with `BINDER_SET_CONTEXT_MGR_EXT` and
`FLAT_BINDER_FLAG_ACCEPTS_FDS`). The client sends requests with `BC_TRANSACTION_SG` and `TF_ACCEPT_FDS`. Each request checks
what the driver delivered, and the server replies with a status word that the client prints. The cases are the ones
libhwbinder sends (its `verifyBufferObject` compares the child's address with the one at `parent_offset`), plus the
malformed ones:

| Case | What it sends | Native (kernel) and engine |
|---|---|---|
| parent-embedded | PTR parent, child with HAS_PARENT at offset 8 | reply status 7 (pointer, child, parent) |
| fda | PTR parent with an fd slot, and an FDA for a pipe | reply status 12 (parent, fd) |
| forward-parent | the child's parent is itself | BR_FAILED_REPLY |
| offset-out | the child's slot runs past its parent | BR_FAILED_REPLY |
| nonptr-parent | the parent is a BINDER_TYPE_FD object | BR_FAILED_REPLY |
| no-flag | a PTR with the flag clear, whose parent fields are ignored | reply status 2 (child) |
| reply-embedded | the server replies with BC_REPLY_SG, embedded | pointer 1, child 1, parent 1 |
| two-children | two children of one parent at offsets 0 and 8 (in order) | reply status 23 (pointer, child, parent, second) |
| out-of-order | the same children at 8, then 0 | BR_FAILED_REPLY |

- The oracle is the builder VM's kernel (`sudo -n build/tests/guest/binder-sg-O0`, since `/dev/binder` is root-only there).
  `tests/run.sh` runs it that way, and the engine and Node runs must match it byte for byte.
- The kernel's own log (`dmesg`) names the rules it applied. For the first attempt, which put the parent at data offset 0,
  it says `got transaction with out-of-order buffer fixup` and `failed to fixup parent`. The out-of-order case gets the same
  line. Before `ACCEPTS_FDS` was set on the manager, it said `got transaction with fd, 6, but target does not allow fds`.
- Red check: the engine with only the parent patch disabled reports `parent-embedded: reply status 6 (pointer 0 ...)`. The
  fixed engine reports 7, the same as the kernel.
- The constants were checked against the builder VM's `linux/android/binder.h` (a small program printed them).

**What the boot shows.** Before the fix, run3 (the B4c part 1 run, `build/tests/boot/run3.err` lines 687-688):
`E/hw-Parcel(1002): Buffer in parent 0x6ffd6fece2d0 differs from embedded buffer 0x6ffd64c1c100`, then
`E/HidlServiceManagement(1009): getService: ... BAD_VALUE`. The direction is corrected here: pid 1002 is hwservicemanager,
which reads system_server's request, so the parent slot was wrong in the request that hwservicemanager received (the B4c
text said "the reply"). After the fix there is no `hw-Parcel` line in any boot. The Watchdog's lookup of
`android.hidl.manager@1.0::IServiceManager/default` succeeds in run8 (the `HwBinder: Starting thread pool for getting` lines at
62.8 s and 98.2 s, and no `NoSuchElementException`).

The `NoSuchElementException` of run4 (85.7 s) had a different cause, fixed separately: hwservicemanager could not register
itself, because `hwservice_manager` was an unknown SELinux class (see "Other engine changes").

### Other engine changes

- **`hwservice_manager` SELinux class** (`engine/selinuxfs.c`): index 61, and `add`, `find`, `list` as 1, 2 and 3 (AOSP
  `system/sepolicy/private/access_vectors`). Without it, libselinux said `Unknown class hwservice_manager`, and
  hwservicemanager denied its own registration (`Missing permissions to add IBase`). Both lines are absent from every boot
  since. The access decision is still allow-all, as B3 designed it.
- **Security-context rule** (`engine/binder.c`, a binder behaviour change, listed on its own): the security context is sent
  when the target node has `FLAT_BINDER_FLAG_TXN_SECURITY_CTX`, as the driver does. Before, a manager set with the EXT ioctl
  always got it. servicemanager's `ProcessState::becomeContextManager` passes that flag
  (`frameworks/native/libs/binder/ProcessState.cpp:191-196`), so servicemanager is unchanged. `make test` (the binder
  conformance cases and servicemanager) is green with the rule.
- **`/proc/meminfo`** (`engine/procfs.c`): a fixed stub, a 2 GiB guest with 1 GiB free and nothing cached or swapped. The
  keys are the ones libmeminfo reads (`kDefaultSysMemInfoTags`), and the file is under the 4095 bytes its single read takes.
  Before: `Failed to open file :/proc/meminfo` and `SysMemInfo read failed`, which failed the ActivityManagerService
  constructor. After: the ActivityManagerService starts.
- **`renameat` (38), `renameat2` (276, no flags), `fsync` (82), `fdatasync` (83)**: `installd` called rename and fsync and
  got `Function not implemented`. Rename is a host rename of the two resolved names (the read-only and synthetic checks as
  `unlinkat`). Sync is a host `fsync` for a host file; a memfd and a directory are 0; other descriptors are EINVAL. Gaps: the
  RENAME_* flags are EINVAL, and renaming across mounts is not EXDEV.
- **`installd` directories** (`image/xiaoji.rc`, the early-init block): `/data/misc`, `/data/misc/user` and
  `/data/misc/installd`, as AOSP's `init.rc` makes them. The engine's `mkdir` takes only the path, so every directory is 0771
  and the owner is not set (a gap; installd does not depend on the modes here).

### The services added to `image/xiaoji.rc`

Each block is the rc of the stock file, with the options the engine does not model ignored by init.

| Service | Binary (rootfs) | Source of the block | State |
|---|---|---|---|
| `system_suspend` | `/system/bin/hw/android.system.suspend@1.0-service` | `system/hardware/interfaces/suspend/1.0/default/android.system.suspend@1.0-service.rc` (`capabilities` ignored) | Runs. Its `main.cpp` keeps it running without suspend when `/sys/power/wakeup_count` or `/sys/power/state` cannot be opened, which is the engine's case. Logs the missing `/sys/power` files and continues. One crash (SIGSEGV) after a system_server restart, not diagnosed (see the gaps). |
| `installd` | `/system/bin/installd` | `frameworks/native/cmds/installd/installd.rc` (the service block; its `on early-boot` is sdcardfs setup and is not run) | Runs, once the directories above and rename and fsync exist. |
| `vendor.power.stats-hal-1-0-mock` | `/vendor/bin/hw/android.hardware.power.stats@1.0-service.mock` | the rootfs's `vendor/etc/init/android.hardware.power.stats@1.0-service.rc` | Runs. Its `interface` line is parsed, and it registers `IPowerStats/default`. Logs `/sys/bus/iio/devices/` missing and continues. |
| `vendor.power-default` | `/vendor/bin/hw/android.hardware.power-service.example` | `hardware/interfaces/power/aidl/default/power-default.rc` (`class` and `user` kept) | Disabled, with `interface aidl android.hardware.power.IPower/default`, so servicemanager's lazy start starts it. It cannot link yet (the VNDK library, below). |

### The linker decision (the blocker)

- The example power HAL needs `android.hardware.power-V3-ndk.so`, and the lights HAL needs `android.hardware.light-V2-ndk.so`.
  Both exist only in the VNDK APEX (`/apex/com.android.vndk.current/lib64`), not in `/system/lib64`. `tools/gen-ldconfig.py`
  leaves the VNDK APEX out of the one flat namespace, on purpose (its docstring: VNDK holds vendor builds of system libraries
  with other ABIs). So a vendor binary fails to link: `CANNOT LINK EXECUTABLE ... library "android.hardware.power-V3-ndk.so"
  not found` (run10, every lazy start).
- **Why not add the VNDK APEX to the search path as it is.** The VNDK copy of `libbinder.so` is built with `__ANDROID_VNDK__`,
  so its `kDefaultDriver` is `/dev/vndbinder` (`frameworks/native/libs/binder/ProcessState.cpp:51-54`). A process that picks it
  up through a broad search path talks to the vendor binder, not `/dev/binder`. That is what an earlier draft of this section
  got wrong: it blamed the empty `/dev/vndbinder` (no `vndservicemanager`), and the experiment below shows that is not the
  cause for this HAL.
- **Experiment 1 (the cause).** A scratch rc with the system `servicemanager`, and the power HAL with `LD_LIBRARY_PATH` set to a
  directory that holds only a symlink to `android.hardware.power-V3-ndk.so` (so libbinder stays the system copy). Result: the
  HAL uses `/dev/binder`, and `servicemanager` logs `Found android.hardware.power.IPower/default in device VINTF manifest.`
  `service check android.hardware.power.IPower/default` prints `found`. No `vndservicemanager` was needed.
- **Experiment 2 (the next stop).** A full boot with that scratch rc (run9, the `build/tests/boot/b4c2-run9.err` log): IPower
  registers at about 33.3 s, and `StartPowerManager` completes in 46 ms. The next wait is `LightsService`, which waits for the
  light HAL (`waitForDeclaredService`). The light example needs the VNDK-only `android.hardware.light-V2-ndk.so`, so it is
  not added yet. The Watchdog then kills system_server at 103 s (main thread in `LightsService.VintfHalCache.get`). In the second
  system_server of that run (after the kill), `system_suspend` crashed (SIGSEGV, exit status 139), not diagnosed. The scratch
  directory and rc were removed afterwards.
- **The decision needed (not made here).** Two options, both real:
  - (A) Search `/apex/com.android.vndk.current/${LIB}` in the flat namespace after `/system` (and after the system APEXes).
    System libraries such as `libbinder.so` still resolve to `/system` first, so processes keep `/dev/binder`. Only
    libraries that exist nowhere else (`power-V3-ndk`, `light-V2-ndk`) come from the VNDK APEX. This changes
    `tools/gen-ldconfig.py`, and the rootfs `linkerconfig` is regenerated from it. The risk is the one its docstring names:
    a process could load a VNDK build of a library that `/system` lacks.
  - (B) A vendor namespace for vendor binaries (the AOSP model). Vendor binaries then get VNDK's `libbinder` and
    `/dev/vndbinder`, and the vendor HALs need `vndservicemanager` on that device. That binary is not in the rootfs (the stock
    vendor partition does not have it, as far as the `debugfs rdump` in `tools/extract-rootfs.sh` shows). It is in the AOSP
    tree (`frameworks/native/cmds/servicemanager`, module `vndservicemanager`), and not in `out/target/product/xiaoji_arm64`.
    Building it is a remote `m vndservicemanager`, which I have not started.
- Until one of these is chosen, the power HAL stays disabled, and the boot stops at `StartPowerManager` (d2 begun). The
  `XIAOJI_BOOT` check requires that line.

### Milestones (B4c part 2)

Timings are elapsed guest time, with the instruction count. The power state is `pmset -g` taken before the run.

| Milestone | Logcat line | AC, LPM off (run8) | Battery, LPM on (run4) |
|---|---|---|---|
| (a) app_process starts ART | `I/AndroidRuntime: Using default boot image` | 1.8 s, 0.21 G | 3.2 s, 0.18 G |
| (b) classes preloaded | `I/Zygote: ...preloaded 16252 classes` | 22.8 s (16.9 s of preload), 2.38 G | 38.5 s (28.5 s), 2.35 G |
| (c) system_server forked | `I/Zygote: System server process N has been created` | 29.9 s, 3.22 G | 50.8 s, 3.19 G |
| (d1) system_server starts | `I/SystemServer: Entered the Android system server!` | 30.4 s, 3.27 G | 51.8 s, 3.24 G |
| (d2) startBootstrapServices begins | `D/SystemServerTiming: startBootstrapServices` | 32.8 s, 3.51 G | 55.7 s, 3.48 G |
| (d2) StartPowerManager (inside it) | `D/SystemServerTiming: StartPowerManager` | 33.5 s, 3.58 G | 56.8 s, 3.55 G |
| (d2) startBootstrapServices done | not reached (the linker decision) | | |
| (d3) startCoreServices done | not reached | | |
| (d4) startOtherServices | not reached | | |
| (d5) System now ready / Boot completed | not reached | | |

- Power state: run8 was taken with `lowpowermode 0`, "Now drawing from 'AC Power'". run4 was taken with `lowpowermode 1` on
  battery (the charge was not recorded for run 4; a charge reading was taken before run 1). These are single runs, not
  benchmarks. The AC figures are the ones comparable with the benchmark rule (AC, Low Power Mode off), one run each.
- The Watchdog (`WAITED_HALF` at 62.8 s in run8) kills system_server at 103.4 s, because the main thread is blocked in
  `PowerManagerService.nativeInit` (run8, before the power HAL was made disabled). With the power HAL disabled (run10), the
  stop is the same `StartPowerManager` point, and the main thread waits in `PowerManagerService` for `IPower`.
- The boot check (`XIAOJI_BOOT=1`) requires the preload, the fork, the d1 entry and `StartPowerManager`. The
  `StartPowerManager` line appeared in every boot that reached it (runs 4, 6, 7, 8, 9, 10 and the check runs).

### The current stop, with the log

Run10 (the committed rc), AC power:

```
[32685 ms 3571873877 insn] D/SystemServerTiming(1014): StartPowerManager
[32714 ms 3574652850 insn] I/servicemanager(1001): Found android.hardware.power.IPower/default in device VINTF manifest.
[32716 ms 3574882357 insn] I/servicemanager(1001): Since 'android.hardware.power.IPower/default' could not be found, trying to start it as a lazy AIDL service
[32749 ms 3577532340 insn] F/linker(1054): CANNOT LINK EXECUTABLE "/vendor/bin/hw/android.hardware.power-service.example": library "android.hardware.power-V3-ndk.so" not found: needed by main executable
xiaoji: init: vendor.power-default exited (status 1)
```

The same line repeats about once a second, from each lazy request (113 in the run). Run9 (the experiment, power-only directory): `Found android.hardware.power.IPower/default in device VINTF manifest.` at 33.3 s, then `StartPowerManager took to complete: 46ms`, then the `LightsService` wait.

### Deadlock lines (diagnostic, reported, not changed)

`xiaoji: init: nothing has been runnable for 5 s` and `xiaoji: deadlock: every thread is blocked and none can wake` appear in
these boots, and they are misleading. In `engine/sched.c` (`run_loop`), `idle_since` is set at the first moment nothing is
runnable (lines 484-485), and it is never cleared when `pick()` returns a thread. So the 5 s is wall-clock time since the first
idle moment, not continuous idleness: time spent running threads in between counts too. Threads that sleep with deadlines
(servicemanager's one-second waits, for example) are then reported as "none can wake". The run continues after the lines,
and it ends only at `--init-timeout` (the lines are a diagnostic). The fix would reset `idle_since` and `idle_reported` when
a thread is picked, and say "none can wake" only when no thread has a deadline. Per the instruction, the threshold is not
raised and the detector is not changed here.

### Tests

- `make test`: 160 passed, 0 failed, 0 xfail (the baseline was 154). The new cases are `binder-sg-O0` and `binder-sg-O2`
  (engine and Node, against the builder VM's kernel) and `init-interface` (engine and Node). The `binder-driver` 17 cases
  still pass (engine and Node).
- `XIAOJI_JIT=0 make test`: 160 passed, 0 failed, 0 xfail.
- `make test-web`: all web checks passed.
- `XIAOJI_BOOT=1 make test`: 161 passed, 0 failed, 0 xfail. The boot check requires the preload, the fork, the d1 entry and
  `StartPowerManager`, with `--init-timeout 90`.
- Warnings: `cc -std=c11 -Wall -Wextra -Wno-unused-parameter -O2` and `emcc` with the same flags, one file at a time, are clean
  for `engine/binder.c`, `init.c`, `procfs.c`, `props.c`, `sysfs.c`, `host.c` and `selinuxfs.c`. The guest test builds clean with
  `gcc -Wall -Wextra`.
- No `xiaoji` process is left running after the runs (checked with `ps`).

### Gaps (B4c part 2)

- The linker decision (above): the VNDK APEX is not searched, so the power HAL is disabled, and the lights HAL is not added.
- accept_fds is not enforced: the driver refuses FDA and FD objects for a target node that does not accept them.
- The scatter-gather area is allocated at its full `buffers_size`, as the driver does. The security-context slot is after
  the area (the driver puts it at the end): the placement differs, the contents do not.
- `FQName` validation of `interface` names is not done. The engine's `mkdir` takes only the path (modes and owners are not
  applied).
- A system_server restart is followed by a SIGSEGV in `system_suspend` (exit status 139), seen once in run9. Not diagnosed.
- The deadlock diagnostic (above).
- Unimplemented syscalls seen in the boots: 282 (`userfaultfd`, ART falls back), 26 (`inotify_init1`).
- Non-fatal, not fixed: `/vendor/etc/public.libraries.txt` missing (SystemConfig), `/sys/bus/iio/devices` (the power stats
  mock), `/data/misc/critical-events` (CriticalEventLog), cgroups (`/dev/cgroup_info`), boot_id, and the HIDL "must be in VINTF
  manifest" line for `android.frameworks.stats@1.0::IStats` (system_server carries on).
- Components not decided yet, and not reached: the lights, sensors, health, thermal and other HALs of the stock table; lmkd,
  statsd, storaged, netd and vold; the telephony, NFC and Bluetooth services. `docs/features.md` has rows for the radio and
  connectivity features but none for power or suspend, which system_server needs at boot (so they belong to the base).

### Component table (B4c part 2)

| Component | Choice | State | Why |
|---|---|---|---|
| zygote, system_server, servicemanager | real, unchanged | runs; d1 reached | B4a to B4c |
| hwservicemanager | real, from the Android 13 block | runs; registers itself | needs the `hwservice_manager` class |
| SELinux | synthetic, allow-all, permissive | `hwservice_manager` class added (data only) | no compiled policy |
| apexservice (apexd) | not started | not needed: `ro.apex.updatable=false` (flattened manager) | APEX updates are not installed |
| `ctl.interface_*` | real init semantics | implemented, tested | servicemanager's lazy starts |
| `/proc/meminfo` | engine stub, fixed 2 GiB | ActivityManager starts | the kernel's file has no equivalent here |
| `renameat`, `renameat2`, `fsync`, `fdatasync` | engine, host-backed | installd runs | syscalls installd needs |
| `system_suspend` | real daemon, from its rc | runs in its no-suspend mode | no `/sys/power`; its source describes this mode |
| `installd` | real daemon, from its rc | runs | system_server's Installer waits for it |
| `vendor.power.stats-hal-1-0-mock` | real mock HAL, from its rc | runs; IPowerStats registers | the stock mock; no `/sys/bus/iio` |
| `vendor.power-default` (IPower) | real example HAL, from its rc, disabled, lazy | cannot link: VNDK-only NDK library | the linker decision (A or B) |
| lights HAL (ILights) | not added | system_server waits for it (the next stop) | needs `light-V2-ndk` from the VNDK APEX: the same decision |
| `vndservicemanager` | not in the rootfs; not built | not needed on the `/dev/binder` path; needed only for option (B) | a remote build would be needed |
| `/dev/binder`, `/dev/hwbinder` | engine binder devices | the scatter-gather fixup is in | |
| `/dev/vndbinder` | engine binder device, no context manager | not used by the current path | option (B) |
| Watchdog | real | fires after the main thread is blocked | consequence of the stop |
| cgroups, boot_id, userfaultfd | missing or ENOSYS | non-fatal | not decided |
| Other HALs, lmkd, statsd, storaged, netd, vold; telephony, NFC, Bluetooth | not decided | not reached | after d2 |

## B4d: the VNDK linker path, the deadlock check, and the power and light HALs (d2 done, stop at d2's end)

Status: **the linker decision (A) is implemented and verified statically and by the test suite. The power HAL and the
light HAL link and run (IPower and ILights register through the lazy start). The deadlock check no longer fires during
the 1-second waits. system_server now gets through `startBootstrapServices` to `InitPowerManagement` (d2's last step),
where `PowerManager` cannot be built: `thermalservice` is not visible to servicemanager, although ThermalManagerService
started and published it. The Java side then throws a NullPointerException, and the zygote restarts system_server (the
cycle repeats). d3, d4 and d5 are not reached. The blocker is below, with its log.**

### Files

- `tools/gen-ldconfig.py`: appends `/apex/com.android.vndk.<name>/${LIB}` to the flat default namespace, last (after
  `/system`, the APEXes, `/system_ext`, `/product` and `/vendor`). The VNDK APEX directory is found, not hardcoded.
  Docstring updated. Both rootfs trees regenerated (`build/rootfs`, `build/rootfs-xiaoji`).
- `engine/sched.c`: the idle streak is reset when a thread runs, the wakeable-deadline count is separate, and the two
  diagnostics are split (see "The deadlock check").
- `engine/init.c`: `init_deadline` returns only service-restart times. The `--init-timeout` end is not a wake-up.
- `image/xiaoji.rc`: `vendor.light-default` (new), a disabled lazy service like `vendor.power-default`.
- `tests/guest/futex-hang.c` (new), `tests/init/idle-sleep.rc` and `tests/init/idle-hang.rc` (new), and
  `tests/expect/init-idle-sleep.out` and `init-idle-hang.out` (new). `tests/run.sh`: the two deadlock cases, the oracle
  skip for `futex-hang-*`, and the `XIAOJI_BOOT` check now also requires `InitPowerManagement`.

### The linker path (decision A)

- The search path of the flat `default` namespace is now `/apex/com.android.art`, `i18n`, `runtime`, `/system`, the other
  APEXes, `/system_ext`, `/product`, `/vendor`, and then `/apex/com.android.vndk.current/${LIB}`. The APEX directory is
  `com.android.vndk.current` on both rootfs trees (checked with `ls`).
- Static check, before any boot. The old and new `ld.config.txt` differ in exactly one path, the appended one, in both
  trees (a script compared the path lists: the old list is a prefix of the new one).
- The VNDK-only sonames (in the VNDK `lib64`, in no earlier directory): 38 in `build/rootfs`, 39 in `build/rootfs-xiaoji`.
  `android.hardware.power-V3-ndk.so` and `android.hardware.light-V2-ndk.so` are VNDK-only. `libbinder.so`, `libbinder_ndk.so`
  and `libhidlbase.so` are in `/system` (earlier), so they still resolve to `/system`. The overlap between the VNDK
  `lib64` and `/vendor/lib64` is 0, so the placement at the end of the list does not change any vendor resolution.
- `DT_NEEDED` (from `readelf -d` in the builder VM): the power HAL needs `libbase`, `libbinder_ndk`, `power-V3-ndk`,
  `libc++`, `libc`, `libm`, `libdl`. The light HAL needs the same with `light-V2-ndk`. Every one except the two NDK
  libraries is in `/system` or the runtime APEXes (`libc.so` is in `/system/lib64`).
- Test results with this change: `make test` 164 passed, 0 failed (`XIAOJI_JIT=0`: 164, 0 failed). `make test-web`
  passed. `build/web/manifest.json` packs `build/rootfs` (including `linkerconfig`), and its make rule does not depend on
  that directory, so the file was deleted and rebuilt before the run.
- Runtime: `IPower` is found in the device VINTF manifest at 56.65 s (lazy start), and `Power isModeSupported` answers at
  56.93 s. `StartPowerManager took to complete: 337ms` (run1). The B4c part 2 experiment (run9) took 46 ms; its power
  state was not recorded, so the two are not compared. In the second system_server cycle, `StartPowerManager` took 77 ms.

### The deadlock check

- The cause (B4c part 2): `idle_since` was set at the first idle moment and never cleared, so the 5 s measured wall time,
  and any thread that had run in between counted.
- The fix (`engine/sched.c`, `run_loop`, init mode only): `idle_since` is reset whenever `pick()` returns a thread. The
  "none can wake" report needs a continuous idle streak of more than 5 s, no guest thread with a deadline, and no
  service-restart deadline in init. It prints `xiaoji: deadlock: every thread is blocked with no deadline, and none can
  wake` and the thread list, once per streak. A second diagnostic, `xiaoji: init: nothing has run for 60 s, with deadlines
  pending`, covers a long continuous idle with a deadline pending (it is a diagnostic, the run continues). The non-init
  path (a process run without `--init`) is unchanged: it still ends with exit 134 when no thread has a deadline.
- `engine/init.c`: `init_deadline` no longer returns the `--init-timeout` end. Before, the timeout made every init run look
  like it had a pending wake-up, so a hang could never be reported. The timeout is still checked on every pass of
  `init_tick`, and the idle sleep is at most 10 ms, so the run still ends on time.
- Tests (engine and Node): `init-idle-sleep` (seven one-second sleeps with output between them; stdout must match, and
  stderr must have no deadlock or "nothing has run" line). `init-idle-hang` (`tests/guest/futex-hang.c`, one thread on a
  futex with no timeout; stderr must have the deadlock line; the run ends at `--init-timeout 9`). The first version of
  `init-idle-hang` failed for the reason above (the timeout made init look busy), which is how the fix was found.
- The boot (run1): no `deadlock` or `nothing has run` line in 3,357 log lines over 240 s. The old lines appeared in
  every run before this change (B4c part 2).
- Red check of `init-idle-sleep` against the pre-fix `sched.c`: not run. The test is expected to fail there (seven
  one-second idle moments exceed the 5 s of the old wall-clock check), but that is not shown here.

### Milestones (B4d)

Run1, the committed rc with the two HAL changes. The power state was **battery, Low Power Mode on** (`pmset -g` in
`build/tests/boot/b4d-run1.pmset.txt`: `lowpowermode 1`, "Now drawing from 'Battery Power'"). The benchmark rule (AC, LPM off)
was not met, because the machine was not on AC. These timings are a single run, not a benchmark, and are not comparable
with the B4c part 2 AC figures.

| Milestone | Logcat line | Time, insn (run1, battery, LPM on) |
|---|---|---|
| (b) classes preloaded | `I/Zygote: ...preloaded 16252 classes` | 38.4 s, 2.37 G |
| (c) system_server forked | `I/Zygote: System server process 1014 has been created` | 50.6 s, 3.22 G |
| (d1) system_server starts | `I/SystemServer: Entered the Android system server!` | 51.5 s, 3.27 G |
| (d2) startBootstrapServices begins | `D/SystemServerTiming: startBootstrapServices` | 55.4 s, 3.50 G |
| (d2) IPower registers (lazy start) | `I/servicemanager: Found android.hardware.power.IPower/default` | 56.65 s |
| (d2) StartPowerManager done | `V/SystemServerTiming: StartPowerManager took to complete: 337ms` | 56.9 s, 3.59 G |
| (d2) InitPowerManagement (the furthest point, XIAOJI_BOOT check) | `D/SystemServerTiming: InitPowerManagement` | 56.96 s, 3.59 G |
| (d2) startBootstrapServices done | not reached: `PowerManager` is null (blocker) | |
| (d3), (d4), (d5) | not reached | |

- The second cycle (run1): the zygote forks system_server 1062 at 108.2 s; its `StartPowerManager` takes 77 ms and it reaches
  `LightsService` at 114.2 s. Lights: `servicemanager: Found android.hardware.light.ILights/default` (lazy start at 114.19 s,
  `Lights reporting supported lights` at 114.45 s). It then fails at the same `thermalservice` point (the cycle repeats:
  the zygote restarts system_server at 166.9 s and 227.4 s).
- The `XIAOJI_BOOT=1` check (`tests/run.sh`) now requires the preload, the fork, the d1 entry, `StartPowerManager` and
  `InitPowerManagement`. It is run with `--init-timeout 90`, and the furthest line is reached at about 57 s natively. It passed.

### The blocker (d2 end): `thermalservice` is not published

`PowerManager`'s service fetcher (`frameworks/base/core/java/android/app/SystemServiceRegistry.java:590`) calls
`ServiceManager.getServiceOrThrow(Context.THERMAL_SERVICE)`. ThermalManagerService publishes `thermalservice` in its
`onStart` (`frameworks/base/services/core/java/com/android/server/power/ThermalManagerService.java:125-126`), and
`StartThermalManager` ran (10 ms, with `Starting ...ThermalManagerService`). The lookup then fails. The servicemanager log
has no line about the add (no error), and no other server reports one. Logcat, run1 (`build/tests/boot/b4d-run1.err`):

```
[56936 ms 3589547725 insn] D/SystemServerTiming(1014): StartThermalManager
[56936 ms 3589557563 insn] I/SystemServiceManager(1014): Starting com.android.server.power.ThermalManagerService
[56946 ms 3590128995 insn] V/SystemServerTiming(1014): StartThermalManager took to complete: 10ms
[56964 ms 3590998362 insn] D/SystemServerTiming(1014): InitPowerManagement
[56977 ms 3591821663 insn] E/SystemServiceRegistry(1014): No service published for: thermalservice
android.os.ServiceManager$ServiceNotFoundException: No service published for: thermalservice
	at android.os.ServiceManager.getServiceOrThrow(ServiceManager.java:166)
	at android.app.SystemServiceRegistry$38.createService(SystemServiceRegistry.java:601)
	at android.app.ActivityTaskSupervisor.initPowerManagement(ActivityTaskSupervisor.java:505)
	...
[56980 ms 3591960182 insn] W/Parcel(1014): **** enforceInterface() expected 'android.os.IThermalService' but read 'android.app.IActivityManager'
[56989 ms 3592519342 insn] E/AndroidRuntime(1014): Error reporting WTF
java.lang.SecurityException: Binder invocation to an incorrect interface
[57025 ms 3594587492 insn] E/AndroidRuntime(1014): *** FATAL EXCEPTION IN SYSTEM PROCESS: main
java.lang.NullPointerException: Attempt to invoke virtual method 'android.os.PowerManager$WakeLock android.os.PowerManager.newWakeLock(int, java.lang.String)' on a null object reference
	at com.android.server.wm.ActivityTaskSupervisor.initPowerManagement(ActivityTaskSupervisor.java:507)
[57151 ms 3594994450 insn] E/Zygote(1003): Exit zygote because system server (pid 1014) has terminated
```

- The `SecurityException` is a second symptom of the same fault. `RuntimeInit.wtf` reports the fatal error through
  `ActivityManager`'s proxy, and that transaction arrives at the `IThermalService` stub of the same process. A transaction
  for one local Binder object reaches another object's stub. The likely place is the engine's binder node table
  (`engine/binder.c`: nodes are looked up by `ptr`, and a local node's `cookie` decides which Java object answers), but this
  is a hypothesis; it is not established by a trace.
- Not decided yet: whether the add is lost in the transaction (the flat object for the thermal service is translated by
  `xl_object`), or whether servicemanager stores it and loses it later. The next step is a binder trace of the `ADD_SERVICE`
  transaction for `thermalservice` (the engine has none; one would be added for this run only), then the fix.
- This is a binder bug in the engine, not a missing HAL, so it is not handled with a component choice (the thermal HAL
  itself is not reached: ThermalManagerService connects to it later, in `onBootPhase`).

### Component table (B4d)

| Component | Choice | State | Why |
|---|---|---|---|
| `vendor.power-default` (IPower) | real example HAL, lazy | **runs** (after the linker path) | the B4c part 2 linker decision (A) |
| `vendor.light-default` (ILights) | real example HAL, lazy, new in this step | **runs** (`Lights reporting supported lights`) | same decision; `light-V2-ndk` is VNDK-only |
| `vendor.power.stats-hal-1-0-mock` | real mock HAL | runs | as B4c part 2 |
| `system_suspend`, `installd` | real daemons | run | as B4c part 2 |
| ThermalManagerService (`thermalservice`) | real system service, started | **blocker**: the add is not visible to lookups | binder fault (above) |
| HintManagerService | real system service, started | starts (`StartHintManager took 14ms`) | |
| BluetoothAdapter | real | `E/BluetoothAdapter: Bluetooth service is null` (logged, not fatal here) | Bluetooth decision pending (docs/features.md: never) |
| Telephony, NFC, UWB, cell broadcast | not reached | not decided in this step | features.md: never in base. The planned mechanism is configuration (`config_voice_capable`, `ro.radio.noril=yes`), not emulation; not applied yet |
| `/proc/uid_time_in_state`, `/sys/devices/system/cpu/cpu0/cpufreq` | missing | non-fatal (`Cannot obtain CPU frequency count`) | no kernel stats in the engine |
| `/d/wakeup_sources`, `/proc/wakelocks` | missing | non-fatal (`KernelWakelockReader`) | no wakelock stats |

### Other observations (run1)

- `Could not find 'aidl/android.system.suspend.ISystemSuspend/default' for ctl.interface_start` (twice): the request
  servicemanager makes for an AIDL name that system_suspend does not provide (it is HIDL). Non-fatal, not fixed.
- `Could not find android.hardware.power.stats.IPowerStats/default in the VINTF manifest` (servicemanager, once per
  boot cycle): the lookup uses the AIDL form, and the stats mock is a HIDL service. Non-fatal, not fixed.
- The Watchdog did not fire in run1 (system_server died before the 60 s half-timeout could matter).

### Tests

- `make test`: 164 passed, 0 failed, 0 xfail (before the boot check; the baseline was 160). The four new lines are
  `init-idle-sleep` and `init-idle-hang`, engine and Node.
- `XIAOJI_JIT=0 make test`: 164 passed, 0 failed, 0 xfail.
- `XIAOJI_BOOT=1 make test`: 165 passed, 0 failed, 0 xfail (the boot check is the 165th).
- `make test-web`: all checks passed (run after the manifest was regenerated).
- Warnings: `clang -std=c11 -Wall -Wextra -Wno-unused-parameter -O2` and `emcc` (same flags, one file at a time) are clean
  for `engine/sched.c` and `engine/init.c`. `tools/gen-ldconfig.py` compiles (`py_compile`).
- No engine or guest process is left running (checked with `ps` on the host and `pgrep` in the builder VM).

### Gaps (B4d)

- The thermal fault above (the blocker). The d3, d4 and d5 milestones are not reached.
- The battery and Low Power Mode state of run1: no AC run was made.
- The deadlock check is a diagnostic in init mode. It does not stop a hang; the run ends at `--init-timeout`.
- The "nothing can wake" test treats any service-restart deadline as a wake-up, and a host-side event (a control socket
  message) as not a wake-up. A hang that only a host event could end would be reported, and that is the intent.

## B4e: binder node reuse, the binder trace, and the thermalservice fault (not fully fixed)

Status: **partly done.** The stale-node defect is found with evidence, fixed, and covered by a regression test that
is red before the fix and green after it. The B4d blocker is **not** resolved: `thermalservice` lookups still return
"not found" in most boots (4 of 5 after the fix), so `InitPowerManagement` throws the NullPointerException there. A
boot that gets the lookup right then stops at a SIGILL in system_server during `StartDisplayManager`.

### The binder trace (`XIAOJI_BINDER_TRACE=1`, `engine/binder.c`)

Off by default. With the variable off, each event is one test of a static flag (`BT_ON`), and the arguments are not
evaluated. On, each binder event is one line on stderr with the guest pid and tid and an instruction stamp
(`insn`, the same counter as the logcat stamp; the millisecond origin is the trace's own, so compare by `insn`).
Events: a send (the target handle and its node's owner, ptr, cookie, node flags, code, flags, the interface token,
and for handle 0 the String16 name), a delivery to a thread, a reply and its build, failures (`BR_FAILED_REPLY`,
`BR_DEAD_REPLY`), each object of a transaction and how it was translated (with the node and ref), node and ref
create and free, owner notifications (BR_ACQUIRE, BR_INCREFS, BR_RELEASE, BR_DECREFS), acknowledgements, buffer
releases, and a node that a new object finds with a different cookie.

Command: `XIAOJI_BINDER_TRACE=1 XIAOJI_STAMP=1 ./build/xiaoji --logcat --init image/xiaoji.rc --root build/rootfs
--init-timeout 75`. Power: battery, Low Power Mode on, for the pre-fix run; battery, LPM off, for the later runs
(`build/tests/boot/b4e-trace.pmset.txt`, `b4e-fixed.pmset.txt`). The Mac was on battery for every run here, not AC, so
no run is a benchmark.

### The evidence

- **The stale node (pre-fix trace).** A node of system_server (pid 1014) was created at 57.994 s with ptr
  `6ffd65f52430` and cookie `...efb8` (a callback object of servicemanager's `IServiceCallback`). servicemanager's
  ref was released at 58.126 s, and the owner was sent RELEASE and DECREFS. The engine never freed the node,
  because it freed a node only when its owner was dead (`node_maybe_free` checked `n->dead`). At 58.128 s
  system_server sent a new BINDER object with the same ptr and a new cookie `...ea30` (a new Java object at a reused
  weakref address). The engine attached it to the old node and kept the old cookie, and logged
  `** COOKIE MISMATCH **`. A call through the handle then reaches the old cookie, the address of a freed object.
  system_server took SIGSEGV at fault address 0x4 at 58.43 s, which was not analysed further.
- **The kernel's rules** (6.12 source, `drivers/android/binder.c`, in the builder VM at
  `/home/rosdyanakusuma/xbuild/linux-6.12.117`). `binder_translate_binder` returns `-EINVAL` when
  `fp->cookie != node->cookie` (the send then fails with `BR_FAILED_REPLY`). `binder_dec_node_nilocked` and the
  `BINDER_WORK_NODE` case erase a live owner's node once it has no refs, no owner-held strong or weak count, and no
  tmp refs. The running kernel is 7.2 (OrbStack); the 6.12 source was used for the rules, and the test ran on the 7.2
  kernel.
- **The `activity` lookup (pre-fix trace; the log was later overwritten).** In that run the lookup of `activity` (txn
  159, made before `setSystemProcess` registers it) was answered by servicemanager with handle 20, which is the thermal
  node (owner 1014, ptr `6ffd69c95ce0`). That run logged no `SecurityException`, so this is not shown to be the B4d
  symptom (an `IActivityManager` call reaching the thermal stub). Why servicemanager named handle 20 for `activity` is
  **not established**. It is an anomaly to explain next, and it may be the same fault as the thermal lookup.
- **The thermal lookup (post-fix).** `ThermalManagerService` adds `thermalservice` (txn 147, `ADD_SERVICE`). The reply
  is `data=4`, a success status. servicemanager takes its own strong and weak counts on the ref (`count ACQUIRE` and
  `count INCREFS`, handle 20) before it frees the request buffer, so the thermal node is kept (confirmed in the trace).
  The `checkService` for `thermalservice` (txn 157) is then answered with no object at all, so servicemanager's
  `tryGetService` returned null. Three branches of `tryGetService` (`frameworks/native/cmds/servicemanager/
  ServiceManager.cpp`, remote tree) return null: the name is not in `mNameToService`, the stored binder is null, or
  `mAccess->canFind` is false. **Which one is not established.** The SELinux emulation is allow-all, so `canFind` is the
  least likely, but it is not ruled out.

### The fix (engine/binder.c)

- `node_maybe_free` frees a live owner's node under the kernel's rule: no ref holds it, the owner holds no count
  (`has_strong`, `has_weak`) and none is pending, no request is queued on it (`async`, `async_busy`), and it is not the
  context manager's node. It unlinks the node from the owner's list. It is called from `ref_free` and after the
  owner's `BC_ACQUIRE_DONE` and `BC_INCREFS_DONE`.
- A translated BINDER whose ptr names an existing node with a different cookie is refused (`-LE_INVAL`), as the
  kernel does. The stored cookie is not overwritten.
- A sync request keeps the node's flags (`node_set`, `node_flags`), not a pointer to the node, because the node can
  be freed between the send and the delivery (`sched_raise` reads the flags at delivery).

### The regression test (tests/guest/binder-node-reuse.c)

A freestanding guest test, like `binder-sg`. The parent is the owner, and a forked child is the context manager (the
kernel refuses a sync transaction between two binder fds of one process, which the first version found). Steps:

1. The owner gives the object (ptr P, cookie 1). The server takes it, frees its buffer and replies.
2. The owner gives the object at the same P with cookie 2. The server keeps the object.
3. The server calls back through its handle (oneway). The owner's request carries the node's cookie.
4. The owner gives P with cookie 3 while the server still holds cookie 2. The driver refuses it.
5. The server frees the step-2 object. The owner is told to release it.

Kernel oracle (builder VM, `sudo -n`, `build/tests/boot/binder-node-reuse-kernel.txt`):

```
step 1: txn-complete 1 reply 1
step 2: txn-complete 1 reply 1
step 3: callback code 9 requests 1 cookie 0xc002
step 4: failed 1 reply 0
step 5: release 1 decref 1
```

- **Green** (the fixed engine): identical to the kernel output (`cmp`).
- **Red** (`build/tests/boot/xiaoji-red`, the engine with the lifetime and cookie-check hunks reverted, the trace and
  the test kept): `step 3: ... cookie 0xc001` (the stale cookie), and `step 4: failed 0 reply 0` (the send was
  accepted). `cmp` reports the difference.
- The test's output does not print the owner's ACQUIRE and INCREFS counts for steps 1 and 2. The engine sends
  BR_ACQUIRE and BR_INCREFS for a node's first translation, and the kernel's first translation gets none (its step 1
  shows `acquire 0 incref 0`). The count protocol of a first translation differs, and it is not part of this fix. It is
  a gap (below). The rest of the output is the kernel's.
- `make test` runs the case in the engine and in Node (`binder-node-reuse-O0` and `-O2`, 4 PASS lines).

### Milestones (this step, the post-fix boots that were run)

Guest time (the logcat stamp), with the instruction count of the first system_server cycle. The Mac was on battery,
Low Power Mode off, for these. Host load changes the time a lot (the same instruction count came at 33 s and at 57 s),
so the instruction count is the figure to compare. Each boot is one system_server cycle unless it has a second one.

| Boot (log) | Thermal lookup (first cycle) | InitPowerManagement (first cycle) | StartDisplayManager (first cycle, pid 1014) | SIGILL (first cycle) | Second cycle |
|---|---|---|---|---|---|
| trace, `b4e-fixed.err` | found (no "No service" line) | 56.78 s, 3.59 G | 56.96 s, 3.60 G | 56.98 s, 3.60 G | StartDisplayManager 114.72 s, SIGILL 114.74 s |
| no trace, `build/tests/out/boot.err` (make test) | not found at 40.48 s: NPE | 40.48 s, 3.59 G | not reached | not reached | StartDisplayManager 76.62 s, SIGILL 76.63 s |
| no trace, `b4e-notrace.err` | not found at 36.68 s: NPE | 36.67 s, 3.59 G | not reached | not reached | StartDisplayManager 74.44 s, SIGILL 74.45 s |
| trace, `b4e-obj-1.err` | not found at 33.70 s: NPE | 33.69 s, 3.59 G | not reached | not reached | StartDisplayManager 68.00 s, SIGILL 68.01 s |
| trace, `b4e-trace.err` (last trace-run) | not found: NPE | reached | not reached | not reached | not reached |

Four of the five post-fix boots lost the thermal lookup. The pre-fix B4d run and the first pre-fix traced run are not
compared (too few pre-fix runs of this step to give a rate).

### The next blocker, with the log

Two stops, depending on the run:

```
[36675 ms 3591718802 insn] E/SystemServiceRegistry(1014): No service published for: thermalservice     (the NPE of B4d)
[56976 ms 3599716735 insn] F/libc(1014): Fatal signal 4 (SIGILL), code 1 (ILL_ILLOPC), fault addr 0x71697ee4 in tid 1014 (system_server), pid 1014
```

The SIGILL is in system_server after `Starting com.android.server.display.DisplayManagerService`. The faulting code is
not identified (the address is in a mapping the engine does not name). It is a decode or an unimplemented instruction
in the guest, to be found with the mapping of `0x71697ee4` (which the engine does not print).

### Tests (this step)

- `make test`: **168 passed, 0 failed, 0 xfail** (164 before: the four `binder-node-reuse` lines).
- `XIAOJI_JIT=0 make test`: **168 passed, 0 failed, 0 xfail.**
- `XIAOJI_BOOT=1 make test`: **169 passed, 0 failed, 0 xfail.** The check is unchanged: the class preload, the fork,
  the d1 entry, `StartPowerManager` and `InitPowerManagement`. `StartDisplayManager` was tried as the new milestone and
  removed: it is reached only when the thermal lookup succeeds, so it is not deterministic. `InitPowerManagement` is
  the furthest point reached in every run of this step.
- `make test-web`: all web checks passed.
- `make` and `make wasm-node`: built. `clang` and `emcc` with `-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -c` on
  `engine/binder.c`: 0 warnings. The guest test with `gcc -Wall -Wextra -Wno-unused-parameter`: 0 warnings.
- `pmset -g` before the runs is in `build/tests/boot/*.pmset.txt`.

### Gaps (B4e)

- **The thermalservice lookup returns null** (above). Resolved in B4f: the cause was the engine's reply routing, not a `tryGetService` branch (see B4f).
- **`activity` was answered with the thermal node** in one pre-fix run. Not explained.
- The owner's count protocol for a node's first translation differs from the kernel (BR_ACQUIRE and BR_INCREFS are sent
  by the engine, not by the kernel, in step 1 of the test). Not changed.
- The SIGILL in `DisplayManagerService`, at `0x71697ee4` (above). Not identified.
- `FLAT_BINDER_FLAG_TXN_SECURITY_CTX` contexts are still `u:r:kernel:s0` for every sender (B4c gap, unchanged).
- The trace's millisecond stamp has its own origin (use `insn` to join it with logcat).

## B4f: one request per binder read (the thermal lookup), the FMOV SIGILL, and the display stop

Status: **the thermal lookup is fixed (5 of 5 boots), and the SIGILL is fixed (checkpoint 2). The boot now stops at the
display stack, which needs a design decision (SurfaceFlinger, below). `startOtherServices` is not reached.**

### The null lookup: the cause (binder reply routing)

The B4e list named four hypotheses. The trace settles this one, and none of the four is the cause:

- **The two lookups were delivered in one read.** The engine's `bread` (`engine/binder.c`) kept emitting work after a
  transaction, so servicemanager's read returned two BR_TRANSACTIONs. With the B4f trace (the deliver line now shows the
  buffer offset, and a `buffer alloc` line is added), txn 157 (`checkService "thermalservice"`, from tid 1014) is at
  buffer offset 0 and txn 158 (`checkService "activity"`, from tid 1045) at offset 128, both at the same instant
  (`build/tests/boot/b4f-r2.err`). The buffers are distinct, so the allocator is not the fault.
- **The replies crossed.** servicemanager reads the first request (157) and replies with the thermal handle, which is
  correct. The engine routed that reply to the head of the thread's receive list (`t->recv`, the LIFO stack of requests
  being served), which was 158, so tid 1045 got the thermal handle for `activity`. The second reply ("no object") went to
  157's sender, tid 1014, which is the `No service published for: thermalservice` of the NPE. This explains the B4e
  anomaly (`activity` answered with handle 20) as well.
- **servicemanager did find the service.** Its first reply carried handle 20, the thermal node (strong=1, weak=1 at the
  lookup). So the null branch of `ServiceManager::tryGetService` that B4e could not name is not reached for
  `thermalservice`. The answer was right and reached the wrong caller.
- **The four hypotheses, checked against the trace.** (1) Lifetime: the node was live at the lookup. Its RELEASE (insn
  3594652342) and DECREFS (3594707983) came after system_server (pid 1014) was killed (`Sending signal`, insn
  3594615979, in `b4f-r1.err`), so they are not the cause. (2)
  Handle reuse: handle 20 was valid and held at the lookup. (3) The security context: the reply to the
  `thermalservice` request carried handle 20, so `canFind` passed for that request. The null was the crossed reply, not a
  rejection. (4) BR_ACQUIRE and BR_INCREFS: they are not part of the crossed replies. The kernel's
  `binder_inc_node_nilocked` and node-work rules were not compared line by line in this step (open, below).
- **The kernel's rule** (6.12 source in the builder VM, `drivers/android/binder.c`): the read loop of
  `binder_thread_read` ends after a transaction or a reply (the `break` at line 5090, after the BINDER_WORK_TRANSACTION
  case, inside the loop that starts at line 4717), so one read returns at most one request. BC_REPLY takes
  `thread->transaction_stack` (line 3054). The engine did not end the read, so its LIFO list paired the replies wrongly.

### The fix (engine/binder.c)

- `bread`: after emitting a `WK_TXN` (sync or oneway) or a `WK_REPLY`, the read ends. Other work (BR_TRANSACTION_COMPLETE,
  the refcount returns, BR_SPAWN_LOOPER) keeps batching, as in the kernel. `cmd_send` and the LIFO `t->recv` are unchanged;
  LIFO is the kernel's transaction stack, and a FIFO would break nested calls.
- Wake-ups (checked): `sched_wake_key` wakes every thread on the key, and `queue_proc` wakes on every queued item. A
  request left in `b->todo` by a read is taken by the next free looper's read (`proc_has_work`), and `binder_readable`
  reports the queue state at poll time.

### The regression test (tests/guest/binder-read-pair.c)

A single-threaded context manager (forked), and two forked clients (codes 11 and 12). The parent queues the two requests
before the server reads (pipe sequencing), so both wait in the proc queue together. The server prints the requests per
read; each client exits 0 if its reply carries its own code. The oracle is the builder VM's kernel (`sudo -n`, as
`binder-node-reuse`). Red and green were recorded by hand (the wired case compares the engine with the oracle):

| Run | Output |
|---|---|
| kernel (`build/tests/boot/binder-read-pair-kernel-O0.txt`) | `server read: requests 1 codes 11`, `server read: requests 1 codes 12`, `client 1 (code 11): reply ok`, `client 2 (code 12): reply ok` |
| red: the engine before this fix (`build/tests/boot/xiaoji-red-b4f`) | `server read: requests 2 codes 11 12`, `client 1 (code 11): reply wrong`, `client 2 (code 12): reply wrong` (differs, `cmp`) |
| green: the fixed engine (`build/xiaoji`) | identical to the kernel output (`cmp`, O0 and O2) |

`make test` runs it in the engine and in Node (`binder-read-pair-O0` and `-O2`, 4 PASS lines).

### Milestones (B4f)

Five boots, **before the SIMD fix** (so each ends at the SIGILL), with the exact B4f command (`XIAOJI_STAMP=1 ./build/xiaoji --logcat --init image/xiaoji.rc --root build/rootfs
--init-timeout 240`, no trace), run one after another. The final binary (with the SIMD fix) was checked by `b4f-next.err` and by the `XIAOJI_BOOT=1` run (`build/tests/boot/b4f-fix-1..5.err`). Power for every boot: AC,
Low Power Mode off (`build/tests/boot/b4f-fix-*.pmset.txt`).

| Boot | system_server cycles entered | `No service published` | NPE | cycles reaching StartDisplayManager | cycles with the SIGILL |
|---|---|---|---|---|---|
| 1 | 7 | 0 | 0 | 7 | 7 |
| 2 | 7 | 0 | 0 | 6 | 6 |
| 3 | 7 | 0 | 0 | 6 | 6 |
| 4 | 7 | 0 | 0 | 6 | 6 |
| 5 | 7 | 0 | 0 | 6 | 6 |

Every cycle that reached `ThermalManagerService` found `thermalservice`. The `deadlock` and `nothing has run` lines are 0 in
all five boots. In boots 2 to 5 the seventh cycle starts but does not reach `StartDisplayManager` before the 240 s limit;
its log was not examined further.

First cycle, boot 1 (guest time, instruction count):

| Milestone | Time, insn |
|---|---|
| InitPowerManagement | 33.01 s, 3.590 G |
| StartDisplayManager (pid 1014) | 33.12 s, 3.599 G |
| SIGILL (pre-fix, `0x4f03f400`) | 33.13 s, 3.600 G |
| StartDisplayManager, after the SIMD fix (`b4f-next.err`, run with `XIAOJI_DEBUG_FAULTS=1`) | 33.42 s, 3.599 G |
| watchdog kill, after the SIMD fix (same run) | 103.05 s, 3.703 G |

### The SIGILL (checkpoint 2)

- **The faulting word.** `XIAOJI_DEBUG_FAULTS=1` now prints the region of the faulting PC (path and file offset; a debug
  line only, `engine/cpu_a64.c`). It printed `unimplemented A64 instruction 0x4f03f400 at pc 0x7297cee4`, and
  `region 72479000-72b9d000 .../boot-framework.oat off 0x1b2000: pc is file offset 0x6b5ee4`. The file's bytes at
  0x6b5ee4 are `00 f4 03 4f`, the same word. Decode (`objdump -D -b binary -m aarch64` in the builder VM):
  `fmov v0.4s, #5.0e-01`. It is the Advanced SIMD FMOV (vector, immediate), single precision, Q=1.
- **The symbol.** `oatdump --addr2instr=0x503ee4` names the method `android.util.Spline$MonotoneCubicSpline.<init>(float[], float[])`
  (dex_method_idx 36831): its compiled code block is at code_offset 0x6b4d50, size 1160, and oatdump's own listing shows
  `0x006b4ee4: 4f03f400  fmov v0.4s, #0x60 (0.5000)`. oatdump's addresses are the file offset minus 0x1000 (its listing
  line `0x006b5ee4: b940021f` is the file's bytes at 0x6b6ee4), so the input is the file offset 0x6b5ee4 minus the
  executable offset 0x1b1000 minus 0x1000. The method is a spline constructor with a float constant, which fits.
- **The fix (engine/simd.c).** `modified_imm` implemented cmode 1111: op 0 is FMOV into 2S or 4S (single), op 1 with Q 1
  is FMOV into 2D (double), and each value is replicated to every element with the high half cleared for Q 0. o2 1 (FP16,
  FEAT_FP16) and op 1 with Q 0 stay unimplemented (SIGILL). `vfp_expand32` and `vfp_expand64` are shared with the scalar
  `fmov_imm`. The translator reaches the same code through `tr_simd` and `simd_exec`, so the Node JIT path is the same
  code (checked in `engine/jit.c`).
- **The guest test (tests/guest/fmov-vec-imm.c).** Thirteen cases (4S, 2S with the high half cleared, 2D), each printing
  the register's two halves. Native run (the oracle) and the engine's output are identical (`build/tests/boot/fmov-oracle.txt`,
  `fmov-engine.txt`, and the interpreter `fmov-engine-int.txt`). The red run (the pre-fix binary) exits 132 with
  `unimplemented A64 instruction 0x4f03f400` (`fmov-red.err`).

### The next blocker: the display stack (the stop)

With the SIGILL gone, system_server reaches `DisplayManagerService` and blocks in its constructor. The watchdog kills it
60 s later. These lines are copied by grep from `build/tests/boot/b4f-next.err` (that run had `XIAOJI_DEBUG_FAULTS=1`):

```
[33420 ms 3599037562 insn] D/SystemServerTiming(1014): StartDisplayManager
[33428 ms 3599723671 insn] I/servicemanager(1001): Since 'SurfaceFlinger' could not be found, trying to start it as a lazy AIDL service
xiaoji: init: Control message: Could not find 'aidl/SurfaceFlinger' for ctl.interface_start
[103050 ms 3703462200 insn] W/Watchdog(1014): *** WATCHDOG KILLING SYSTEM PROCESS: Blocked in handler on main thread (main)
[103055 ms 3703690319 insn] W/Watchdog(1014):     at android.view.SurfaceControl.nativeGetCompositionDataspaces(Native Method)
[103056 ms 3703757025 insn] W/Watchdog(1014):     at com.android.server.display.DisplayManagerService.<init>(DisplayManagerService.java:501)
[103062 ms 3704111965 insn] I/Process(1014): Sending signal. PID: 1014 SIG: 9
```

**The probe (one boot; `build/tests/boot/b4f-sf.rc` is `image/xiaoji.rc` plus three service blocks copied from the image's
own rc files; log `b4f-sf.err`).** The image does not start SurfaceFlinger or the graphics HALs. With them started, the
first failures are:

```
[1587 ms 189543094 insn] E/libEGL(1007): load_driver(/vendor/lib64/egl/libEGL_angle.so): dlopen failed: library "libGLESv2_angle.so" not found: needed by /vendor/lib64/egl/libEGL_angle.so in namespace (default)
[1587 ms 189546675 insn] F/libEGL(1007): couldn't find an OpenGL ES implementation, make sure you set ro.hardware.egl or ro.board.platform
[1587 ms 189551291 insn] F/libc(1007): Fatal signal 6 (SIGABRT), code -1 (SI_QUEUE) in tid 1016 (surfaceflinger), pid 1007 (<unknown>)
[588 ms 94536430 insn] E/android.hardware.graphics.composer@2.4-service(1008): failed to open framebuffer device: No such file or directory
[799 ms 115944198 insn] E/android.hardware.graphics.allocator@3.0-service(1009): open_verbose_path:50: Could not open '/dev/goldfish_pipe': No such file or directory
```

The composer (`android.hardware.graphics.composer@2.4-service`, in `vendor/bin/hw`, with its VINTF fragment) exits at its
framebuffer open. The allocator registers `IAllocator/default` after its `/dev/goldfish_pipe` error. The image itself says
that the engine has no OpenGL ES driver (`image/xiaoji.rc`: ZygoteInit's GL preload is disabled for that reason).

**Why this is a design question, not a fix.** A real SurfaceFlinger needs three things the engine does not have: an OpenGL
ES implementation (the image has `libEGL_angle.so` but not the `libGLESv2_angle.so` it needs), a framebuffer device for the
composer, and the goldfish pipe to a host renderer for the allocator. Two directions are open, and neither is chosen here:
(1) a software GLES, a framebuffer, and a composer, so the real SurfaceFlinger runs (a large component, and slow in the
interpreter); or (2) a boot without SurfaceFlinger, which changes the framework's display path (outside "prefer real
components"). I did not try a stub, and I did not change the rc beyond the probe copy.

### Tests (this step)

- `make && make wasm-node && make test`: **176 passed, 0 failed, 0 xfail** (168 before; +4 `binder-read-pair` and +4
  `fmov-vec-imm`, engine and Node, O0 and O2).
- `XIAOJI_JIT=0 make test`: **176 passed, 0 failed, 0 xfail.**
- `XIAOJI_BOOT=1 make test`: **177 passed, 0 failed, 0 xfail.** The check now also requires `StartDisplayManager`, which is
  reached in every boot of this step (the thermal lookup is 5 of 5). The comment in `tests/run.sh` is updated.
- `make test-web`: all web checks passed.
- Warnings: `clang -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 -c` on `engine/binder.c`, `engine/cpu_a64.c` and
  `engine/simd.c`: 0. The same with `emcc`, one file at a time: 0. `gcc -Wall -Wextra` on the two new guest tests (and
  `binder-node-reuse.c`): 0.
- Power: `build/tests/boot/b4f-fix-*.pmset.txt` and `b4f-verify.pmset.txt` (AC Power, `lowpowermode 0`). The runs
  `b4f-r1` and `b4f-r2` overlapped in time, so no timing is taken from them.

### Gaps (B4f)

- **The display stack** (the next blocker, above): an OpenGL ES implementation, a framebuffer and the goldfish pipe are
  missing. The `startOtherServices` steps and the d3 to d5 milestones are not reached. Telephony, NFC, Bluetooth and UWB
  are not configured yet (the planned properties were not applied, since the boot does not get past the display).
- **The first-translation count protocol** (BR_ACQUIRE and BR_INCREFS, B4e) differs from the kernel. Not changed.
- **FP16 vector immediate** (`o2` 1) is not implemented. Not needed yet.
- **`FLAT_BINDER_FLAG_TXN_SECURITY_CTX`** contexts are still `u:r:kernel:s0` for every sender (B4c gap, unchanged).

## B5: the graphics stack (SurfaceFlinger is published; system_server stops at idmap2d)

Status: **on the product tree (`build/rootfs-xiaoji`), SurfaceFlinger starts and is published as a service, and
`DisplayManagerService` completes its constructor (`StartDisplayManager took to complete`), the plan's
`nativeGetCompositionDataspaces` step. `system_server` then reaches `Starting phase 100`, and it stops there: it waits
for `idmap2d` (`OverlayManagerService` through `IdmapDaemon.getIdmapService`), the watchdog fires, and zygote starts
`system_server` again. Four such cycles ran in the long boot (about 580 s of guest time). The stop is not
`lmkd`: `lmkd` exits, and ActivityManager retries it once a second (343 retries) without blocking.**
The design and the device work are in `docs/graphics.md`, G1.

### What changed in this step

- **Binder: a nested synchronous call went to the wrong thread** (`engine/binder.c`, the send path). The composer, while
  serving transaction 98 from SurfaceFlinger's main thread (tid 1007), called `onHotplug` back on SurfaceFlinger
  (transaction 99, synchronous, `IComposerCallback.hal` does not mark it `oneway`). The engine chose the target from the
  sender's outgoing calls, found none, and queued the request on the process: binder thread 1020 took it and blocked,
  while the main thread waited for the composer. The driver's rule (6.12 source, `drivers/android/binder.c`, lines
  3208-3230 in the builder VM: the walk over `thread->transaction_stack` from the top, `target_thread = from` for the
  first `from` in the target proc) takes the thread that sent the request this thread is serving, here tid 1007. The fix
  walks `t->recv` (the requests this thread is serving, the newest first) first, and keeps the old walk as the fallback.
  **No guest test covers it yet** (see gaps).
- **SIMD: `fabd s0, s0, s1` (0x7ea1d400)** was not implemented, in SurfaceFlinger's hotplug path. It is in the scalar
  floating-point three-same group (`engine/simd.c`, `scalar_simd` calls `fp_three_same_impl` with `scalar = 1`: lane 0,
  the rest of the register zeroed). Only the opcodes the architecture allocates in that group are decoded
  (`scalar_fp3_allocated`). Tests: `tests/guest/fabd-scalar.c` and `tests/guest/fp-scalar3.c` (FMULX, FCMEQ, FCMGE,
  FCMGT, FACGE, FACGT, FRECPS, FRSQRTS), each compared with the native run: identical.
- **GL: `glShaderString` (opcode 2254) and `glFinishRoundTrip` are decoded by hand** (`engine/gl_host.c`), as the host does
  (`gles2_dec.cpp`). The spec marks them `not_api`, so the generator skips them; the first shader source then stopped the
  GL stream (`opcode 2254 is not decoded`).
- **GL: every pointer argument carries its wire length** (`tools/gen-glproto.py`). The null backend bounds each write by it
  (`engine/gl_null.c`), and keeps colour-buffer pixels (RGBA8888).
- **Init** (`image/xiaoji.rc`): the graphics services, `vndservicemanager` (the composer's vendor binder), `lmkd` and
  `idmap2d`, from the image's own rc files.

### Milestones (product tree)

The first cycle, from `build/tests/boot/b5f.err` (`XIAOJI_STAMP=1 ... --init-timeout 90`); time and instructions are guest time:

| Milestone | Time | Instructions |
|---|---|---|
| allocator@3.0 registered | 1.62 s | 0.179 G |
| SurfaceFlinger main thread ready; RenderEngine on the GLES backend | 2.02 s | 0.221 G |
| composer@2.4 registered | 2.18 s | 0.232 G |
| `Connecting HAL display 0` (SurfaceFlinger) | 3.87 s | 0.411 G |
| `Entered the Android system server!` | 35.63 s | 3.793 G |
| `StartDisplayManager took to complete: 17ms` | 39.14 s | 4.137 G |

The long run (`build/tests/boot/long2.err`, `--init-timeout 600`, outer `timeout 900`), last occurrences:

| Milestone | Time | Instructions |
|---|---|---|
| `Starting phase 100` (the last of four `system_server` cycles) | 473.6 s | 19.79 G |
| `WAITED_HALF` (the watchdog, the same cycle) | 532.8 s | 20.61 G |
| `GOODBYE` (the watchdog kills `system_server`) | 573.4 s | 20.63 G |

Wall clock of the first cycle run: 1:30.06, 51.2 s user and 0.69 s system (57% CPU). B4f reached `StartDisplayManager` at
33.1 s (3.599 G) on `build/rootfs`. Power: `pmset -g` before and after the run shows `lowpowermode 0`
(`build/tests/boot/b5f-pmset-before.txt`, `b5f-pmset-after.txt`); the source was AC Power (`pmset -g batt`, checked for
earlier runs).

### The next blocker (logcat)

`idmap2d` is started by init. It runs, but it refuses the overlay work and then dies:

```
W/idmap2d(1788): failed to parse idmap header of '/data/resource-cache/product@overlay@EmulatorConnectivityOverlay.apk@idmap'
E/idmap2d(1788): will not write to /data/resource-cache/product@overlay@EmulatorConnectivityOverlay.apk@idmap: calling uid 0 lacks write accesss
xiaoji: init: idmap2d exited (status 134, by a signal)
```

`system_server` waits in it:

```
at com.android.server.om.IdmapDaemon.getIdmapService(IdmapDaemon.java:292)
at com.android.server.om.IdmapDaemon.connect(IdmapDaemon.java:316)
at com.android.server.om.IdmapDaemon.idmapExists(IdmapDaemon.java:171)
```

The cause is the engine's credential model. Every service runs as uid 0: `engine/init.c` does not model `user` or `group`
(the stock blocks name `system`, `lmkd` and so on), and `engine/cred.c` states that no permission is enforced. The
framework's uid checks (`idmap2d` checks its caller is `system`, uid 1000) therefore fail. Making the stock services
(system_server, idmap2d, lmkd) run as their users is a cross-cutting change to init and to the credential checks, and it
was not made in this step. It is the next task.

`lmkd` (`service lmkd`, started by init) exits at once, because this kernel model has no PSI and no cgroup-v1 memory
hierarchy. ActivityManager retries it once a second (343 retries in the long run) and continues. It is noise on the
critical path, not the stop.

### Tests (this step)

- The boot check (`tests/run.sh`, `XIAOJI_BOOT=1`) requires the lines of the first cycle on both trees (the allocator, the
  SurfaceFlinger main thread, the RenderEngine GLES start) and no `is not decoded` line. On the product tree (a
  `vndservicemanager` is present) it also requires `StartDisplayManager took to complete` and no `Since 'SurfaceFlinger'
  could not be found` line. This is the checkpoint-4 smoke test, read from the log, not a separate `service check` init
  run.
- The Google image (`build/rootfs`) does not get the product's composer: it has no `vndservicemanager`, so its composer
  never registers and SurfaceFlinger never connects its display. Its check does not require the product lines.

### Gaps (B5)

- **The uid model** (above): `idmap2d`, `system_server` and `lmkd` run as root, so `system_server` stops at phase 100.
- **The binder fix has no guest test** with the kernel as oracle. The plan asked for one. The case to write: a two-hop
  call (A calls B, B calls back to A before replying, the callback lands on A's calling thread) and a three-hop call
  (A to B to C to A). A caller waiting for a reply takes no process work, so a misrouted callback deadlocks a single-
  threaded client. Node passing for the fixture is in `binder-node-reuse.c` and `binder-sg.c`. Not written.
- **No frame is posted** in these runs (`frame N posted` never appears), so `rcFBPost` has not been exercised.
- **The GLES calls answered by the default (no-op) backend**, listed once in the log (`xiaoji: gl: <call> answered by the
  default (no-op) backend`), from the long run: `glAttachShader`, `glBindAttribLocation`, `glBindTexture`,
  `glCompileShader`, `glEnableVertexAttribArray`, `glGetError`, `glGetUniformLocation`, `glLinkProgram`, `glPixelStorei`,
  `glTexImage2D`, `glUniformMatrix4fv`, `glUseProgram`, `rcCreateDisplayById`, `rcOpenColorBuffer2`,
  `rcSetDisplayPoseDpi`, `rcSetPuid`, `rcSetWindowColorBuffer`. None of them stops the boot so far.
- **`XIAOJI_GF_TRACE=1`** (goldfish) and **`XIAOJI_BINDER_TRACE=1`** (binder) are the two traces used here.

## B6: the credentials model, init's ids, and the boot past idmap2d (stops at the sensors HAL)

Status: **the credentials model is built and checked against the native root oracle. init applies `user`, `group` and
`capabilities`. idmap2d passes (it was never refused for uid 0; see the correction below). system_server now reaches
`Starting phase 100` and `StartDisplayManager`, and waits for the `health` and `sensors` HALs, which the image's init does
not start. `System now ready` is not reached. The next blocker is the sensors HAL, which needs the goldfish `qemud`
service (`pipe:qemud:sensors`), not in the engine.**

### Correction to B5

B5 said `idmap2d` refused its caller because every service ran as uid 0. That was wrong. The log line is
`will not write ... calling uid 0 lacks write accesss`, and `UidHasWriteAccessToPath` (frameworks/base/cmds/idmap2/libidmap2/
FileUtils.cpp) returns true for root. It returns false only when `Realpath(dirname(path))` fails. The directory
`/data/resource-cache` did not exist: the engine's `image/xiaoji.rc` does not run the stock `init.rc` line
`mkdir /data/resource-cache 0771 system system`. Adding the `mkdir` (in the early-init block) was the fix that made idmap2d
pass. The uid model is still needed, because idmap2d now runs as `system` as the image's rc says, and its checks compare
against the real uid.

### The credentials model (engine/cred.c, engine/proc.h)

- **State:** real, effective, saved and fs uids and gids; the supplementary groups (up to 32, `CRED_MAX_GROUPS`);
  permitted, effective, inheritable and ambient sets (`proc_cred_t.cap_*`); the bounding set (`proc_t.capbset`); keep-caps
  (`proc_t.keepcaps`). A fork copies all of it. The first process is root with the full effective and permitted sets and
  an empty inheritable and ambient set, as in the oracle VM (`CapBnd 1ffffffffff`, `CapEff` and `CapPrm` full).
- **Rules, from the 6.12 sources in the builder VM** (`kernel/sys.c`, `kernel/groups.c`, `kernel/capability.c`,
  `security/commoncap.c`): a privileged check is the effective set holding `CAP_SETUID`, `CAP_SETGID` or `CAP_SETPCAP`
  (`ns_capable`), not `euid == 0`. The set*id calls, setgroups, setfsuid and setfsgid follow `kernel/sys.c`.
  `cap_emulate_setxuid` runs for setreuid, setuid and setresuid (a uid change from or to root clears or restores the
  sets, keep-caps keeps them, and the ambient set is cleared when all ids become non-root). `cap_task_fix_setuid` for
  setfsuid moves `CAP_FS_SET` with fsuid 0.
- **capget and capset** (`kernel/capability.c`, `cap_capset`): the version selects 1 or 2 structs; an unknown version is
  answered with v3 and EINVAL (NULL data is a probe that returns 0). capget of another pid reads that process (ESRCH if
  none). capset checks the inheritable set against the old inheritable and bounding sets, and against the old permitted
  set without CAP_SETPCAP. The ambient set is masked to permitted and inheritable.
- **prctl:** PR_CAPBSET_READ and PR_CAPBSET_DROP (CAP_SETPCAP), PR_SET_KEEPCAPS, PR_CAP_AMBIENT (IS_SET, RAISE, LOWER,
  CLEAR_ALL, with the argument checks of `cap_task_prctl`).
- **exec** (`cred_exec`, called from `exec_run` and from init's service start, after the image is in place): keep-caps is
  cleared, and the sets are recomputed as `cap_bprm_creds_from_file` does for a program with no file capabilities and no
  set-id bits: a root process (real or effective uid 0) gets permitted = bounding | inheritable and effective when the
  effective uid is 0; a non-root process gets permitted = ambient and effective = ambient.
- **Not modelled:** file capabilities (`security.capability`), set-uid and set-gid bits (the rootfs has no owner metadata,
  see the DAC decision), the securebits lock (SECBIT_KEEP_CAPS_LOCKED is not kept, so keep-caps can be set again after
  exec), SECURE_NOROOT and the other securebits, the 65536-group limit (32 here; setgroups above 32 is EINVAL), and
  user namespaces.
- **Uses that now check capabilities instead of euid:** mount, umount2 and unshare(CLONE_NEWNS) (CAP_SYS_ADMIN),
  setrlimit raising a hard limit (CAP_SYS_RESOURCE), PR_SET_SECUREBITS (CAP_SETPCAP).
- **/proc/<pid>/status** shows the real Uid and Gid lines, Groups, and CapInh, CapPrm, CapEff, CapBnd, CapAmb.
  `/proc/self/status` was not served before; it now is (the same text as the current process's).

### init: user, group, capabilities (engine/init.c, engine/ids_gen.h)

- `user NAME` and `group NAME [SUPP...]`: AID names from bionic's table (`system/core/libcutils/include/private/
  android_filesystem_config.h`, 142 entries), or a number. Not /etc/passwd. `capabilities NAME...`: the names init's
  `capabilities.cpp` accepts (41 entries), with the numbers from bionic's `linux/capability.h`. Unknown names fail the
  config. `tools/fetch-remote-ids.sh` copies the three AOSP files from the remote tree and runs `tools/gen-ids.py`, which
  writes `engine/ids_gen.h`.
- The service's process applies init's `SetProcessAttributesAndCaps` (`system/core/init/service.cpp`,
  `service_utils.cpp`, `capabilities.cpp`) before its program loads (`cred_apply_service`): keep-caps when there are
  capabilities and the uid is not 0; then setgid (when the gid is not 0), setgroups, setuid (when the uid is not 0). With
  capabilities: capset with the set and SETPCAP, drop the bounding set outside the set, capset with the set only, and raise
  the ambient set. Without capabilities, a non-root service clears its inheritable set. Then the program loads and the exec
  transform runs.
- `image/xiaoji.rc`: the image's own user, group and capability lines for `lmkd` (`user lmkd`, `group lmkd system readproc`,
  `capabilities DAC_OVERRIDE KILL IPC_LOCK SYS_NICE SYS_RESOURCE`), `idmap2d` (`user system`, `group system`) and
  `surfaceflinger` (`capabilities SYS_NICE`). The other blocks already had them.

### Binder and sockets

- **Binder:** the sender's euid is the sender's effective uid at send time (`btxn_t.sender_euid`, set in
  `engine/binder.c` for requests and replies). It was a constant 0. The guest's own value in its transaction data is
  ignored, as the driver does.
- **SO_PEERCRED** is the peer's effective uid and gid (`cur_cred`), as `net/core/sock.c` `cred_to_ucred` does.
- **SCM_CREDENTIALS, implicit** (when the receiver has SO_PASSCRED): the sender's real uid and gid (`cur_scm_cred`), as
  `scm_set_cred` with `current_uid()` and `current_gid()` does.
- **SCM_CREDENTIALS, explicit** (a sendmsg cmsg): `sock_scm_check` is `scm_check_creds`: the pid must be the sender's
  (or CAP_SYS_ADMIN), the uid one of its real, effective or saved uids (or CAP_SETUID), the gid likewise (or CAP_SETGID),
  otherwise EPERM; -1 is EINVAL. The last one in the message wins.

### DAC: not enforced (decision)

Permission checks on file mode bits are **not** enforced in this step. The evidence:

- The rootfs is extracted with `debugfs rdump` (`tools/extract-rootfs.sh`, `tools/fetch-remote-rootfs.sh`). On the host,
  every file and directory of `build/rootfs-xiaoji` is owned by the Mac user and group `staff`, and the files have mode
  `775`. Android's system binaries are `0755` and owned by root or by their service user. So the extraction does not
  keep owner or mode (`stat -f '%Sp %Su %Sg'`, `ls -ln`). The `rdump` run is probably not root, which would make its
  `fchown` a no-op, and the mode `775` matches a process default (umask 002 on `0777`), not the ext4 mode. This is inferred
  from the output, not checked against the tool's source.
- The canonical owner and mode table (AOSP's `fs_config`) is not in the places searched on the remote tree
  (`system/core/libcutils`, `build/make/tools`, `build/make/target/product`, `system/core/rootdir`). The record is therefore
  the extraction result, not an fs_config comparison.
- Enforcing DAC on `/data` and `/system` would deny services that the stock images run correctly (for example writes
  to `/data/misc` by the stock services under the wrong mode). With no owner and mode data, enforcement would be a guess.
- Guest `stat` keeps reporting uid and gid 0 for rootfs files. Bionic's property-area checks depend on it.

**Gap:** the rootfs needs owner, mode, file capability and set-id metadata (from the image's ext4 metadata, read with
`debugfs stat` on the system image, or fs_config) before DAC can be enforced. setuid and file capabilities are not used
by the boot path yet.

### Milestones (product tree, `build/rootfs-xiaoji`, `--logcat --init-timeout 90`, the boot check's command)

Times and instruction counts are guest time (`[ms insn]` in the log):

| Milestone | Time | Instructions |
|---|---|---|
| `Started idmap2d` (init) | 1.34 s | 0.157 G |
| `Entered the Android system server!` | 36.74 s | 3.817 G |
| `StartDisplayManager` (SystemServerTiming) | 40.40 s | 4.160 G |
| `Starting phase 100` | 40.42 s | 4.161 G |
| idmap2d `failed to parse idmap header` warnings (no refusal) | 48.8 s | 4.87 G |
| end of the 90 s run (init timeout) | 89 s | 5.10 G |

Wall clock of that run: 1:30.06, 48.98 s user, 0.67 s system (55% CPU). Power: `pmset -g` before the run showed `lowpowermode 0` on AC power
(the scratchpad copy is `pmset-before2.txt`).

A 400 s run (`--init-timeout 400`, no health or sensors blocks, the state of this step) reached the same phase 100 at
39.3 s (4.161 G). The first system_server (pid 1042) then waited on the HAL lookups, and a later system_server (pid 1517)
was waiting in the same place at 398 s (15.18 G).

### The next blocker (logcat)

`system_server` waits for HIDL services that init does not start. hwservicemanager tries to start them as lazy HALs, and
init reports that it cannot find them:

```
W/HidlServiceManagement(1042): Waited one second for android.hardware.health@2.0::IHealth/default
I/hwservicemanager(1002): Since android.hardware.health@2.0::IHealth/default is not registered, trying to start it as a lazy HAL.
xiaoji: init: Control message: Could not find 'android.hardware.sensors@2.1::ISensors/default' for ctl.interface_start
E/ActivityManager(1042): Connection failed: java.io.IOException: Connection refused
W/ActivityManager(1042): Failed to connect to lowmemorykiller, retry later
```

The image has both HALs: `vendor/bin/hw/android.hardware.health@2.1-service` and
`android.hardware.sensors@2.1-service.multihal`, with their rc blocks in `vendor/etc/init/`. Both were added to
`image/xiaoji.rc` (class hal, user, group and capabilities, as their rc files say; the `task_profiles`, `rlimit` and `file`
lines are not modelled) and run in a 400 s boot. The sensors HAL then aborts at once:

```
E/android.hardware.sensors@2.1-service.multihal(1014): qemu_pipe_open_ns:159: Could not connect to the 'pipe:qemud:sensors' service: Invalid argument
E/android.hardware.sensors@2.1-impl.ranchu(1014): MultihalSensors:39: m_qemuSensorsFd is not opened
xiaoji: killed by signal 6
F/crash_dump64(1027): failed to open /proc/1023: No such file or directory
```

The engine has no `qemud` service (the goldfish pipe services `opengles`, `GLProcessPipe` and `refcount` only, `engine/goldfish.c`).
The sensors HAL restarts in a loop and does not make its interface available. The two HAL blocks were **reverted** from
`image/xiaoji.rc` after this run, so the committed config does not start them. The health HAL's result alone was not
separated out in that run.

### Checkpoints (this step)

1. Creds syscalls and the guest test: done (`tests/guest/creds.c`, oracle as root, identical output in the engine at O0
   and O2, and in Node).
2. init user, group and capabilities, and the id test: done (`tests/init/ids.rc`, `tests/expect/init-ids.out`:
   `uid=1000(system) gid=1000(system) groups=1000(system),3003(inet)`). A scratch check with `capabilities NET_RAW SYS_NICE`
   gave Inh, Prm, Eff, Bnd and Amb all `0000000000802000`.
3. idmap2d passes: done (the `resource-cache` directory; no refusal in the 90 s run; idmap2d runs as `system`).
4. Boot progress: partial. Past idmap2d to `Starting phase 100` and `StartDisplayManager`; stopped at the sensors HAL.

### Tests (this step)

- `make` and `make wasm-node`: exit 0. `make test`: 194 passed, 0 failed (188 before, plus the creds cases (engine and
  Node, O0 and O2) and the init id case (engine and Node)).
- `XIAOJI_JIT=0 make test`: 194 passed, 0 failed.
- `XIAOJI_BOOT=1 make test`: 195 passed, 0 failed (the boot check now also requires `Starting phase 100` and no
  `lacks write` line on the product tree only).
- `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh`: 161 passed, 0 failed.
- `make test-web`: exit 0.
- `clang -std=c11 -Wall -Wextra` on the changed engine files: no warnings. `emcc -std=c11 -Wall -Wextra` per file (cred, init,
  sock, sockcall, binder, simd, procfs, syscall, sysfs, exec, proc): no warnings.
- Guest tests `creds-O0` and `creds-O2` (engine and Node) compare with the native root oracle.

### Gaps (B6)

- **Sensors HAL:** the `qemud` pipe service is needed (`pipe:qemud:sensors`). Without it the HAL aborts. Either the engine
  serves that pipe, or the sensors HAL is left out and SensorService's wait is handled by config. This is the next decision.
- **Health HAL:** its result was not separated from the sensors abort (see above).
- **lmkd** exits at once (no PSI, no cgroup memory), and ActivityManager retries (B5). The `lmkd` block now runs as `lmkd`.
- **Binder:** no guest test that checks `sender_euid` against the kernel (the B5 gap for the reply routing is still open).
  The socket credentials are tested (`tests/guest/creds.c`).
- **DAC and metadata:** not enforced (see the decision). setuid, set-gid bits and file capabilities are not modelled.
- **securebits lock** and the other securebits are not modelled (see above).
- `/data/resource-cache` is created with the engine's mkdir, which ignores the mode and owner (`0771 system system` in the
  stock rc).

## B7: the sensors and health HALs, /dev/ashmem, per-user data, and the boot past phase 100 (stops at installd's owner check)

Status: **the sensors and health HALs run on the product tree, and system_server starts their services. `Starting
phase 100` is passed (3 of 3 runs), and BatteryService receives the health HAL's first update (3 of 3 runs, 63 to 66 s
into a 90 s run). `System now ready` is not reached. The boot stops at a decision that is not made yet, the owners of
files (B6 deferred it): installd checks the owner of a package directory it has just created, the engine reports every
file as root, and the check fails (next blocker 1). Behind it is a second gap, the engine has no `fcntl` record locks,
which SQLite needs (next blocker 2).**

### Checkpoints

| # | Checkpoint | Result |
|---|---|---|
| 1 | Sensors pipe and its guest test | Done. `engine/sensors.c`, `engine/goldfish.c`, `tests/guest/sensors.c`, `tests/expect/sensors.out`. |
| 2 | Health HAL, sysfs and a power_supply check | Done. `engine/powersupply.c`, the `power-supply` case (`tests/expect/power-supply.out`). The HAL opens every file it needs (below). |
| 3 | Phase 100 passed | Done (3 of 3 runs). |
| 4 | Boot progress | Partial: past phase 100 to BatteryService's health update. Stopped at the owner decision, then the lock gap. |

### Decisions

- **Sensors: the engine serves the goldfish qemud sensors protocol** on `/dev/goldfish_pipe` (service `qemud:sensors`:
  `engine/sensors.c` for the protocol and the data, `engine/goldfish.c` for the pipe). The guest HAL is the real
  `android.hardware.sensors@2.1-service.multihal`. The data source is `sensors_set(handle, values, n)` (the plan wrote
  `(type, values)`; the handle is the qemud index, and `n` is the value count), so a browser bridge (DeviceMotion,
  DeviceOrientation) can feed it later.
- **Health: the real `android.hardware.health@2.1-service`**, with its sysfs inputs served by the engine
  (`engine/powersupply.c`). The uevent socket and the wake alarm are not provided. Both calls fail (observed below), and
  healthd continues past them.
- **/dev/ashmem is served by the engine** (`engine/ashmem.c`). libcutils and libfmq use it, because `sys.use_memfd` is
  unset in Android 13, so the device is the fix, not the property.
- **Per-user data: `mkdir` lines in the early-init block**, standing in for vold (which the engine does not have).
- **Not added:** an `interface` line. Neither HAL's stock rc has one (both start at boot, and the engine starts any
  service that is not `disabled`), so the plan's "interface lines" do not apply to these two.
- **Telephony, NFC, Bluetooth and UWB are not disabled.** None has blocked the boot yet.

### Protocols implemented (with the evidence from the sources)

Sources are the remote tree `~/xiaoji-aosp` on `build-host`, read only. Functions are named rather than lines, so the
references hold after an edit.

**Sensors (goldfish qemud).**
- Pipe: `pipe:qemud:sensors` (`qemu_pipe_guest.cpp`, `qemu_pipe_open_ns`: `"pipe:%s:%s"`, written with its NUL).
- Framing: `qemud.cpp`, `qemud_channel_send` writes `"%04x"` of the payload length, then the payload; `qemud_channel_recv`
  reads 4 hex digits, then the body. The engine uses the same framing both ways. A zero-length send writes nothing.
- Handshake (`multihal_sensors.cpp`, constructor): `"time:%" PRId64` (no reply), then `"list-sensors"`, and the reply is
  parsed with `sscanf(buffer, "%u", ...)`. The engine answers with the decimal mask of the advertised handles: bits 0, 1,
  2, 3, 5 and 6 (accelerometer, gyroscope, magnetic field, orientation, proximity, light), which is `111`. In the engine's
  boot the HAL logs `host sensors mask=6f` (the same bits, in hex) as the first line of its log.
- Enable and delay (`multihal_sensors.cpp`): `"set:%s:%d"` with the name from `getQemuSensorNameByHandle` (`kQemuSensorName`
  in `sensor_list.cpp`: `acceleration`, `gyroscope`, `magnetic-field`, `orientation`, `proximity`, `light`). `"set-delay:%d"`
  in milliseconds, at least 1. `initialize()` calls `setAllQemuSensors(true)`, so every advertised sensor is enabled.
- Events (`multihal_sensors_qemu.cpp`, `parseQemuSensorEvent`): `testPrefix` matches `acceleration:%f:%f:%f`,
  `gyroscope:%f:%f:%f`, `magnetic:%f:%f:%f` (the prefix is `magnetic`, not `magnetic-field`), `orientation:%f:%f:%f`,
  `proximity:%f`, `light:%f` and `sync` (accepted, no effect). Anything else logs `don't know how to parse`. No run logged
  that line, and no run logged a `qemud_channel` error.
- Proximity: `sensor_list.cpp`, the proximity `SensorInfo` has `maxRange = 1.0`, so "far" is 1.0.

**Health (healthd's BatteryMonitor, `system/core/healthd`).** The directory scan reads `type` in each subdirectory: `Battery`
is the battery (unless its `scope` is `Device`), and `Mains` is a charger, which needs `online`. The battery reads `status`,
`health`, `present`, `capacity`, `voltage_now`, `charge_full`, `charge_full_design`, `current_now`, `cycle_count`, `temp`,
`technology` and `capacity_level` when they exist. The engine's tree (`/sys/class/power_supply`): `battery/` with `type
Battery`, `status Full` (AC online at capacity 100), `capacity_level Full`, `health Good`, `present 1`, `capacity 100`,
`technology Li-ion`, `voltage_now 4200000`, `current_now 0`, `charge_full 5000000`, `charge_full_design 5000000`,
`cycle_count 0`, `temp 250`; and `ac/` with `type Mains` and `online 1`. In a boot with `XIAOJI_PS_TRACE=1` every one of
these files is opened (by the health HAL, and by system_server's health wrapper, which reads the same files).

**Health loop (`hardware/interfaces/health/utils/libhealthloop/HealthLoop.cpp`).** `WakeAlarmInit` calls
`timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_NONBLOCK)`, and `UeventInit` calls `uevent_open_socket`; on failure each logs and
returns, and `StartLoop` continues into `MainLoop`. Both calls were observed in a traced boot (`--trace`, 25 s; the
trace file is `trace-h.err` in the session scratchpad): `timerfd_create(0x9, 0x800)` = -22 (EINVAL; the engine's
timerfd takes only `CLOCK_MONOTONIC` and `CLOCK_BOOTTIME`, B3), and `socket(AF_NETLINK 0x10, SOCK_RAW|CLOEXEC, NETLINK_KOBJECT_UEVENT 0xf)`
= -97 (EAFNOSUPPORT). The HAL logs `Hal init done` after them in every run. Their messages are `KLOG` lines, which logcat
does not show.

**/dev/ashmem (`system/core/libcutils/ashmem-dev.cpp`, `system/libfmq/include/fmq/MessageQueueBase.h`).**
- `get_ashmem_device_path` reads `/proc/sys/kernel/random/boot_id` and uses `/dev/ashmem<boot_id>`. If the file cannot be
  read it returns an empty path, and `ashmem_create_region` fails. (The zygote logged `Failed to read
  /proc/sys/kernel/random/boot_id`.) The open falls back to `/dev/ashmem`.
- `__ashmem_open_locked` then requires `fstat` to show `S_ISCHR` with a nonzero `st_rdev`. The engine reports major 10,
  minor 61 (the kernel's misc device).
- `ashmem_create_region` sets the name and size (`ASHMEM_SET_NAME`, `ASHMEM_SET_SIZE`), and `ashmem_set_prot_region` sets the
  mask. libfmq's `MessageQueueBase` calls it with `"MessageQueue"` and maps the fd with `mmap(..., handle->data[fdIndex],
  ...)`. Before the device existed, the same path gave `mmap failed: 9` (EBADF) and the fatal `mReadPtr is null`.

**Labels (`external/selinux/libselinux/src/android/android_platform.c`, `restorecon_sb`).** `restorecon_sb` calls
`lgetfilecon` and goes to `err` on any failure, so a file with no label fails it. A kernel with SELinux labels every new
file at creation, so the engine gives an unlabelled file a default label on a read: `u:object_r:unlabeled:s0`, with its NUL
(`engine/xattr.c`). Before the fix, a new file read `?` from `ls -Z`, and `restorecon` printed `Could not set context ... No
data available` and failed. After it, `restorecon` exits 0 and labels the file. The store (`engine/xattr.c`) keeps the
attributes in memory for the run, keyed by the canonical guest path, with `XATTR_CREATE` and `XATTR_REPLACE`.

**Per-user data (`system/core/rootdir/init.rc` lines 808 to 810 and 912 to 919; `system/vold/vold_prepare_subdirs.cpp`,
`prepare_subdirs`).** `/data/user`, `/data/user/0`, `/data/user_de`, `/data/user_de/0`, the `system_ce`, `system_de`,
`misc_ce`, `misc_de`, `vendor_ce` and `vendor_de` user-0 directories, and the profile directories `misc/profiles/{cur,cur/0,ref}`,
as `mkdir` lines in `image/xiaoji.rc`'s early-init block. The engine's `mkdir` takes no mode and no owner (B6).

### Milestones (product tree, `build/rootfs-xiaoji`, from a cleared `data/`, `--logcat --init-timeout 90`)

Times and instruction counts are guest time (`[ms insn]` in the log). Wall clock is 90.1 s per run. Power: `pmset -g`
shows `lowpowermode 0`, and `pmset -g batt` shows `Now drawing from 'AC Power'` (scratchpad: `pmset-b7.txt`,
`pmset-batt-b7.txt`).

| Milestone | run 1 | run 2 | run 3 |
|---|---|---|---|
| Sensors HAL: `host sensors mask=6f` | 0.65 s, 0.096 G | 0.64 s, 0.096 G | 0.62 s, 0.096 G |
| `Registered android.hardware.sensors@2.1::ISensors/default` | 2.08 s, 0.216 G | 2.34 s, 0.218 G | 1.94 s, 0.214 G |
| `Registered android.hardware.health@2.1::IHealth/default` | 2.10 s, 0.217 G | 2.35 s, 0.219 G | 1.96 s, 0.216 G |
| `Starting phase 100` | 47.9 s, 4.697 G | 46.9 s, 4.695 G | 45.6 s, 4.604 G |
| `BatteryService: health: Waited 0ms and received the update` | 65.7 s, 6.494 G | 64.9 s, 6.489 G | 63.0 s, 6.395 G |
| `FATAL EXCEPTION IN SYSTEM PROCESS` (the next blocker, below) | 66.4 s, 6.561 G | 65.6 s, 6.554 G | 63.6 s, 6.462 G |

Phase 100 was 4.16 G (40.4 s) in B6. B7 reaches it later because the HALs and `/dev/ashmem` add work before it. Before the
device existed (the first run with the two HALs) it was 4.18 G (41.0 s). Every run has no `don't know how to parse`
line and no `qemud_channel` error from the sensors HAL, and the HAL does not abort. The product boot check in `tests/run.sh`
requires the mask line, both registrations and the BatteryService update, with `Waited [0-9]*ms`.

Run 4 (after the default label, the same command) went to phase 100 at 45.0 s (4.696 G) and to the BatteryService update at
65.2 s (6.783 G). Its installd no longer logs `Could not set context`.

### Next blocker 1: installd's owner check (the decision)

With the labels fixed, installd creates the device-encrypted package directory, then checks it again in the same boot. The
check is strict (`fs_prepare_dir_strict`, `system/core/libcutils/fs.cpp`), so an existing directory must have the owner the
package has, and the engine's `chown` records nothing, so `stat` reports `0:0`:

```
E/cutils(1005): Expected path /data/user_de/0/com.android.uwb.resources with owner 10055:10055 but found 0:0
E/installd(1005): Failed to prepare /data/user_de/0/com.android.uwb.resources: Success
W/PackageManager(1047): Failed to create app data for com.android.uwb.resources, but trying to recover: com.android.server.pm.Installer$InstallerException: Failed to prepare
```

This is the owner and mode model that B6 left out (`DAC and metadata: not enforced`). It is a design decision, not a bug in
one call. Recommendation: record the owner and mode per canonical guest path (`chown`, `fchown`, `fchownat`, `chmod`, `fchmod`,
`fchmodat` set them; `stat` reports them, and the defaults stay root and 0755/0644 for unrecorded paths), keep the record in
memory for the run, as the xattr store does, and clear `build/rootfs-xiaoji/data` before every product boot (the boot check
should do this itself). Storing owners in host xattrs was rejected: it is not portable (macOS), and it would make `/data`
state carry over between runs. The alternative is the fs_config table from the image, which the B6 notes say is not in the
searched places of the remote tree.

### Next blocker 2: record locks (SQLite)

After the DE directories, SettingsProvider gets past its directory check and fails on the database's first lock:

```
E/SQLiteLog(1047): (3850) disk I/O error in "PRAGMA journal_mode"
android.database.sqlite.SQLiteDiskIOException: disk I/O error (code 3850 SQLITE_IOERR_LOCK): , while compiling: PRAGMA journal_mode
```

SQLite takes POSIX record locks with `fcntl(F_SETLK)`. The engine's `sys_fcntl` (`engine/sysfs.c`) handles the pipe size, dup, the
descriptor and file flags, and has no lock commands, so `F_GETLK`, `F_SETLK` and `F_SETLKW` fail. Recommendation: implement
record locks in the engine: a table keyed by the file (the memfd or host file object) and the byte range, shared by all
processes of the engine (a fork inherits no locks, and an exit or a close drops the process's locks, as Linux does), with
`F_SETLKW` waiting through the restart mechanism that pipes use (B1).

### The engine path after the blockers

Not reached: `System now ready` and `sys.boot_completed=1`. The `Invalid path /system_ext/priv-app/Settings: No such file
or directory` and `Invalid path /system/framework/framework-res.apk` lines from installd's path check remain unexplained
(both files exist, and `realpath` works from the guest shell).

### Tests

- `make build/xiaoji && make wasm-node && tests/run.sh`: **200 passed, 0 failed.** The 194 from B6, plus `sensors`,
  `power-supply` and `ashmem` in the engine and in Node.
- `XIAOJI_JIT=0 tests/run.sh`: **200 passed, 0 failed.**
- `XIAOJI_BOOT=1 tests/run.sh` (the Google image): **201 passed, 0 failed.**
- `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh` (product, from a cleared `data/`): **167 passed, 0 failed**,
  the 161 from B6 plus the six new engine and Node checks. Its boot check now also requires the mask line, both
  registrations and the BatteryService update.
- `make test-web`: all checks passed.
- Warnings: `clang -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter` on `engine/{sensors,goldfish,vfs,poll,powersupply,ashmem,xattr,sysfs,procfs}.c`:
  0. `emcc` with the same flags on the same files: 0. `clang -ffreestanding -Wall -Wextra` on `tests/guest/{sensors,ashmem}.c`: 0.
- The guest tests. `tests/guest/sensors.c` opens `/dev/goldfish_pipe`, writes `pipe:qemud:sensors`, sends `time:` and
  `list-sensors` (checks `111`), checks that nothing is readable before an enable (an unknown command, an unadvertised
  sensor), sends `set-delay:20` and `set:acceleration:1`, reads three batches (exact `acceleration:0.000000:9.810000:0.000000`
  lines and `sync:<digits>`), and checks that `set:acceleration:0` stops the stream. `tests/guest/ashmem.c` checks the ioctls
  (name, size, protection mask, pinning), `fstat` as a character device, a write shared with a forked child, EINVAL for a
  second size after mapping, and ENOTTY for a bogus ioctl and for `/dev/null`. The `power-supply` case lists every file under
  `/sys/class/power_supply` with its value.
- The first build of `tests/guest/ashmem.c` in the builder VM failed with a "missing terminating" error that clang does not
  reproduce, and a rebuild succeeded. Probably the builder read the file mid-write, but this is not confirmed.

### Engine choices (not from the HAL sources)

- The default batch period is 200 ms (`SENSOR_DELAY_NORMAL`) until `set-delay`. A batch is made lazily by a read or a poll
  when its time is due. Only one batch is sent for an overdue period (nothing is caught up), and no batch is made while the
  previous one is unread. No thread and no busy loop: the pipe's deadline goes to `sched_block_restart_until`, to ppoll and to
  epoll, on the host clock timerfd uses.
- Values: accelerometer (0, 9.81, 0), gyroscope (0, 0, 0), magnetic field (20, 0, -40) µT (a mid-latitude value, not measured),
  orientation (0, 0, 0), proximity 1.0 (far), light 300 lux.
- `sync:<us>` is the host monotonic time in microseconds, sent after each batch.
- `/proc/sys/kernel/random/boot_id` is a fixed UUID (the kernel's is random per boot). `ro.vndk.version` (33) comes from the image.
- `/dev/ashmem` rounds a size up to a page. Linux keeps the size as set, which matters only for a size that is not a page
  multiple. A second `ASHMEM_SET_SIZE` is refused once a size is set, where Linux allows it until the first map.
- An unlabelled file reads back `u:object_r:unlabeled:s0`.
- `XIAOJI_PS_TRACE=1` prints each power-supply file open (stderr), like `XIAOJI_GF_TRACE`.

### Gaps (B7)

- **Boot does not reach `System now ready`.** Blockers 1 and 2 above.
- **Persistent `/data`.** `build/rootfs-xiaoji/data` keeps what each boot writes, and installd's strict check then fails on the
  owners left by an earlier run (`Expected path ... with owner 10055:10055 but found 0:0`). `tests/run.sh` does not clear
  it. The runs in this document started from a cleared `data/`. When this step began the directory held 12 entries from earlier
  boots, and it was then cleared. Its state before my first boot was not checked.
- **Extended attributes** are keyed by path. They survive an unlink, and a rename does not move them.
- The sensors protocol is checked against the HAL sources and by the guest test. The HAL logs no parse error, but no test
  checks that SensorService receives events.
- The batch period is not measured in a test (the guest sees time after the engine's, so only the order and the format are
  checked). The schedule is in `sns_batch`.
- Telephony, NFC, Bluetooth and UWB are still on.
- `sys.use_memfd` stays unset, and `vold` is not modelled (the mkdir lines stand in for it).
- `mls` is `0` in the synthetic SELinux (`engine/selinuxfs.c`). Whether that affects installd is not tested.
- `stat` on `/proc/sys` and `/proc/sys/kernel` (directories) is ENOENT, as for `/sys/devices`.
- `make` with no target printed `Nothing to be done` and did not rebuild `build/xiaoji` in this tree; `make build/xiaoji`
  does. `make test` output is cut by the `rtk` filter, so the summaries here come from `tests/run.sh` run directly.

## B8: the file metadata layer, POSIX record locks and flock, utimensat, and the boot past SettingsProvider (stops at the lmkd and vibrator services)

Status: **B8 parts 1 and 2 done (uncommitted). SettingsProvider now opens its database and generates the Android ID on the
product tree (3 of 3 runs). `System now ready` and `sys.boot_completed=1` are not reached. The boot is blocked by ActivityManager's
lmkd connection (lmkd crash-loops in init) and by the missing vibrator service (VibratorManagerService waits for it). Both are
engine or product-configuration gaps, listed below as the next blocker.** The sections below are in the order of the B8 plan.

### Checkpoints

| # | Checkpoint | Result |
|---|---|---|
| 1 | Record locks, flock, and their guest test | Done. `engine/locks.c`, `engine/locks.h`, `tests/guest/flock.c`. |
| 2 | utimensat, futimens, and the B8 docs | Done. `sys_utimensat` (`engine/sysfs.c`), `host_utimens` (`engine/host.c`), `meta_utime_check` (`engine/meta.c`), `tests/guest/utimes.c`. |
| 3 | Boot progress past SettingsProvider | Partial. The lock gap, the ftruncate gap and the pwrite64 gap are fixed. Stopped at the lmkd and vibrator services (next blocker). |

### Part 1: the file metadata layer (`engine/meta.c`, `engine/meta.h`)

- **Design.** Each guest file's owner, group and mode (07777, with setuid, setgid and sticky) live in a record keyed by the
  host (dev, ino) of the file, so a hard link and a rename keep their record. A stale record is replaced when the same inode
  number is created again (`meta_new`). A file with no record reports root (0:0) and its host permission bits with the group
  and other write bits removed. Synthetic files, pipes, sockets and memfds have no record. Enforcement of mode bits on access is
  not part of this layer. The rules follow Linux 6.12 (`fs/attr.c`, `fs/inode.c`, `fs/namei.c`, `fs/open.c`).
- **Sidecar format.** With `--root`, the records of the writable trees (`/data`, `/cache`, `/metadata`) are kept in
  `<root>/.xiaoji-meta`. The first line is `xiaoji-meta 1`. Each other line is one record: the guest path, the uid, the gid
  and the mode in octal, separated by tabs. The file is loaded at startup (a record whose host file is gone is skipped, and a
  line that does not parse is ignored). It is written at exit, through an `atexit` handler, to `.xiaoji-meta.tmp` and then
  renamed over the file. The guest cannot see or name the two files (`meta_hidden_guest`).
- **`make reset-data`** runs `tools/reset-data.py` on `build/rootfs-xiaoji`. It removes every entry under `data/` and drops the
  sidecar lines for `/data` and below, so the next boot starts from no records there. The product boot check runs it first (and
  only the product tree is reset: see Part 4).
- **Enforcement.** The records answer `access(2)`/`faccessat` (`meta_access`: the caller's fsuid and groups, the owner, group and
  other bits, and the DAC override and read-search capabilities, as Linux's `generic_permission`), and `execve` (through the
  guest mode, as before). `open` and the other calls do not enforce mode bits, as before. This is a decision: the shell checks
  `access(X_OK)` before it runs a script, so `chmod` (which changes only the record) must show up there.
- **Known limits.**
  - The sidecar is saved only at a normal exit (a return from `main` or `exit`). A kill by a signal writes nothing, so the
    records of that run are lost.
  - The records are keyed by host inode, and the sidecar stores guest paths. A copy of the root that changes the inode numbers
    loads the records again from the sidecar by path, and a file that is replaced by a new inode with a reused number takes a
    new record.
  - Records are not kept for synthetic files, pipes, sockets and memfds.
  - **The browser build keeps no sidecar** (`XIAOJI_LAZY_FS`, set by the `wasm` target only). Its root is a lazy HTTP file system,
    and loading the sidecar stats every recorded file: the first stat of a lazy gzip file downloads it, and the Emscripten
    runtime prints a warning on stdout. That warning broke the `test-web` stdout check, so the browser build skips the load and
    the save, and its records last for the session. The native build and `wasm-node` (real host files) keep the sidecar.

### Part 2: POSIX record locks and flock (`engine/locks.c`, `engine/locks.h`)

- **Identity and owner.** A lock belongs to an inode: the host (dev, ino) of a regular file (the identity of `meta.c`), or the
  object of a memfd or a pipe. Other kinds give EINVAL for POSIX locks. The owner of a POSIX lock is the process's fd table
  (Linux's `files_struct`): threads share the locks, and a fork gets a new table, so the child has none. The owner of a flock
  lock is the open file description.
- **Split, merge and replace.** `posix_apply` ports Linux 6.12 `posix_lock_inode` step for step: the same-owner walk, the merge of
  adjacent or overlapping locks of one type, the split of a lock of another type (with the copy inserted before the right part,
  as Linux does), and the drop of an unlock. The list order is Linux's (a new lock goes before the first lock of its owner that
  lies after it), because F_GETLK reports the first conflicting lock in list order. The guest test checks the ranges that
  come out, through F_GETLK from another process.
- **Ranges and types.** `l_whence` (SEEK_SET, SEEK_CUR, SEEK_END), a negative `l_len` (the range runs backwards from `l_start`),
  and `l_len` 0 (to end of file). The read-only and write-only descriptor checks are EBADF, and the type and whence checks are
  EINVAL, in Linux's order (`fs/locks.c` `flock_to_posix_lock`, `check_fmode_for_setlk`, `fcntl_getlk`).
- **F_GETLK** reports the first conflicting lock in list order, with its type, its range, and the pid of its holder (`l_pid`,
  the pid of the process that took the lock). With no conflict only `l_type` changes (F_UNLCK), as on Linux.
- **F_SETLKW** blocks through the restart mechanism (`sched_block_restart_eintr`). A change on the inode (an unlock, a
  downgrade, a close, the owner's exit) wakes the waiters, and each rerun repeats the request from the start. A waiter's edge (the
  lock it waits for) is kept for the deadlock walk, and it is dropped at every rerun, when the thread is gone (found by its tid),
  and when that lock goes.
- **EINTR.** A signal that is pending and not blocked, and that the engine would deliver (a caught signal), ends a blocked
  F_SETLKW or flock with EINTR, as Linux does without SA_RESTART. The engine has no SA_RESTART, so every caught signal does this
  for these two calls. The pipes keep the restart. The new wait kind completes the call (pc past the `svc`, x0 = -EINTR) when a
  signal wakes it, so the handler runs before the call returns, and the call is not replayed (`sched_wake`, `wake`).
- **EDEADLK.** `posix_deadlock` is Linux's `posix_locks_deadlock` walk (at most 10 steps through the waiters' edges). It runs
  before a thread blocks.
- **Release.** A close of any descriptor of a file releases the POSIX locks that the process holds on that file (Linux's
  `filp_close`, through `vfs_close`, `vfs_dup` over a slot, and the close-on-exec close at exec). The process's exit releases all
  of them (`vfs_fdtab_release`, and an owner purge as a backstop). A flock lock goes with its last open file description
  (`desc_unref`).
- **Fork and exec.** A fork gives the child a new owner, so no POSIX lock is inherited. An exec keeps the table, so the locks stay
  (the close-on-exec slots are closed, as Linux does). The exec case is not in the test.
- **flock** (`SYS_FLOCK`, 32): LOCK_SH, LOCK_EX, LOCK_UN and LOCK_NB. A lock of the same description is replaced: the old lock goes
  before the conflict test, so a failed upgrade drops the old lock (Linux's `flock_lock_inode`). LOCK_MAND is ignored, as Linux
  does. It was a stub (always 0) before this step.
- **Not done.** OFD locks (not needed, as B8 found for SQLite and bionic). Leases. Locks on sockets and other synthetic kinds
  (EINVAL for POSIX). A woken waiter reruns its request from the start, so the order in which several eligible waiters get the
  lock is not Linux's wake order, and it is not tested.

### Part 3: utimensat and futimens (`sys_utimensat`, `host_utimens`)

- **Syscalls.** `utimensat` (88) with a path, `dirfd` and `AT_SYMLINK_NOFOLLOW` or `AT_EMPTY_PATH`. `futimens` is the same call
  with a NULL path (or an empty path with `AT_EMPTY_PATH`) and a descriptor.
- **Order of the checks** (`fs/utimes.c`): flags (EINVAL), then both times OMIT (a no-op, which does not look at the path), then the
  path (ENOENT), then the nanoseconds (EINVAL for a value outside 0 to 10^9 - 1 that is neither UTIME_NOW nor UTIME_OMIT), then
  a read-only mount (EROFS), then the permission. NULL times and both times NOW are the same ("touch").
- **Permission** (`meta_utime_check`, `fs/attr.c` setattr_prepare): the owner (the file's record uid is the caller's fsuid) or
  CAP_FOWNER may set any time. Anyone else may not set an explicit time (EPERM), and may touch the file only with write
  permission (EACCES): CAP_DAC_OVERRIDE passes, else the group or other class bit is checked.
- **Synthetic files, pipes and memfds** keep no times, so the call succeeds for them without effect.
- **Host.** `host_utimens` calls `utimensat` or `futimens` on the host file. The guest's UTIME_NOW and UTIME_OMIT are translated to
  the host's constants.
- **Test.** `tests/guest/utimes.c` covers explicit times, OMIT on one time, futimens, both OMIT with a missing path, EINVAL for
  nanoseconds 10^9 and -1, ENOENT, UTIME_NOW and the NULL form (both checked against the clock), and AT_EMPTY_PATH. The times
  are compared in whole seconds: the Node build (Emscripten NODERAWFS) keeps no nanoseconds, so the nanosecond fields are not
  printed (the EINVAL checks still cover the nanosecond argument). The native run is not root, so only the owner case is compared
  with the native run. The non-owner rules (EPERM, EACCES) are not tested.

### Part 4: boot-path engine gaps fixed in this step

- **ftruncate on host files** (`sys_ftruncate`, `host_ftruncate`). It handled memfds only, so SQLite's truncate of its rollback
  journal (`settings.db-journal`) failed with EINVAL (SQLITE_IOERR_TRUNCATE). A host regular file opened for writing is truncated now.
- **pwrite64** (68), which was unimplemented. `vfs_pwrite` follows `vfs_write`'s rules per kind, takes the offset, and leaves the
  description's offset alone (pipes and sockets are ESPIPE). A host file with O_APPEND appends, as Linux does. SQLite's page
  writes use it. The engine printed `unimplemented syscall 68` right before the first SQLITE_IOERR_WRITE.
- **Found by the suite's order** (B8 part 1's boot check). The Google-image boot check (`XIAOJI_BOOT=1`) ran `reset-data` on
  `build/rootfs`, which removed everything under its `data/`: `/data/local/tmp` (used by `sh-script`) and
  `build/rootfs/data/hello.dex` (used by `hello-int` and `hello-jit`). Two fixes. The reset now runs only for the product tree
  (`*-xiaoji`); the Google tree keeps its `/data`. And `sh-script` runs `mkdir -p /data/local/tmp` first (`tests/run.sh` and
  `tools/oracle.sh`; the expected output is unchanged).
- **Found by the suite** (the `sh-script` case, after the boot reset above). After `chmod 755`, the shell's `access(X_OK)` read the
  host file's mode (644) through `faccessat`, so the script was refused (`Permission denied`). `faccessat` now answers from the
  guest record (`meta_access`, Part 1).
- **Found by `make test-web`** (after the steps above). The browser build's sidecar load, from part 1, made the first ART case
  print the lazy-file warning on stdout. The fix is the browser-only exclusion in Part 1.
- **Stale notes.** B1 and B7 say `fchmodat` (53) is missing. `sys_fchmodat` exists in `engine/sysfs.c`, so those notes are out of date.

### Part 5: milestones (product tree, `build/rootfs-xiaoji`, `make reset-data` first, `--logcat --init image/xiaoji.rc --init-timeout 150`)

Times and instruction counts are guest time (`[ms insn]` in the log). Power: `pmset -g` showed `lowpowermode 1` and
`pmset -g batt` showed `Now drawing from 'Battery Power'` for these runs (B7 ran on AC with Low Power Mode off, so the times here are
not comparable to B7's). Scratchpad logs: `boot-r1.err`, `boot-r2.err` (the same command, two runs).

| Milestone | run 1 | run 2 |
|---|---|---|
| Sensors HAL: `host sensors mask=6f` | 0.99 s, 0.096 G | 0.98 s, 0.096 G |
| `Registered android.hardware.sensors@2.1::ISensors/default` | 3.32 s, 0.222 G | 4.72 s, 0.222 G |
| `Registered android.hardware.health@2.1::IHealth/default` | 3.35 s, 0.223 G | 4.76 s, 0.224 G |
| `Entered the Android system server!` | 71.6 s, 4.369 G | 73.5 s, 4.372 G |
| `Starting phase 100` | 77.2 s, 4.696 G | 86.6 s, 4.725 G |
| `BatteryService: health: Waited 0ms and received the update` | 106.2 s, 6.489 G | 127.7 s, 6.709 G |
| `SettingsProvider: Generated and saved new ANDROID_ID` (the furthest milestone, in the product check) | 109.3 s, 6.674 G | 131.9 s, 6.949 G |
| `Failed to connect to lmkd, retry after 1000 ms` (ActivityManager, first) | 97.1 s, 5.921 G | 118.3 s, 6.152 G |

The order of the milestones is the same in both runs and in a third run (the one with a 300 s timeout, below). The times and
instruction counts are not: phase 100 is 4.70 G and 4.72 G, and BatteryService is 6.49 G and 6.71 G. This boot is not
instruction-deterministic, so the product check tests presence and order, not counts. The product check (`tests/run.sh`, with
`XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1`) now requires the ANDROID_ID line, and runs the engine with `--init-timeout 150`.

SettingsProvider's SQLite path before this step: `SQLITE_IOERR_LOCK` (fixed by the record locks), then `SQLITE_IOERR_TRUNCATE`
(ftruncate, fixed), then `SQLITE_IOERR_WRITE` (pwrite64, fixed). The database opens, and the "Generated and saved new ANDROID_ID" line
follows.

### Next blocker: lmkd and the vibrator service (the framework does not finish its boot)

After SettingsProvider, ActivityManager cannot reach lmkd, and it retries every second. lmkd crash-loops in init (its cgroup and
psi files are missing), so `/dev/socket/lmkd` is never served. VibratorManagerService then waits for a vibrator service that init
cannot start. Logcat excerpt from run 2 (times are guest time):

```
xiaoji: init: started lmkd (pid 1011)
[83 ms 52213578 insn] E/lmkd(1011): open() failed for /dev/cgroup_info/cgroup.rc: No such file or directory
[118273 ms 6151906153 insn] E/ActivityManager(1047): Connection failed: java.io.IOException: Connection refused
[118273 ms 6151911960 insn] W/ActivityManager(1047): Failed to connect to lowmemorykiller, retry later
[118273 ms 6151917780 insn] I/ActivityManager(1047): Failed to connect to lmkd, retry after 1000 ms
[132633 ms 6993237372 insn] I/servicemanager(1001): Found android.hardware.vibrator.IVibratorManager/default in device VINTF manifest.
[132636 ms 6993471315 insn] I/servicemanager(1001): Since 'android.hardware.vibrator.IVibratorManager/default' could not be found, trying to start it as a lazy AIDL service
xiaoji: init: Control message: Could not find 'aidl/android.hardware.vibrator.IVibratorManager/default' for ctl.interface_start
```

Counts over run 2: 31 lines of `Connection refused`, 17 lines of the vibrator's `Could not find`. The logs show that psi and cgroup v1 are missing, so lmkd cannot run as the stock service. The design choices for the next step (not made here):
a lmkd replacement or a socket endpoint on `/dev/socket/lmkd` (the engine has no kernel psi or cgroup v1, so lmkd cannot run as
the stock service), and a vibrator HAL in the product (the manifest lists it, and no init service provides it). The engine also
prints `unimplemented syscall` for 126 (sched_get_priority_min), 282 (userfaultfd) and 26 (inotify_init1).

In a longer run (300 s, `boot3`), a second system_server (pid 1194) appears at 249 s and re-runs phase 100, so pid 1047 ended
before that. The log has no crash, tombstone or watchdog line for 1047, and no line that shows why it ended. This is open.

### Test results (final, this step)

- `make && make wasm-node && make wasm && tests/run.sh`: **214 passed, 0 failed, 0 xfail** (206 before this step, plus the flock and
  utimes cases in the engine and in Node, and the 2 binder-driver cases that were restored, see below).
- `XIAOJI_JIT=0 tests/run.sh`: **214 passed, 0 failed, 0 xfail.**
- `XIAOJI_BOOT=1 tests/run.sh` (Google image): **215 passed, 0 failed, 0 xfail.**
- `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh` (product, cleared `/data`): **181 passed, 0 failed, 0 xfail**,
  including the new ANDROID_ID check.
- `make test-web`: all checks passed (`hello-int` and `hello-jit`, stdout and exit code).
- Warnings: `clang -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter` and `emcc` with the same flags on `engine/{locks,sysfs,vfs,
  signals,syscall,sched,host,meta}.c`: 0 each. `meta.c` with `-DXIAOJI_LAZY_FS=1` (clang and emcc): 0. `gcc -Wall -Wextra` on
  `tests/guest/{flock,utimes}.c` in the builder VM: 0.
- The binder conformance binaries (`build/rootfs/data/nativetest64`) were removed by an earlier boot's reset. They are copied back
  read-only from the remote build output (`~/xiaoji-aosp/out/target/product/xiaoji_arm64/data/nativetest64`, 4 files, 1.1 MB).

### Tests

- New: `tests/guest/flock.c` (`flock-O0`, `flock-O2`) and `tests/guest/utimes.c` (`utimes-O0`, `utimes-O2`). Each is compared
  with the native builder-VM output (stdout and exit code) in the engine and in Node. The flock case covers: conflicts between
  processes, F_GETLK's first conflict, its range and its holder's pid, the split and merge of ranges (through F_GETLK from another
  process), EAGAIN, EBADF, EINVAL, and negative and to-EOF lengths; the release by a close of another descriptor of the file; F_SETLKW
  blocking (the pipe shows the child still blocked) and waking on an unlock; the release at a child's exit; EDEADLK with two
  processes; EINTR from SIGUSR1 with a handler and no SA_RESTART; and flock (exclusive, shared, LOCK_NB, a downgrade, an upgrade that
  fails and drops the old lock, the wake on an unlock, and the release at the last close of a description, with a dup).
- The native run is deterministic (O0 and O2 give the same bytes).

### Gaps (B8)

- **alarm and setitimer are not implemented** (`alarm` is `setitimer(ITIMER_REAL)` on arm64). The plan asked for the EINTR test
  to use an alarm. The test uses kill(2) from the parent instead. Building the timer (a per-process deadline that raises SIGALRM,
  through the scheduler's deadline hook) is a separate decision. The EINTR path it would test is the one built here.
- The non-owner rules of utimensat (EPERM, EACCES) are not tested: the native run is not root, and the engine's test runs as root.
- Only the owner case of utimensat is compared natively, and only in whole seconds. On Node the nanoseconds of a time are
  dropped (the set and the stat both), so the engine (which keeps them) is not checked for a sub-second value on Node.
- `wait4`, `waitid` and `/proc/<pid>/maps` are unchanged from B1.
- **xattr labels are not folded into the metadata record.** The labels are keyed by guest path (they survive an unlink, and a
  rename does not move them), while the metadata records are keyed by host inode. A fold would change the key and the sidecar
  format (labels are variable-length and may hold spaces), so it is left as it is. The B7 xattr store is unchanged.
- Locks on sockets and synthetic files give EINVAL.
- The order in which several waiters that are all eligible get the lock is not tested against Linux's wake order.
- `pwritev` (70) and `preadv2` are not implemented (SQLite on this path did not need them).
- The sidecar is saved only at a normal exit (Part 1).
- Telephony, NFC, Bluetooth and UWB are still on. None of them has blocked the boot so far.
- `System now ready` and `sys.boot_completed=1` are not reached in any run (checked in the three product runs).
- The product boot reads the record sidecar (`build/rootfs-xiaoji/.xiaoji-meta`) at start, and `reset-data` is the only thing
  that clears it for `/data`. The Google tree keeps its sidecar between runs (see Part 4).

## B9: lmkd on PSI, the vibrator HAL, POSIX and interval timers, the death of system_server, and the boot past the lmkd and vibrator services (stops at vold)

Status: **the boot passes ActivityManager's lmkd connection and VibratorManagerService's start (3 of 3 runs of the product
tree from a cleared `data/`). system_server then dies in StorageManagerService (the `vold` service is not in the engine,
decision below). `System now ready` and `sys.boot_completed=1` are not reached.** The death of pid 1047 in B8's 300 s run is
explained (the Watchdog, below), and the fix is in place. The timers are done and tested.

### Checkpoints

| # | Checkpoint | Result |
|---|---|---|
| 0 | The death of pid 1047, captured before the fixes | Done (a 300 s run with `XIAOJI_DEBUG_FAULTS=1`). The cause is below. |
| 1 | ITIMER_REAL, POSIX timers, and their guest test | Done. `engine/timers.c`, `engine/timers.h`, `tests/guest/timers.c`. |
| 2 | lmkd running, and the ActivityManager connection | Done. `image/xiaoji.rc`, `engine/procfs.c`, `engine/sock.c`, `engine/init.c`. |
| 3 | The vibrator HAL, and the death cause | Done. The vibrator block in `image/xiaoji.rc`. The watchdog no longer fires. |
| 4 | Boot progress | Partial: past lmkd and the vibrator service. Stopped at `vold` (next blocker). |

### Debug output (`XIAOJI_DEBUG_FAULTS=1`)

The existing flag now also logs, on stderr: every guest process end (`xiaoji: exit: pid N (name) status S` or
`killed by signal S`, with the name of its last thread), every fatal signal with its pid, name and pc
(`xiaoji: fatal signal S in pid N (name) at pc ...`, `sched.c sched_exit_signal`), every kill(2) between guest processes
(`xiaoji: kill: pid A sends signal S to pid B`, `signals.c sig_kill`), and each write to `oom_score_adj`. Init's restart
decisions are already logged (`init: X exited (status S, by a signal)`, `crash-looping`).

### Timers (`engine/timers.c`)

- **ITIMER_REAL** (`setitimer`, `getitimer`): per process, with SIGALRM (SI_KERNEL). A zero value disarms, a zero interval
  makes a one-shot, and a usec outside [0, 10^6) is EINVAL. `alarm(3)` is `setitimer` on arm64, so it needs no syscall.
  ITIMER_VIRTUAL and ITIMER_PROF give ENOSYS (not built).
- **POSIX timers** (`timer_create`, `timer_settime`, `timer_gettime`, `timer_getoverrun`, `timer_delete`): clocks
  REALTIME, MONOTONIC and BOOTTIME. SIGEV_SIGNAL, SIGEV_NONE and SIGEV_THREAD_ID (the kernel's, to one thread of the process,
  by tid). A NULL sigevent is SIGEV_SIGNAL with SIGALRM and the timer id as the value. TIMER_ABSTIME. The id is the table slot.
  Timers are per process, so a fork child has none. An exec drops the POSIX timers and keeps ITIMER_REAL (the man page's
  rule, not checked against the oracle). A process's end drops them all.
- **Clock and wake-up.** The engine clock is the host monotonic clock (realtime for CLOCK_REALTIME). `sched.c` runs
  `timers_expire` on every pass, and a due timer raises its signal through `signals.c` (`sig_timer_raise`). A blocked thread
  wakes with EINTR, as it does for any signal. The scheduler's waits (`pending_deadlines`, `wait_for_timers`, `idle_wait`)
  include the earliest armed timer, so nothing spins.
- **Overrun.** If the target still has the signal pending, the expirations are counted and no second signal is raised. Otherwise
  the count is the expirations this firing skipped. Linux keeps the count for the last delivered signal; the engine's is
  set at the firing, so it can differ when a signal is delivered late.
- **The test** (`tests/guest/timers.c`, cases `timers-O0` and `timers-O2`, compared with the builder-VM oracle). It covers a
  one-shot ITIMER_REAL cutting a 2 s nanosleep short (EINTR), the si_code of that signal, getitimer after the one-shot,
  the remaining time of a 2 s timer and its disarm (with the old value), an interval (at least three alarms), a POSIX timer
  to SIGUSR1 (si_code SI_TIMER and si_value 42), its gettime and getoverrun, timer_delete and the EINVAL after it, a
  SIGEV_THREAD_ID timer to this thread (periodic, value 7), an absolute time in the past (1 ns on CLOCK_MONOTONIC, which is not
  zero), the EINVAL cases (clock 99, signal 0, a tid outside the process, a bad id, bad flags, a usec of 10^6, a nsec of 10^9),
  and a fork child whose getitimer is zero. Output holds comparisons and small numbers only, never a time or a timer id.
- **Differences from the kernel found by the oracle** (the test avoids them, and they are gaps): the kernel accepts
  clock 2 (CLOCK_PROCESS_CPUTIME_ID), and the engine returns EINVAL. The kernel accepts sigev_notify 2 (SIGEV_THREAD) in
  timer_create, and the engine returns EINVAL. glibc and bionic convert SIGEV_THREAD in userspace and pass SIGEV_THREAD_ID to
  the kernel, so no library call reaches this case. The siginfo written for a timer leaves si_overrun at 0.
- The Node build ran the first version of the test with a negative absolute time (`now - 1 s`), because the Node host clock
  starts near zero. The test now uses 1 ns, and the value is the same on every host.

### lmkd: the decision and its evidence

Sources: the remote tree `~/xiaoji-aosp/system/memory/lmkd/` (read only). Functions are named, not lines.

- **Why it crashed.** `init_monitors` picks PSI, and falls back to vmpressure. The engine had no `/proc/pressure`, so
  `init_psi_monitors` failed. Its fallback, `init_mp_common`, returns false unless `memcg_version()` is kV1 (checked in
  `init_mp_common` before any file is opened). `memcg_version` reads cgroup v1 through `CgroupGetControllerPath`, which reads
  `/dev/cgroup_info/cgroup.rc`. Neither exists, so `init` fails, `main` exits with status 0, and the `critical` service is
  restarted until init gives up (the B8 crash loop).
- **Why PSI is the cheap path.** In `init_psi_monitors` the cgroup check runs only when `use_new_strategy` is false. The
  default is `low_ram_device || !use_minfree_levels`, and `use_minfree_levels` defaults to false, so the default path reads no
  cgroup. The vmpressure fallback is not built; it would need a v1 memcg hierarchy the engine does not have.
- **The PSI files** (`engine/procfs.c`, `pressure_file`, `procfs_write`, `psi_trigger_ok`). `/proc/pressure/{memory,cpu,io}` are
  generated text with a `some` and a `full` line, all counters zero, since the engine never reports a stall. lmkd opens
  `/proc/pressure/memory` O_WRONLY and writes its trigger (`some 70000 1000000` and `full ...`, with the NUL, from
  `libpsi/psi.cpp`). `procfs_write` accepts a well-formed trigger (`some|full`, a window of 0.5 to 10 s, a stall in (0, window])
  and never fires it. `epoll_ctl(EPOLLPRI)` on it succeeds, and poll reports a synthetic file as IN/OUT only, so a fd
  registered for EPOLLPRI is never ready (`poll_revents` masks by the registered events). lmkd sleeps in epoll as it should.
  Not built: a trigger that fires on real engine pressure (no engine pressure source exists).
- **What lmkd reads, and what the engine does not serve.** `/proc/meminfo` (a stub since B5), `/proc/zoneinfo` and
  `/proc/vmstat` are read in the PSI event handlers and in the watermark refresh (`mp_event_psi` path,
  `find_and_kill_process`). Those never run when the trigger never fires, so the engine serves neither file. The one
  exception is init's read of zoneinfo for a buffer size, which logs `E/lowmemorykiller: Failed to read /proc/zoneinfo` once.
  It is harmless. `pidfd_open` (434) is not built, so lmkd logs `Process polling is not supported` and uses the pid path.
  `mlockall` (230), `process_mrelease` (448) and `pthread_setname_np` are not built, and each logs a warning. They are not
  needed for the protocol.
- **The socket needs passcred.** `ctrl_data_read` closes a connection whose message has no SCM_CREDENTIALS. The rc block is
  `socket lmkd seqpacket+passcred 0660 system system`. `init.c` (`parse`, `make_socket`) sets SO_PASSCRED on the listener
  (`sock_set_passcred`), and `sock.c` (the connect path that creates the accepted socket) copies it to that socket, as Linux does in unix_stream_connect. The
  engine already attaches the credentials on receive (`sockcall.c`, `sock_passcred`), so only these two pieces were missing.
- **The per-app memcg branch.** `vendor/build.prop` sets `ro.config.low_ram=true`, which makes `per_app_memcg` default to true.
  `cmd_procprio` then reads a v1 soft-limit path, logs `Querying MemSoftLimit path failed`, and returns before it records the
  process. The engine has no memcg, so the image sets `ro.config.per_app_memcg false` in the early-init block. `low_ram` is
  not changed.
- **The procprio path.** `cmd_procprio` reads `/proc/<pid>/status` (Tgid, present), writes `/proc/<pid>/oom_score_adj`
  (new: `procfs_write`, per process, `proc_t.oom_score_adj`, copied by a fork), and then records the process. The evidence is the
  debug line `xiaoji: oom_score_adj of pid 1045 is -900`, after `ActivityManager: Connection with lmkd established`.
- **lmkd stays up.** In the 200 s runs `lowmemorykiller(1011)` logs the connection and no exit (`init: lmkd exited` does
  not appear).

### The death of pid 1047 (B8's 300 s run), and the fix

The log of the 300 s run (scratchpad `run300.err`, with the B9 diagnostics on) shows the cause:

1. system_server's main thread is in `VibratorManagerService.<init>`, in `nativeGetCapabilities` (the binder wait for
   `IVibratorManager/default`, which no service provides; the log has `Waited one second for ...` repeated).
2. The Watchdog reports `Blocked in handler on main thread (main)` and `WATCHDOG KILLING SYSTEM PROCESS`, then sends SIGKILL
   to itself: `Sending signal. PID: 1047 SIG: 9`, `xiaoji: kill: pid 1047 sends signal 9 to pid 1047`.
3. Zygote (pid 1003): `Process 1047 exited due to signal 9`, `Exit zygote because system server (pid 1047) has terminated`.
   Init restarts zygote, which forks the second system_server (pid 1194) at about 249 s. That is the second process in B8.

So the "unknown reason" was the Watchdog, and the root cause is the missing vibrator service. The fix is the vibrator block
in `image/xiaoji.rc` (below). In the runs after it, the Watchdog does not fire and VibratorManagerService's onStart returns.

### The vibrator HAL

- `android.hardware.vibrator-service.example` is the image's binary. Its block is the one in
  `hardware/interfaces/vibrator/aidl/default/vibrator-default.rc` (class `hal`, user and group `nobody`), added as
  `vendor.vibrator-default` in `image/xiaoji.rc`. The service registers `IVibrator/default` and `IVibratorManager/default`, the two
  names in `android.hardware.vibrator.xml` (the manifest servicemanager reads). The stock rc has no socket and no interface line.
- **No sysfs.** The example (`Vibrator.cpp`, `VibratorManager.cpp`) reads no sysfs file (checked: no `/sys` or `fopen`), so
  the `/sys/class/leds/vibrator` line in the plan is not needed.

### Milestones (product tree, `build/rootfs-xiaoji`, `make reset-data` first, `--logcat --init image/xiaoji.rc`)

Times and instruction counts are guest time (`[ms insn]`, with `XIAOJI_STAMP=1`). Power: `pmset -g` shows `lowpowermode 0`, and
`pmset -g batt` shows `Now drawing from 'AC Power'` (scratchpad: `pmset-b9.txt`, `pmset-batt-b9.txt`).

| Milestone | run `t200` (200 s) | run `t200b` (200 s) |
|---|---|---|
| `Found android.hardware.vibrator.IVibratorManager/default` (servicemanager) | 1.9 s | 1.9 s |
| `Entered the Android system server!` | 43.4 s, 4.37 G | 43.4 s, 4.37 G |
| `Starting phase 100` | 45.1 s | 47.0 s, 4.69 G |
| `ActivityManager: Connection with lmkd established` | 56.4 s, 5.88 G | 59.4 s, 5.89 G |
| `SettingsProvider: Generated and saved new ANDROID_ID` | 62.1 s | 65.3 s, 6.48 G |
| `StartVibratorManagerService took to complete` (VibratorManagerService `onStart`) | 63.0 s, 0.143 s | 66.3 s, 6.57 G (250 ms) |
| `FATAL EXCEPTION IN SYSTEM PROCESS: StorageManagerService` (next blocker) | not reached in this run | 79.6 s, 7.92 G |

The order is the same in every run (lmkd connection, then ANDROID_ID, then the vibrator service). The times vary by about 3 s,
so the product check (`tests/run.sh`) tests presence and order, not counts. Two more 150 s runs (not in the table) show the same
order.

### Next blocker: vold (the framework does not finish its boot)

After the vibrator service, `StorageManagerService`'s idle maintenance calls `fstrim`, which calls `IVold.needsCheckpoint()` on
a null object (`vold` is not registered; the engine has no vold). The handler thread dies, and system_server aborts:

```
[79573 ms 7912121713 insn] I/Watchdog(1045): ...
[79601 ms 7915080270 insn] I/StorageManagerService(1045): Running fstrim idle maintenance
[79609 ms 7915902107 insn] E/AndroidRuntime(1045): *** FATAL EXCEPTION IN SYSTEM PROCESS: StorageManagerService
java.lang.NullPointerException: Attempt to invoke interface method 'boolean android.os.IVold.needsCheckpoint()' on a null object reference
	at com.android.server.StorageManagerService.needsCheckpoint(StorageManagerService.java:3175)
	at com.android.server.StorageManagerService.fstrim(StorageManagerService.java:2589)
[79646 ms 7919272731 insn] I/Process(1045): Sending signal. PID: 1045 SIG: 9
xiaoji: kill: pid 1045 sends signal 9 to pid 1045
```

**Decision for the user (not made here).** The real vold needs device-mapper, loop devices, fs-verity keys and fscrypt, and the
engine has none of them. Two options: (a) a minimal `vold` binder service in the engine (answers `needsCheckpoint` false and the
other calls the boot makes, and fails the rest), which keeps the framework's own code paths; or (b) keep vold absent and turn
off idle fstrim by a configuration property, which avoids the null call but leaves the framework without its storage daemon.
Recommendation: (a), with the calls logged, so the next boot shows which ones the framework makes.

### inotify and the alarm clocks (found on the way)

- **Alarm clocks.** AlarmManagerService's JNI aborts unless `timerfd_create` takes CLOCK_REALTIME, CLOCK_BOOTTIME_ALARM and
  CLOCK_REALTIME_ALARM (`com_android_server_alarm_AlarmManagerService.cpp`, `log_timerfd_create_error`). The engine now accepts
  0, 1, 7, 8 and 9. The ALARM clocks need CAP_WAKE_ALARM (EPERM without it, as Linux), and run on the engine's realtime and
  boottime clocks (the engine does not suspend). An absolute deadline on a realtime clock is converted to the engine's
  monotonic clock (`fdobj.c`, `tfd_abs_deadline`).
- **TFD_TIMER_CANCEL_ON_SET.** The RTC-change timer passes it with ABSTIME on CLOCK_REALTIME (Linux's rule, checked in
  `sys_timerfd_settime`). The engine never sets the realtime clock, so the cancel is never due.
- **inotify** (`engine/inotify.c`, `engine/inotify.h`, syscalls 26, 27 and 28, the `VFS_INOTIFY` kind). EventHub aborts with
  `Could not register INotify for /dev` unless `inotify_add_watch` succeeds. The instance records its watches (the path must
  exist: ENOENT; the mask must name an event: EINVAL) and never queues an event (a read gives EAGAIN, and poll never reports it
  readable). **Gap:** a consumer that waits on inotify (FileObserver's native thread) does not wait; it logs
  `android_os_fileobserver_observe() got a short event` (seen twice in the run). No file change is ever reported.

### Tests

- `tests/run.sh`: the product check (`XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1`) now also requires ActivityManager's
  lmkd connection, VibratorManagerService's `StartVibratorManagerService took to complete`, and no `Could not find
  'aidl/android.hardware.vibrator` line. Its furthest milestone is the vibrator start (about 63 to 67 s, under the 150 s timeout).
- New guest test: `tests/guest/timers.c` (`timers-O0`, `timers-O2`), compared with the builder-VM oracle, engine and Node.
- Results: see the final section below.

### Gaps (B9)

- **The boot stops at vold** (above). `System now ready` and `sys.boot_completed=1` are not reached.
- **inotify never reports an event**, and its blocking read does not wait (above).
- **lmkd never reports pressure.** Its kill path does not run (no engine pressure source). The `cgroup.rc` error line from
  libprocessgroup is logged once and does not stop lmkd. Where it is called from was not traced.
- **Timers:** CPU-time clocks, SIGEV_THREAD and ITIMER_VIRTUAL/PROF are not built (the kernel accepts the first two). Overrun is
  approximate (above). The exec rule for POSIX timers is the man page's, not checked.
- **Passcred on accepted sockets** is copied at connect, as Linux does for unix stream sockets. Only lmkd uses it so far.
- **The second system_server** in a 300 s run comes from zygote being restarted after a system_server death (B8's explanation).
  It is not a separate fault.

### Test results (B9)

- `make && make wasm-node && make wasm && tests/run.sh`: **218 passed, 0 failed, 0 xfail** (214 before B9, plus `timers-O0` and
  `timers-O2` in the engine and in Node).
- `XIAOJI_JIT=0 tests/run.sh`: **218 passed, 0 failed, 0 xfail.**
- `XIAOJI_BOOT=1 tests/run.sh` (Google image): **219 passed, 0 failed, 0 xfail.**
- `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh` (product, cleared `/data`): **185 passed, 0 failed, 0 xfail**
  (181 before, plus the lmkd and vibrator checks).
- `make test-web`: all web checks passed.
- Warnings: `clang -O2 -std=c11 -Wall -Wextra` and `emcc` with the same flags on the 14 changed engine files
  (`timers, sched, signals, syscall, exec, procfs, sock, init, fdobj, sysfs, vfs, poll, inotify, proc`): 0 each.
- The first Node run of `timers` failed on the negative absolute time in the test (fixed, above). The first build of
  `inotify.c` failed under `-std=c11` on `strdup` (replaced with `malloc` and `memcpy`).

## B10: the vold decision (the real vold), the netlink stub, and the boot past StorageManagerService (stops at netd)

Status: **partial.** The real `/system/bin/vold` runs in the product boot. Its binder service is up, and
StorageManagerService connects to it (`mDaemonConnected=true`). The fstrim NullPointerException of B9 is gone. The main
thread of system_server now blocks in `NetworkManagementService.create`, which waits for netd. netd exits (status 1) because
the engine has no cgroup v2 root and no NETLINK_ROUTE. `System now ready` and `sys.boot_completed=1` are not reached.
Verification (the full B10 suite) has not been run; see "Not done".

### Checkpoints

| # | Checkpoint | Result |
|---|---|---|
| 1 | vold assessment and decision | Done (below). Decision: the real vold. |
| 2 | vold running, IVold calls answered | Done for the boot path: the real vold answers them. The engine's stub is the netlink socket. |
| 3 | Boot progress | Partial: stops at netd (next blocker). |

### The vold assessment (evidence)

Sources: remote `~/xiaoji-aosp/system/vold/` (read only) and the probe runs (scratchpad `vold-probe.err`, `vold-probe2.err`).
The image already contains the real `system/bin/vold` (927 KB), so nothing new was built for it.

- **Startup (`main.cpp`).** `process_config` reads the default fstab. `GetFstabPath` looks for `fstab.<suffix>` where the
  suffix is `ro.boot.fstab_suffix`, `ro.boot.hardware` or `ro.boot.hardware.platform` (the engine sets `ro.hardware=xiaoji`).
  No `fstab.xiaoji` exists in the image, so the probe logs `failed to find device default fstab` and `continuing anyways`.
  **So device-mapper is not used at startup.** The logical-partition check (`PLOG(FATAL)`) and `defaultkey_precreate_dm_device`
  (device-mapper, but non-fatal) are both behind the fstab, and never run. Consequence: the image's `/data` fileencryption
  and keydirectory entries are not read by vold in this product, as long as the fstab is absent.
- **`VolumeManager::start`.** `unmountAll`, `Loop::destroyAll`, and create an `EmulatedVolume("/data/media")`. No device-mapper,
  no FUSE. It ran in the probe.
- **`VoldNativeService::start`.** Binder publish, ran in the probe (`completed OK`).
- **`NetlinkManager::start`.** `socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT)`, then SO_RCVBUFFORCE (falls back to
  SO_RCVBUF), SO_PASSCRED, bind. **The only fatal startup gap.** The engine had no AF_NETLINK
  (`Unable to create uevent socket: Address family not supported by protocol`, exit 1, in the unmodified probe).
- **Emulated storage.** `EmulatedVolume::doMount` opens `/dev/fuse` through `MountUserFuse` (and runs sdcardfs only when
  `mUseSdcardFs`). The engine has no `/dev/fuse`. The framework's `mount(vol)` catches the exception and logs `Slog.wtf`.
  This is not needed to start vold. Whether the boot can pass without a mounted `/storage/emulated` is not decided here; the
  boot of this step got past the framework's mount path without a failure that stops it.
- **Checkpoint.** `cp_needsCheckpoint` calls `IBootControl::getService()`. The image has no `android.hardware.boot` HAL. No
  hang was seen in the probe runs.
- **Repeated vold starts.** The probe's first rc block was broken (the rc reader does not join `\` lines; the stock block was
  put on one line). That made vold abort on `sBlkidContext`. Fixed in the product rc.

**Decision: the real vold, with the engine's netlink stub.** The fallback (a minimal `xiaoji_vold` binder service) was not
built. The evidence says the real vold needs only the netlink stub to start, and its other gaps (FUSE, fscrypt keys) do not
stop the boot path reached so far.

### The netlink stub (`engine/sock.c`, `engine/sock.h`, `engine/sockcall.c`)

- `socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT)` (`sc_socket_netlink`) returns an unbound datagram socket with
  `sock_set_netlink(s, 1)`. Other netlink families and protocols return EPROTONOSUPPORT. Because the engine has no kernel,
  the socket never receives a message.
- `bind` (`sc_bind`, netlink branch) accepts a 12-byte `sockaddr_nl` with family 16 and state 2. Any group or pid is accepted.
  Gap: `getsockname` on a netlink socket is not AF_NETLINK (it reports the unbound AF_UNIX state).
- The netlink state is the new field `struct sock.netlink` (0, 1, 2), with `sock_netlink` and `sock_set_netlink`.
- Compile check: `clang -O2 -std=c11 -Wall -Wextra -c engine/sock.c` and `engine/sockcall.c`: 0 warnings. `make` rebuilt `build/xiaoji`.
- No guest test was written for it yet (see "Not done").

### The product rc (`image/xiaoji.rc`)

- `service vold` (the stock `system/vold/vold.rc` block, class, ioprio and group; the command is on one line, since the rc reader
  does not join `\` lines).
- `service netd` (the stock `system/netd/server/netd.rc` block: class, capabilities, group, the three sockets, onrestart).
- The storaged block is not added. Its absence logs `storaged not found; trying again` once a second on a background
  thread, and it does not block the main thread.

### Milestones (product tree, `build/rootfs-xiaoji`, `make reset-data` first)

Run: `XIAOJI_STAMP=1 XIAOJI_DEBUG_FAULTS=1 ./build/xiaoji --logcat --init <rc> --root build/rootfs-xiaoji --init-timeout 150`,
with the probe rc (`image/xiaoji.rc` plus the stock vold block, scratchpad `vold-probe.rc`), and the same run with the
product rc. Times are guest time `[ms insn]`. Power: `pmset -g` shows `lowpowermode 0` and `Now drawing from 'AC Power'`.

| Milestone | time | instructions |
|---|---|---|
| vold started, VoldNativeService start OK | 2.0 s | 0.24 G |
| `Connection with lmkd established` | 56.6 s | 5.82 G |
| `StartVibratorManagerService` | 62.95 s | 6.49 G |
| StorageManagerService `Thinking about init, mDaemonConnected=true` | 64.98 s | 6.69 G |
| main thread blocked in `NetworkManagementService.create` (netd missing) | 64.9 s to 145 s | to 17.1 G |
| Watchdog: `WATCHDOG KILLING SYSTEM PROCESS: Blocked in handler on main thread` | 145.1 s | 17.15 G |

### Next blocker: netd (the framework's main thread waits for it)

`NetworkManagementService.create` calls `NetdService.get`, which sleeps until netd answers. The stock netd exits:

```
[2072 ms 239529120 insn] E/netd(1017): open() failed for /dev/cgroup_info/cgroup.rc: No such file or directory
[2085 ms 240772666 insn] E/Netd(1017): Failed to find cgroup v2 root No such file or directory
xiaoji: exit: pid 1017 (netd) status 1
xiaoji: init: netd exited (status 1)
```

The same cgroup message appears for lmkd, surfaceflinger and netd. netd also needs NETLINK_ROUTE (interface and route
control) and the netfilter tools. **Design decision for the user (not made here):** (a) give netd a real cgroup v2 root and
a NETLINK_ROUTE socket with the link and address dumps it reads (a sizeable engine change); or (b) remove netd from the
product's main-thread path, which needs a framework change or an overlay, and that is a product decision. Recommendation:
(a) in steps, starting with the cgroup v2 root (the same file that lmkd and surfaceflinger need), then the netlink route
dumps, logged per request, so the next netd run shows what it asks for.

### Not done (B10)

- The full verification suite (`make && make wasm-node && make wasm && tests/run.sh`, `XIAOJI_JIT=0`, `XIAOJI_BOOT=1`,
  `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1`, `make test-web`, the emcc build of the changed files). Not run in this step.
- A guest test for the netlink stub, and the `XIAOJI_BOOT=1` extension to the furthest milestone.
- `tools/remote-build.sh` and `tools/fetch-remote-rootfs.sh` were not used (no product change was needed). `build/rootfs-xiaoji`
  was not refreshed; only its `data/` was cleared with `make reset-data`. The worktree `xiaoji-w1` was not touched.
- The emulated storage plan (`/sdcard` needs FUSE in the engine, or sdcardfs-style bind mounts) is not built.

## B11: the cgroups (synthetic, from cgroups.json), the netd decision, and the boot past the cgroup error

Status: **checkpoint 1 done** (cgroups and `cgroup.rc`). **netd decided**, not built: the real netd cannot start in this engine
(evidence below), and the replacement `xiaoji_netd` is the next step. NETLINK_ROUTE is not started (nothing needs it until the
replacement exists). The product boot check passes with a 300 s limit (187 passed, 0 failed). At the old 150 s limit it
passed in 1 of 4 runs (the vibrator at 123 s); the others ended before the vibrator line (see "Milestones"). The runs are on battery
with Low Power Mode on, where the guest runs at 40 to 60 M instructions per second.

| 1 | cgroups and cgroup.rc (`engine/cgroupfs.c`) | Done. Verified by `tests/init/cgroups.rc` through the real `settaskprofile`. |
| 2 | netd decision, and netd running | Decided (xiaoji_netd). Not built. The real netd no longer exits: it waits at BPF forever. |
| 3 | NETLINK_ROUTE | Not started. |
| 4 | Boot progress | Partial: the boot reaches `StartNetworkManagementService`, which gets a null `INetd` (below). `System now ready` is not reached. |

### Cgroups: design (`engine/cgroupfs.c`, `engine/cgroupfs.h`)

- **Source of truth.** `cgroupfs_init` (called by `init_main`, before the on-block mkdirs) reads `/system/etc/cgroups.json`
  and `/vendor/etc/cgroups.json` with the rules of libprocessgroup's `setup/cgroup_map_write.cpp`: each `Cgroups` entry is a
  v1 mount at its `Path` (`Optional` sets the OPTIONAL flag), and `Cgroups2` is the v2 root at its `Path`, with its
  `Controllers` (the root controllers, `Path` "."). A controller named twice replaces the first. `cgroups_<api>.json` is not
  read: `ro.product.first_api_level` is 33, and the product has only the 28 to 30 files (a gap, below).
- **cgroup.rc.** The binary of `WriteRcFile`: `CgroupFile { version=1, count }`, then one `CgroupController { version, flags,
  name[16], path[32] }` (56 bytes) per controller, in name order. Flags: MOUNTED (0x1) on every mount, OPTIONAL (0x4) for
  `memory`. It is a read-only **memfd** (as the selinux status page), because libprocessgroup `mmap`s it `MAP_SHARED`. Its size
  is 8 + 56 × count (344 bytes: blkio, cgroup2, cpu, cpuset, freezer, memory). Without cgroups.json there is no cgroup.rc, and
  the error of the stock netd and lmkd stays.
- **Namespace.** The mounts are `/dev/blkio`, `/dev/cpuctl`, `/dev/cpuset`, `/dev/memcg` (v1) and `/sys/fs/cgroup` (v2).
  `/dev/cgroup_info` holds `cgroup.rc`. VFS: the syn kinds `SYN_CGROUP` (files) and `SYN_CGROUP_RC` (the memfd), with `SYN_DIR`
  for directories. A missing path under a mount is `SYN_CGROUP` with `exists=0` and never the host (`vfs_resolve`, vfs.c);
  `mkdirat` and `unlinkat` (sysfs.c) call `cgroupfs_mkdir` and `cgroupfs_rmdir`; `fchmodat` on a cgroup path succeeds (modes are
  not kept). `fchownat` already succeeded for synthetic paths.
- **Files.** A static table in cgroupfs.c, per mount kind (v1 cpu, cpuset, blkio, memory, other; v2). Only the names the platform
  code reads or writes: `cgroup.procs`, `tasks` (v1), `cgroup.threads`, `cgroup.events`, `cgroup.freeze`,
  `cgroup.controllers`, `cgroup.subtree_control`, `cgroup.event_control`, `cpu.uclamp.{min,max,latency_sensitive}`,
  `cpu.shares`, `cpus` and `mems` (cpuset, unprefixed as task_profiles.json uses them), and `memory.*` (stat, usage,
  memsw.usage, limit, soft_limit, swappiness, pressure_level). A name not in the mount's table is ENOENT.
- **Membership.** `proc_t.cg[mount]` holds the directory (0 is the mount root). `proc_fork` copies it. `proc_release` clears it,
  and the existing `exited` flag keeps exited processes out of every listing. `cgroup.procs` and `tasks` list the live members,
  and a write moves the named pids (0 is the writer). `rmdir` is EBUSY for a populated group, for a group with children, and for
  a mount root. `cgroup.events` reports `populated`. Other writable files store the value and return it on read. Nothing is
  scheduled, limited or frozen.
- **/proc/<pid>/cgroup and /proc/self/cgroup.** One `N:name:/path` line per v1 controller and one `0::/path` line for the v2 mount,
  from the membership. libprocessgroup's `GetTaskGroup` parses these.
- **Created by init.** `image/xiaoji.rc` (early-init) makes the subgroups of the stock `init.rc`: `cpuctl` (foreground, background,
  top-app, rt, system, system-background, dex2oat, nnapi-hal, camera-daemon), `cpuset` (foreground, background,
  system-background, restricted, top-app, camera-daemon), `blkio/background`, and `memcg/apps` and `memcg/system`. The stock
  file's `copy` lines and its schedtune mount are left out (the mount is not in cgroups.json for API 33).
- **Simplifications.** `tasks` lists processes (their main thread id), not each thread. A write of a tid that is not a process
  is ESRCH, which libprocessgroup's `AddTidToCgroup` treats as a thread that is exiting (no error).
- **Permissions.** The synthetic files keep no owner or mode (the stock init's `chown` is accepted and not recorded), and no
  DAC check is made on them. So any uid can write them. libprocessgroup's `access(W_OK)` passes. The test below runs the
  profile calls as `system` (uid 1000) to show it. A v2 controller at a sub-path of the root is
  reported and not mounted (the product has none). New uid and pid groups get mode 0755 (libprocessgroup's chown is accepted and
  ignored).

### Tests

- `tests/init/cgroups.rc`, expected text `tests/expect/init-cgroups.out`, run by `tests/run.sh` on the engine and in Node, on
  `build/rootfs` (the Google tree has a cgroups.json too). It checks the cgroup.rc size (8 + 56n), a uid/pid group made and
  removed, a shell's membership (its grep is the second member), the cat it starts in `/proc/self/cgroup`, EBUSY on a populated
  group, a store in a v1 file, and libprocessgroup's own `settaskprofile` (`ProcessCapacityLow LowIoPriority` applied, the shell in
  `/dev/cpuset/background`; `NoSuchProfile` refused). This is the real libprocessgroup task-profile path, from the product image.
  The cgroup.rc parse is checked by size and by that path, not by a separate guest program.
- A second service, `cgsys`, runs as `user system`: it applies `ProcessCapacityLow` (the uid is shown as 1000), and makes and
  removes `/sys/fs/cgroup/uid_2000`.
- The product check (`XIAOJI_ROOT=... XIAOJI_BOOT=1`, B11 part of the PASS line) asserts, on the boot's early lines (which are not
  timing-sensitive): no `Failed to find cgroup v2 root`, no `cgroup_info/cgroup.rc: No such file`, no `Failed to open /dev/cpuset/`
  (SurfaceFlinger's `SFMainPolicy` profile needs the cpuset subgroup), no `init: netd exited`, and `libnetd_updatable_init: Initializing`.
- **A bug found by this check.** The on-block `mkdir` list had a cap of 16 entries, and the parser dropped the rest silently. The
  product rc has 46. So the subgroups were never made, and SurfaceFlinger's profile failed. The cap is now 128 (`engine/init.c`),
  and an overflow is reported on stderr.

### netd: the decision (evidence)

The stock netd (`system/netd/server/main.cpp`, remote `~/xiaoji-aosp`) starts in this order, and the first gates decide it:

1. `CgroupGetControllerPath(cgroup2)`: needs cgroup.rc. **Passed** now (the root is `/sys/fs/cgroup`).
2. `libnetd_updatable_init` → `BpfHandler::init` (`packages/modules/Connectivity/netd/BpfHandler.cpp`) →
   `android::bpf::waitForProgsLoaded()` (`frameworks/libs/net/common/native/bpf_headers/include/bpf/WaitForProgsLoaded.h`).
   That loop **never returns** unless the property `bpf.progs_loaded=1` is set. Only `bpfloader` sets it
   (`system/bpf/bpfloader/BpfLoader.cpp:202`), after it loads the eBPF programs with `bpf(2)`. The engine implements no `bpf(2)`
   (`engine/syscall.c` has no such case). `initPrograms` also attaches the programs to the cgroup with `bpf(2)`, so forcing the
   property would only move the exit.
3. (`initPrograms` then attaches the eBPF programs to the cgroup with `android::bpf::attachProgram`,
   `BpfHandler.cpp:51` to `:75`, which is a `bpf(2)` call. Forcing the property would only move the exit to there.)
4. `Controllers::init` (`system/netd/server/Controllers.cpp:276`) runs the bandwidth control, which drives the iptables and
   ip6tables restore tools, and `exit(1)` on failure. `NetlinkManager::start` needs NETLINK_ROUTE and NETLINK_NETFILTER (B10's stub
   has neither), and the NFLOG listener needs NETLINK_NETFILTER.
5. The binder services (`netd`, `mdns`), the DnsResolver and the fwmark socket come after all of this.

**Decision: the real netd is impractical in this engine.** Gate 2 cannot pass without `bpf(2)`, and the BPF maps and programs
are the kernel's part of the design, not something the engine can supply cheaply. Gate 3 needs the iptables pipeline. The
alternative is a minimal `xiaoji_netd`: a binder service that registers `netd` with the `INetd` AIDL (V10 of
`frameworks/libs/net/common/netd/`, 109 methods; the generated `BnNetd` is in `out/soong/.intermediates/frameworks/libs/net/common/netd/netd_aidl_interface-V10-cpp-source`),
answering the boot path with success or empty values and logging every other call. The C++ stub should be generated from that
header, not written by hand. It also needs `dnsresolver` and `mdns` only if the framework waits for them, and the dnsproxyd and
fwmarkd sockets only if bionic's resolver uses them. **Not built in this step** (the remote build is the next step).

### The next blocker (product boot, with the real netd)

With cgroups in place, the stock netd stays up, and waits in `BpfHandler` (`waitForProgsLoaded`, no exit). Its back-off log, from the
A/B run of the new engine:

```
[5092 ms 239897655 insn] I/netd(1017): netd 1.0 starting
[5097 ms 240031488 insn] I/NetdUpdatable(1017): libnetd_updatable_init: Initializing
[10137 ms 426093918 insn] W/BpfHandler(1017): Waited 5s for bpf.progs_loaded, still waiting...
[80159 ms 3178331749 insn] W/BpfHandler(1017): Waited 40s for bpf.progs_loaded, still waiting...
```

The HEAD tree (the same host) runs the stock netd, which exits at once, and init gives up:
`xiaoji: init: netd is crash-looping; not restarting` at 14.5 s (0.66 G instructions).

The framework's `NetworkManagementService` start still does not get netd. `NetdService` returns no instance (null) on each
try, and each try logs it. This is B10's blocker in a new form: netd registers its binder service only after the BPF wait, so
`NetdService` finds nothing, and the start keeps retrying. Excerpt of the passing product run (`build/tests/out/boot.err`):

```
[144553 ms 7992440617 insn] D/SystemServerTiming(1049): StartNetworkManagementService
[144568 ms 7993497037 insn] W/NetdService(1049): WARNING: returning null INetd instance.
[144675 ms 8001579392 insn] W/NetdService(1049): WARNING: returning null INetd instance.
   ... (one line per retry, to the end of the run) ...
[147421 ms 8229614784 insn] W/NetdService(1049): WARNING: returning null INetd instance.
[147507 ms 8237689932 insn] W/StorageManagerService(1049): storaged not found; trying again
xiaoji: killed by signal 9        (the 150 s init timeout)
```

So `System now ready` is still not reached. The next step is the `netd` service that `xiaoji_netd` provides: `NetdService`
looks it up by name.

### Milestones (product tree, `make reset-data` first; guest time and instruction count)

Power, recorded with `pmset -g` for these runs: **not on AC.** `Now drawing from 'Battery Power'`, `lowpowermode 1`. In this state
the guest runs about 2 to 3 times slower than in B10 (B10 ran at about 120 M instructions per second, here it is 40 to 60 M), and
the boot's wall-clock waits (the 150 s init timeout, the PackageManager scan, the `Waiting 1s` loops) take different paths. **These
times are not comparable with B9 and B10.** The B9 milestones (lmkd at 56.6 s, vibrator at 63 s) were recorded on AC.

| Milestone | HEAD, A/B run (`ab-base`) | new, final product run (passed) | new, A/B run (killed) |
|---|---|---|---|
| netd, the stock one | exits; crash-loop stops at 14.5 s, 0.66 G | starts; waits for `bpf.progs_loaded` | starts at 5.09 s, 0.24 G; waits |
| `InitBeforeStartServices took` | 6158 ms, at 111.8 s, 4.66 G | 4139 ms, at 80.9 s, 4.67 G | 6061 ms, at 114.5 s, 4.67 G |
| `Connection with lmkd established` | not reached by 147 s (6.05 G) | at 105.9 s, 5.99 G | not reached by 140 s (5.72 G) |
| `StartVibratorManagerService took` | not reached | at 123.1 s, 6.75 G (324 ms) | not reached |
| `StartNetworkManagementService` | not reached | at 144.6 s, 7.99 G (null INetd from 144.6 s) | not reached |

The product check was run five times in this step, all after `make reset-data`, all on battery with Low Power Mode on. At the
150 s limit: four runs, one pass (the vibrator at 123 s); of the three others, one reached the lmkd line and stopped before the
vibrator, and two stopped before the lmkd line. With the 300 s limit (the check now): one run, which passed (the lmkd line at 118.4 s, 6.04 G; ANDROID_ID at 133.6 s;
the vibrator at 135.8 s, 6.81 G). In that run the boot went on to `StartNetworkManagementService` at 169.3 s, 8.00 G, where
`NetdService` returned null (84 retries logged) until the run was stopped at the limit. HEAD failed the same check at the same
stage on the same host (the A/B run, which is not the product check). **To verify:** rerun on AC with Low Power Mode off, and record `pmset -g` with it.

### Verification (this step)

| Command | Result |
|---|---|
| `make && make wasm-node && make wasm && tests/run.sh` | **220 passed, 0 failed, 0 xfail** (218 before, plus `init-cgroups` on the engine and in Node) |
| `XIAOJI_JIT=0 tests/run.sh` | **220 passed, 0 failed, 0 xfail** |
| `XIAOJI_BOOT=1 tests/run.sh` (Google image) | **221 passed, 0 failed, 0 xfail** (219 before, plus `init-cgroups`) |
| `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh` (after `make reset-data`, 300 s boot limit) | **187 passed, 0 failed, 0 xfail** (185 in B9, plus `init-cgroups` on both runners, and the new B11 markers in the boot check); the vibrator at 135.8 s, on battery with Low Power Mode on |
| `make test-web` | all web checks passed (exit 0) |
| `clang -O2 -std=c11 -Wall -Wextra` and `emcc -O2 -std=c11 -Wall -Wextra` on `cgroupfs.c`, `vfs.c`, `sysfs.c`, `init.c`, `proc.c`, `procfs.c` | 0 warnings each (`init.c` rechecked after the mkdir cap change) |

The product check was run three times in this step. The first two failed at the lmkd and vibrator lines (after the 150 s limit,
on a busier host, and the same boot at HEAD failed at the same point in the A/B run). The third passed. Those counts are in the
table, as runs of the same check. The baselines at HEAD, measured in this session before the change: `tests/run.sh` 218 passed,
`XIAOJI_JIT=0` 218, `XIAOJI_BOOT=1` 219. The product run at HEAD was not measured (an edit to `tests/run.sh` during that run
invalidated it); **185** is B9's recorded figure. The step's counts include the new `init-cgroups` case (two runners in the default
suite, one per runner in the Google boot run).

### Gaps and open items (B11)

- **xiaoji_netd is not built** (the decision above). Its build, `tools/remote-build.sh`, `tools/fetch-remote-rootfs.sh` and the
  linkerconfig regeneration are the next step. `build/rootfs-xiaoji` was refreshed by none of this step: only `make reset-data`.
- **NETLINK_ROUTE** (checkpoint 3) is not started.
- **The product check passes with the 300 s limit** (one run), and passed at 150 s in 1 of 4 runs. Its milestones are guest
  instruction counts, but the boot also waits on the clock, so a slow host can still push a milestone past the limit. The check
  is on battery with Low Power Mode on; a run on AC is needed to call the timing settled.
- **Checkpoint 2 was not started in this step.** The `xiaoji_netd` build (the 109-method stub generated from the V10 header, the
  remote build, the fetch, the linkerconfig, and a boot run for it) was left as the next step, because checkpoint 1's checks
  and this section's evidence used the context I had. This is my decision, not a blocker found in the engine.
- **NETLINK_ROUTE** (checkpoint 3) is not started, and it has no test.
- **`cgroups_<api>.json`** is not read (API 33 has no file; the property is not read). It would matter for a product with a 28 to 30
  first API level.
- **Limits of the cgroup model** (above): tasks are processes; v2 sub-path controllers are not mounted; no scheduling effect.
- **The BPF gate** is a fundamental design issue, not a bug: the real netd needs `bpf(2)` and the BPF maps. The recommendation is
  the replacement (`xiaoji_netd`), with the BPF-dependent framework calls (BpfNetMaps, NetworkStatsService) checked for fatal
  failures when the replacement runs.

## B12: xiaoji_netd (the replacement netd), the dnsresolver service, and the boot past StartConnectivityService (stops at audio)

Status: **partial.** `xiaoji_netd` replaces the stock netd in the product and in the engine's rc. It registers the `netd`
binder service (the real `android.net.INetd`, V10) and the `dnsresolver` service (`IDnsResolver`, V10, from a copy of its
AIDL). In the product boot, `NetworkManagementService` gets a non-null `INetd` (no `returning null INetd` line), and
`ConnectivityService` is constructed. `System now ready` and `sys.boot_completed=1` are **not** reached. Between 145 s and
208 s the audio policy service is missing, and the watchdog kills system_server (its main thread is in `AudioService`). The
stock audio HAL aborts at its start in this tree (below). Making audio work is a design decision, not made here.

### Checkpoints

| # | Checkpoint | Result |
|---|---|---|
| 1 | xiaoji_netd built and registered; NetworkManagementService gets a non-null INetd | Done. `registered netd` at 3.3 s; `StartNetworkManagementService took to complete: 15ms`; zero `returning null INetd`. |
| 2 | Companion services as needed | `dnsresolver` done: `ConnectivityService.getDnsResolver` needs the framework's `DnsResolverServiceManager`, which is built from the `dnsresolver` binder; without it the boot died (below), and the constructor does `requireNonNull` on the interface. `netd_listener` is not registered: it is hosted by system_server (`NetdEventListenerService`), and netd is only its client. `mdns` is not served: the NSD path logs `No service published for: mdns` and goes on. |
| 3 | NETLINK_ROUTE | Not started. No boot-path call needed it; the stock netd's NETLINK uses are gone with the stock netd. |
| 4 | Boot progress | Partial: past `StartConnectivityService`; the watchdog kills system_server in the audio service. |

### What xiaoji_netd implements

Sources (`aosp/device/xiaoji/xiaoji_netd/`, copied to `~/xiaoji-aosp/device/xiaoji/xiaoji_netd/` by `tools/remote-build.sh`):
`Android.bp`, `xiaoji_netd.cpp` (main and the hand-written methods), `xiaoji_netd.rc`, `gen_stub.py` and its output
`xiaoji_netd_gen.h`, `gen_dnsresolver_stub.py` and its output `xiaoji_dnsresolver_gen.h`, and `aidl/` (eight DnsResolver AIDL
files, copied from `packages/modules/DnsResolver/binder`, read only).

- **INetd** (`netd_aidl_interface-V10-cpp`, generated header `INetd.h`; `gen_stub.py` asserts 107 `Status` methods):
  - Hand-written (5): `isAlive` (true); `interfaceGetList` (`{"lo"}`); `interfaceGetCfg("lo")` (127.0.0.1/8, flags up,
    loopback, running; other names ENODEV); `getProcSysNet` and `setProcSysNet` on `/proc/sys/net/<ipv4|ipv6>/<conf|neigh>/
    <if>/<param>`, which is used when `/proc/sys/net` exists, otherwise `""` and success. Names with `/` or a leading `.`
    are EINVAL.
  - Success no-ops (87): `firewall*`, `trafficSwap*`, `bandwidth*`, `interface*` (the rest), `network*`, `tether*`,
    `registerUnsolicitedEventListener`, `setTcpRWmemorySize`. Also the configure-only methods found by grepping the framework's
    callers: `strictUidCleartextPenalty`, `setIPv6AddrGenMode`, `wakeupAddInterface`/`wakeupDelInterface`,
    `idletimerAddInterface`/`idletimerRemoveInterface`, `socketDestroy`, `ipfwdEnableForwarding`/`ipfwdDisableForwarding`,
    `ipfwdAddInterfaceForward`/`ipfwdRemoveInterfaceForward`, `trafficSetNetPermForUids`. Getters with a benign value:
    `ipfwdEnabled` (false), `ipfwdGetRequesterList` (empty), `getFwmarkForNetwork` (empty mark). Boolean outputs are true
    except `tetherIsEnabled` and `networkCanProtect` (false).
  - Logged `EX_UNSUPPORTED_OPERATION` (15): `clatdStart`, `clatdStop` (464xlat, only on IPv6-only networks), the 12
    `ipSec*` (IpSecService, for apps), and `getOemNetd`.
  - Why the configure-only methods are no-ops and not unsupported: the framework callers catch only `RemoteException` or
    `ServiceSpecificException` (e.g. `NetworkManagementService.setUidCleartextNetworkPolicy`), so an
    `EX_UNSUPPORTED_OPERATION` would reach them as an unchecked exception and crash the caller.
  - **What the boot called:** only `registerUnsolicitedEventListener` (4 times, no-op success), up to the watchdog kill. No
    other `INetd` method was called, so none of the unsupported answers was reached. The callers of the rest were checked by
    grep (`frameworks/base/services`, `packages/modules/Connectivity`, `packages/modules/NetworkStack`): `trafficSetNetPermForUids`
    is only reached when `USE_NETD` (`!SdkLevel.isAtLeastT()`, `BpfNetMaps.java`), which is false on this T product.
    The `ipfwd*` callers are Tethering and NetworkManagementService's `getIpForwardingEnabled`, which run later than the
    boot reached.
- **IDnsResolver** (14 methods, cpp backend, from the copied AIDL): each method is a logged no-op success;
  `getResolverInfo` returns empty lists, `getPrefix64` returns `""`, `isAlive` is true. The boot made no DNS call.
  **Wire compatibility (checked):** the copy declares the same 14 methods in the same order as the frozen
  `aidl_api/dnsresolver_aidl_interface/10` (a diff of the declarations shows only type qualification and line wrapping). The
  framework's ConnectivityService uses `dnsresolver_aidl_interface-V9-java` (`Connectivity/service/Android.bp:162`), and its
  frozen V9 declares the same 14 methods in the same order, so the transaction codes agree. Not exercised at runtime.
- **Why the DNS AIDL is copied.** The `dnsresolver_aidl_interface` module is `apex_available` to `com.android.resolv` only,
  so the platform cannot link it (`platform_availability_check`). The copy is unversioned (`unstable: true`), with the
  platform-available `netd_event_listener_interface-V1` as its import.
- **Not served:** the `dnsproxyd` socket (bionic's resolver, so app DNS does not resolve through the engine), `fwmarkd`, and
  the `mdns` service and socket. The rc does not create their sockets, so a client gets ENOENT (not a hang) for them. For app
  DNS later: a `dnsproxyd` server that relays to the engine's network.

**Product wiring.**
- `aosp/device/xiaoji/xiaoji_system.mk`: `PRODUCT_PACKAGES += xiaoji_netd`. `Android.bp` has `overrides: ["netd"]`, so the
  stock `netd` binary and `netd.rc` are not installed: checked in the product's `installed-files.txt` (no `/system/bin/netd`),
  and `build/rootfs-xiaoji/system/bin/netd` is gone after the fetch.
- `image/xiaoji.rc` (the only rc the engine reads: `engine/init.c` has no `etc/init` path): `service netd /system/bin/xiaoji_netd`
  with the stock class, capabilities, group and `onrestart restart zygote`. The stock sockets (`dnsproxyd`, `fwmarkd`, `mdns`)
  are left out. `xiaoji_netd.rc` is the module's copy (installed as `/system/etc/init/xiaoji_netd.rc`; the engine does not read
  it). The two blocks are the same.
- `build/rootfs-xiaoji` refreshed: `tools/remote-build.sh` (product build, exit 0) and `tools/fetch-remote-rootfs.sh` with its
  default arguments (linkerconfig regenerated). `make reset-data` before each boot.
- `tests/run.sh` (`XIAOJI_BOOT=1`, product): the B11 `libnetd_updatable_init: Initializing` marker is gone (the stock netd
  printed it, xiaoji_netd does not). It asserts `xiaoji_netd([0-9]*): registered netd`, `registered dnsresolver`, no
  `returning null INetd`, and `StartNetworkManagementService` and `StartConnectivityService took to complete`.

### Milestones (product tree, final run; guest time `[ms insn]`)

Run: `./build/xiaoji --logcat --init image/xiaoji.rc --root build/rootfs-xiaoji --init-timeout 300` with `XIAOJI_STAMP=1`, after
`make reset-data`. Power: `pmset -g batt` shows `Now drawing from 'AC Power'`; `pmset -g` shows `lowpowermode 0`. Checked at 23:34 CST (2026-11-23),
and again after the final run (see the verification table). Times vary between runs on this host (the first B12 run reached
`InitBeforeStartServices` at 41.5 s, this run at 76.9 s), so use the instruction counts for comparison.

| Milestone | time | instructions |
|---|---|---|
| `xiaoji_netd starting` | 0.70 s | 0.09 G |
| `registered netd`, `registered dnsresolver` | 3.3 s | 0.24 G |
| `InitBeforeStartServices took` 4891 ms | 76.9 s | 4.65 G |
| `Starting phase 100` | 78.4 s | 4.73 G |
| `Connection with lmkd established` | 98.3 s | 5.96 G |
| `Generated and saved new ANDROID_ID` (SettingsProvider) | 107.8 s | 6.55 G |
| `StartVibratorManagerService took` 340 ms | 109.9 s | 6.68 G |
| `StartLockSettingsService`: BOOT FAILURE (SQLite, caught; see the engine gap below) | 130.9 s | 8.02 G |
| `StartNetworkManagementService took` 15 ms (INetd non-null) | 131.2 s | 8.03 G |
| `StartConnectivityService took` 990 ms (ConnectivityService constructed) | 139.1 s | 8.62 G |
| `Service media.audio_policy didn't start` (repeated every ~5.5 s from 145 s) | 145.1 s | 9.11 G |
| watchdog: main thread blocked in AudioService; system_server killed | 207.8 s | 13.63 G |

Before the dnsresolver (the first B12 run, with the INetd stub but no `dnsresolver` binder): `NullPointerException:
Attempt to invoke virtual method 'android.os.IBinder android.net.DnsResolverServiceManager.getService()' on a null object
reference` at `ConnectivityService.getDnsResolver`, from `ConnectivityServiceInitializer`. system_server died and the zygote
with it at 152.1 s (15.86 G).

### Next blocker: the audio service (the stock audio HAL aborts in this tree)

`AudioService` (system_server, main thread) calls `AudioProductStrategy.getAudioProductStrategies`, which waits for
`media.audio_policy`. The product has no `audioserver` rc block (`image/xiaoji.rc` has only `onrestart restart audioserver`
lines), and the HAL it would need is `vendor.audio-hal` (`/vendor/bin/hw/android.hardware.audio.service`).

A probe in the scratchpad (`audio-probe.rc`: `image/xiaoji.rc` plus the stock `audioserver` block and the stock
`vendor.audio-hal` block from `frameworks/av/media/audioserver/audioserver.rc` and
`hardware/interfaces/audio/common/all-versions/default/service/android.hardware.audio.service.rc`, with the group
`mediadrm` written as `media_drm`, the name in `engine/ids_gen.h`) shows the HAL dies at its start:

```
[987 ms 108975422 insn] F/ProcessState(1019): Binder threadpool cannot be shrunk after starting
[987 ms 108981003 insn] F/libc(1019): Fatal signal 6 (SIGABRT), code -1 (SI_QUEUE) in tid 1019 (android.hardwar), pid 1019 (<unknown>)
xiaoji: init: vendor.audio-hal exited (status 134, by a signal)
...
xiaoji: init: vendor.audio-hal is crash-looping; not restarting
```

The HAL's `main` (`service.cpp:80-82`) calls `ProcessState::self()->startThreadPool()` and then
`ABinderProcess_setThreadPoolMaxThreadCount(1)`. libbinder's `setThreadPoolMaxThreadCount` has
`LOG_ALWAYS_FATAL_IF(mThreadPoolStarted && maxThreads < mMaxThreads, ...)`, which is what fires. The engine only runs this code,
so this is the tree's HAL and libbinder as built, not an engine fault. Whether a real device avoids it is not checked. The
probe's audioserver then waits for `media.audio_policy`, and the watchdog kills system_server (probe log: one kill).

**Decision for the user (not made):** make the audio path boot. Options: (a) a device-local HAL in `device/xiaoji` (a null sink
HAL that accepts and drops samples) with the audioserver block, which is a new component; (b) patch the stock HAL's `main`
(a `hardware/interfaces` change, outside `device/xiaoji`, not allowed here). **Recommendation:** (a), a null-sink HAL in
`device/xiaoji`, then the goldfish audio HAL when the engine has an audio device.

### Engine gaps found in the boot (not fixed here)

- **Writable MAP_SHARED of a host file (SQLite WAL).** `LockSettingsService` opens `/data/system/recoverablekeystore.db` in WAL
  mode, and SQLite maps the `-shm` file writable:

  ```
  [130940 ms 8017600727 insn] E/SystemServer(1050): BOOT FAILURE starting LockSettingsService service
  ```
  (the SQLite lines before it: `mmap(/data/system/recoverablekeystore.db-shm)` failing with errno 13, code 5386
  `SQLITE_IOERR_SHMMAP`). `engine/syscall.c`, `sys_mmap`, refuses a writable `MAP_SHARED` of a host file (`only read-only
  MAP_SHARED is supported`, EACCES). The failure is caught by `SystemServer`, and the keystore is not published. Fixing it needs
  a coherence rule: for SQLite's `-shm` a volatile memfd per host path would do; for real files, write-back on `msync` and
  `munmap`. A design decision, not made here. The B8 to B11 notes do not mention this failure (checked by grep). LockSettingsService
  starts before NetworkManagementService, so the B11 runs also passed through it; its log was not kept, so whether the same
  error appeared there is not checked.
- **BPF maps.** `nativeBpfFdGet failed: ENOSYS` in `BpfInterfaceMapUpdater`, `NetworkStats` (five maps), `ClatCoordinator`
  (`ingress6`, `egress4`), and `ConnectivityService: Unable to create DscpPolicyTracker`. They are logged errors at 137 to 139 s,
  and the boot goes on. NetworkStats will fail when stats are queried. This is the B11 BPF gap (no `bpf(2)`), now visible
  through the framework.
- **`/proc/sys/net`** is absent: `getProcSysNet` and `setProcSysNet` answer success with no effect.

### Verification (B12, final sources)

Run after the last source change (the INetd categories, the rc, `overrides: ["netd"]`). Power: AC with Low Power Mode off
at 23:34 CST and for the final product boot (the boot3 run started about 23:45). At 00:16 CST the machine was on battery
with Low Power Mode on (`pmset -g batt`: `Now drawing from 'Battery Power'`, `lowpowermode 1`). The verification runs
straddle that change, so the timing-dependent product boot check was run once; its milestones are the instruction counts above.

| Command | Result |
|---|---|
| `make && make wasm-node && make wasm && tests/run.sh` | **220 passed, 0 failed, 0 xfail** |
| `XIAOJI_JIT=0 tests/run.sh` | **220 passed, 0 failed, 0 xfail** |
| `XIAOJI_BOOT=1 tests/run.sh` (Google image) | **221 passed, 0 failed, 0 xfail** |
| `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh` (after `make reset-data`, 300 s limit) | **187 passed, 0 failed, 0 xfail** (includes the B12 assertions) |
| `make test-web` | **all web checks passed**, with `PLAYWRIGHT_BROWSERS_PATH` set to a Chromium installed under the scratchpad. Without it, the run fails before any check: Playwright's browser is missing from `~/Library/Caches/ms-playwright` (a Mac path outside the repo, not reinstalled there). |
| `clang` / `emcc -Wall -Wextra` | Not run: this step changed no engine C file. The INetd and DNS stubs are C++ built by the AOSP toolchain. The product build of `xiaoji_netd` has no warnings from our files after the log-format fix. |
| `pgrep -fl "build/xiaoji|wasm-node/xiaoji"` at the end | Empty (exit 1). A run in `xiaoji-w1` was listed earlier in the session; it was another agent's and was not touched. |

### Gaps (B12)

- `dnsproxyd`, `fwmarkd`, `mdns` are not served (app DNS does not work). The DNS AIDL copy has no runtime calls yet.
- The `-shm` gap above (LockSettingsService). The BPF gap (NetworkStats and the other BPF map users, logged).
- Audio: the decision above (system_server's watchdog kill).
- NETLINK_ROUTE (checkpoint 3): not started.
- The `getOemNetd`, `clatd*`, and `ipSec*` methods are unsupported: their callers are IPv6-only networks, IPsec apps, and OEM
  code; none reached in the boot.

## B13: the audio HAL (null sink), the shared page cache for MAP_SHARED, and the boot past System now ready (stops at the zygote's data mirror)

Status: **partial.** Audio is up: the XiaoJi HAL registers, `audioserver` loads the primary module, `media.audio_policy`
comes up, and `AudioService` completes (in B12 the watchdog killed system_server in it). The SQLite `-shm` failure is gone:
`LockSettingsService` maps the file writable and starts (184 ms). In the product boot, `ActivityManager` reports
**System now ready** (102 s). `sys.boot_completed=1` and the launcher are **not** reached: zygote's app specialization for
`com.android.permissioncontroller` aborts on a data mirror mount, and the watchdog then kills system_server (next blocker,
below).

**Decision for the user: the page cache's memory (measured).** The first cache version loaded a host file's whole contents
at its first `MAP_SHARED` mapping. Peak RSS of `hello-int` (ART, Google image, `build/rootfs`), one alternating pair: 132 MiB
without the cache (`git HEAD`), 163 MiB with it. The cache now fills each page from the host file on first use. Three
alternating pairs after that change: without the cache 159, 210 and 190 MiB; with it 156, 226 and 190 MiB. The same binary
varies by about 70 MiB between runs, so the lazy version shows no regression I can measure, and the whole-file version cost
about 30 MiB on that one pair. Not measured: the web snapshot (the 180 MB fetched by `make test-web` was not compared with a
baseline build). Recommendation: keep the lazy fill. If the snapshot size matters, the choice is between this cache for
every `MAP_SHARED` (the spec) and w1's lazy windows for read-only file views; that is the user's call.

### The audio HAL (`device/xiaoji/audio`, mirrored in `aosp/device/xiaoji/audio`)

The decision in B12 was a device-local HAL. What is in the tree:

- **Sources.** The HIDL 7.0 implementation of `device/generic/goldfish/audio` (the ranchu HAL: entry, device factory,
  primary device, streams, io thread, ring buffer, ports), copied as they are except the points below. The backend
  `talsa.cpp`/`talsa.h` (tinyalsa over the goldfish device) is replaced by a null PCM with the same functions: a write is
  paced to the sample rate (it sleeps for the frames' duration) and discarded; a read is paced and returns silence. The
  HAL has no mixer. Writes to `/dev/xiaoji_audio` are **not** done: output is discarded (the plan allowed this).
- **Device factory.** The ranchu factory dlopens the legacy ranchu library and asserts it (`Assertion failed: !mLegacyLib`,
  the boot's first HAL abort). This product has one module, the primary one, so the legacy loader is removed; other modules
  get `INVALID_ARGUMENTS`.
- **Service** (`service/xiaoji_audio_hal.cpp`, binary `xiaoji_audio_hal`, `/vendor/bin/hw/xiaoji_audio_hal`). The stock
  service main (`hardware/interfaces/audio/common/all-versions/default/service/service.cpp`) starts the vndbinder thread
  pool before `ABinderProcess_setThreadPoolMaxThreadCount(1)`, and libbinder aborts on that order. This service has no
  vndbinder (every HAL it serves is HIDL), so the path is not there. It registers, as the stock service does: Audio Core
  `IDevicesFactory` 7.1 then 7.0 (7.0 is the one built), Audio Effect `IEffectsFactory` 7.0 (the product's passthrough
  library), and the optional Soundtrigger family 2.3 to 2.0 (2.2 is the one present). The soundtrigger part is needed:
  the framework's `SoundTriggerMiddlewareService` connects to `ISoundTriggerHw` on the main thread at boot with no timeout.
- **Policy** (`audio_policy_configuration.xml`, copied to `/vendor/etc` in place of the goldfish file). One module,
  `primary`: `Speaker` (output, `primary output`, 48 kHz stereo PCM 16) and `Built-In Mic` (input, `primary input`). The
  goldfish configuration's a2dp, remote-submix, Bluetooth, telephony and FM ports are left out. The volume tables are the
  framework's (`audio_policy_volumes.xml`, `default_volume_tables.xml`, as the goldfish configuration includes them).
- **Product** (`aosp/device/xiaoji/xiaoji_vendor.mk`): `PRODUCT_PACKAGES` gets `xiaoji_audio_hal` and
  `android.hardware.audio@7.0-impl.xiaoji` (a vintf fragment with the same HAL line as ranchu's). The ranchu audio HAL, its
  legacy library and the stock `android.hardware.audio.service` are no longer in `PRODUCT_PACKAGES`, so the product does not
  install them (checked in `build/rootfs-xiaoji`: no ranchu audio library, and `vendor/bin/hw` has only `xiaoji_audio_hal`).
  The effects library and the goldfish soundtrigger library stay.
- **rc** (`image/xiaoji.rc`): `service audioserver /system/bin/audioserver` (the stock block; the group `mediadrm` is
  `media_drm` in `engine/ids_gen.h`) and `service vendor.audio-hal /vendor/bin/hw/xiaoji_audio_hal` (`class hal`, the stock
  groups, with `interface` lines for the audio and effects factories, so hwservicemanager can map them to the service).

**Checked in the rootfs and the boot:** `build/rootfs-xiaoji` has `vendor/bin/hw/xiaoji_audio_hal`,
`vendor/lib64/hw/android.hardware.audio@7.0-impl.xiaoji.so`, `vendor/etc/audio_policy_configuration.xml` (ours), and no ranchu
audio library. The remote product build (`tools/remote-build.sh`, `m -j20`, `xiaoji_arm64-userdebug`) exits 0; the
rootfs was refreshed with `tools/fetch-remote-rootfs.sh` (defaults, linkerconfig regenerated) after the last source change.

### Other framework blockers met on the way (each one a product or engine fix)

| Blocker (seen in the boot) | Fix | Where |
|---|---|---|
| `Could not find ...IDevicesFactory/default for ctl.interface_start` (no lazy start) | `interface` lines in the HAL block | `image/xiaoji.rc` |
| `F/android.hardware.audio@7.0-impl.xiaoji: Assertion failed: !mLegacyLib` | legacy loader removed | `device_factory.cpp` |
| system_server killed: main thread in `SoundTriggerMiddlewareService` (ISoundTriggerHw 2.0, no instance) | soundtrigger passthrough family registered in the audio HAL service | `service/xiaoji_audio_hal.cpp` |
| system_server killed: main thread in `HardwarePropertiesManagerService` (`IThermal` 1.0, no instance; the product has only the 2.0 mock) | `android.hardware.thermal@1.0-service` and `-impl` in the product, and the 1.0 rc block | `xiaoji_vendor.mk`, `image/xiaoji.rc` |
| system_server killed: main thread in `LockSettingsService` (`IAuthSecret`, no instance; the binary is installed but has no rc block) | rc block | `image/xiaoji.rc` |
| zygote: `Failed to mount tmpfs to /data/data: No such file or directory`, then `Failed to opendir /mnt/expand` | stock `mkdir` lines for `/data/data` and the `/mnt` tree | `image/xiaoji.rc` |
| zygote: `Failed to create symlink /data/user/0 Function not implemented` | engine: `symlinkat` (syscall 36) was missing | `engine/sysfs.c` (`sys_symlinkat`), `engine/host.c` (`host_symlink`) |
| zygote: `Failed to mount tmpfs to /data/data` is the mount point's absence; the `/data/data` directory is made here | see the layout note below | `image/xiaoji.rc` |

The `symlinkat` engine gap is a real syscall gap, not a product workaround. An absolute link target is kept as written
(`host_symlink`), and the engine's path walk resolves it inside the guest root, as it already does for the links of the
tree. No owner record is kept for a new link (as `linkat` keeps none).

### The user-0 data layout (decision for the user, not made)

The product's rc makes `/data/user/0` as a directory (the B7 data root) and `/data/data` as a directory (the mount point
of zygote's tmpfs; `/data/data` is missing without it). After the final boot, `build/rootfs-xiaoji` has:

- `data/user/0`: empty.
- `data/data`: 94 entries (one per package), and `data/user_de/0`: 94 entries.

So installd and PackageManager create the app directories under `/data/data` and `/data/user_de/0`, while the framework
resolves the credential-encrypted user-0 path, `/data/user/0`, which stays empty. The stock layout avoids the split with
`mount none /data/data /data/user/0 bind rec` (init.rc, `on post-fs-data`), and this engine's init has only `mkdir` (no
`symlink`, no `mount`). Options: (a) add a `symlink` builtin to `engine/init.c` (about ten lines, like its `mkdir`), and make
`/data/user/0` a symlink to `/data/data` in the rc, which is the host-tree equivalent of the stock bind; (b) engine bind mounts
for the rootfs view (`engine/mntns.c`); (c) keep the split, which the data mirror below does not tolerate. Recommendation:
(a). Not done here: it needs a boot and a full verification round, and the boot's next blocker is the data mirror below.

Zygote makes its own `/data/user` tmpfs and link inside its mount namespace, which is why the host tree shows no link of its
own. The `symlinkat` the zygote calls (`Failed to create symlink /data/user/0 Function not implemented`, boot 8) is the one
observed failure.

### The shared page cache (`engine/filecache.c`, `engine/filecache.h`)

Design, as in the plan (the plan's Decision 2):

- **One cache per host file** (dev, ino): a memfd of the file's size, created at the first `MAP_SHARED` mapping (read-only
  or writable). Each page is filled from the host file when it is first used: a view's fault, a `read`, or a `write`
  (`memfd_set_fill`, `fc_fill`). Every `MAP_SHARED` view of the file, in every guest process, is a view of that memfd through
  the existing `mem_map_shared`, so fork, futex keys and `mremap` work as they do for memfd views.
- **read, pread, write, pwrite** go through the cache while it exists. A write goes to the host file first (the host fd
  owns the position and `O_APPEND`), then the same bytes are patched into the cache (`fc_patch`). A read comes from the
  cache, so it sees the views' stores that are not yet written back. A private mapping of the file is filled from the
  cache too (`fill_from_cache`).
- **Lifetime.** The cache holds no reference of its own. Its memfd's references are the views (one per region, across
  every address space). `memfd.c` gained a file hook (`memfd_set_file`): when the last reference goes, the hook writes the
  cache back and removes the entry, before the pages are freed. So the cache lives as long as a view does.
- **Write-back** (`fc_write_back`): page by page (4 KiB), the cache's bytes are compared with the host file's, and only the
  pages that differ are written; the host file is never written past the cache's size, and its size is set to the cache's.
  Write-back runs on `msync` (`fc_sync_va`), `fsync` and `fdatasync` (`fc_flush_file`), the last unmapping (the hook),
  a guest process's exit (`proc_release` calls `fc_flush_all`), and the engine's exit (`atexit`).
- **Write fd.** A write-back needs a host fd open for writing: the cache keeps a `dup` of the first `O_RDWR` fd that maps
  it. A read-only mapping (no write fd) has nothing to write back, since its views cannot store.
- **ftruncate and O_TRUNC** call `fc_truncate`: the memfd is truncated (its tail zeroed), and the views are resized.
- **SIGBUS past the end of the file.** `mem_shm_resize` walks every live address space (`mem.c` keeps a list of them now)
  and clears `PTE_MAPPED` on the pages of the cache's views that lie wholly past its size; `mem_past_eof` turns the fault
  into `MEM_FAULT_BUS`, and the CPU raises SIGBUS with `BUS_ADRERR`. Growing the file sets the bits back. `fork` copies
  the unmapped shared entries too, so a child sees the same map. The SIGBUS plumbing (`MEM_FAULT_BUS`, `SIGBUS_N`,
  `BUS_ADRERR`, `sig_bus_fatal`, the `setup_frame` address) is the w1 branch's (822c424), copied as it is.

**Merge with `w1-browser-boot` (822c424 lazy file-backed mmap, not in main).** Both key a host file by (dev, ino). In w1,
`mem_map_lazy` and `mfile_t` cover `MAP_PRIVATE` and read-only file windows; this cache covers every `MAP_SHARED` of a file.
The SIGBUS hunks in `mem.h`, `signals.c`, `signals.h` and `cpu_a64.c` are copied from w1's text, so a three-way merge should take
each of them once (not yet tried: w1 is not merged here). The
other overlaps are real: `mem.c` (the region and PTE code, `mem_segv`), `syscall.c` (`sys_mmap` and `map_file`), and
`memfd.c`. The recommended merge: keep w1's lazy path for `MAP_PRIVATE`, keep this cache for `MAP_SHARED`, and let the
cache use w1's `mfile_t` for its (dev, ino) identity instead of its own table if the two are merged. Nothing here depends
on w1's lazy windows.

**Limits (B13).**
- Memory: the pages touched (4 KiB each), not the file. The first version loaded whole files (the decision at the top).
- A write-back compares the pages the cache has materialised (the others cannot differ from the host), and every `fsync`
  does the same. Process exit writes back every cache, including caches of other processes still mapping them.
- A host write that does not go through `vfs_write` or `vfs_pwrite` (`sendfile`, `copy_file_range`, or any other path
  that writes a host file directly) does not patch the cache; a page already materialised then goes stale. Not checked
  beyond the code.
- A host-level read that does not go through the engine's read path (exec and the loader's whole-file reads,
  `sendfile`, `copy_file_range`) sees the host bytes: stores through a view are visible there only after a write-back.
- `/proc/<pid>/maps` names a cache view `/memfd:xiaoji-file (deleted)`, not the file's path.
- An engine killed by a signal (not by `exit`) does not run `atexit`: unwritten view stores are lost. `msync` is the
  guest's way to make them durable before that.

### Tests

- `tests/guest/shmmap.c` (freestanding; `argv[1]` is a scratch directory, `tests/run.sh` gives `build/tests/shmmap-work` to
  the engine and `/tmp/xiaoji-shmmap-work` to the native oracle). Two processes map the same file shared; a store through
  a view is read by `pread`; a `write` is read through the view; a child's store is seen by the parent, and the parent's
  store by the child; `msync` and `munmap`; the contents read back after reopening; the SQLite WAL pattern (`wal.db-shm`
  sized with `ftruncate`, mapped by two processes, a header and a counter); and SIGBUS for a page past the end after a
  shrink, with zeros after the file grows again. Across address spaces: a child forked while the file is full blocks on a
  pipe, the parent shrinks the file, and the child touches a page past the new end (SIGBUS, which needs the shrink to reach
  the child's address space); a child forked while the file is short reads the pages the parent grows the file to (zeros).
  The engine and the native oracle print the same 28 lines, for `-O0` and `-O2` (`cmp`).
- `build/tests/msync-probe.c` (not a test case, under the gitignored `build/`): a store, `msync(MS_SYNC)`, then a 4 s sleep.
  Read while the engine was alive, the host file already held `MSYNC-OK` (`od -c`): `msync` writes back, not only exit.
- The product boot check (`XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1`) now also asserts the audio HAL registrations,
  `Loaded primary audio interface`, `StartAudioService took to complete`, and `System now ready` (see the table below).

### Milestones (product tree, final sources; guest time `[ms insn]`)

Run: `./build/xiaoji --logcat --init image/xiaoji.rc --root build/rootfs-xiaoji --init-timeout 300` with `XIAOJI_STAMP=1`, after
`python3 -I tools/reset-data.py build/rootfs-xiaoji`. Power: `pmset -g batt` shows `Now drawing from 'AC Power'`, and
`pmset -g` shows `lowpowermode 0` (checked before the final run and after it).

| Milestone | time | instructions |
|---|---|---|
| `xiaoji_audio_hal`: `Registered android.hardware.audio@7.0::IDevicesFactory/default` | 2.8 s | 0.33 G |
| `Registered android.hardware.soundtrigger@2.2::ISoundTriggerHw/default` | 3.0 s | 0.35 G |
| audioserver: `loadHwModule() Loaded primary audio interface` | 9.6 s | 1.02 G |
| `InitBeforeStartServices took` 2479 ms | 46.1 s | 4.82 G |
| `StartLockSettingsService took` 182 ms (no `SQLITE_IOERR_SHMMAP`, no `BOOT FAILURE`) | 76.9 s | 8.11 G |
| `StartConnectivityService took` 807 ms | 82.1 s | 8.76 G |
| `StartAudioService took` 10231 ms (B12: a watchdog kill here) | 92.7 s | 10.46 G |
| `StartHardwarePropertiesManagerService took` 11 ms | 92.8 s | 10.47 G |
| `ActivityManager: System now ready` | 99.9 s | 11.31 G |
| zygote: `Failed to mount /data_mirror/data_de/null/0/com.android.permissioncontroller ...` (fatal) | 156.7 s | 19.13 G |
| watchdog: main thread blocked; system_server killed | 176.8 s | 21.67 G |

The table is the final product run (`XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh`, `build/tests/out/boot.err`).
Times vary between runs on this host: in the earlier runs `System now ready` came at 98 to 102 s, and `StartAudioService`
took 10.2 s each time. The instruction counts are the comparison.

### Next blocker: zygote's data mirror (`/data_mirror`), then the watchdog

After `System now ready`, `ActivityManager` starts `com.android.permissioncontroller`. Zygote specializes the process and
aborts (the product's zygote, from the stock `zygote64` binary, `Zygote.cpp:792`):

```
[156687 ms 19129483699 insn] F/zygote64(1346): jni_internal.cc:825] JNI FatalError called: (com.android.permissioncontroller) frameworks/base/core/jni/com_android_internal_os_Zygote.cpp:792: Failed to mount /data_mirror/data_de/null/0/com.android.permissioncontroller to /data/user_de/0/com.android.permissioncontroller: No such file or directory
[176792 ms 21671151631 insn] W/Watchdog(1068): *** WATCHDOG KILLING SYSTEM PROCESS: Blocked in handler on main thread (main)
```

The stock init creates `/data_mirror` as a tmpfs (`mount tmpfs tmpfs /data_mirror nodev noexec nosuid mode=0700,uid=0,gid=1000`)
with its `data_ce`/`data_de` trees, and zygote bind-mounts each app's data from there (`/data_mirror/data_de/<vol>/<user>/<pkg>`)
onto `/data/user_de/<user>/<pkg>`. The product's rc does not make that tree, and the engine's mount namespace (`engine/mntns.c`)
has to make the bind. This is the next step: the `/data_mirror` tmpfs and its directories in the product rc, then the engine's
bind-mount support for the zygote's per-app namespace (`mntns.c`). The watchdog kill follows because the system_server's main
thread waits for the failed app start (the zygote's child dies with the fatal).

Also seen, not blocking: `E/AudioFlinger`/`APM_AudioPolicyManager: invalid volume index range` (the cause is not determined; the
minimal policy has no volume groups of its own), `Bluetooth service is null` (Bluetooth is not
configured out), `W/StorageManagerService: storaged not found` (no storaged), `Failed to read /proc/zoneinfo`, `Failed to
set SurfaceFlinger binder threadpool priority to SCHED_FIFO`, and dexopt children killed by signal 9 (`pidfd_open` is not
implemented; installd's dex2oat for `ExtServices`).

### Verification (B13, final sources)

Power: `pmset -g batt` showed AC power and `pmset -g` showed `lowpowermode 0` before the first and the last boot of this round; the
verification sequence ran in between, and its power state was not logged run by run.

| Command | Result |
|---|---|
| `make && make wasm-node && make wasm` | exit 0 (no warnings in the build log) |
| `tests/run.sh` (default) | **224 passed, 0 failed, 0 xfail** |
| `XIAOJI_JIT=0 tests/run.sh` | **224 passed, 0 failed, 0 xfail** |
| `XIAOJI_BOOT=1 tests/run.sh` (Google image) | **225 passed, 0 failed, 0 xfail** |
| `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh` (after `reset-data`, 300 s limit; the check now includes the audio and System-now-ready assertions) | **191 passed, 0 failed, 0 xfail** |
| `make test-web` | **all web checks passed** (exit 0), with Playwright's Chromium from `~/Library/Caches/ms-playwright` |
| `clang -std=c11 -Wall -Wextra -fsyntax-only` on the 10 changed engine files | 0 warnings, exit 0 each |
| `emcc -std=c11 -Wall -Wextra -fsyntax-only` on the same files (`$HOME/emsdk`) | 0 warnings, exit 0 each |
| remote product build (`tools/remote-build.sh`), with the audio sources touched so that all of them recompile (14 `clang++` steps) | exit 0; no warning from `device/xiaoji` in its log |
| `pgrep -fl "build/xiaoji|wasm-node/xiaoji"` at the end | empty (exit 1) |

### Gaps (B13)

- `/dev/xiaoji_audio` is not written: the HAL discards output. The engine has no sound device.
- The audio HAL's volume curves: `invalid volume index range` messages from the policy manager (see above; the cause is not
  determined).
- The real-time pacing (a sleep per write and per read) is not exercised: no client wrote audio during a boot, so its
  timing against AudioFlinger's mixer has not been checked.
- The next blocker above (the data mirror and per-app bind mounts), then `sys.boot_completed` and the launcher. Nothing
  about the launcher was reached.
- Telephony, NFC, UWB and Bluetooth are not configured out. Bluetooth prints `Bluetooth service is null` and does not block.
  Nothing else was turned off by property.
- The page cache's limits (above), and the w1 merge, which is not done here.

## B14: the data layout and the mount walk (part 1), the thermal 2.0 HAL, the saturating narrows, the accepted socket name, the mountinfo listing, and the boot past System now ready (stops at a bpf map open in libmeminfo)

Part 1 (commit a15d9d1) made the stock post-fs and post-fs-data data layout and the Linux-style mount walk. Part 2 (this section) is the
boot's next run: the thermal HAL, four engine gaps (the saturating narrowing SIMD ops, the name of an accepted socket, the mountinfo listing,
and a debug mapping of fatal signals), the product rc lines the boot needed, and the guest tests for the mount walk. The product boot reaches
`ActivityManager: System now ready` at about 98 s. Its next blocker is an abort in system_server at about 122 s: libmeminfo's GPU memory
read opens a pinned BPF map, the engine answers `bpf(2)` with ENOSYS, and libmeminfo aborts. The decision on that is below.
`sys.boot_completed` is not reached, and the launcher was not started.

### Part 1: the data layout and the mount walk (a15d9d1)

- Init actions, in the stock order (`image/xiaoji.rc`, `post-fs` and `post-fs-data`): `/storage` (a recursive bind of `/mnt/user/0`, then
  slave), the `/mnt/sdcard` and `/mnt/runtime/default/self/primary` symlinks, `/mnt/runtime/full/self`, `/data/user/0` as a recursive bind
  of `/data/data` (stock `init.rc` line 919: `mount none /data/data /data/user/0 bind rec`, after its `mkdir /data/user/0`), a `/data_mirror`
  tmpfs (`mode=0700,uid=0,gid=1000`) with `data_ce/null` and `data_de/null` bound from `/data/user` and `/data/user_de`, and the profile binds
  (`cur_profiles`, `ref_profiles`).
- Not made: the stock `mount rootfs rootfs / remount bind ro` (the engine's root has no flags to change), the restorecon lines (the engine
  keeps no labels), and the stock `rm /data/user/0` (the engine keeps `/data/user/0` as a directory, never a symlink).
- The mount walk (`engine/mntns.c`): a lookup enters a mount only as the child of the mount the walk is at, as Linux does. A tmpfs mounted
  over a path therefore hides the binds made below it before, and a mount made under a hidden mount is hidden with it.

### Thermal HAL: the 2.0 mock replaces the 1.0 service (`image/xiaoji.rc`)

- The product rc now carries the stock block of `/vendor/etc/init/android.hardware.thermal@2.0-service.rc`
  (`vendor.thermal-hal-2-0-mock`, both `interface` lines). It replaces the 1.0 block of B13, because both would register
  `@1.0::IThermal/default`.
- The mock registers `android.hardware.thermal@2.0::IThermal/default` (`HidlServiceManagement`, 2.5 s). HardwarePropertiesManagerService's
  1.0 lookup then completes (`StartHardwarePropertiesManagerService took to complete: 10ms`), so the 2.0 mock serves the 1.0 call, as its rc
  says.
- `tests/run.sh` (the XIAOJI_BOOT check) asserts the 2.0 registration and that StartHardwarePropertiesManagerService completes, in place of the
  1.0 registration.

### Engine and rc changes in this section

- `engine/simd.c`: the saturating narrows were missing. SQXTUN (U=1, opcode 0x12, and its `2` form) and SQXTN (U=0) and UQXTN (U=1) at
  opcode 0x14 are implemented. libhwui.so (system_server's binder thread) hit SQXTUN (`0x2e612800`) and then UQXTN (`0x2e214800`). QC is not
  kept (as the file's note says for the doubling forms).
- `engine/sock.c`: an accepted socket reports its listener's bound name from getsockname (Linux copies the address in unix_stream_connect).
  Without it, zygote's webview socket aborted with `Unsupported AF_UNIX socket (fd=45) with empty path`.
- `engine/mntns.c`: mountinfo (and /proc/mounts) lists every mount of the namespace in the order they were made, the covered ones too. It
  listed only the live mounts, so a bind hidden under a tmpfs was missing. The table holds only attached mounts (umount compacts it, and
  MNT_DETACH takes the subtree), so no further filter is needed.
- `engine/sched.c`: under `XIAOJI_DEBUG_FAULTS=1`, a fatal signal also prints the file and the offset of the pc, of x30, and of up to 12
  frame-pointer return addresses (`debug_region`). This is what identified the blocker below.
- `image/xiaoji.rc`: (1) `symlink /apex/com.android.vndk.current /apex/com.android.vndk.v33`. nativeloader reads
  `/apex/com.android.vndk.v<ro.vndk.version>/etc/llndk.libraries.<version>.txt` (vendor/build.prop: `ro.vndk.version=33`), and the image
  carries only `com.android.vndk.current`, which has that file. Without the name, the WebView relro process aborted in
  `ClassLoaderFactory.createClassloaderNamespace`. (2) `mkdir /data/misc/shared_relro` (stock: `mkdir /data/misc/shared_relro 0771
  shared_relro shared_relro`; the engine's mkdir takes the path only). The WebView relro file is still not created with this directory; the
  WebView process proceeds without it (see the blocker section).
- The VNDK link is written into the rootfs by the boot, in `build/rootfs-xiaoji/apex/` and in `build/rootfs/apex/` (by the Google-image
  XIAOJI_BOOT run). Its target is absolute and resolves only inside the guest, so it is dangling on the Mac. `tools/gen-ldconfig.py` still
  finds one VNDK directory in both trees (`com.android.vndk.current`), because it tests `isdir` on each entry, and the link does not resolve on
  the Mac (checked read-only after the link existed in both trees).

### Tests in this section

- `tests/guest/simd.c`: 18 narrowing cases, each over two input sets (36 output lines): SQXTN, UQXTN and SQXTUN, with and without the `2`
  form, from 16-, 32- and 64-bit sources. The inputs sit at the bounds (0x7f/0x80, 0x7fff/0x8000, 0xffff, 0x7fffffff/0x80000000,
  0xffffffff, 0x100000000), and v0 is a fixed pattern that the `2` forms keep.
- `tests/guest/unixsock.c`: `accepted getsockname path` (the accepted socket reports the listener's path).
- `tests/guest/mountns.c`, three sections added to the existing test:
  - the mount walk: a tmpfs over a path hides an earlier bind below it; the bind is back after the umount; a bind made below the tmpfs is
    visible; the mountinfo lines show the covered bind;
  - zygote's per-app isolation: a clone child (`clone(SIGCHLD)`) runs `unshare(CLONE_NEWNS)`, a private remount of `/`, a tmpfs over
    `zd/user`, and a bind of the app's directory from a mirror under it. The child sees the app file and not the parent's file; the parent
    sees neither the tmpfs nor the bind. The child and the parent each print their mountinfo;
  - a snapshot while an rbind and its submount copy are mounted (`rbind src2 mnt5`), before the MNT_DETACH.
  The recursive bind with a submount is also the existing `rbind src2 mnt3` case.
- The XIAOJI_BOOT product check (`XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1`) also asserts that no `Unsupported AF_UNIX socket` and no
  `Failed to mount /data_mirror` line appears.

### Milestones (product tree, final sources; guest time `[ms insn]`)

Run: `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh` (after `reset-data`, which the check does itself), so the table is the
`build/tests/out/boot.err` of that run, as in B13. Its power state is below. Times vary between runs (the boot11 run of the same sources
reached System now ready at 95.8 s).

| Milestone | time | instructions |
|---|---|---|
| `Registered android.hardware.thermal@2.0::IThermal/default` | 2.5 s | 0.30 G |
| `InitBeforeStartServices took to complete: 2466ms` | 45.4 s | 4.81 G |
| `StartLockSettingsService took to complete: 204ms` | 77.1 s | 8.17 G |
| `StartConnectivityService took to complete: 389ms` | 81.8 s | 8.74 G |
| `StartAudioService took to complete: 10395ms` | 92.6 s | 10.36 G |
| `StartHardwarePropertiesManagerService took to complete: 10ms` | 92.7 s | 10.37 G |
| `ActivityManager: System now ready` | 98.1 s | 10.99 G |
| `WebViewLibraryLoader: failed to create relro file` (not fatal) | 122.1 s | 13.72 G |
| system_server `Thread-10`: SIGABRT (the blocker below) | 122.3 s | 13.73 G |

### The next blocker: system_server's abort in libmeminfo's GPU memory read (a pinned BPF map)

After `System now ready`, ActivityManager reports memory. When a process dies, system_server's Thread-10 reads the GPU memory of the
processes through `libmeminfo.so`, and the read aborts. Final product run (`build/tests/out/boot.err`):

```
[122072 ms 13715964248 insn] E/WebViewLibraryLoader(1310): failed to create relro file
[122104 ms 13718792797 insn] I/ActivityManager(1068): Process WebViewLoader-arm64-v8a (pid 1310) has died: psvc PER
[122252 ms 13733401906 insn] F/libc(1068): Fatal signal 6 (SIGABRT), code -1 (SI_QUEUE) in tid 1331 (Thread-10), pid 1068 (<unknown>)
[122737 ms 13772939664 insn] W/webviewchromiumloader(1332): Failed to open relro file /data/misc/shared_relro/libwebviewchromium64.relro: No such file or directory
[122738 ms 13772944602 insn] W/WebViewLibraryLoader(1332): failed to load with relro file, proceeding without
```

The relro line is not fatal: the WebView process proceeds without the relro file. The abort is. The engine's `XIAOJI_DEBUG_FAULTS=1` mapping of the
same kind of abort (boot11, at 119.4 s) gives the frames: `libc.so` at file offset 0x8a914 (abort), a return address into `libmeminfo.so`
at 0x117c4, and then `libmeminfo.so` at 0xfa1c, which is inside `android::meminfo::ReadProcessGpuUsageKb` (its exported symbol is at 0xf9d4).
Disassembling `libmeminfo.so` (`llvm-objdump -d`, the emsdk copy) shows the function that aborts:

- at 0x1160c it calls `syscall(280, 7, ...)`. On arm64 280 is `bpf(2)` and 7 is `BPF_OBJ_GET`: it opens a pinned BPF map by path. The
  path is in `strings libmeminfo.so`: `/sys/fs/bpf/map_gpu_mem_gpu_mem_total_map`.
- the result is tested for the sign bit (`tbnz w8, #0x1f` at 0x11698), and a negative fd branches to `bl abort` at 0x117c0. The return
  address of that call is 0x117c4, which is the frame in the log.

So the abort is a fatal check of a failed `bpf(2)`. The engine answers `bpf(2)` with ENOSYS: the final log has `xiaoji: unimplemented
syscall 280` (`engine/syscall.c` returns `-LE_NOSYS` for an unimplemented number), and the same boot logs `nativeBpfFdGet failed: ENOSYS`
from `BpfInterfaceMapUpdater` and `NetworkStats`. The trigger, as far as the logs show, is ActivityManager's memory report after a
process death: the abort follows a process death by 0.2 to 0.4 s in each run (boot10: SystemUI, pid 1230, dies at 98.7 s and the abort is at
99.1 s; boot11: WebView, pid 1311, dies at 119.2 s and the abort at 119.4 s; the final run: pid 1310 at 122.1 s and 122.3 s). So the abort
time is not a fixed milestone: it follows the first process death after `System now ready`.

**Decision needed (not built).** B11 and B12 decided that the engine does not implement `bpf(2)`, that the real netd is replaced by
`xiaoji_netd`, and that the BPF-dependent framework calls must be checked for fatal failures (docs/boot.md, B11 "The BPF gate", and B12).
This call is fatal in libmeminfo, so the B11 policy does not cover it. The options:

1. **Recommended.** Answer `bpf(BPF_OBJ_GET)` for the one pinned path above with the fd of an empty map the engine keeps (a hash map whose
   lookups return ENOENT). This adds a narrow `bpf(2)` surface (map create for that type, object get by path, lookup), and it keeps the B11
   decision for everything else: no programs, no attach, no iptables. ActivityManager then gets no GPU memory, which the framework handles
   when the driver reports none. A guest test would check the error paths against the native oracle; a successful open has no oracle
   counterpart, because the native kernel has no such pinned map.
2. Keep the BPF gate as it is, and turn the GPU memory report off. The read is native and inside ActivityManager's memory report, and no
   property or rc line is known to disable it. Not checked further.
3. Implement `bpf(2)` for the framework's maps. That reverses the B11 decision, and it is the largest of the three.

Option 1 is my recommendation. It is a design decision, so nothing was built for it. After the decision: the engine's `bpf` case and the
test.

### SystemUI's first crash (not GL)

The first SystemUI process death (boot10, 98.7 s, pid 1230) is a Java NullPointerException in `SystemUIFactory.createFromConfig`
(`SystemUIFactory.java:82`, reached from `KeyguardSliceProvider`): `Attempt to invoke interface method 'void
android.content.pm.ILauncherApps.addOnAppsChangedListener(...)' on a null object reference`. The stack has no EGL, GLES or renderControl
frame, so the null GL backend is not the cause and was not changed. Why `ILauncherApps` is null is not determined (in boot11,
`StartLauncherAppsService` ran at 90.9 s). SystemUI's later deaths (pid 1503 in the final run at 213 s) were not analysed.

### Not reached, and not checked

- `sys.boot_completed=1`, the launcher, and the HOME intent: not reached. Launcher3QuickStep is in `/system_ext/priv-app` and is parsed
  (`Parse times for '/system_ext/priv-app/Launcher3QuickStep'`), but no HOME resolution is logged before the abort.
- Telephony, NFC, Bluetooth and UWB: not configured out in this section. No trace shows them blocking before the abort.
- The later restart cycles of system_server (`System now ready` again at 205 s and 281 s in boot11) are not analysed. The run ends at the
  init timeout (300 s).
- The relro file: the `shared_relro` directory exists, but WebView still does not create the file (the reason is not checked).

### Power and the host

`pmset -g batt` showed `Now drawing from 'AC Power'` and `pmset -g` showed `lowpowermode 0` before the first boot of this section and again
at the end of the verification (06:36). The power state was not logged run by run.

A Maintenance Sleep on AC (`pmset -g log`: 05:35:46 to 05:50:40) paused the first full `tests/run.sh` of this section, which is the run that
reported the four mountns failures. Those failures are the mountinfo difference fixed below, not a sleep effect; the suite was run again
after the fix (see the verification table). Earlier, boot5 (03:31 to 03:48) was paused the same way and is not counted. From then on every engine run
and every verification run is wrapped in `caffeinate -dims`.

### Verification (B14, final sources)

| Command | Result |
|---|---|
| `make && make wasm-node && make wasm` (final chain) | exit 0; no warning in the log |
| `tests/run.sh` (default), final sources | **228 passed, 0 failed, 0 xfail** (the mountns checks, O0 and O2, engine and node, pass against the native oracle) |
| `XIAOJI_JIT=0 tests/run.sh`, final sources | **228 passed, 0 failed, 0 xfail** |
| `XIAOJI_BOOT=1 tests/run.sh` (Google image; its boot creates the VNDK link in `build/rootfs/apex`) | **229 passed, 0 failed, 0 xfail** (run before the zygote section of `mountns.c` was reworked; the check does not use that test) |
| `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh` (reset-data first) | **195 passed, 0 failed, 0 xfail** (before the `mountns.c` rework; the engine and rc are unchanged since) |
| `make test-web` | all web checks passed, exit 0 (before the `mountns.c` rework; not affected by it) |
| `clang -std=c11 -Wall -Wextra -fsyntax-only` and `emcc` on `engine/simd.c`, `sock.c`, `sched.c`, `mntns.c` | 0 warnings each |
| `pgrep -fl "build/xiaoji|wasm-node/xiaoji"` at the end | empty |

Two runs of the final default suite before the repeat: the first (after the mountinfo fix, before the zygote rework) reported 228 passed
with the mountns checks failing; after the rework, one default run reported `FAIL init-ctl engine` (exit 0: its output lacked the last line,
`crasher stopped`). That case passed in three standalone runs, in the no-JIT run, and in the clean repeat (the last row of the final
default run); the cause of the intermittent failure is not determined.

The mountns mismatch found by the first default run: the engine's mountinfo left out the bind that a tmpfs covers
(`/wk/a/x rw bind`, the first bind), and the oracle lists it. The fix (`engine/mntns.c`, `mntns_text`) lists every mount of the
namespace in creation order, covered ones included. The zygote section and the rbind snapshot were then added to the test as the plan lists
them, and the engine output matched the oracle again (gcc builds, O0 and O2).

### Gaps (B14)

- The boot stops at the libmeminfo bpf abort (above). `System now ready` is reached; `sys.boot_completed` is not. The bpf decision is open.
- The VNDK link and the `shared_relro` directory are created by the boot in the rootfs (`build/rootfs-xiaoji`, and `build/rootfs` by its
  XIAOJI_BOOT run), not by the image build. The rootfs builder does not know about them yet.
- The mountinfo fields the test prints are the target, the options and the fs type. The device numbers and the `shared:` peer groups are not
  compared with Linux.

## B15: bpf(2) for the maps (the subset), the bpfloader's maps pinned at boot, and the boot past the libmeminfo abort (stops at a watchdog kill in systemReady)

Status: **partial.** The decision of B14 (option 3, maps only) is implemented: `bpf(2)` has the map subset and a synthetic bpffs,
and the maps of the image's BPF objects are created and pinned at init, as bpfloader does (without the programs). The libmeminfo read
that aborted in B14 no longer aborts, and system_server reaches `ActivityManager: System now ready` in these runs. The product boot does
not reach `sys.boot_completed=1`, the launcher or a HOME intent. It stops in one of two ways, with the same binary: an illegal
instruction in the `android.display` thread at about 128 s (`fcvtzs w10, s0, #6`, the scalar fixed-point convert, not decoded; it kills
system_server, and it makes the product check fail), or a watchdog kill at 211 s in `systemReady` (the first run, below). The product
boots ran in Low Power Mode (see Power), so their times are not comparable with B14's.

### Checkpoints

| # | Checkpoint | Result |
|---|---|---|
| 1 | bpf subset, and the guest test against the native oracle | Done. `tests/guest/bpfmap.c` (oracle as root in the builder VM, `sudo -n`, 81 lines of output) and the engine print the same 81 lines, including the bpffs tree and the open errnos. |
| 2 | map pre-creation, and the libmeminfo abort gone | Done. 49 maps pinned at init (`bpfloader: 49 maps pinned under /sys/fs/bpf`); no `Fatal signal` in the product boot. |
| 3 | sys.boot_completed, the launcher | Not reached. Two blockers, both in system_server's boot after `System now ready`: the fixed-point FCVTZS in `libhwui.so` (SIGILL, the next blocker below), and a watchdog kill at 211 s in `ConnectivityService.systemReadyInternal` (the first run). |

### The bpf subset (`engine/bpf.c`, `engine/bpf.h`)

Syscall 280 (`engine/syscall.c`) calls `bpf_syscall`. Its semantics are those of Linux 6.12 (`kernel/bpf/syscall.c`, `hashtab.c`,
`arraymap.c`, `inode.c`, read from the builder VM's source), and every errno below comes from the native oracle, not from memory:

- `BPF_MAP_CREATE` for HASH (1), LRU_HASH (9, as a HASH: a full map gives E2BIG where Linux evicts), PERCPU_HASH (5) and
  LRU_PERCPU_HASH (10), ARRAY (2) and PERCPU_ARRAY (6), each PERCPU type with one CPU slot (a lookup fills the first slot of the
  caller's buffer; an update reads the first slot), and DEVMAP_HASH (25) as a HASH (the image's `tether_dev_map`; no interface is
  checked and no program redirects into it). Any other known type: EOPNOTSUPP. A type number outside the enum: EINVAL. The create
  checks (zero sizes, key size 4 for arrays, flags, E2BIG for an element past 4 MiB or a per-CPU value past 32 KiB) are the kernel's.
- `BPF_MAP_LOOKUP_ELEM`, `UPDATE_ELEM` (BPF_ANY, BPF_NOEXIST, BPF_EXIST; BPF_F_LOCK is EINVAL), `DELETE_ELEM`, `GET_NEXT_KEY`
  (a key that is missing starts at the first element; a last key gives ENOENT; an array's out-of-range key starts at 0).
- `BPF_OBJ_PIN` and `BPF_OBJ_GET` on a bpffs at `/sys/fs/bpf` (the root is mode 01777 and cannot be removed: EBUSY). A pin is a
  regular file with the owner and mode of the pin (0600 for a pin made by a process, umask applied). `BPF_OBJ_GET` checks the access
  the `file_flags` ask for (`BPF_F_RDONLY`, `BPF_F_WRONLY`, else read and write) against the owner, group or other bits;
  CAP_DAC_OVERRIDE bypasses them. A get of a directory is EACCES. `bpf_fd` must be 0 for a get.
- Filesystem: `mkdir`, `rmdir` and `unlink` under the bpffs (EISDIR for unlink on a directory, ENOTEMPTY for a non-empty rmdir),
  `stat` (`S_IFDIR` or `S_IFREG` with the node's owner and mode), `faccessat`, `openat` (EIO for an existing pin, which has no
  file operations because a map without BTF uses `bpffs_obj_fops`; ENOENT for a missing name; EACCES for O_CREAT on a missing
  name). A bpffs directory cannot be listed yet (`getdents` on it is not built).
- `BPF_OBJ_GET_INFO_BY_FD` for a map fd: type, id, key and value sizes, max entries, flags and name (88 bytes, as
  `struct bpf_map_info`).
- Map fds are `VFS_BPF` descriptions (`vfs.c`): `read` and `write` are EINVAL, `lseek` is ESPIPE, `fstat` is an anonymous inode
  (`S_IFREG|0600`), and the description holds a reference to the map. fork, dup and SCM_RIGHTS share the description, so the map is
  shared engine-wide.
- Every other command: EPERM for a known command (`PROG_LOAD`, `PROG_ATTACH`, links, tokens, iterators and the batch commands), and
  EINVAL for a number above the last command of 6.12. No program is loaded, so nothing reads a program's output.

`XIAOJI_DEBUG_BPF=1` prints one stderr line per failing command (`bpf: cmd N errno E`) and one per `BPF_OBJ_GET`
(`bpf: OBJ_GET <path> file_flags <flags>`).

### The bpfloader's maps (`bpf_preload`, `engine/bpf.c`; called from `init.c` after `bpffs_init`)

The directories and pin prefixes are the loader's (`system/bpf/bpfloader/BpfLoader.cpp`, 0.19): `/apex/com.android.tethering/etc/bpf/`
(prefix `tethering/`) and its `netd_shared/`, `netd_readonly/`, `net_shared/` and `net_private/` subdirectories, `/system/etc/bpf/`
(no prefix) and `/vendor/etc/bpf/` (`vendor/`). Each `*.o` directly in a directory is read through the guest tree, and for each one:

- the object's `bpfloader_min_ver` and `bpfloader_max_ver` sections gate the whole object (the image's `offload.o` and `test.o`
  have a minimum of 2; the engine's loader version is 0.19, `0x13`);
- `size_of_bpf_map_def` (default 32) is how much of each def is read, and the fields past it take the loader's defaults;
- each map's def gives the type, key and value sizes, max entries and flags, the owner (`uid:gid`) and the mode. A def with a
  `pin_subdir` is not built (the image's objects have none), and a map whose pin exists is reused, as the loader does;
- the map is pinned at `/sys/fs/bpf/<prefix>map_<file>_<map>` (`<file>` empty when `shared`), in its prefix directory (mode 01777,
  made first, as `createSysFsBpfSubDir` does), with the def's owner and mode.

The product image has 49 maps in 9 objects with maps (`/system/etc/bpf`: `gpu_mem`, `gpu_work`, `time_in_state`; the apex: `offload` and
`test` for tethering, and the four `net_shared` and `netd_shared` objects; `fuse_media` has none). Their types are 1, 2, 5, 6 and 25 (no other type is in the image).
`XIAOJI_DEBUG_BPF=1` also prints each pin. No `bpf.progs_loaded` property is set: no program is loaded. Whether a caller waits for it
was not checked.

### What the boot asks of the maps (per caller, from the product boot's trace)

The trace below is the product boot's, with `XIAOJI_DEBUG_BPF=1` (the log of the run in Milestones: `/private/tmp/claude-501/-Users-rosdyanakusuma-Ross-projects-xiaonux/02218bd1-8168-4b28-a0fa-30c789f8f133/scratchpad/boot-cp2.err`). The trace has no pid, so the callers are not named. It lists each `BPF_OBJ_GET` and its result:

| `BPF_OBJ_GET` path (file_flags) | Result | Note |
|---|---|---|
| `netd_shared/map_netd_*` (uid_owner, uid_permission, uid_counterset, cookie_tag, stats A and B, app_uid_stats, iface_index_name, iface_stats, configuration) (0) | ok | the pins exist; the trace's one `GET_NEXT_KEY` ENOENT is the end of a key walk, not a missing map |
| `net_shared/map_clatd_*`, `map_dscp_policy_*`, `map_block_blocked_ports_map` (0) | ok | |
| `net_shared/prog_block_bind4_block_port` (0x8, RDONLY) | ENOENT (2) | a program pin: no program is loaded, so nothing is pinned under a `prog_` name |
| `KernelCpuUid{FreqTime,ActiveTime,ClusterTime}BpfMapReader` (logged at 80 s, not in the trace) | "Failed to start tracking" (a warning, not fatal) | the map pins are present; the readers' own program is not loaded (not checked in the framework source) |

The libmeminfo read (`map_gpu_mem_gpu_mem_total_map`) is the one B14 aborted on. In this boot it is not called before the watchdog
kill; its own access check is exercised by `tests/guest/bpfpin.c` (the init case below), not by the boot.

The errnos are the kernel's, not chosen per caller: an absent pin is ENOENT, and a program command is EPERM (no CAP_BPF). Within
this boot, no caller got EPERM from a program command, because none was issued (the trace shows no `PROG_*` command).

### The init-level check (`tests/init/bpf.rc`, `tests/expect/init-bpf.out`, `tests/guest/bpfpin.c`)

A service runs as `system` with the `graphics` group (the way system_server does) and reads the gpu_mem pin as libmeminfo does.
It prints the pin's owner and mode (`0x1a0` = 0640, uid 0, gid 1003), the groups, then a get with the read-write flags (EACCES: the
group has no write bit), a get with `BPF_F_RDONLY` (ok), the info (type 1, key 8, value 8, max 1024), a lookup of a missing key
(ENOENT) and an update through the read-only fd (EPERM). The root oracle cannot check these: root bypasses the access checks. The
expected text is the Linux DAC result for those modes and was not run against a non-root oracle (the builder VM has no gid 1003
user). It is checked for engine and Node by `tests/run.sh`, together with the bpfloader summary line.

### Milestones (product tree, `make reset-data` first; guest time and instruction count)

Run: `XIAOJI_DEBUG_BPF=1 build/xiaoji --logcat --init image/xiaoji.rc --root build/rootfs-xiaoji --init-timeout 300`, the same
arguments as the `XIAOJI_BOOT` check. The times are the engine's guest clock (`[ms insn]` in the log).

| Milestone | time | instructions |
|---|---|---|
| `Registered android.hardware.thermal@2.0::IThermal/default` | 4.3 s | 0.30 G |
| `InitBeforeStartServices took to complete` | 79.7 s | 4.81 G |
| `StartAudioService took to complete: 10659ms` | 153.9 s | 9.81 G |
| `ActivityManager: System now ready` | 163.8 s | 10.47 G |
| `WATCHDOG KILLING SYSTEM PROCESS: Blocked in handler on main thread` | 211.2 s | 13.53 G |
| `failed to create relro file` (WebView, not fatal) | 212.4 s | 13.60 G |
| `*** GOODBYE!` (watchdog, system_server killed with SIGKILL) | 213.0 s | 13.64 G |
| `Fatal signal` (libmeminfo or any abort) | none | none |

The run ended at the 300 s init timeout (exit 0). `sys.boot_completed` and `Launcher` were not logged.

### Next blocker: the fixed-point FCVTZS in libhwui (android.display, SIGILL)

The unimplemented instruction is `0x1e18e80a`, at pc `0x6fffe7c4014c` (file offset `0x44014c` of
`build/rootfs-xiaoji/system/lib64/libhwui.so`). `llvm-objdump -d` (the emsdk copy) gives `fcvtzs w10, s0, #0x6`: the scalar FCVTZS
with fixed-point scale (bit 21 clear, scale 58 in bits 15:10, so 6 fractional bits), used on a single-precision value in a thread of
system_server (`android.display`). `engine/simd.c` `fp_to_int` decodes only the integer form (bit 21 set). The fix is to add the
fixed-point form: `x * 2^fbits`, truncated toward zero, saturated as the integer form, with NaN giving 0 (`fbits = 64 - scale`, and
`fbits` 1..32 for a 32-bit result). It needs a case in `tests/guest/simd.c` checked against the native oracle. Not built here.

### The watchdog kill in `systemReady` (PermissionMonitor, `registerReceiver`), the first run

In the first product run (`boot-cp2`, with `XIAOJI_DEBUG_BPF=1`) no SIGILL occurred, and the watchdog killed system_server instead.

The watchdog's annotated stack (the log of the run in Milestones, lines at 211 s to 213 s) puts the main thread here:

```
at android.app.ContextImpl.registerReceiverInternal(ContextImpl.java:1798)
at com.android.server.connectivity.PermissionMonitor.startMonitoring(PermissionMonitor.java:400)
at com.android.server.ConnectivityService.systemReadyInternal(ConnectivityService.java:3107)
at com.android.server.ConnectivityService.systemReady(ConnectivityService.java:3089)
at android.net.ConnectivityManager.systemReady(ConnectivityManager.java:1162)
at com.android.server.am.ActivityManagerService.systemReady(ActivityManagerService.java:8141)
at com.android.server.SystemServer.startOtherServices(SystemServer.java:2801)
```

The main thread was in this call for about 60 s of guest time (the watchdog's limit), and the log shows the same main thread's
`Slow dispatch` lines before it (`android.ui` 231 ms, `android.fg` 450 ms). I did not determine why the registration takes that long:
the boot ran in Low Power Mode, and the trace has no BPF call on the main thread in that window. A cascade follows: the
WebView relro process dies (`webviewchromiumloader`: `libdl.so` not found, not fatal), and the permission controller's
`DeadSystemException` is the death of system_server, not a cause.

**Recommendation** (not built): run this boot once more with `XIAOJI_DEBUG_FAULTS=1` and the power state recorded at each point
(`pmset -g`, both before and after), and with the trace of the main thread's binder transactions. If the time is the Low Power Mode
throttle, rerun on AC with Low Power Mode off before any change. If it is a binder call that never returns, the fix belongs in the
service it waits for (the blocked transaction's target is in the log).

### Power

`pmset -g` recorded `lowpowermode 1` before this boot (`/private/tmp/claude-501/-Users-rosdyanakusuma-Ross-projects-xiaonux/02218bd1-8168-4b28-a0fa-30c789f8f133/scratchpad/pmset-boot15-start.txt`) and again after it
(`/private/tmp/claude-501/-Users-rosdyanakusuma-Ross-projects-xiaonux/02218bd1-8168-4b28-a0fa-30c789f8f133/scratchpad/pmset-boot15-end.txt`), with the Mac on `Battery Power` at the end. The memory note requires AC and Low Power Mode off before timing; this
boot did not meet that, so its milestone times are not used for comparison.

### Verification (B15, final sources)

Final sources. The suites ran after the last code change (`bpfpin` excluded from the oracle cases, and the product check's `Fatal signal` condition).

| Command | Result |
|---|---|
| `make && make wasm-node && make wasm` | exit 0 |
| `tests/run.sh` (default) | **234 passed, 0 failed, 0 xfail** (228 before, plus `bpfmap-O0/O2` on engine and Node, and `init-bpf` on engine and Node) |
| `XIAOJI_JIT=0 tests/run.sh` | **234 passed, 0 failed, 0 xfail** |
| `XIAOJI_BOOT=1 tests/run.sh` (Google image) | **235 passed, 0 failed, 0 xfail** |
| `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh` (product) | **200 passed, 1 failed**: the zygote boot check. Its one failing condition is the new `! grep 'Fatal signal'`, and the log has the SIGILL in `android.display` (below). Every other condition of the check matched. |
| `make test-web` | all web checks passed, exit 0 |
| `clang -std=c11 -Wall -Wextra -fsyntax-only` and `emcc -std=c11 -Wall -Wextra -fsyntax-only` on `engine/bpf.c`, `vfs.c`, `sysfs.c`, `syscall.c`, `init.c` | 0 warnings each |
| `pgrep -fl "build/xiaoji|wasm-node/xiaoji"` at the end | no process of this checkout. Two processes of another checkout (`/Users/rosdyanakusuma/Ross/projects/xiaoji-w1`) are listed; they were not touched |

The product check's failure is a real boot failure, not a test artefact: the same SIGILL was reproduced in a foreground run
(`XIAOJI_DEBUG_FAULTS=1`): `xiaoji: unimplemented A64 instruction 0x1e18e80a at pc 0x6fffe7c4014c`,
`xiaoji: region ... libhwui.so off 0x200000: pc is file offset 0x44014c`, `F/libc(1068): Fatal signal 4 (SIGILL)` in `android.display`,
and `exit: pid 1068 (LockSettingsSer) killed by signal 4`. That run's `System now ready` line comes before the SIGILL (line order; it
has no time stamps, since `XIAOJI_STAMP` was not set).

### Gaps (B15)

- Not reached: `sys.boot_completed=1`, the launcher and the HOME intent. The next blocker is the fixed-point FCVTZS in libhwui (not built,
  above); the watchdog kill of the first run is not diagnosed beyond its stack.
- The product check (`XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1`) fails on the SIGILL. That is the check working, not a regression of the
  bpf work, and it stays failing until the instruction is decoded.
- No programs: `PROG_LOAD` and the attach commands are EPERM, so no BPF program runs and the time_in_state and tracking readers
  stay in their "not tracking" state.
- LRU maps are HASH maps (no eviction), per-CPU maps have one CPU slot, and DEVMAP_HASH has no interface checks.
- The bpffs cannot be listed (`getdents` on `/sys/fs/bpf` is not built), and the pins' open returns EIO for the non-BTF path only.
- `bpf.progs_loaded` is not set. Whether a caller waits on it was not checked.
- The guest test and the init case use the engine's own uid and group checks for the gpu_mem pin; the oracle cannot check them.

## B16: the clone limit, the networks off, and the boot to `sys.boot_completed=1` with the launcher home

Status: **partial.** On the product tree (`build/rootfs-xiaoji`, boot13) the boot reaches `sys.boot_completed=1` at 268.9 s
(31.3 G instructions), the launcher is the HOME activity at 285.0 s, and the launcher, SystemUI and system_server are alive for
the 60 s after the marker (`boot_alive_60s` in `tests/run.sh`, window 269 to 329 s). **SystemUI's first instance is still hung.** It
logs its last line at 274.8 s (after its first `eglMakeCurrent`), then nothing, and its service ANR kills it at 344.7 s, after the
window. The restarted instance lives to the end of the run. The demo's timing rule holds; the hang is open (Blocker 2). The product
check is not green (see the verification table).

### Goal 1: `clone` EAGAIN and RLIMIT_NPROC

- **Root cause.** `engine/sched.c` kept a fixed table of `SCHED_MAX_THREADS` (256) thread slots and refused `clone` with EAGAIN
  once `live_count()` reached 256, for the whole machine. The Wi-Fi HAL's lazy start (hwservicemanager's `pthread_create`) hit
  that limit. `poll.c` (deadline waiters) and `sock.c` (timed socket waits) had fixed tables of the same size.
- **Fix.** The thread table starts at 256 slots and doubles (`free_slot`); the waiter and timed-wait tables grow the same way.
  The limit is Linux's `RLIMIT_NPROC` (`copy_process`; `nproc_refused` in `sched.c`): a new task is refused with EAGAIN when the
  tasks of the caller's real uid reach the soft limit. The tasks are its live threads plus one for each unreaped zombie process of
  that uid. Uid 0, a process with CAP_SYS_RESOURCE or CAP_SYS_ADMIN, and an infinite limit are not refused. The engine's default
  limit is infinite (`proc.c`), as before.
- **Also from the inherited work:** `pidfd_open` (434; `VFS_PIDFD`, readable when the process ends), the `attr` files that
  libselinux opens (`procfs.c`, `attr_names`), and the keystore2 selinuxfs class (below).
- **Tests** (engine and node, O0 and O2, against the native oracle): `tests/guest/threads-many.c` (three rounds of 600 threads
  alive at once, 2000 create/join pairs, and an RLIMIT_NPROC of 20 for uid 31337: 19 threads, then 19 more after the join),
  `nproc-root.c` (uid 0 is not refused), `pidfd.c`, `attrs.c`. The RLIMIT tests run as root in the builder VM (`sudo -n`).

### Goal 2: networks off (the Android way)

The framework's start condition is taken from `SystemServer.startOtherServices` (the remote AOSP tree, read only) and the
connectivity and permission modules. "Boot evidence" is what the boot11 log shows.

| Stack | Start condition in the framework | Product change | Boot evidence |
|---|---|---|---|
| Wi-Fi | `FEATURE_WIFI` (StartWifi, StartWifiScanning); `FEATURE_WIFI_RTT`, `FEATURE_WIFI_AWARE`, `FEATURE_WIFI_DIRECT` (SystemServer around line 1954) | the `android.hardware.wifi*.xml` feature files are not copied (`xiaoji_vendor.mk`); the `android.hardware.wifi@1.0-service` HAL package is removed from `xiaoji_vendor.mk` | no `StartWifi` line; the HAL binary is not in `vendor/bin/hw` |
| Bluetooth / BLE | `FEATURE_BLUETOOTH` (SystemServer line 1630: "No Bluetooth Service" path) | the bluetooth feature XMLs are not copied; `bt_vhci_forwarder`, `android.hardware.bluetooth@1.1-service.btlinux` and `android.hardware.bluetooth.audio@2.1-impl` are removed from `xiaoji_vendor.mk` | no `StartBluetoothService` line |
| Ethernet | `FEATURE_ETHERNET` (read by AdbService; no ethernet service in SystemServer) | not declared | no ethernet service starts. NetworkStack looks up `ethernet` and gets "No service published" (harmless) |
| UWB | `FEATURE_UWB` (SystemServer line 2749) | not declared | no `UwbService` line; the `com.android.uwb` APEX is still installed |
| NFC | `FEATURE_NFC` | not declared; NfcNci is removed by the handheld fork | **not off:** `com.android.se` (the SecureElement app, `system/app`) starts. Recommendation below |
| Telephony | `FEATURE_TELEPHONY` (SystemServer lines 2615 and 3012); `FEATURE_MICROPHONE`, `FEATURE_TELECOM` or `FEATURE_TELEPHONY` start TelecomLoaderService (line 1492) | `android.hardware.telephony*` and `android.software.telecom` are not declared. `telephony_system.mk` is no longer inherited (ONS, CarrierDefaultApp, CallLogBackup, CellBroadcastLegacyApp; the cell broadcast APEX is still installed by another makefile), and `TeleService` (process `com.android.phone`) is removed from `xiaoji_handheld_system.mk` | boot6 had the `com.android.phone` process restarting in a loop with no radio (330 `IRadioModem` waits, "crashed too many times"); boot10 and boot11 have none. `Telecom` stays (the mic feature starts its loader) |
| Location network | `FEATURE_LOCATION_NETWORK` has no consumer in `services/`; `config_networkLocationProviderPackageName` is `@null` | `android.hardware.location.network` is not declared | none |

Kept, as the framework requires: `xiaoji_netd`, ConnectivityService (`StartConnectivityService` completes at 197.8 s in boot11),
NetworkStack, NetworkPolicy and NetworkStats. TelephonyRegistry starts unconditionally in SystemServer, so no feature turns it off.

Not done, with reasons:

- **A `PRODUCT_PACKAGES` filter does not remove inherited packages.** The first attempt (`filter-out` in `xiaoji_arm64.mk`) left
  every package installed: the build collects each inherited file's words, so `base_system.mk` (`com.android.wifi`,
  `wificond`, `com.android.btservices`) and the goldfish `vendor.mk` (`wpa_supplicant`, `hostapd`, `libgoldfish-rild`) still
  install. They are not started. Removing them needs the inherit lines changed; left for a follow-up.
- **`ro.radio.noril=yes`** is set as requested, but no framework reader was found in `frameworks/base`,
  `frameworks/opt/telephony` or `packages/services/Telephony`, so it has no effect.
- **`config_voice_capable`** stays `true` (no overlay was built; the boot does not need it).
- **Provision stays.** Its HOME activity writes `DEVICE_PROVISIONED` and `USER_SETUP_COMPLETE`, then disables itself. Removing it
  leaves the device unprovisioned. An attempt to drop it from the system_ext list was reverted for that reason.

### Lean list for the demo (started processes, boot11, with a recommendation; none removed unilaterally)

| Process | Why it runs | Recommendation |
|---|---|---|
| `com.android.se` | SecureElement app (`system/app`), no SE HAL | remove: NFC and SE are off |
| `com.android.cellbroadcastreceiver.module` | cell broadcast APEX (`com.android.cellbroadcast`), started by a broadcast | remove the APEX (features.md: "never") |
| `com.android.emulator.multidisplay` | the goldfish MultiDisplayProvider (emulator multi-display) | remove: a tab has one display |
| `com.android.dynsystem` (and `:dynsystem`) | DynamicSystem installer, started by a boot broadcast | remove (features.md: not in the base) |
| `com.android.localtransport` | backup transport (features.md: backup "never") | remove |
| `com.android.sdksetup` | the emulator's setup wizard; it held the HOME fallback at first | remove after a clean boot shows the launcher as the only HOME app |
| `com.android.providers.calendar` | calendar provider, started by an upgrade broadcast (Calendar is an add-on) | move to the add-on pack |
| `WebViewLoader-arm64-v8a` | WebView's relro creation (it fails: no relro file) | keep lazy (features.md: base-lazy), and fix the relro file |
| `com.android.keychain`, `android.ext.services`, `com.android.remoteprovisioner`, `com.android.packageinstaller`, `com.android.provision`, `com.android.externalstorage`, `com.android.documentsui`, `android.process.media`, `android.process.acore`, `com.android.providers.media.module`, `com.android.settings`, `com.android.permissioncontroller`, `com.android.networkstack.process`, `com.android.launcher3`, `com.android.systemui` | base services (features.md) | keep |

### Other changes in this section

- **Binder: a read ends after `BR_DEAD_BINDER`** (`engine/binder.c`, `bread`). The kernel's `binder_thread_read` ends the read
  after a death notification ("DEAD_BINDER notifications can cause transactions"). The engine batched it. libbinder's handler for
  the first death made a sync call, and `talkWithDriver` does not read while commands are unread, so the next command answered
  that call. Boot traces showed `BR_REPLY` reaching a looper (`*** BAD COMMAND`, value 0x80407203; boot4), and the aborts that
  followed are the system_server SIGABRTs of the earlier boots. Oracle test: `tests/guest/binder-death-batch.c`. Each node is sent
  by its own child (one request per sender), the parent replies, takes both handles and requests both deaths; the output is the
  commands per read and the sorted cookies. The kernel and the engine print the same lines for O0 and O2, three runs each. An
  earlier version of the test (the children sent their requests from one thread) returned both deaths in one read on the engine,
  and it hung on the native oracle; that version is not the test.
- **The GL requests the generator skips** (`engine/gl_host.c`, `not_api_decode`). The generator marks 66 calls (58 void) of
  the GLES1 and GLES2 protocols `not_api` and leaves their decoders out. An undecoded request sets the pipe's `broken`
  flag (`goldfish.c`), and the pipe stops answering, so the guest's thread waits for its reply. Boot12 shows it: the first
  such call, `glDrawElementsInstancedOffsetAEMU` (2315), arrived at 263.0 s, the same moment SystemUI's log stopped. The
  table decodes all 66 by their fixed sizes (`gen_report.txt`) and answers with the return type's size: zero, except the sync
  handles (nonzero), GL_TRUE for the two boolean sync calls, and GL_CONDITION_SATISFIED for the wait. Boot13 has no undecoded
  request. It did not stop SystemUI's hang (Blocker 2).
- **keystore2's selinux class** (`engine/selinuxfs.c`): libselinux's `discover_class` rejects a perm value of 0 and stores
  `value - 1`, so the permissions run 1 to 16. The inherited table had 0 to 15, so every keystore2 check failed with "Unknown
  class keystore2" and `KeyStore.state` returned SYSTEM_ERROR, which aborted LockSettingsService.
- **gatekeeperd and its software HAL** (`image/xiaoji.rc`, with `mkdir /data/misc/gatekeeper`). LockSettingsService calls
  `IGateKeeperService.reportDeviceSetupComplete` when `DEVICE_PROVISIONED` changes (the Provision app sets it). With no service
  the call is a NullPointerException in system_server (boot5, 293 s).
- **`/mnt/installer/0`** (`image/xiaoji.rc`). Zygote specializes an installer app by mounting `/mnt/installer/0` on `/storage`.
  The directory is made by vold on a device; the engine has no vold, and the product rc did not make it. The mount failed with
  "Failed to mount /mnt/installer/0 to /storage" (`JNI FatalError` in `Zygote.cpp`) and aborted `com.android.packageinstaller`
  each time its `TemporaryFileManager` broadcast started (four aborts in boot9 and boot10). The rc now makes it as a bind of user
  0's view. boot11 has no such abort.
- **`ro.hw_timeout_multiplier=4`** (`xiaoji_arm64.mk`). `ActiveServices.SERVICE_TIMEOUT` is 20 s times
  `Build.HW_TIMEOUT_MULTIPLIER`, read from this property. The reason is restarted SystemUI instances: their `SystemUIService` onCreate
  ran past 20 s (boot6 and boot7: ANR about 20.7 s after the restart). The multiplier does **not** fix the first instance's hang: its
  ANR fires at 4 x 20 s after its service started (76 s after the marker in boot10 and boot11, against 16.7 s with no multiplier).
  It only moves the kill out of the 60 s window. The engine runs guest code at about 110 million instructions a second on the host.
- **Diagnostics under `XIAOJI_DEBUG_FAULTS=1`:** each `BR_FAILED_REPLY` branch in `binder.c` prints its cause (target not mapped,
  a request buffer not built, the object that did not translate, a reply not built), and selinuxfs opens are logged (`vfs.c`).
  The end of a boot is marked by `xiaoji: sys.boot_completed=1` (`logd.c` `logd_note`, called from `props.c`), stamped like the
  logcat lines under `XIAOJI_STAMP=1`.
- **`tests/run.sh`, the product check:** the timeout is 540 s (480 s for the engine), which covers the marker at about 261 s plus 60 s.
  `boot_alive_60s` requires the marker, the latest launcher, SystemUI and system_server started before it, and no exit line of any of
  them within 60 s after it (exit lines carry no time, so the last stamped line before them gives the time). The check also requires
  `START u0 ... HOME ... cmp=com.android.launcher3/` and no `Unknown class`. It was tested on synthetic logs (pass, exit inside the
  window, exit after it, short log, restart before the marker) and on boot10 and boot11 (pass).

### Milestones (product tree, `build/rootfs-xiaoji`, boot13; guest time and instructions)

Run: `XIAOJI_DEBUG_FAULTS=1 XIAOJI_STAMP=1 build/xiaoji --logcat --init image/xiaoji.rc --root build/rootfs-xiaoji --init-timeout 600`
after `make reset-data`. `pmset -g` before and after: `lowpowermode 0`, `Now drawing from 'AC Power'`.

| Milestone | time | instructions |
|---|---|---|
| `Registered android.hardware.thermal@2.0::IThermal/default` | 3.2 s | 0.38 G |
| `System server process 1076 has been created` | 44.1 s | 4.60 G |
| `InitBeforeStartServices took to complete` | 47.1 s | 4.91 G |
| `StartLockSettingsService took to complete` | 193.9 s | 22.56 G |
| `StartConnectivityService took to complete` | 198.4 s | 23.11 G |
| `StartAudioService took to complete` | 209.3 s | 24.78 G |
| `ActivityManager: System now ready` | 214.7 s | 25.40 G |
| `xiaoji: sys.boot_completed=1` | 268.9 s | 31.30 G |
| `START u0 ... HOME ... cmp=com.android.launcher3/.uioverrides.QuickstepLauncher` | 285.0 s | 32.94 G |
| `ActivityManager: ANR in com.android.systemui` (first instance, `SystemUIAuxiliaryDumpService`; killed; the restart, pid 1816, lives) | 344.7 s | 39.73 G |

`StartVibratorManagerService` completes at 65.0 s (6.74 G). `StartWifi`, `StartBluetoothService` and `StartUwbService` do not
appear. The 60 s window is 269 to 329 s.

### Blockers, with the evidence

1. **Fixed (boot11):** the installer's `/mnt/installer/0` abort (above).
2. **Open: SystemUI's first instance hangs after its first EGL context.** Its last log line is `eglMakeCurrent` at 262 s (boot10,
   pid 1250) and 274.8 s (boot13), and nothing else until its ANR. The GL pipe was broken by undecoded requests until the table
   (above); that is fixed, and the hang remains, so it has another cause. The DropBox entries for the ANRs are 0 bytes, so the
   stack is not known. What is known: the goldfish sync device answers `QUEUE_WORK` with a signalled eventfd (`goldfish.c`), so
   a fence wait returns; SurfaceFlinger's `sync_file_info returned NULL` lines (`E/Fence`, pid 1007) are its reads of those fds
   as sync files, and they recur in the boots that run fine. The next step is a stack of the first instance's main thread: the
   engine has no way yet to dump a blocked guest thread's stack on SIGQUIT in this case (the ANR dump is written to DropBox).
3. **Open: the `txn_build` refusal.** At 210 s (boot10) `SdkSandboxManagerService` sends a `BINDER` object to servicemanager, and
   `xiaoji: binder: pid 1076 -> pid 1001: object 0 at data offset 100 (type 0x73622a85) of 136: -22` refuses it: the engine's
   node at that address has a different cookie (`xl_binder`, the kernel's cookie check in `binder_translate_binder`). So the old
   node is still referenced by servicemanager: a reference-lifetime gap (the `binder-node-reuse` case of B4e covers the simple
   version). Its effect here is that `sdk_sandbox` is not published; system_server is not aborted.
4. **Open: the wasm exit trap in the product check** (`prop-list node` and `binder-sm node`): see the verification table.
5. **Open: `StorageManagerService`**: `IStoraged.onUserStarted` is a null call (`storaged` is not in the product; boot8, 268 s).

### Verification (B16, final sources)

| Command | Result |
|---|---|
| `make && make wasm-node && make wasm` | exit 0; no warning in the log |
| `tests/run.sh` (default) | **250 passed, 0 failed, 0 xfail** (includes `binder-death-batch`, `threads-many`, `nproc-root`, `pidfd` and `attrs` on engine and node, O0 and O2) |
| `XIAOJI_JIT=0 tests/run.sh` | **250 passed, 0 failed, 0 xfail** |
| `XIAOJI_BOOT=1 tests/run.sh` (Google image) | **251 passed, 0 failed, 0 xfail** |
| `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh` (product) | **215 passed, 2 failed: NOT green.** The zygote boot check passes (the boot conditions, no `Fatal signal`, no undecoded GL request, the launcher as HOME, `sys.boot_completed=1`, and `boot_alive_60s`). The two failures are `prop-list node` and `binder-sm node`: each exits with status 1 after printing the expected output, from a wasm `RuntimeError: memory access out of bounds` in the exit path (`exitRuntime` to `quit`). They failed in all four product runs made since the boot became reachable. They passed when run alone with the harness's environment and file output (four runs), and under a native build of the engine with AddressSanitizer (four runs, no report). The cause is open |
| `make test-web` | all web checks passed, exit 0 |
| `clang -std=c11 -Wall -Wextra -fsyntax-only` and `emcc -std=c11 -Wall -Wextra -fsyntax-only` on the 17 changed engine files (`binder.c gl_host.c gl_null.c poll.c powersupply.c proc.c procfs.c sched.c selinuxfs.c signals.c simd.c sock.c syscall.c timers.c vfs.c logd.c props.c`) | 0 warnings, 0 errors |
| The native oracle for `binder-death-batch` (builder VM, `sudo -n`, O0 and O2, three runs each) | identical output; the engine matches it |
| `pgrep -fl "build/xiaoji|wasm-node/xiaoji"` at the end | nothing of this work. Leftover `threads-many-O0` oracle processes (root, from an earlier hung run) were killed in the builder VM |

Remaining, in order of importance: SystemUI's first instance hangs after its first EGL context (Blocker 2); the two wasm exit traps
of the product check (above); the `txn_build` refusal (Blocker 3); `StorageManagerService`'s null `IStoraged` (Blocker 5); and the
inherited Wi-Fi, Bluetooth and telephony packages that the image still carries (not started).

Files changed in this section (all under the repo): `engine/binder.c`, `engine/vfs.c`, `engine/selinuxfs.c`, `engine/logd.c`,
`engine/logd.h`, `engine/props.c`, `engine/sched.c`, `engine/poll.c`, `engine/sock.c`, `engine/procfs.c`, `engine/syscall.c`,
`engine/signals.c`, `engine/simd.c`, `engine/gl_host.c` (the `not_api` table), `engine/gl_null.c` (the inherited changes only, as
they were; the real WebGL2 backend is separate), `image/xiaoji.rc` (keystore, gatekeeper, `/data/misc/gatekeeper`, `/mnt/installer/0`,
`/data/media`), `tests/run.sh` (the product check's conditions, the timeout, the sudo list), `tests/guest/binder-death-batch.c`,
`tests/guest/threads-many.c`, `tests/guest/nproc-root.c`, `tests/guest/pidfd.c`, `tests/guest/attrs.c`, and the product under
`aosp/device/xiaoji/` (`xiaoji_arm64.mk`, `xiaoji_vendor.mk`, `xiaoji_system.mk`, `xiaoji_handheld_system.mk`,
`permissions/handheld_core_hardware.xml`). `tools/remote-build.sh` synced that tree to the remote, and `tools/fetch-remote-rootfs.sh`
refreshed `build/rootfs-xiaoji` (it regenerates the apex-info list and linker config).
