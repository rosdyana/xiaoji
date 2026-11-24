# Testing

The guest suite is `tests/run.sh`. It compares the native engine (and the Node build of the engine, when it is
present) with native runs of the same guest programs in the OrbStack builder VM, and with recorded expectations
(`tests/expect/`, `build/oracle/`). `make test` builds what it needs and then runs it. `make test-web` is the
browser check (headless Chromium). This page covers how the suite runs locally and on the build machine, the
job limits, the rules that keep parallel cases apart, and the timings.

## Local: tests/run.sh

```sh
make test                      # build the engine, wasm-node and guest tests, then tests/run.sh
tests/run.sh                   # the suite alone (needs build/xiaoji, build/wasm-node, build/tests/guest)
XIAOJI_JOBS=1 tests/run.sh     # serial, the order the suite had before it ran in parallel
```

The report is printed in the original order, whatever the job count, and saved to `build/tests/out/report.txt`.
The per-case outputs stay in `build/tests/out/` (`<name>.<engine|node>.{out,err}`). The count is the number of
`PASS`, `FAIL` and `XFAIL` lines. At 3a04429 it is 228 passed, 0 failed, 0 xfail, with the serial run, the parallel
run, `XIAOJI_JIT=0` and the recorded-oracle mode; the reports are byte-identical.

### Job limits

| Variable | Default | Meaning |
|---|---|---|
| `XIAOJI_JOBS` | 4 | Concurrency budget in units. A native guest job (one base name, engine only) weighs 1, a guest job with a Node run weighs 2, the root lane weighs 2. A job starts when its weight fits. `1` is the serial order. |
| `XIAOJI_ORACLE_JOBS` | 2 | Builder-VM oracle commands (`orb -m builder`) at once. A regenerated oracle is the only thing that calls the builder VM. |
| `XIAOJI_ORACLE` | `live` | `recorded` uses `build/oracle` as it is and never calls the builder VM (the remote runner uses it). |

Peak RSS, measured with `/usr/bin/time -l` on the Mac: a freestanding guest program (`control-O0`) takes 2.6 MB under
the engine and 125 MB under Node; the ART case `hello-jit` (a root case) takes 108 MB under the engine and 325 MB
under Node. The unit weights follow that: a Node run is the heavy part of a job. The default budget of 4 keeps the
M2 (8 GB, already swapping) well inside its memory.

### Modes

| Variable | Effect |
|---|---|
| `XIAOJI_JIT=0` | The engine's block translator is off (the interpreter only). The same report as with the JIT. |
| `XIAOJI_BOOT=1` | Boots the zygote of `image/xiaoji.rc` (about 34 s natively) and checks the log. It runs after the other jobs drain. |
| `XIAOJI_ROOT=build/rootfs-xiaoji` | Runs the root cases on another tree (the product tree). The builder-VM oracle cases are skipped. |
| `XIAOJI_BINDERLIB=1` | Adds the binderLibTest report (report only, slow). |
| `XIAOJI_SKIP="exec"` | Leaves the named guest programs out (base names, space-separated). The report says so on a `note` line. The remote runner uses it for `exec`; see below. |

### Shared state, and what keeps cases apart

- **Root cases are one serial lane.** Every case with `--root` (rootfs, props, binder, init, metadata, the boot)
  writes `<root>/.xiaoji-meta` when the engine exits (`engine/main.c`, `engine/meta.c`), and it writes the shared
  `.xiaoji-meta.tmp` first. The engine rewrites the sidecar from its loaded copy on every exit, so two `--root`
  engines on one root would race: a concurrent exit can drop a record that the metadata case reads back. The lane
  runs them in their original order. Root cases also write under `<root>/data/local/tmp` (the copied test programs,
  the init report files).
- **Guest jobs are one job per base name.** `tests/guest/<name>-O0` and `-O2` run in the same job, in that order.
  Several programs use fixed host paths (`/tmp/xiaoji-exec-*`, `/tmp/xiaoji-unixsock-*`, `/tmp/xiaoji-flock-test`,
  `/tmp/xiaoji-utimes-test`) and per-name work directories (`build/tests/{fmeta,lazymap,mountns,shmmap}-work`), and
  the two variants of a name share them. Different base names do not share paths.
- **Timed boots run alone.** `XIAOJI_BOOT=1` and binderLibTest run after the lanes drain, one after the other.
  `XIAOJI_BOOT=1` on a `*-xiaoji` tree first runs `tools/reset-data.py`.
- **Oracle commands are limited** to `XIAOJI_ORACLE_JOBS` at a time (`orb_acquire` in `tests/run.sh`).
- **One checkout, one run.** Do not run two `tests/run.sh` in the same worktree at once: they share the root
  trees, the sidecar, and the host `/tmp` paths above. Another worktree has its own `build/` and is safe, except
  for the host `/tmp` paths.

## Remote: tools/remote-test.sh

The build machine (`build-host`: Ubuntu 20.04, x86_64, 20 threads, 64 GB, gcc 9.4, no clang, no node) runs the
same suite in `~/xiaoji-remote/`. Nothing else on that machine is written: `~/xiaoji-aosp` is not touched, no sudo,
and the shell profile is not changed.

```sh
tools/remote-test.sh               # sync, build, run (all three)
tools/remote-test.sh sync          # sources, build/gen, rootfs trees, guest binaries, oracle outputs, wasm-node
tools/remote-test.sh build         # Node (user-local) and the gcc build of the native engine
XIAOJI_JIT=0 tools/remote-test.sh run   # one run with a mode variable (also XIAOJI_BOOT, XIAOJI_ROOT, XIAOJI_BINDERLIB)
XIAOJI_REMOTE_JOBS=12 tools/remote-test.sh run
```

