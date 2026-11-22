# Boot milestones

Status of the path to a booted Android guest: init, servicemanager with binder,
zygote and system_server. Android is multi-process, so the engine needs real
guest processes first (B1).

## B1: multiple guest processes

Status: **not started in the engine.** Step 0 (the oracle cases and the test
harness) is done. The engine changes are the remaining work.

### What exists

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

### Test results at this step

Run after adding the cases, with no engine change:

- `tests/run.sh`: 80 passed, 0 failed, 12 xfail (6 cases x engine and Node).
- `XIAOJI_JIT=0 tests/run.sh`: 80 passed, 0 failed, 12 xfail.
- Not run: `make wasm`, `make test-web`, and the per-file emcc and clang
  warning checks. No engine source changed, so they are unaffected.

### Design (planned, not built)

Checked against the code at commit `78520a3`:

- **Address space.** `engine/mem.c` keeps the page table (`l1`), the region
  list (`regs`), and the TLBs as file-scope statics. A process switch swaps
  `l1`, `regs`, `nregs` and `cregs`, and flushes the TLBs.
- **TLBs stay at fixed addresses.** `jit.c` reads `mem_jit_tlb()` once, in
  `jit_enabled()`, and bakes the addresses into generated code. The TLB
  arrays therefore stay where they are. A switch clears them instead of
  swapping pointers.
- **JIT.** Blocks are keyed by (host page, VA page), so a host page shared
  after fork is valid in both processes. The chain cache `fast[]` is keyed
  by PC alone, so `jit_chain_flush()` must run on every switch. It already
  runs inside `tlb_flush()`.
- **Copy-on-write.** A new PTE bit (`PTE_COW`), kept apart from `PTE_W`, marks
  a private page shared after fork. `page_lookup` with write intent copies the
  page when its share count is above 1, and clears the bit. `mem_poke`,
  `page_any` and the TLB paths all go through it. Freeing a host page must call
  `jit_page_changed()` first, as `mem_unmap` and `mem_drop` do now.
- **Futex keys** are VA-only today. They need the process id added.
- **Fault path.** `mem_segv` longjmps to the `fault_jb` set by the run loop.
  That loop is not checked yet, and the switch must happen only between
  slices.

### Remaining work, in order

1. Refactor per-process state (mem, vfs fd table, signal handlers, brk,
   cwd) behind a current-process pointer. No behaviour change, and the
   instruction counts must stay identical.
2. Refcounted open-file objects, with CLOEXEC per fd. Pipes, `dup3`,
   `fcntl` (`F_GETFL` / `F_SETFL`).
3. Share counts, address-space clone, fork and vfork, per-process exit,
   zombies, `wait4` and `waitid`, SIGCHLD, reparenting to the initial process.
4. `execve`, and `#!` scripts.
5. `sh-pipe` passes. That is the first checkpoint.
6. `tests/guest/procs.c` (copy-on-write, exec, wait status, pipe, vfork,
   SIGCHLD, reparenting), `/proc/<pid>`, process groups and sessions, `kill`
   to a group.

### Gaps

- Nothing in the engine is per-process yet, so `make test` cannot exercise
  the design.
- The JIT on/off instruction-count check for multi-process runs is not
  possible until step 3.
