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
