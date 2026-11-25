#!/bin/bash
# Guest tests: compare the engine (and the Node build, if present) with native
# runs of the same binaries in the builder VM (M0 freestanding programs), and
# with the oracle outputs of the guest's own dynamically linked toybox binaries
# (rootfs section, M1). Skipped if build/rootfs is missing.
#
# Oracle outputs are cached in build/oracle/guest/<name>.{out,rc} and
# regenerated whenever the binary is newer than the cache.
# Exit status is nonzero if any non-stretch case fails.
#
# Parallel run (T1). Scheduling, in order of the report:
#   - One job per guest base name (tests/guest/<name>-O0 and -O2 run in the same job, in that order): these
#     share host /tmp paths and work directories. Native jobs weigh 1 unit, jobs with a Node run 2.
#   - One root lane: every case with --root (rootfs, props, binder, init, metadata). Each --root engine
#     exit rewrites <root>/.xiaoji-meta and its .tmp file (engine/meta.c, engine/main.c), so these run one
#     after another, in their original order. The lane weighs 2 (it runs Node too).
#   - After the lanes drain: the zygote boot (XIAOJI_BOOT=1, with reset-data), binderlib and the input boot
#     (XIAOJI_INPUT_BOOT=1), one after the other, so no timed boot shares the machine with another case.
#   XIAOJI_JOBS (default 4) is the unit budget for the first two groups. XIAOJI_JOBS=1 is the serial order.
#   XIAOJI_ORACLE_JOBS (default 2) limits the builder-VM oracle commands at once (orb_acquire).
#   XIAOJI_ORACLE=recorded uses build/oracle and build/oracle/guest as they are and never calls orb (used by
#   tools/remote-test.sh; the oracle refresh stays on the Mac).
# The report is assembled in the original order, so it is the same whatever the job count. The pass, fail and
# xfail counts are the PASS, FAIL and XFAIL lines of the report.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
# The guest root for the expectation cases and the boot check. XIAOJI_ROOT selects another tree (for example
# build/rootfs-xiaoji); the cases that compare with the builder-VM oracle use build/rootfs and are skipped then.
RFS=${XIAOJI_ROOT:-build/rootfs}
OWN_TREE=0
[ -n "${XIAOJI_ROOT:-}" ] && OWN_TREE=1

ORB=/Applications/OrbStack.app/Contents/MacOS/bin/orb
ENGINE=build/xiaoji
NODE_JS=build/wasm-node/xiaoji.js
ORACLE=build/oracle/guest
OUT=build/tests/out
RES=$OUT/.run
JOBS=${XIAOJI_JOBS:-4}
ORACLE_JOBS=${XIAOJI_ORACLE_JOBS:-2}
ORACLE_MODE=${XIAOJI_ORACLE:-live}
case "$JOBS" in '' | *[!0-9]* | 0)
    echo "xiaoji: XIAOJI_JOBS must be a positive integer (got '$JOBS')" >&2
    exit 2
    ;;
esac
case "$ORACLE_JOBS" in '' | *[!0-9]* | 0)
    echo "xiaoji: XIAOJI_ORACLE_JOBS must be a positive integer (got '$ORACLE_JOBS')" >&2
    exit 2
    ;;
esac
case "$ORACLE_MODE" in live | recorded) ;; *)
    echo "xiaoji: XIAOJI_ORACLE must be live or recorded (got '$ORACLE_MODE')" >&2
    exit 2
    ;;
esac

rm -rf "$RES"
mkdir -p "$ORACLE" "$OUT" "$RES"

# Extra arguments per test program (the name without -O0/-O2).
args_for() {
    case "$1" in
    args) echo "alpha beta" ;;
    mountns) echo "$ROOT/build/tests/mountns-work" ;; # absolute: a guest process starts in "/"
    fmeta) echo "$ROOT/build/tests/fmeta-work" ;;
    lazymap) echo "$ROOT/build/tests/lazymap-work" ;;
    shmmap) echo "$ROOT/build/tests/shmmap-work" ;;
    *) echo "" ;;
    esac
}

# The native oracle's scratch directory. fmeta runs its chown and chmod on the builder VM's own filesystem:
# the repository is a Mac mount there, which does not keep owners the way Linux does.
oracle_args_for() {
    case "$1" in
    fmeta) echo "/tmp/xiaoji-fmeta-work" ;;
    shmmap) echo "/tmp/xiaoji-shmmap-work" ;;
    *) args_for "$1" ;;
    esac
}

# Rootfs cases: the argv of each oracle case (tools/oracle.sh), without its
# "exec". The oracle outputs are build/oracle/<case>.{out,rc}. rootfs_argv sets
# the array ARGV (arrays keep quoted arguments such as sh -c 'echo $((6*7))').
rootfs_argv() {
    case "$1" in
    toybox-echo) ARGV=(/system/bin/toybox echo hi from bionic) ;;
    toybox-ls) ARGV=(/system/bin/toybox ls /system/bin/linker64) ;;
    toybox-uname) ARGV=(/system/bin/toybox uname -m) ;;
    toybox-wc) ARGV=(/system/bin/toybox wc -c /system/build.prop) ;;
    toybox-sha) ARGV=(/system/bin/toybox sha256sum /apex/com.android.runtime/bin/linker64) ;;
    toybox-lsdir) ARGV=(/system/bin/toybox ls /apex/com.android.art/bin) ;;
    toybox-cat) ARGV=(/system/bin/toybox cat /system/etc/hosts) ;;
    sh-arith) ARGV=(/system/bin/sh -c 'echo $((6*7))') ;;
    sh-pipe) ARGV=(/system/bin/sh -c 'echo hello | /system/bin/toybox tr a-z A-Z') ;;
    sh-pipeline) ARGV=(/system/bin/sh -c '/system/bin/toybox ls /system/bin | /system/bin/toybox wc -l') ;;
    sh-subshell) ARGV=(/system/bin/sh -c 'x=$(/system/bin/toybox uname -m); echo "arch=$x"; (exit 3); echo "rc=$?"') ;;
    sh-bg) ARGV=(/system/bin/sh -c '/system/bin/toybox sleep 0.1 & wait; echo done') ;;
    sh-loop) ARGV=(/system/bin/sh -c 'for i in 1 2 3; do /system/bin/toybox echo $i; done | /system/bin/toybox tac') ;;
    sh-script) ARGV=(/system/bin/sh -c 'mkdir -p /data/local/tmp; echo "#!/system/bin/sh" > /data/local/tmp/s.sh; echo "echo script ran \$1" >> /data/local/tmp/s.sh; chmod 755 /data/local/tmp/s.sh; /data/local/tmp/s.sh arg1') ;;
    hello-int) ARGV=(/apex/com.android.art/bin/dalvikvm64 -Xint -Ximage:/system/framework/boot.art:/system/framework/boot-framework.art -cp /data/hello.dex Hello) ;;
    hello-jit) ARGV=(/apex/com.android.art/bin/dalvikvm64 -Ximage:/system/framework/boot.art:/system/framework/boot-framework.art -cp /data/hello.dex Hello) ;;
    esac
}
ROOTFS_CASES="toybox-echo toybox-ls toybox-uname toybox-wc toybox-sha toybox-lsdir toybox-cat sh-arith hello-int hello-jit sh-pipe sh-pipeline sh-subshell sh-bg sh-loop sh-script"
# Rootfs cases the engine does not pass yet (multiple processes, B1): a failure is
# XFAIL, not FAIL. Remove a name here once it passes.
ROOTFS_PENDING=""

# The guest environment of the ART cases, from tools/guest-env.sh, as --env
# arguments (the same variables the oracle exports with . /data/env.sh).
GUEST_ENV_ARGS=()
while IFS= read -r kv; do
    GUEST_ENV_ARGS+=(--env "$kv")
done < <(bash -c '. "$1"; for v in ANDROID_ROOT ANDROID_DATA ANDROID_ART_ROOT ANDROID_I18N_ROOT ANDROID_TZDATA_ROOT BOOTCLASSPATH; do printf "%s=%s\n" "$v" "${!v}"; done' _ "$ROOT/tools/guest-env.sh")

# Every binary in build/tests/guest/*-O0 and *-O2 is a case. glibc_hello* is a
# stretch case: failure is XFAIL, not FAIL.

# The builder VM runs at most ORACLE_JOBS oracle commands at once. A slot is a directory under $RES (mkdir is
# atomic); orb_acquire sets ORB_SLOT.
orb_acquire() {
    local s
    while :; do
        s=1
        while [ "$s" -le "$ORACLE_JOBS" ]; do
            if mkdir "$RES/orb.$s" 2>/dev/null; then
                ORB_SLOT=$s
                return 0
            fi
            s=$((s + 1))
        done
        sleep 0.2
    done
}

# Creates build/oracle/guest/<name>.{out,rc} by running the binary natively in
# the builder VM, if the cache is missing or older than the binary. With
# XIAOJI_ORACLE=recorded the cache is used as it is and the builder VM is not called.
make_oracle() {
    local name=$1 bin=$2 args=$3
    local out="$ORACLE/$name.out" rc="$ORACLE/$name.rc"
    if [ -f "$rc" ] && { [ "$ORACLE_MODE" = recorded ] || [ "$rc" -nt "$bin" ]; }; then
        return 0
    fi
    [ "$ORACLE_MODE" = recorded ] && return 1
    # mountns makes mounts, and binder-sg opens the builder VM's /dev/binder (root-only there): their
    # oracles run as root (passwordless sudo in the builder VM). threads-many needs root to start its NPROC child as
    # another uid (setuid from 0), as the engine does; nproc-root drops capabilities as root (B16).
    local runas=""
    case "$name" in mountns-* | binder-sg-* | binder-node-reuse-* | binder-read-pair-* | binder-death-batch-* | creds-* | fmeta-* | bpfmap-* | threads-many-* | nproc-root-*) runas="sudo -n" ;; esac
    orb_acquire
    "$ORB" -m builder sh -c "cd '$ROOT' && $runas '$bin' $args > '$out' 2>/dev/null; echo \$? > '$rc'" \
        >/dev/null 2>&1
    rmdir "$RES/orb.$ORB_SLOT"
    [ -f "$rc" ]
}

