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

Status: **done up to the checkpoints.** The real `/system/bin/servicemanager` runs as the
binder context manager. `service list` shows `manager`, and `service check manager` finds it.
`binderDriverInterfaceTest` passes all 17 cases with servicemanager running, in the engine and
in Node. `binderLibTest` runs case by case with servicemanager and its forked servers:
48 gtest cases in the report: 26 pass, 7 fail, 1 is skipped (Freeze), 10 pass their test body and then hang in TearDown until the watchdog kills them, and 4 parameterized RPC names match no test ("0 tests", counted as passed by the script, not real passes). `binderSafeInterfaceTest` and `binderThroughputTest` were not run
in this pass. Nothing is committed.

### Design as built

- **Files.** `engine/binder.c` and `binder.h` (the driver), `engine/poll.c` and `poll.h`
  (readiness, ppoll, epoll), `engine/fdobj.c` and `fdobj.h` (eventfd and timerfd),
  `engine/selinuxfs.c` and `selinuxfs.h` (the synthetic selinuxfs). Hooks: `engine/vfs.c`
  and `vfs.h` (fd kinds, synthetic directories and files, `vfs_resolve`), `engine/sysfs.c`
  (ioctl, statfs, the new syscalls), `engine/syscall.c`, `engine/signals.c` and `signals.h`
  (sigqueue, the per-signal value), `engine/procfs.c` (`attr/current`), `engine/sched.c`,
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
- **ppoll, epoll, eventfd, timerfd** (`fdobj.c`, `poll.c`). One key serves every poll
  waiter, and each readiness change calls `poll_notify`. epoll is level-triggered, with a
  16-byte `epoll_event`. eventfd2 supports EFD_SEMAPHORE, EFD_NONBLOCK and EFD_CLOEXEC.
  timerfd supports CLOCK_MONOTONIC and CLOCK_BOOTTIME, relative and absolute, and intervals.
  Both clocks are the host's monotonic clock.

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

### Results (B3)

- `make test`: **124 passed, 0 failed** (122 before, minus the 14-case binder entry, plus the
  17-case entry and `binder-sm`, each in engine and Node).
- `XIAOJI_JIT=0 tests/run.sh`: **124 passed, 0 failed**.
- `make test-web`: all checks passed.
- binderLibTest (report run, engine): 
  - **Pass (26):** CannotUseBinderAfterFork, WasParceled, NopTransaction, NopTransactionOneway,
    NopTransactionClear, SetError, GetId, CallBack, BinderCallContextGuard,
    DeathNotificationStrongRef, PassFile, PassParcelFileDescriptor, PromoteLocal,
    LocalGetExtension, CheckHandleZeroBinderHighBitsZeroCookie, CheckNoHeaderMappedInUser,
    WorkSourceUnsetByDefault, WorkSourceSet, WorkSourceSetWithoutPropagation,
    WorkSourceCleared, WorkSourceRestored, PropagateFlagSet, PropagateFlagCleared,
    PropagateFlagRestored, WorkSourcePropagatedForAllFollowingBinderCalls,
    ServiceNotifications.Unregister.
  - **Skipped (1):** Freeze (the cgroup freezer file is absent, so the test skips itself).
  - **Body ok, hangs in TearDown (10):** PtrSize, IndirectGetId2, IndirectGetId3, AddServer,
    DeathNotificationMultiple, DeathNotificationThread, RemoteGetExtension, VectorSent,
    BufRejected, WeakRejected. Cause: see Gaps.
  - **Fail (7):**
    - SchedPolicySet and InheritRt: the engine does not implement `sched_getparam` (121) and
      `sched_setscheduler` (119). Cause known; not implemented in this pass.
    - FreedBinder: an earlier run gave `FAILED_TRANSACTION` (-2147483646) where the test
      expects -32, and a SIGSEGV at address 0x4 in libutils. Not investigated further.
    - OnewayQueueing: fails and is killed by the watchdog. Not investigated.
    - GotSid: fails. Not investigated (the sender's security context is `u:r:kernel:s0`).
    - SetRpcClientDebug and SetRpcClientDebugTwice: binder RPC needs sockets the engine does
      not provide (`BinderLibRpcTest`). Not investigated.
  - **Matches no test (4):** the `BinderLibTest/BinderLibRpcTestP.*` names, which the per-case
    filter does not match.
- Warnings: `clang -Wall -Wextra -Wno-unused-parameter -c` on each changed engine file
  (`binder.c`, `signals.c`, `vfs.c`, `selinuxfs.c`, `procfs.c`, `sysfs.c`, `syscall.c`): 0
  warnings. `emcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -c` on the same files:
  0 warnings.

### Gaps (B3)

- **binderLibTest is not fully green.** The per-case list above is the state; the causes
  are listed under it. A case whose body passes and then hangs in TearDown is the largest
  group. TearDown sends `EXIT` (oneway) to the main server, then waits for the server's pid.
  The server's `EXIT` handler waits for its children, which are the servers that `addServer`
  forked. Those children exit only when their last strong ref is released. A temporary log
  of node count changes (removed) showed decrements reaching zero for some nodes, but I did
  not find which child's count is kept. This is the known cause for that group, not fixed.
- **Not run:** `binderSafeInterfaceTest`, `binderThroughputTest`.
- **Not verified by any run:** oneway queueing order, fd passing (FD and FDA), PTR
  scatter-gather (`BC_TRANSACTION_SG`), `BR_TRANSACTION_SEC_CTX` beyond the fixed context,
  death notifications beyond the request and clear pair, the count acknowledgements
  (`BC_ACQUIRE_DONE`, `BC_INCREFS_DONE`) in general, and the sigqueue EPERM path.
- **Reply path counts.** Only the sender's thread is used for a transaction's counts. The
  reply path uses the same rule, but no test here checks a reply that carries a binder.
- `BC_ATTEMPT_ACQUIRE` and `BC_ACQUIRE_RESULT` are accepted and ignored.
- `eventfd` writes that would overflow return EAGAIN even on a blocking fd. `ppoll` ignores
  its signal mask. epoll is level-triggered only.
- Threads leave the proc when `BINDER_THREAD_EXIT` runs or when the proc closes.
- The clock is the host's monotonic clock. `CLOCK_REALTIME` timers are EINVAL.
- The engine's AF_UNIX DGRAM sockets (liblog's logd path) are refused (EPROTONOSUPPORT), so
  Android log lines from servicemanager and binderLibTest do not reach a log. Errors that
  servicemanager logs are therefore invisible, and the CHECK messages are lost.