- **Sync.** The tracked sources are `git ls-files` of the working tree (the manifest `~/xiaoji-remote/.sources`
  removes files that are no longer tracked). The rootfs trees are rsynced with deltas. Their `data/` keeps only the
  static inputs, `data/hello.dex` and `data/nativetest64/` (the binder conformance binaries); the rest is written
  by runs and boots, so it is not copied. The metadata sidecar is not copied either. The guest binaries, the
  recorded oracle outputs (`build/oracle/*.{out,rc}`) and `build/wasm-node/` come from the Mac. The first sync sends
  the whole trees (about 2.8 GB, 67 minutes over this link); later syncs send only changes (5 s when nothing changed).
- **Node.** There is no Node on the remote. `tools/remote-test.sh build` installs the same version as the local
  `node`, from nodejs.org, into `~/xiaoji-remote/tools/node`, and checks the tarball against `SHASUMS256.txt`. The
  Node cases use that directory through `PATH` for the remote run only. Emscripten is not installed on the remote:
  `build/wasm-node/` is built on the Mac (`make wasm-node`) and synced.
- **Build.** `make engine CC=gcc` on the remote, a fresh link, warnings counted. The engine builds warning-free with
  gcc 9 (`-Wall -Wextra`), clang and emcc.
- **Run.** `tests/run.sh` with `XIAOJI_ORACLE=recorded`, `XIAOJI_JOBS` (default 16) and `XIAOJI_SKIP=exec` (see
  below). The fixed `/tmp/xiaoji-*` files are removed on the remote before and after the run. The report comes back
  to `build/tests/remote-out/report.txt`, and the outputs of each failing case are copied to
  `build/tests/remote-out/`. The exit status is that of `tests/run.sh`.
- **Skipped on the remote: exec.** `exec` (the guest's execve and friends) is left out, with a note in the report.
  Its expected output names the checkout path (`argv[0]` and `/proc/self/exe` of the guest program, which the
  builder VM and the Mac wrote as `/Users/...`), so it cannot match a checkout elsewhere. It also depends on the
  `/tmp` state: `exec-O2` expects `/tmp/xiaoji-exec-text` to be executable, which only holds when the engine itself
  created that file in the same process (a file from an earlier run is read back as mode 0644). From a clean `/tmp`
  the O0-then-O2 order gives `-13` for exec-O2 on the Mac as well, so the local suite passes only with the files
  left by an earlier engine (mode 0755). Fixing the test (unlink its fixtures before creating them, and print the
  program's base name) is a small follow-up in `tests/guest/exec.c`; it is not done here. Set
  `XIAOJI_REMOTE_SKIP=` (empty) to run it on the remote anyway.
- **Oracle.** The native-oracle comparisons need arm64 Linux, which is the builder VM on the Mac. The remote uses
  the recorded outputs and never calls the builder VM. After a guest binary changes, refresh the oracle on the Mac
  (`make oracle` for the rootfs cases, or delete `build/oracle/guest/<name>.rc` and run `make test`), then run
  `tools/remote-test.sh sync`.

### What stays local

- The oracle refresh (`tools/oracle.sh`, `make oracle`, `build/oracle/guest` regeneration): it needs the OrbStack
  builder VM, which the remote does not have.
- `make test-web` (headless Chromium) and the wasm builds (`make wasm`, `make wasm-node`).
- Anything that runs in the builder VM (`make guest-tests`, the oracle).

## Timings

Measured on the Mac (M2, 8 GB) and on the remote (build-host, Ubuntu 20.04, gcc 9.4). The local serial and
parallel runs were back to back, with no other engine process running before or after either one (checked with
`ps`). The remote numbers are one run each.

| Run | Wall time | Result |
|---|---|---|
| Local, serial (`XIAOJI_JOBS=1`) | 132 s | 228 passed, 0 failed |
| Local, parallel (default, 4) | 110 s | 228 passed, 0 failed; report byte-identical to serial |
| Remote, first sync (whole trees, about 2.8 GB) | 67 min | rc 0 |
| Remote, sync after a change (delta) | 5 s | |
| Remote, build (Node install, gcc engine) | 20 s | gcc 9.4.0, 0 warnings |
| Remote, default suite (`XIAOJI_SKIP=exec`) | 109 to 114 s, with the sync | 224 passed, 0 failed (exec: 4 cases not run) |
| Remote, product tree (`XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1`) | 383 s | 190 passed, 1 failed (the zygote boot check) |
| Local, product tree | not completed | see below |

The local product-tree run was started, and I stopped it: B15's boot started on the same machine while it ran, and
the boot check is timing-sensitive. The remote product failure is the boot check (`XIAOJI_BOOT`): the boot reaches
the same milestones as the Google-image boot (zygote preload, the fork and entry of system_server, StartPowerManager,
InitPowerManagement, StartDisplayManager, the allocator and SurfaceFlinger's start), and then the product tree's
audio HAL is missing. `vendor.audio-hal: cannot read /vendor/bin/hw/xiaoji_audio_hal` (the binary is built from
`aosp/device/xiaoji/audio` by the AOSP build; the copy of `build/rootfs-xiaoji` on the Mac has no such binary either),
so AudioService and `System now ready` never come, and the boot is stopped at its 300 s limit. So the remote and the
Mac agree on this tree: the check needs the audio HAL in the product tree, which is not in this copy.

The root lane is the critical path: every `--root` case is serial (the sidecar), so the parallel run cannot be faster
than that lane. The guest jobs fit in its shadow, which is why the parallel gain on the Mac is about 17%. Cloned roots
would remove that limit, but they would need a separate sidecar per root and are not done.