# Runs one case through one runner and compares stdout and exit code with the
# oracle. Prints PASS, FAIL or XFAIL for the case.
check() {
    local label=$1 runner=$2 name=$3 bin=$4 args=$5 stretch=$6
    local tag="$name $label"
    local out="$OUT/$name.$label.out" err="$OUT/$name.$label.err"
    local expect_out="$ORACLE/$name.out" expect_rc
    expect_rc=$(cat "$ORACLE/$name.rc")

    if [ "$runner" = engine ]; then
        "$ENGINE" "$bin" $args >"$out" 2>"$err"
    else
        node "$NODE_JS" "$bin" $args >"$out" 2>"$err"
    fi
    local rc=$?

    if cmp -s "$out" "$expect_out" && [ "$rc" = "$expect_rc" ]; then
        echo "PASS  $tag"
        return 0
    fi
    local why="stdout/exit differ: exit $rc, expected $expect_rc"
    if [ -s "$err" ]; then
        why="$why; stderr: $(tail -n 1 "$err")"
    fi
    if [ "$stretch" = 1 ]; then
        echo "XFAIL $tag ($why)"
    else
        echo "FAIL  $tag ($why)"
        diff "$expect_out" "$out" | head -n 6 | sed 's/^/        /'
    fi
    return 1
}

# The instruction count the Node build reports (XIAOJI_STATS=1) with the JIT on (1) or
# off (0).
icount_of() {
    env XIAOJI_STATS=1 XIAOJI_JIT="$1" node "$NODE_JS" "$2" $3 2>&1 >/dev/null |
        sed -n 's/^xiaoji: \([0-9][0-9]*\) instructions in .*/\1/p' | tail -n 1
}

# A deterministic case must count the same instructions with the JIT on and off. Hello
# and the threads case vary from run to run, so they are not checked. jitthreads waits on a
# futex the same way every run, so its count is checked.
check_icount() {
    local name=$1 bin=$2 args=$3 on off
    on=$(icount_of 1 "$bin" "$args")
    off=$(icount_of 0 "$bin" "$args")
    if [ -n "$on" ] && [ "$on" = "$off" ]; then
        echo "PASS  $name icount (jit on = jit off = $on)"
    else
        echo "FAIL  $name icount (jit on $on, jit off $off)"
    fi
}

# Snapshots (docs/snapshot.md, S2): the case runs with --snapshot-at time:0.3 (it writes a snapshot and stops), then
# with --restore, which goes on from there; the two outputs, one after the other, must be the oracle's, with its exit
# code. The restore runs with two extra host descriptors open (7 and 8), so the engine's own descriptors must be put
# back at their numbers, not found free by luck. Then the chain: the restored run takes a second snapshot 0.1 s on,
# and a third run restores that one. A host where the engine cannot run with a fixed layout (ASLR off) skips them.
run_engine() {
    if [ "$1" = engine ]; then
        shift
        "$ENGINE" "$@"
    else
        shift
        node "$NODE_JS" "$@"
    fi
}

check_snapshot() {
    local runner=$1 name=$2 bin=$3
    local tag="$name snapshot $runner" base="$OUT/$name.snap.$runner"
    local expect_rc
    expect_rc=$(cat "$ORACLE/$name.rc")
    rm -f "$base".a "$base".b
    run_engine "$runner" --snapshot-at time:0.3 --snapshot-out "$base.a" "$bin" >"$base.1" 2>"$base.1.err"
    local rc1=$?
    if grep -q "cannot run with a fixed layout" "$base.1.err"; then
        echo "note: $tag skipped (no fixed layout on this host)"
        return 0
    fi
    run_engine "$runner" --restore "$base.a" >"$base.2" 2>"$base.2.err" 7<Makefile 8</dev/null
    local rc2=$?
    run_engine "$runner" --restore "$base.a" --snapshot-at time:0.1 --snapshot-out "$base.b" >"$base.3" 2>"$base.3.err"
    local rc3=$?
    run_engine "$runner" --restore "$base.b" >"$base.4" 2>"$base.4.err"
    local rc4=$?
    if [ "$rc1" = 0 ] && [ "$rc2" = "$expect_rc" ] && cat "$base.1" "$base.2" | cmp -s - "$ORACLE/$name.out"; then
        echo "PASS  $tag"
    else
        echo "FAIL  $tag (snapshot exit $rc1, restore exit $rc2; stderr: $(tail -n 1 "$base.2.err"))"
        cat "$base.1" "$base.2" | diff "$ORACLE/$name.out" - | head -n 6 | sed 's/^/        /'
    fi
    if [ "$rc3" = 0 ] && [ "$rc4" = "$expect_rc" ] && cat "$base.1" "$base.3" "$base.4" | cmp -s - "$ORACLE/$name.out"; then
        echo "PASS  $tag chained"
    else
        echo "FAIL  $tag chained (exits $rc3, $rc4; stderr: $(tail -n 1 "$base.4.err"))"
    fi
    rm -f "$base".a "$base".b
}

# The load meter (--load-every, docs/demo.md): snapwait runs with a report every 100 ms. Its stdout must still be the
# oracle's (the reports go to stderr); there must be at least three reports in the line's format; one, inside the
# main thread's 600 ms sleep, must count the 2 processes and 5 threads with none runnable and the engine mostly idle
# (busy 50% or less, which leaves room for a loaded host); one must name the program as the busiest process. Then a
# snapshot at 0.3 s (written without the meter) and a restore with it: the restored run reports too (a restore applies
# the option again), with the same processes and threads.
LOAD_RE='^xiaoji: load: [0-9]+ ms, [0-9.]+ M insn \([0-9.]+ M/s\), busy [0-9]+%, [0-9]+ processes, [0-9]+ threads \([0-9]+ runnable\); top:.*; threads:'
check_load() {
    local runner=$1 name=$2 bin=$3
    local tag="$name load $runner" base="$OUT/$name.load.$runner" why=""
    run_engine "$runner" --load-every 100 "$bin" >"$base.out" 2>"$base.err"
    local rc=$? expect_rc n
    expect_rc=$(cat "$ORACLE/$name.rc")
    n=$(grep -cE "$LOAD_RE" "$base.err")
    [ "$rc" = "$expect_rc" ] && cmp -s "$base.out" "$ORACLE/$name.out" || why="stdout/exit differ (exit $rc)"
    [ "$n" -ge 3 ] || why="$why; $n reports"
    grep -E "$LOAD_RE" "$base.err" | grep 'busy \([0-9]\|[1-4][0-9]\|50\)%, 2 processes, 5 threads (0 runnable)' >/dev/null ||
        why="$why; no quiet report with 2 processes and 5 threads"
    grep -E "$LOAD_RE" "$base.err" | grep "top: ${name}/1000 " >/dev/null || why="$why; ${name} is not the busiest process"
    if run_engine "$runner" --snapshot-at time:0.3 --snapshot-out "$base.snap" "$bin" >/dev/null 2>"$base.err2" &&
        [ -s "$base.snap" ]; then
        run_engine "$runner" --restore "$base.snap" --load-every 100 >/dev/null 2>"$base.err3"
        grep -E "$LOAD_RE" "$base.err3" | grep '2 processes, 5 threads' >/dev/null || why="$why; no report after a restore"
    elif ! grep -q 'fixed layout' "$base.err2"; then
        why="$why; the snapshot failed"
    fi
    rm -f "$base.snap"
    if [ -z "$why" ]; then
        echo "PASS  $tag ($n reports)"
    else
        echo "FAIL  $tag (${why#; })"
        grep 'xiaoji: load:' "$base.err" | head -n 3 | cut -c1-160 | sed 's/^/        /'
    fi
}

# The quiet snapshot (--snapshot-quiet, docs/demo.md): snapwait with the condition at 0.05 s and a quiet window of
# 0.2 s at 50% busy or less: the window falls in the main thread's 600 ms sleep, so the snapshot says "quiet" and is
# taken inside the sleep; the restore goes on, and the two outputs are the oracle's.
check_quiet() {
    local runner=$1 name=$2 bin=$3
    local tag="$name snapshot quiet $runner" base="$OUT/$name.quiet.$runner"
    run_engine "$runner" --snapshot-at time:0.05 --snapshot-quiet 50:0.2:5 --snapshot-out "$base.snap" "$bin" \
        >"$base.1" 2>"$base.1.err"
    local rc1=$?
    if grep -q "cannot run with a fixed layout" "$base.1.err"; then
        echo "note: $tag skipped (no fixed layout on this host)"
        return 0
    fi
    run_engine "$runner" --restore "$base.snap" >"$base.2" 2>"$base.2.err"
    local rc2=$?
    if [ "$rc1" = 0 ] && [ "$rc2" = "$(cat "$ORACLE/$name.rc")" ] && grep -q 'snapshot: quiet: busy' "$base.1.err" &&
        cat "$base.1" "$base.2" | cmp -s - "$ORACLE/$name.out"; then
        echo "PASS  $tag"
    else
        echo "FAIL  $tag (exits $rc1, $rc2; $(grep 'snapshot: \(quiet\|no quiet\)' "$base.1.err" | head -n 1))"
    fi
    rm -f "$base.snap"
}

# The pre-hot set (--jit-prehot, docs/demo.md P1; wasm only, so Node): jitstress snapshots after 20 M instructions,
# when the translator has blocks; the restore with --jit-prehot must report a set of those blocks' pcs on its load+
# line, queue at least one of them at its first run, and the two outputs together must be the oracle's.
check_prehot() {
    local name=$1 bin=$2
    local tag="$name jit pre-hot node" base="$OUT/$name.prehot.node" why=""
    node "$NODE_JS" --snapshot-at icount:20000000 --snapshot-out "$base.snap" "$bin" >"$base.1" 2>"$base.1.err"
    node "$NODE_JS" --restore "$base.snap" --jit-prehot --load-every 20 >"$base.2" 2>"$base.2.err"
    local rc=$?
    [ "$rc" = "$(cat "$ORACLE/$name.rc")" ] && cat "$base.1" "$base.2" | cmp -s - "$ORACLE/$name.out" || why="output or exit differ (exit $rc)"
    local line set queued
    line=$(grep -m 1 'xiaoji: load+: .*pre-hot: ' "$base.2.err")
    set=$(sed -n 's/.*pre-hot: [0-9]* of \([0-9]*\) queued.*/\1/p' <<<"$line")
    queued=$(grep 'xiaoji: load+: ' "$base.2.err" | tail -n 1 | sed -n 's/.*pre-hot: \([0-9]*\) of.*/\1/p')
    [ "${set:-0}" -gt 0 ] || why="$why; no pre-hot set (${line:0:120})"
    [ "${queued:-0}" -gt 0 ] || why="$why; nothing queued from it"
    rm -f "$base.snap"
    if [ -z "$why" ]; then
        echo "PASS  $tag ($queued of $set queued)"
    else
        echo "FAIL  $tag (${why#; })"
    fi
}

run_case() {
    local name=$1 stretch=$2
    local bin="build/tests/guest/$name" args
    args=$(args_for "${name%-O?}")
    if [ ! -x "$bin" ]; then
        echo "FAIL  $name (not built; run make guest-tests)"
        return
    fi
    if ! make_oracle "$name" "$bin" "$(oracle_args_for "${name%-O?}")"; then
        if [ "$ORACLE_MODE" = recorded ]; then
            echo "FAIL  $name (no recorded oracle in build/oracle/guest; refresh it on the Mac: make test)"
        else
            echo "FAIL  $name (could not create the native oracle; is the builder VM running?)"
        fi
        return
    fi
    check engine engine "$name" "$bin" "$args" "$stretch"
    if [ -f "$NODE_JS" ]; then
        check node node "$name" "$bin" "$args" "$stretch"
        case "${name%-O?}" in
        jitstress | jitthreads | control | arith | memory | signals | simd) check_icount "$name" "$bin" "$args" ;;
        esac
    fi
    [ "$name" = jitstress-O2 ] && [ -f "$NODE_JS" ] && check_prehot "$name" "$bin"
    case "${name%-O?}" in
    snapwait)
        check_snapshot engine "$name" "$bin"
        [ -f "$NODE_JS" ] && check_snapshot node "$name" "$bin"
        check_load engine "$name" "$bin"
        [ -f "$NODE_JS" ] && check_load node "$name" "$bin"
        check_quiet engine "$name" "$bin"
        [ -f "$NODE_JS" ] && check_quiet node "$name" "$bin"
        ;;
    esac
}

# One job: the guest units with the given indices (UNIT_NAME, UNIT_STRETCH), in order. Each unit's
# lines go to $RES/g<index>, so the report can be put back together in the original order. A unit that
# finished leaves $RES/d<index>; the report says FAIL for a unit without it (its job was killed).
guest_units() {
    local i name stretch
    for i in "$@"; do
        name=${UNIT_NAME[$i]}
        stretch=${UNIT_STRETCH[$i]}
        run_case "$name" "$stretch" >"$(printf '%s/g%03d' "$RES" "$i")"
        : >"$(printf '%s/d%03d' "$RES" "$i")"
    done
}

# Runs one rootfs case through one runner and compares stdout and the exit code
# with the oracle. The guest sees the root build/rootfs.
check_rootfs() {
    local label=$1 runner=$2 name=$3
    local tag="$name $label"
    local out="$OUT/$name.$label.out" err="$OUT/$name.$label.err"
    local expect_out="$O_ORACLE/$name.out" expect_rc_file="$O_ORACLE/$name.rc"
    if [ ! -f "$expect_out" ] || [ ! -f "$expect_rc_file" ]; then
        echo "FAIL  $tag (no oracle output; run tools/oracle.sh $name)"
        return 1
    fi
    local expect_rc
    expect_rc=$(cat "$expect_rc_file")
    if [ "$runner" = engine ]; then
        "$ENGINE" --root "$RFS" "${GUEST_ENV_ARGS[@]}" -- "${ARGV[@]}" >"$out" 2>"$err"
    else
        node "$NODE_JS" --root "$RFS" "${GUEST_ENV_ARGS[@]}" -- "${ARGV[@]}" >"$out" 2>"$err"
    fi
    local rc=$?
    if cmp -s "$out" "$expect_out" && [ "$rc" = "$expect_rc" ]; then
        echo "PASS  $tag"
        return 0
    fi
    local why="stdout/exit differ: exit $rc, expected $expect_rc"
    if [ -s "$err" ]; then
        why="$why; stderr: $(tail -n 1 "$err")"
    fi
    case " $ROOTFS_PENDING " in
    *" $name "*)
        echo "XFAIL $tag ($why)"
        return 1
        ;;
    esac
    echo "FAIL  $tag ($why)"
    diff "$expect_out" "$out" | head -n 6 | sed 's/^/        /'
    return 1
}

# Property cases (B2): expectation tests. The builder VM has no property service,
# so there is no oracle; stdout must equal tests/expect/<name>.out and the exit code
# must be 0. Properties live in the engine process, so each case is one sh -c, where
# setprop and getprop share the store.
# Binder (B3): the real servicemanager is the context manager. A binder case starts it in
# the background, waits until "service check manager" finds it (up to about 10 s), runs,
# then kills it and waits, so the engine exits. $! is the servicemanager pid.
SM_WAIT='i=0; until /system/bin/service check manager 2>/dev/null | grep -q found; do i=$((i+1)); [ $i -gt 100 ] && break; sleep 0.1; done'
PROP_CASES="prop-get prop-set prop-ro prop-list prop-wait evt binder-sm goldfish-pipe goldfish-gl goldfish-as sensors power-supply ashmem evdev kernel binder-freeze"
prop_argv() {
    case "$1" in
    prop-get) ARGV=(/system/bin/sh -c '/system/bin/getprop ro.build.version.release; /system/bin/getprop ro.product.cpu.abi; /system/bin/getprop ro.hardware') ;;
    prop-set) ARGV=(/system/bin/sh -c '/system/bin/setprop debug.xiaoji.test hello; /system/bin/getprop debug.xiaoji.test') ;;
    prop-ro) ARGV=(/system/bin/sh -c '/system/bin/setprop ro.xiaoji.once a; echo rc=$?; /system/bin/setprop ro.xiaoji.once b 2>/dev/null; echo rc=$?; /system/bin/getprop ro.xiaoji.once; /system/bin/setprop ro.hardware x 2>/dev/null; echo rc=$?') ;;
    prop-list) ARGV=(/system/bin/sh -c 'n=$(/system/bin/getprop | /system/bin/toybox grep -c "^\["); names=$(/system/bin/getprop | /system/bin/toybox sed "s/^\[\([^]]*\)\].*/\1/"); sorted=$(printf "%s\n" "$names" | /system/bin/toybox sort); if [ "$n" -gt 100 ] && [ "$names" = "$sorted" ]; then echo above 100 and sorted; fi') ;;
    prop-wait) ARGV=(/data/local/tmp/prop-wait) ;;
    evt) ARGV=(/data/local/tmp/evt) ;;
    goldfish-pipe) ARGV=(/data/local/tmp/goldfish-pipe) ;;
    goldfish-gl) ARGV=(/data/local/tmp/goldfish-gl) ;;
    goldfish-as) ARGV=(/data/local/tmp/goldfish-as) ;;
    sensors) ARGV=(/data/local/tmp/sensors) ;;
    ashmem) ARGV=(/data/local/tmp/ashmem) ;;
    evdev) ARGV=(/data/local/tmp/evdev) ;;
    binder-freeze) ARGV=(/data/local/tmp/binder-freeze) ;;
    # The kernel the engine presents (B18): uname, /proc/version and /proc/config.gz agree, and toybox's zcat
    # (which checks the gzip trailer's CRC and length) reads the configuration of the matrix's 5.15 entry.
    kernel) ARGV=(/system/bin/sh -c 'uname -r; uname -v; cat /proc/version; zcat /proc/config.gz > /data/local/tmp/kconfig && echo zcat ok; grep -c "^CONFIG_" /data/local/tmp/kconfig; grep -c "is not set" /data/local/tmp/kconfig; grep -E "^CONFIG_(ARM64|ANDROID_BINDER_DEVICES|BPF_SYSCALL|OF)=" /data/local/tmp/kconfig; rm /data/local/tmp/kconfig') ;;
    power-supply) ARGV=(/system/bin/sh -c 'for d in /sys/class/power_supply/*; do echo "dir ${d##*/}"; for f in $d/*; do echo "${f##*/} $(cat $f)"; done; done') ;;
    binder-sm) ARGV=(/system/bin/sh -c "/system/bin/servicemanager & $SM_WAIT; /system/bin/service list; /system/bin/service check manager; kill \$!; wait; exit 0") ;;
    esac
}

# Runs one property case through one runner and compares stdout with the expectation.
check_expect() {
    local label=$1 runner=$2 name=$3
    local tag="$name $label"
    local out="$OUT/$name.$label.out" err="$OUT/$name.$label.err"
    local expect="tests/expect/$name.out"
    if [ ! -f "$expect" ]; then
        echo "FAIL  $tag (no $expect)"
        return 1
    fi
    # The input devices' test source (evdev.c): the evdev case reads its scripted events from the environment.
    # (An empty value is no script: the engine reads the variable only when it is set.)
    local input_script=""
    [ "$name" = evdev ] && input_script="$ROOT/tests/input/evdev.script"
    if [ "$runner" = engine ]; then
        XIAOJI_INPUT_SCRIPT="$input_script" "$ENGINE" --root "$RFS" "${GUEST_ENV_ARGS[@]}" -- "${ARGV[@]}" >"$out" 2>"$err"
    else
        XIAOJI_INPUT_SCRIPT="$input_script" node "$NODE_JS" --root "$RFS" "${GUEST_ENV_ARGS[@]}" -- "${ARGV[@]}" >"$out" 2>"$err"
    fi
    local rc=$?
    if cmp -s "$out" "$expect" && [ "$rc" = 0 ]; then
        echo "PASS  $tag"
        return 0
    fi
    echo "FAIL  $tag (exit $rc)"
    diff "$expect" "$out" | head -n 6 | sed 's/^/        /'
    return 1
}

# The root lane: every case that runs with --root, in the original order. It runs as one job, so the
# sidecar <root>/.xiaoji-meta is written by one engine at a time.
root_lane() {
    if [ "$OWN_TREE" = 1 ]; then
        echo "note: XIAOJI_ROOT is set, skipping the builder-VM oracle cases (they are for build/rootfs)"
    elif [ -d build/rootfs/system/bin ]; then
        O_ORACLE=build/oracle
        for name in $ROOTFS_CASES; do
            rootfs_argv "$name"
            check_rootfs engine engine "$name"
            if [ -f "$NODE_JS" ]; then
                check_rootfs node node "$name"
            fi
        done
    else
        echo "note: build/rootfs missing, skipping rootfs cases (tools/extract-rootfs.sh)"
    fi

    if [ -d build/rootfs/system/bin ]; then
        # The futex program (tests/guest/prop-wait.c) runs from the guest: copy it in.
        if [ -f build/tests/guest/prop-wait-O0 ]; then
            mkdir -p "$RFS/data/local/tmp"
            cp build/tests/guest/prop-wait-O0 "$RFS"/data/local/tmp/prop-wait
        fi
        # Event objects (B3): eventfd, timerfd, epoll and ppoll (tests/guest/evt.c).
        if [ -f build/tests/guest/evt-O0 ]; then
            mkdir -p "$RFS/data/local/tmp"
            cp build/tests/guest/evt-O0 "$RFS"/data/local/tmp/evt
        fi
        # The goldfish pipe (G1): /dev/goldfish_pipe and the renderControl handshake (tests/guest/goldfish-pipe.c).
        if [ -f build/tests/guest/goldfish-pipe-O0 ]; then
            mkdir -p "$RFS/data/local/tmp"
            cp build/tests/guest/goldfish-pipe-O0 "$RFS"/data/local/tmp/goldfish-pipe
        fi
        # The not_api GL calls with out-parameters and data (B17): tests/guest/goldfish-gl.c.
        if [ -f build/tests/guest/goldfish-gl-O0 ]; then
            mkdir -p "$RFS/data/local/tmp"
            cp build/tests/guest/goldfish-gl-O0 "$RFS"/data/local/tmp/goldfish-gl
        fi
        # The address space and the sync device (G1): tests/guest/goldfish-as.c.
        if [ -f build/tests/guest/goldfish-as-O0 ]; then
            mkdir -p "$RFS/data/local/tmp"
            cp build/tests/guest/goldfish-as-O0 "$RFS"/data/local/tmp/goldfish-as
        fi
        # The goldfish qemud sensors service (B7): tests/guest/sensors.c, which speaks it as the sensors HAL does.
        if [ -f build/tests/guest/sensors-O0 ]; then
            mkdir -p "$RFS/data/local/tmp"
            cp build/tests/guest/sensors-O0 "$RFS"/data/local/tmp/sensors
        fi
        # /dev/ashmem (B7): tests/guest/ashmem.c, the ioctls libcutils and libfmq use.
        if [ -f build/tests/guest/ashmem-O0 ]; then
            mkdir -p "$RFS/data/local/tmp"
            cp build/tests/guest/ashmem-O0 "$RFS"/data/local/tmp/ashmem
        fi
        # /dev/input (B16): tests/guest/evdev.c, the input devices of evdev.c; its scripted events come from the environment.
        if [ -f build/tests/guest/evdev-O0 ]; then
            mkdir -p "$RFS/data/local/tmp"
            cp build/tests/guest/evdev-O0 "$RFS"/data/local/tmp/evdev
        fi
        # The binder freezer ioctls (docs/demo.md, P1): tests/guest/binder-freeze.c.
        if [ -f build/tests/guest/binder-freeze-O0 ]; then
            mkdir -p "$RFS/data/local/tmp"
            cp build/tests/guest/binder-freeze-O0 "$RFS"/data/local/tmp/binder-freeze
        fi
        for name in $PROP_CASES; do
            prop_argv "$name"
            check_expect engine engine "$name"
            if [ -f "$NODE_JS" ]; then
                check_expect node node "$name"
            fi
        done
    fi

    # Binder (B3): the AOSP driver conformance test, all cases, with the real servicemanager as
    # the context manager (SM_WAIT above). The engine and Node must both report 17 passed.
    local BINDER_TEST=/data/nativetest64/binderDriverInterfaceTest/binderDriverInterfaceTest
    local BINDER_SH="/system/bin/servicemanager & $SM_WAIT; $BINDER_TEST --gtest_brief=1; rc=\$?; kill \$!; wait; exit \$rc"
    if [ ! -f "$RFS$BINDER_TEST" ]; then
        # The AOSP binder conformance binary is in the builder-VM image (build/rootfs), not in device/xiaoji's product.
        echo "note: $RFS has no $BINDER_TEST, skipping the binder-driver cases"
    else
        local runner
        for runner in engine node; do
            if [ "$runner" = engine ]; then
                "$ENGINE" --root "$RFS" -- /system/bin/sh -c "$BINDER_SH" >"$OUT/binder.$runner.out" 2>&1
            elif [ -f "$NODE_JS" ]; then
                node "$NODE_JS" --root "$RFS" -- /system/bin/sh -c "$BINDER_SH" >"$OUT/binder.$runner.out" 2>&1
            else
                continue
            fi
            if grep -q '\[  PASSED  \] 17 tests\.' "$OUT/binder.$runner.out"; then
                echo "PASS  binder-driver 17 cases $runner"
            else
                echo "FAIL  binder-driver 17 cases $runner"
            fi
        done
    fi

    # Init (B4a): the built-in init (--init) starts servicemanager and a oneshot that lists the
    # services, then stops init through the xiaoji.exit property. Stdout must equal
    # tests/expect/init-smoke.out and the exit code must be 0. Engine and Node.
    local INIT_SMOKE="--init tests/init/smoke.rc --root "$RFS" --init-timeout 60 --init-stdio host"
    local runner rc
    if [ -d build/rootfs/system/bin ]; then
        for runner in engine node; do
            if [ "$runner" = engine ]; then
                "$ENGINE" $INIT_SMOKE >"$OUT/init-smoke.$runner.out" 2>"$OUT/init-smoke.$runner.err"
            elif [ -f "$NODE_JS" ]; then
                node "$NODE_JS" $INIT_SMOKE >"$OUT/init-smoke.$runner.out" 2>"$OUT/init-smoke.$runner.err"
            else
                continue
            fi
            rc=$?
            if [ "$rc" = 0 ] && cmp -s "$OUT/init-smoke.$runner.out" tests/expect/init-smoke.out; then
                echo "PASS  init-smoke $runner"
            else
                echo "FAIL  init-smoke $runner (exit $rc)"
                diff tests/expect/init-smoke.out "$OUT/init-smoke.$runner.out" | head -n 6 | sed 's/^/        /'
            fi
        done
    fi

    # Snapshots under --root (docs/snapshot.md, S2). binder: the binder-sm case with a snapshot while servicemanager
    # waits in its binder read and the shell sleeps; the restored run's client lists the services. init: the same under
    # the built-in init (tests/init/snapshot.rc), restored into init's own loop. Each output must be the uninterrupted
    # run's (binder-sm.out, init-smoke.out). The writable trees of the root are part of the snapshot and are put back
    # by the restore.
    if [ -d build/rootfs/system/bin ]; then
        local SNAP_SH="/system/bin/servicemanager & $SM_WAIT; /system/bin/setprop debug.xiaoji.snap 1; /system/bin/toybox sleep 1; /system/bin/service list; /system/bin/service check manager; kill \$!; wait; exit 0"
        local snap="$OUT/snap-root.snap" what expect
        for runner in engine node; do
            [ "$runner" = node ] && [ ! -f "$NODE_JS" ] && continue
            for what in binder init; do
                rm -f "$snap"
                if [ "$what" = binder ]; then
                    run_engine "$runner" --snapshot-at prop:debug.xiaoji.snap=1 --snapshot-after 0.3 --snapshot-out "$snap" \
                        --root "$RFS" "${GUEST_ENV_ARGS[@]}" -- /system/bin/sh -c "$SNAP_SH" \
                        >"$OUT/snap-$what.$runner.1" 2>"$OUT/snap-$what.$runner.1.err"
                    expect=tests/expect/binder-sm.out
                else
                    run_engine "$runner" --snapshot-at prop:debug.xiaoji.snap=1 --snapshot-after 0.3 --snapshot-out "$snap" \
                        --init tests/init/snapshot.rc --root "$RFS" --init-timeout 60 --init-stdio host \
                        >"$OUT/snap-$what.$runner.1" 2>"$OUT/snap-$what.$runner.1.err"
                    expect=tests/expect/init-smoke.out
                fi
                rc=$?
                if grep -q "cannot run with a fixed layout" "$OUT/snap-$what.$runner.1.err"; then
                    echo "note: snapshot-$what $runner skipped (no fixed layout on this host)"
                    continue
                fi
                run_engine "$runner" --restore "$snap" --init-timeout 60 >"$OUT/snap-$what.$runner.2" \
                    2>"$OUT/snap-$what.$runner.2.err" 7<Makefile 8</dev/null
                local rc2=$?
                if [ "$rc" = 0 ] && [ "$rc2" = 0 ] && cat "$OUT/snap-$what.$runner.1" "$OUT/snap-$what.$runner.2" | cmp -s - "$expect"; then
                    echo "PASS  snapshot-$what $runner"
                else
                    echo "FAIL  snapshot-$what $runner (exits $rc, $rc2; stderr: $(tail -n 1 "$OUT/snap-$what.$runner.2.err"))"
                    cat "$OUT/snap-$what.$runner.1" "$OUT/snap-$what.$runner.2" | diff "$expect" - | head -n 6 | sed 's/^/        /'
                fi
            done
        done
        rm -f "$snap"
    fi

    # Init classpaths (B4a): the four classpath variables init derives from the classpath fragments
    # must equal the guest's derive_classpath output (tests/expect/init-classpath.out, made by running
    # /apex/com.android.sdkext/bin/derive_classpath in the engine). Engine and Node.
    if [ -d build/rootfs/system/bin ]; then
        for runner in engine node; do
            if [ "$runner" = engine ]; then
                "$ENGINE" --init tests/init/classpath.rc --root "$RFS" --init-timeout 60 --init-stdio host >"$OUT/init-classpath.$runner.out" 2>"$OUT/init-classpath.$runner.err"
            elif [ -f "$NODE_JS" ]; then
                node "$NODE_JS" --init tests/init/classpath.rc --root "$RFS" --init-timeout 60 --init-stdio host >"$OUT/init-classpath.$runner.out" 2>"$OUT/init-classpath.$runner.err"
            else
                continue
            fi
            rc=$?
            if [ "$rc" = 0 ] && cmp -s "$OUT/init-classpath.$runner.out" tests/expect/init-classpath.out; then
                echo "PASS  init-classpath $runner"
            else
                echo "FAIL  init-classpath $runner (exit $rc)"
                diff tests/expect/init-classpath.out "$OUT/init-classpath.$runner.out" | head -n 6 | sed 's/^/        /'
            fi
        done
    fi

    # Init ids (B6): a service's user, group and supplementary groups (user system, group system inet), and the ids its
    # process has when toybox id runs (tests/init/ids.rc). The report must equal tests/expect/init-ids.out. Engine and Node.
    if [ -f "$RFS/system/bin/toybox" ]; then
        mkdir -p "$RFS/data/local/tmp"
        for runner in engine node; do
            rm -f "$RFS"/data/local/tmp/ids-init.txt
            if [ "$runner" = engine ]; then
                "$ENGINE" --init tests/init/ids.rc --root "$RFS" --init-timeout 60 >"$OUT/init-ids.$runner.out" 2>"$OUT/init-ids.$runner.err"
            elif [ -f "$NODE_JS" ]; then
                node "$NODE_JS" --init tests/init/ids.rc --root "$RFS" --init-timeout 60 >"$OUT/init-ids.$runner.out" 2>"$OUT/init-ids.$runner.err"
            else
                continue
            fi
            rc=$?
            if [ "$rc" = 0 ] && cmp -s "$RFS"/data/local/tmp/ids-init.txt tests/expect/init-ids.out; then
                echo "PASS  init-ids $runner"
            else
                echo "FAIL  init-ids $runner (exit $rc; see $OUT/init-ids.$runner.err)"
            fi
        done
    fi

    # Init cgroups (B11): the cgroup.rc image, a group made and removed, a process's membership (inherited by its child,
    # seen in /proc/self/cgroup), EBUSY on a populated group, and a store in a v1 file. The report must equal
    # tests/expect/init-cgroups.out. Engine and Node.
    if [ -d build/rootfs/system/bin ]; then
        for runner in engine node; do
            if [ "$runner" = engine ]; then
                "$ENGINE" --init tests/init/cgroups.rc --root "$RFS" --init-timeout 60 --init-stdio host >"$OUT/init-cgroups.$runner.out" 2>"$OUT/init-cgroups.$runner.err"
            elif [ -f "$NODE_JS" ]; then
                node "$NODE_JS" --init tests/init/cgroups.rc --root "$RFS" --init-timeout 60 --init-stdio host >"$OUT/init-cgroups.$runner.out" 2>"$OUT/init-cgroups.$runner.err"
            else
                continue
            fi
            rc=$?
            if [ "$rc" = 0 ] && cmp -s "$OUT/init-cgroups.$runner.out" tests/expect/init-cgroups.out; then
                echo "PASS  init-cgroups $runner"
            else
                echo "FAIL  init-cgroups $runner (exit $rc)"
                diff tests/expect/init-cgroups.out "$OUT/init-cgroups.$runner.out" | head -n 6 | sed 's/^/        /'
            fi
        done
    fi

    # Input devices (B16): getevent -lp under init lists both virtual devices (tests/init/evdev.rc, tests/expect/init-evdev.out).
    if [ -d build/rootfs/system/bin ]; then
        for runner in engine node; do
            if [ "$runner" = engine ]; then
                "$ENGINE" --init tests/init/evdev.rc --root "$RFS" --init-timeout 60 --init-stdio host >"$OUT/init-evdev.$runner.out" 2>"$OUT/init-evdev.$runner.err"
            elif [ -f "$NODE_JS" ]; then
                node "$NODE_JS" --init tests/init/evdev.rc --root "$RFS" --init-timeout 60 --init-stdio host >"$OUT/init-evdev.$runner.out" 2>"$OUT/init-evdev.$runner.err"
            else
                continue
            fi
            rc=$?
            if [ "$rc" = 0 ] && cmp -s "$OUT/init-evdev.$runner.out" tests/expect/init-evdev.out; then
                echo "PASS  init-evdev $runner"
            else
                echo "FAIL  init-evdev $runner (exit $rc)"
                diff tests/expect/init-evdev.out "$OUT/init-evdev.$runner.out" | head -n 6 | sed 's/^/        /'
            fi
        done
    fi

    # Init bpf (B15): bpfloader's pre-created maps (the stderr of the engine names the gpu_mem pin), and the pin as
    # libmeminfo reads it from a service that runs as system with the graphics group (tests/guest/bpfpin.c, expected text
    # tests/expect/init-bpf.out). The access checks are the engine's own: the root oracle does not run them. Engine and Node.
    if [ -d build/rootfs/system/bin ] && [ -f build/tests/guest/bpfpin-O0 ]; then
        mkdir -p "$RFS/data/local/tmp"
        cp build/tests/guest/bpfpin-O0 "$RFS"/data/local/tmp/bpfpin
        for runner in engine node; do
            if [ "$runner" = engine ]; then
                "$ENGINE" --init tests/init/bpf.rc --root "$RFS" --init-timeout 60 --init-stdio host >"$OUT/init-bpf.$runner.out" 2>"$OUT/init-bpf.$runner.err"
            elif [ -f "$NODE_JS" ]; then
                node "$NODE_JS" --init tests/init/bpf.rc --root "$RFS" --init-timeout 60 --init-stdio host >"$OUT/init-bpf.$runner.out" 2>"$OUT/init-bpf.$runner.err"
            else
                continue
            fi
            rc=$?
            if [ "$rc" = 0 ] && cmp -s "$OUT/init-bpf.$runner.out" tests/expect/init-bpf.out &&
                grep -q '^bpfloader: [0-9][0-9]* maps pinned under /sys/fs/bpf$' "$OUT/init-bpf.$runner.err"; then
                echo "PASS  init-bpf $runner"
            else
                echo "FAIL  init-bpf $runner (exit $rc)"
                diff tests/expect/init-bpf.out "$OUT/init-bpf.$runner.out" | head -n 6 | sed 's/^/        /'
            fi
        done
    fi

    # Init stdio (B4b): with the default --init-stdio null, a service's stdin, stdout and stderr are
    # /dev/null, so the probe writes its report to a file. The report (fdnames, tests/guest/fdnames.c)
    # must equal tests/expect/init-stdio.out. Engine and Node.
    if [ -d build/rootfs/system/bin ] && [ -f build/tests/guest/fdnames-O0 ]; then
        mkdir -p "$RFS/data/local/tmp"
        cp build/tests/guest/fdnames-O0 "$RFS"/data/local/tmp/fdnames
        for runner in engine node; do
            rm -f "$RFS"/data/local/tmp/fdnames-init.txt
            if [ "$runner" = engine ]; then
                "$ENGINE" --init tests/init/stdio.rc --root "$RFS" --init-timeout 60 >"$OUT/init-stdio.$runner.out" 2>"$OUT/init-stdio.$runner.err"
            elif [ -f "$NODE_JS" ]; then
                node "$NODE_JS" --init tests/init/stdio.rc --root "$RFS" --init-timeout 60 >"$OUT/init-stdio.$runner.out" 2>"$OUT/init-stdio.$runner.err"
            else
                continue
            fi
            rc=$?
            if [ "$rc" = 0 ] && [ ! -s "$OUT/init-stdio.$runner.out" ] &&
                cmp -s "$RFS"/data/local/tmp/fdnames-init.txt tests/expect/init-stdio.out; then
                echo "PASS  init-stdio $runner"
            else
                echo "FAIL  init-stdio $runner (exit $rc)"
                diff tests/expect/init-stdio.out "$RFS"/data/local/tmp/fdnames-init.txt | head -n 6 | sed 's/^/        /'
            fi
        done
    fi

    # Init cases (B4a): each config runs under --init and must print tests/expect/<name>.out, exit 0.
    # initsock is the program the socket case runs (tests/guest/initsock.c), copied into the guest.
    if [ -d build/rootfs/system/bin ] && [ -f build/tests/guest/initsock-O0 ]; then
        mkdir -p "$RFS/data/local/tmp"
        cp build/tests/guest/initsock-O0 "$RFS"/data/local/tmp/initsock
    fi
    local case_name init_rc
    for case_name in init-sockets init-ctl init-interface; do
        case "$case_name" in
        init-sockets) init_rc=tests/init/sockets.rc ;;
        init-ctl) init_rc=tests/init/ctl.rc ;;
        init-interface) init_rc=tests/init/interface.rc ;;
        esac
        for runner in engine node; do
            if [ "$runner" = engine ]; then
                "$ENGINE" --init "$init_rc" --root "$RFS" --init-timeout 90 --init-stdio host >"$OUT/$case_name.$runner.out" 2>"$OUT/$case_name.$runner.err"
            elif [ -f "$NODE_JS" ]; then
                node "$NODE_JS" --init "$init_rc" --root "$RFS" --init-timeout 90 --init-stdio host >"$OUT/$case_name.$runner.out" 2>"$OUT/$case_name.$runner.err"
            else
                continue
            fi
            rc=$?
            if [ "$rc" = 0 ] && cmp -s "$OUT/$case_name.$runner.out" "tests/expect/$case_name.out"; then
                echo "PASS  $case_name $runner"
            else
                echo "FAIL  $case_name $runner (exit $rc)"
                diff "tests/expect/$case_name.out" "$OUT/$case_name.$runner.out" | head -n 6 | sed 's/^/        /'
            fi
        done
    done

    # Deadlock check (B4d). idle-sleep: a thread sleeps one second at a time, seven times (the check waits 5 s
    # of continuous idleness, and that is not idleness here), so stderr must have no deadlock line. idle-hang: a
    # real hang (tests/guest/futex-hang.c, no deadline anywhere) must be reported on stderr; the run ends at
    # --init-timeout, so its exit status is not checked. Engine and Node.
    # idle-sleep is also the idle check (docs/demo.md, P1): the run is mostly sleeps, so the engine's host CPU time
    # (user + system, bash's time) must be under half of its wall time. Emscripten's nanosleep spun, which kept the
    # Node and browser engines at 100% of a core while every guest thread slept; host_sleep_ns waits in Atomics.wait.
    if [ -d build/rootfs/system/bin ] && [ -f build/tests/guest/futex-hang-O0 ]; then
        mkdir -p "$RFS/data/local/tmp"
        cp build/tests/guest/futex-hang-O0 "$RFS"/data/local/tmp/futex-hang
    fi
    for runner in engine node; do
        TIMEFORMAT='%R %U %S'
        if [ "$runner" = engine ]; then
            { time "$ENGINE" --init tests/init/idle-sleep.rc --root "$RFS" --init-timeout 90 --init-stdio host >"$OUT/init-idle-sleep.$runner.out" 2>"$OUT/init-idle-sleep.$runner.err"; } 2>"$OUT/init-idle-sleep.$runner.time"
        elif [ -f "$NODE_JS" ]; then
            { time node "$NODE_JS" --init tests/init/idle-sleep.rc --root "$RFS" --init-timeout 90 --init-stdio host >"$OUT/init-idle-sleep.$runner.out" 2>"$OUT/init-idle-sleep.$runner.err"; } 2>"$OUT/init-idle-sleep.$runner.time"
        else
            continue
        fi
        rc=$?
        # real, user, system: CPU under half of the wall time
        cpu_ok=$(awk '{ print ($2 + $3 < 0.5 * $1) ? 1 : 0 }' "$OUT/init-idle-sleep.$runner.time")
        cpu_note=$(awk '{ printf "%.1f s CPU in %.1f s", $2 + $3, $1 }' "$OUT/init-idle-sleep.$runner.time")
        if [ "$rc" = 0 ] && cmp -s "$OUT/init-idle-sleep.$runner.out" tests/expect/init-idle-sleep.out &&
            ! grep -q "deadlock\|nothing has run" "$OUT/init-idle-sleep.$runner.err"; then
            echo "PASS  init-idle-sleep $runner"
        else
            echo "FAIL  init-idle-sleep $runner (exit $rc)"
            grep "deadlock\|nothing has run" "$OUT/init-idle-sleep.$runner.err" | head -n 3 | sed 's/^/        /'
        fi
        if [ "$cpu_ok" = 1 ]; then
            echo "PASS  init-idle-sleep idle cpu $runner ($cpu_note)"
        else
            echo "FAIL  init-idle-sleep idle cpu $runner ($cpu_note: the idle engine should not spin)"
        fi
        if [ "$runner" = engine ]; then
            "$ENGINE" --init tests/init/idle-hang.rc --root "$RFS" --init-timeout 9 --init-stdio host >"$OUT/init-idle-hang.$runner.out" 2>"$OUT/init-idle-hang.$runner.err"
        elif [ -f "$NODE_JS" ]; then
            node "$NODE_JS" --init tests/init/idle-hang.rc --root "$RFS" --init-timeout 9 --init-stdio host >"$OUT/init-idle-hang.$runner.out" 2>"$OUT/init-idle-hang.$runner.err"
        else
            continue
        fi
        if cmp -s "$OUT/init-idle-hang.$runner.out" tests/expect/init-idle-hang.out &&
            grep -q "deadlock: every thread is blocked with no deadline" "$OUT/init-idle-hang.$runner.err"; then
            echo "PASS  init-idle-hang $runner"
        else
            echo "FAIL  init-idle-hang $runner"
            head -n 6 "$OUT/init-idle-hang.$runner.err" | sed 's/^/        /'
        fi
    done

    # Guest metadata persistence (B8): an owner set in one engine run is read back by a second run from the
    # sidecar (<root>/.xiaoji-meta). The file is made, given an owner, read in two later runs, and removed; after
    # the removal the sidecar has no record of it. Engine and Node. The guest file is in /data of $RFS.
    local META_F=/data/xiaoji-meta-persist
    if [ -f "$RFS/system/bin/toybox" ]; then
        for runner in engine node; do
            if [ "$runner" = engine ]; then
                "$ENGINE" --root "$RFS" -- /system/bin/sh -c "rm -f $META_F; touch $META_F && chown 1234:4321 $META_F" >/dev/null 2>&1
                got=$("$ENGINE" --root "$RFS" -- /system/bin/toybox ls -ln $META_F 2>/dev/null | awk '{print $3" "$4}')
                "$ENGINE" --root "$RFS" -- /system/bin/toybox rm -f $META_F >/dev/null 2>&1
            elif [ -f "$NODE_JS" ]; then
                node "$NODE_JS" --root "$RFS" -- /system/bin/sh -c "rm -f $META_F; touch $META_F && chown 1234:4321 $META_F" >/dev/null 2>&1
                got=$(node "$NODE_JS" --root "$RFS" -- /system/bin/toybox ls -ln $META_F 2>/dev/null | awk '{print $3" "$4}')
                node "$NODE_JS" --root "$RFS" -- /system/bin/toybox rm -f $META_F >/dev/null 2>&1
            else
                continue
            fi
            left=0
            [ -f "$RFS/.xiaoji-meta" ] && left=$(grep -c "^$META_F	" "$RFS/.xiaoji-meta")
            if [ "$got" = "1234 4321" ] && [ "$left" = 0 ]; then
                echo "PASS  metadata persistence: owner read back by a second run, record dropped after removal $runner"
            else
                echo "FAIL  metadata persistence $runner (read '$got', records left $left)"
            fi
        done
    fi
}

# Zygote (B4a, B4b, B4c), opt-in: XIAOJI_BOOT=1 boots the zygote of image/xiaoji.rc. The log must show
# the class preload ("...preloaded N classes"), the fork of system_server (zygote's own
# "System server process N has been created", B4b milestone c), system_server's entry ("Entered
# the Android system server!", B4c milestone d1) and the furthest deterministic point of
# startBootstrapServices, "InitPowerManagement" (B4d), and StartDisplayManager (B4f: the thermal
# lookup now succeeds, so the boot goes on to DisplayManagerService; it is reached in every cycle
# of every B4f boot, and the thermal NullPointerException is gone). About 34 s natively. Not in the
# default run, which is kept short; the boot is stopped by --init-timeout.
# On a tree with a vendor servicemanager (the product), SurfaceFlinger's service must be found and
# StartDisplayManager must complete: the smoke test of checkpoint 4, read from the log (no separate init run).
# G1 adds three graphics lines that both trees produce (the allocator's registration, SurfaceFlinger's main
# thread, and RenderEngine on the GLES backend), and no GL request may go undecoded. The product tree also
# reaches SurfaceFlinger's service and the end of StartDisplayManager; that is not required here, because the
# builder image (build/rootfs) has no vndservicemanager and so no composer (docs/boot.md, B5).
# On the product tree B7 adds the health and sensors HALs: the sensors HAL reports the goldfish qemud mask (6f,
# docs/boot.md B7), both HALs register, and BatteryService receives the health HAL's first update. These are
# the furthest deterministic points (3 of 3 runs); system_server then dies in SettingsProvider (B7, next blocker).
# B16: the boot completed, and the home app, SystemUI and system_server were alive for 60 s after it. The engine's note
# "xiaoji: sys.boot_completed=1" marks the boot's end (props.c), and XIAOJI_STAMP=1 times each logcat line. The processes
# are the latest launcher, SystemUI and system_server started before the note (their "Start proc" and "System server
# process N has been created" lines). The check fails if the log ends less than 60 s after the note, if one of them was
# not started, or if its "xiaoji: exit: pid N " line comes within 60 s after the note (the exit lines carry no time, so
# the time is the last stamped line before them). A later death or restart is allowed.
# This runs after the root lane and the guest jobs have drained (it is timed).
boot_alive_60s() {
    awk '
        function ms(s) { return substr(s, 2, index(s, " ms") - 2) + 0 }
        function pid_after(s, key,   t) { t = s; sub(".*" key, "", t); sub(/[^0-9].*/, "", t); return t }
        /^\[[0-9]+ ms/ { last = ms($0) }
        !marked && /Start proc [0-9]+:com\.android\.launcher3\// { lpid = pid_after($0, "Start proc ") }
        !marked && /Start proc [0-9]+:com\.android\.systemui\// { spid = pid_after($0, "Start proc ") }
        !marked && /System server process [0-9]+ has been created/ { sspid = pid_after($0, "System server process ") }
        /xiaoji: sys\.boot_completed=1/ && !marked { marked = 1; mt = last; fl = lpid; fs = spid; fss = sspid }
        marked && /xiaoji: exit: pid [0-9]+ / {
            p = $0; sub(/.*xiaoji: exit: pid /, "", p); sub(/[^0-9].*/, "", p)
            if ((p == fl || p == fs || p == fss) && last <= mt + 60000) dead = 1
        }
        END {
            if (!marked || fl == "" || fs == "" || fss == "" || last - mt < 60000 || dead) exit 1
            exit 0
        }' "$1"
}

boot_section() {
    if [ "${XIAOJI_BOOT:-0}" = 1 ] && [ -d "$RFS/system/bin" ]; then
        # The product tree starts from a cleared /data and no /data records (B8, tools/reset-data.py), so its boot is
        # deterministic. The Google-image tree keeps its /data: the other cases read files that a reset would remove
        # (build/rootfs/data/hello.dex, /data/local/tmp).
        case "$RFS" in
        *-xiaoji) python3 -I tools/reset-data.py "$RFS" >/dev/null || { echo "FAIL  reset-data $RFS"; } ;;
        esac
        # The limit is 480 s (B16): the sys.boot_completed time is set by the boot, not by the limit, and the check needs 60 s
        # after it (boot_alive_60s); 480 s covers a boot that completes by 420 s. The milestones are fixed in guest
        # instructions, but the boot waits on the clock too, and at Low Power Mode or on a busy host the lmkd and vibrator lines
        # came after 150 s (B11). The wrapper timeout is above the limit.
        XIAOJI_STAMP=1 timeout 540 "$ENGINE" --logcat --init image/xiaoji.rc --root "$RFS" --init-timeout 480 \
            >"$OUT/boot.out" 2>"$OUT/boot.err"
        if grep -q '\.\.\.preloaded [0-9][0-9]* classes' "$OUT/boot.err" &&
            grep -q 'System server process [0-9][0-9]* has been created' "$OUT/boot.err" &&
            grep -q 'Entered the Android system server!' "$OUT/boot.err" &&
            grep -q 'SystemServerTiming([0-9]*): StartPowerManager' "$OUT/boot.err" &&
            grep -q 'SystemServerTiming([0-9]*): InitPowerManagement' "$OUT/boot.err" &&
            grep -q 'SystemServerTiming([0-9]*): StartDisplayManager' "$OUT/boot.err" &&
            grep -q 'Registered android.hardware.graphics.allocator@3.0::IAllocator' "$OUT/boot.err" &&
            grep -q "SurfaceFlinger's main thread ready to run" "$OUT/boot.err" &&
            grep -q 'RenderEngine with GLES Backend' "$OUT/boot.err" &&
            ! grep -q 'is not decoded' "$OUT/boot.err" &&
            { [ ! -f "$RFS/vendor/bin/vndservicemanager" ] ||
              { grep -q 'StartDisplayManager took to complete' "$OUT/boot.err" &&
                ! grep -q "Since 'SurfaceFlinger' could not be found" "$OUT/boot.err" &&
                grep -q 'SystemServiceManager([0-9]*): Starting phase 100' "$OUT/boot.err" &&
                ! grep -q 'lacks write' "$OUT/boot.err" &&
                grep -q 'host sensors mask=6f' "$OUT/boot.err" &&
                grep -q 'Registered android.hardware.sensors@2.1::ISensors/default' "$OUT/boot.err" &&
                grep -q 'Registered android.hardware.health@2.1::IHealth/default' "$OUT/boot.err" &&
                grep -q 'BatteryService([0-9]*): health: Waited [0-9]*ms and received the update' "$OUT/boot.err" &&
                grep -q 'SettingsProvider([0-9]*): Generated and saved new ANDROID_ID' "$OUT/boot.err" &&
                grep -q 'ActivityManager([0-9]*): Connection with lmkd established' "$OUT/boot.err" &&
                grep -q 'SystemServerTiming([0-9]*): StartVibratorManagerService took to complete' "$OUT/boot.err" &&
                ! grep -q "Could not find 'aidl/android.hardware.vibrator" "$OUT/boot.err" &&
                ! grep -q 'Unsupported AF_UNIX socket' "$OUT/boot.err" &&
                ! grep -q 'Failed to mount /data_mirror' "$OUT/boot.err" &&
                ! grep -q 'Failed to find cgroup v2 root' "$OUT/boot.err" &&
                ! grep -q 'cgroup_info/cgroup.rc: No such file' "$OUT/boot.err" &&
                ! grep -q 'Failed to open /dev/cpuset/' "$OUT/boot.err" &&
                ! grep -q 'init: netd exited' "$OUT/boot.err" &&
                grep -q 'xiaoji_netd([0-9]*): registered netd' "$OUT/boot.err" &&
                grep -q 'xiaoji_netd([0-9]*): registered dnsresolver' "$OUT/boot.err" &&
                ! grep -q 'returning null INetd' "$OUT/boot.err" &&
                grep -q 'SystemServerTiming([0-9]*): StartNetworkManagementService took to complete' "$OUT/boot.err" &&
                grep -q 'SystemServerTiming([0-9]*): StartConnectivityService took to complete' "$OUT/boot.err" &&
                grep -q 'Registered android.hardware.audio@7.0::IDevicesFactory/default' "$OUT/boot.err" &&
                grep -q 'Registered android.hardware.audio.effect@7.0::IEffectsFactory/default' "$OUT/boot.err" &&
                grep -q 'AudioFlinger([0-9]*): loadHwModule() Loaded primary audio interface' "$OUT/boot.err" &&
                grep -q 'SystemServerTiming([0-9]*): StartAudioService took to complete' "$OUT/boot.err" &&
                grep -q 'Registered android.hardware.thermal@2.0::IThermal/default' "$OUT/boot.err" &&
                grep -q 'SystemServerTiming([0-9]*): StartHardwarePropertiesManagerService took to complete' "$OUT/boot.err" &&
                grep -q 'Registered android.hardware.authsecret@1.0::IAuthSecret/default' "$OUT/boot.err" &&
                grep -q 'ActivityManager([0-9]*): System now ready' "$OUT/boot.err" &&
                grep -q '^bpfloader: [0-9][0-9]* maps pinned under /sys/fs/bpf$' "$OUT/boot.err" &&
                ! grep -q 'Fatal signal' "$OUT/boot.err" &&
                ! grep -q 'FATAL EXCEPTION IN SYSTEM PROCESS' "$OUT/boot.err" &&
                grep -qE 'ActivityManager\([0-9]*\): Start proc [0-9]*:com\.android\.launcher3/' "$OUT/boot.err" &&
                ! grep -q 'Unknown class' "$OUT/boot.err" &&
                grep -qE 'ActivityTaskManager\([0-9]*\): START u0 .*cat=\[android\.intent\.category\.HOME\].*cmp=com\.android\.launcher3/' "$OUT/boot.err" &&
                grep -q 'xiaoji: sys.boot_completed=1' "$OUT/boot.err" &&
                boot_alive_60s "$OUT/boot.err"; }; }; then
            echo "PASS  cgroups (B11) and xiaoji_netd (B12): no cgroup.rc or cgroup v2 root error, no cpuset open error, xiaoji_netd registers netd and dnsresolver, no null INetd, StartNetworkManagementService and StartConnectivityService complete, no netd exit; the audio HAL (device/xiaoji/audio) registers, audioserver loads the primary module, StartAudioService completes, and ActivityManager reports System now ready (B13); zygote preload, system_server fork, its entry, StartPowerManager, InitPowerManagement, StartDisplayManager, the allocator, SurfaceFlinger's start and RenderEngine, and no undecoded GL request (XIAOJI_BOOT, B4f, G1); on the product tree also the sensors, health, BatteryService and SettingsProvider's ANDROID_ID (B8), ActivityManager's lmkd connection and VibratorManagerService's start (B9); no Fatal signal, no system_server FATAL EXCEPTION, the launcher's process starts, and sys.boot_completed=1 with the launcher, SystemUI and system_server alive 60 s after it (B16)"
        else
            echo "FAIL  zygote boot (XIAOJI_BOOT): a preload, fork, entry, StartPowerManager, InitPowerManagement, StartDisplayManager, graphics line, or an undecoded GL request ('is not decoded'), a Fatal signal, a system_server FATAL EXCEPTION, no launcher process start, or no sys.boot_completed with the launcher, SystemUI and system_server alive 60 s after it, in $OUT/boot.err"
        fi
    fi
}

# binderLibTest (B3): libbinder end to end, with the real servicemanager and the forked
# servers. Report only, not a gate (a stretch goal, docs/boot.md B3): each case runs in its
# own process (tests/binderlib.sh), the per-case lines go to build/tests/out/binderlib.txt,
# and the counts are printed. Engine only, and it never changes the fail count. Off by
# default (XIAOJI_BINDERLIB=1 turns it on): a case that hangs in teardown is left to the
# per-case watchdog, and the whole run takes long.
binderlib_section() {
    local BL_TEST=/data/nativetest64/binderLibTest/binderLibTest
    if [ -f "build/rootfs$BL_TEST" ] && [ "${XIAOJI_BINDERLIB:-0}" != 1 ]; then
        echo "note  binderLibTest report skipped (set XIAOJI_BINDERLIB=1; it is slow, docs/boot.md B3)"
    elif [ -f "build/rootfs$BL_TEST" ]; then
        mkdir -p "$RFS/data/local/tmp"
        cp tests/binderlib.sh "$RFS"/data/local/tmp/binderlib.sh
        local BL_SH="/system/bin/servicemanager & $SM_WAIT; LIMIT=20 /system/bin/sh /data/local/tmp/binderlib.sh; kill \$!; wait \$!; exit 0"
        "$ENGINE" --root "$RFS" -- /system/bin/sh -c "$BL_SH" >"$OUT/binderlib.txt" 2>&1
        awk '/ rc=[0-9]+ /{
                t++
                if ($0 ~ / rc=0 \[  PASSED/) p++
                else if ($0 ~ /\[  FAILED/) f++
                else if ($0 ~ /\[  SKIPPED/) k++
                else if ($0 ~ /body ok, no summary/) h++
                else o++
             }
             END { printf "INFO  binderLibTest (report only): %d cases: %d passed, %d failed, %d skipped, %d body ok but no summary (hung after the body), %d no result\n", t, p, f, k, h, o }' "$OUT/binderlib.txt"
    fi
}

# Input devices (B16), opt-in: XIAOJI_INPUT_BOOT=1 boots the zygote of image/xiaoji.rc on the tree in XIAOJI_ROOT (or
# build/rootfs) and requires EventHub to log both virtual devices as added, the touchscreen with its idc file
# (engine/evdev.c, docs/input.md). The boot is stopped by --init-timeout, and by a watchdog when the engine's resident
# memory passes XIAOJI_INPUT_BOOT_MAX_KIB (default 4 GiB): the host is an 8 GB machine. Not in the default run.
# It is for the product tree (XIAOJI_ROOT=build/rootfs-xiaoji): on the Google-image tree system_server stops in
# DisplayManagerService (no SurfaceFlinger, no composer), before InputManagerService is created (docs/input.md).
# Like the zygote boot, it runs after the lanes have drained, after binderlib.
input_boot_section() {
    if [ "${XIAOJI_INPUT_BOOT:-0}" = 1 ] && [ -d "$RFS/system/bin" ]; then
        local max_kib=${XIAOJI_INPUT_BOOT_MAX_KIB:-4194304}
        # The init timeout (90 s) normally ends the run first. The watchdog stops the engine at 150 s of wall time, or when
        # its resident memory (ps, KiB) passes the limit; it checks every 2 s.
        "$ENGINE" --logcat --init image/xiaoji.rc --root "$RFS" --init-timeout 90 >"$OUT/input-boot.out" 2>"$OUT/input-boot.err" &
        local eng=$!
        ( elapsed=0
          while kill -0 "$eng" 2>/dev/null; do
            rss=$(ps -o rss= -p "$eng" 2>/dev/null | tr -d ' ')
            if [ -n "$rss" ] && [ "$rss" -gt "$max_kib" ]; then
                echo "watchdog: engine RSS ${rss} KiB over ${max_kib} KiB" >>"$OUT/input-boot.err"
                kill "$eng" 2>/dev/null
                break
            fi
            if [ "$elapsed" -ge 150 ]; then
                echo "watchdog: 150 s of wall time" >>"$OUT/input-boot.err"
                kill "$eng" 2>/dev/null
                break
            fi
            sleep 2
            elapsed=$((elapsed + 2))
          done ) &
        local dog=$!
        wait "$eng"
        local rc=$?
        kill "$dog" 2>/dev/null
        wait "$dog" 2>/dev/null
        local err="$OUT/input-boot.err"
        local ok0=0 ok1=0 okidc=0
        grep -q "New device: id=[0-9]*, fd=[0-9]*, path='/dev/input/event0', name='xiaoji_touchscreen'" "$err" && ok0=1
        grep -q "New device: id=[0-9]*, fd=[0-9]*, path='/dev/input/event1', name='xiaoji_keyboard'" "$err" && ok1=1
        grep -q "path='/dev/input/event0'.*configuration='/vendor/usr/idc/xiaoji_touchscreen.idc'" "$err" && okidc=1
        if [ "$ok0" = 1 ] && [ "$ok1" = 1 ] && [ "$okidc" = 1 ]; then
            echo "PASS  input boot (B16): EventHub adds event0 (xiaoji_touchscreen, its idc) and event1 (xiaoji_keyboard) (exit $rc)"
        else
            echo "FAIL  input boot (B16): event0 $ok0, event1 $ok1, idc $okidc (exit $rc; $err)"
        fi
    fi
}

if [ ! -x "$ENGINE" ]; then
    echo "xiaoji: $ENGINE not built; run make first" >&2
    exit 2
fi
if [ -f "$NODE_JS" ]; then
    : >"$RES/h0"
else
    echo "note: $NODE_JS not built, skipping Node runs (make wasm-node)" >"$RES/h0"
fi
# XIAOJI_SKIP: base names of guest programs left out (for a checkout whose path the expected output names, such as
# exec: remote-test.sh uses it). Unset, nothing is skipped and the report has no line for it.
if [ -n "${XIAOJI_SKIP:-}" ]; then
    echo "note  guest programs skipped (XIAOJI_SKIP): $XIAOJI_SKIP" >>"$RES/h0"
fi

# The guest units, in the original order (all O0 binaries, then all O2): UNIT_NAME and UNIT_STRETCH.
# Programs the guest cannot run as cases (prop-*, the futex hang, device probes) are skipped here, as before.
UNIT_NAME=()
UNIT_STRETCH=()
cases=0
for bin in build/tests/guest/*-O0 build/tests/guest/*-O2; do
    [ -e "$bin" ] || continue
    name=$(basename "$bin")
    stretch=0
    case "$name" in glibc_hello*) stretch=1 ;; esac
    # prop-* programs need the engine's property service, which the native run lacks:
    # they are expectation cases (PROP_CASES below), not oracle cases. futex-hang blocks forever by
    # design: it is run only by the deadlock cases (idle-hang, below), never as a case here.
    # bpfpin-* runs as the system user with the graphics group, as an init service (init-bpf, B15): not an oracle case.
    case "$name" in prop-*|initsock-*|futex-hang-*|goldfish-pipe-*|goldfish-gl-*|goldfish-as-*|sensors-*|ashmem-*|bpfpin-*|evdev-*|binder-freeze-*) continue ;; esac
    cases=$((cases + 1))
    case " ${XIAOJI_SKIP:-} " in *" ${name%-O?} "*) continue ;; esac
    UNIT_NAME+=("$name")
    UNIT_STRETCH+=("$stretch")
done
NU=${#UNIT_NAME[@]}
if [ "$cases" = 0 ]; then
    echo "FAIL  no guest binaries in build/tests/guest (run make guest-tests)" >"$RES/static"
fi

# The jobs: index 0 is the root lane; the guest jobs follow, one per base name, in order of first appearance.
NODE_W=1
[ -f "$NODE_JS" ] && NODE_W=2
JFN=(root_lane)
JARG=("")
JW=($NODE_W)
JBASE=("")
NJ=1
i=0
while [ "$i" -lt "$NU" ]; do
    base=${UNIT_NAME[$i]%-O?}
    found=""
    j=1
    while [ "$j" -lt "$NJ" ]; do
        if [ "${JBASE[$j]}" = "$base" ]; then
            found=$j
            break
        fi
        j=$((j + 1))
    done
    if [ -z "$found" ]; then
        JFN[$NJ]=guest_units
        JARG[$NJ]="$i"
        JW[$NJ]=$NODE_W
        JBASE[$NJ]=$base
        NJ=$((NJ + 1))
    else
        JARG[$found]="${JARG[$found]} $i"
    fi
    i=$((i + 1))
done
JW[0]=$NODE_W

# Scheduler (bash 3.2: no wait -n, so it polls). A job starts when its weight fits in JOBS units; when nothing
# runs, one job always starts, even if it weighs more than JOBS.
trap 'kill $(jobs -p) 2>/dev/null; exit 130' INT TERM
used=0
next=0
NR=0
while [ "$next" -lt "$NJ" ] || [ "$NR" -gt 0 ]; do
    k=0
    nk=0
    while [ "$k" -lt "$NR" ]; do
        if kill -0 "${RPID[$k]}" 2>/dev/null; then
            RPID[$nk]=${RPID[$k]}
            RW[$nk]=${RW[$k]}
            nk=$((nk + 1))
        else
            used=$((used - RW[$k]))
        fi
        k=$((k + 1))
    done
    NR=$nk
    while [ "$next" -lt "$NJ" ]; do
        w=${JW[$next]}
        if [ "$used" -gt 0 ] && [ $((used + w)) -gt "$JOBS" ]; then
            break
        fi
        ( ${JFN[$next]} ${JARG[$next]}; : >"$RES/done.$next" ) >"$RES/job.$next" 2>&1 &
        RPID[$NR]=$!
        RW[$NR]=$w
        NR=$((NR + 1))
        used=$((used + w))
        next=$((next + 1))
    done
    sleep 0.2
done
wait

# The timed boots and binderlib, one after the other, with the lanes drained.
boot_section >"$RES/boot.txt" 2>&1
binderlib_section >"$RES/binderlib.txt" 2>&1
input_boot_section >"$RES/input-boot.txt" 2>&1

# Assemble the report in the original order.
{
    cat "$RES/h0"
    i=0
    while [ "$i" -lt "$NU" ]; do
        if [ -f "$(printf '%s/d%03d' "$RES" "$i")" ]; then
            cat "$(printf '%s/g%03d' "$RES" "$i")"
        else
            echo "FAIL  ${UNIT_NAME[$i]} (no result: its job stopped before the case finished)"
        fi
        i=$((i + 1))
    done
    [ -f "$RES/static" ] && cat "$RES/static"
    if [ -f "$RES/done.0" ]; then
        cat "$RES/job.0"
    else
        echo "FAIL  root lane (no result: the job stopped)"
    fi
    cat "$RES/boot.txt" "$RES/binderlib.txt" "$RES/input-boot.txt"
} >"$RES/report.txt"
for f in "$RES"/job.[1-9]*; do
    [ -s "$f" ] && { echo "xiaoji: output of the job $f:" >&2; cat "$f" >&2; }
done

pass=$(awk '/^PASS  /{n++} END{print n+0}' "$RES/report.txt")
fail=$(awk '/^FAIL  /{n++} END{print n+0}' "$RES/report.txt")
xfail=$(awk '/^XFAIL /{n++} END{print n+0}' "$RES/report.txt")
{
    cat "$RES/report.txt"
    echo
    echo "summary: $pass passed, $fail failed, $xfail xfail"
} | tee "$OUT/report.txt"
[ "$fail" = 0 ]
