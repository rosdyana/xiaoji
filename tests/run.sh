#!/bin/bash
# Guest tests: compare the engine (and the Node build, if present) with native
# runs of the same binaries in the builder VM (M0 freestanding programs), and
# with the oracle outputs of the guest's own dynamically linked toybox binaries
# (rootfs section, M1). Skipped if build/rootfs is missing.
#
# Oracle outputs are cached in build/oracle/guest/<name>.{out,rc} and
# regenerated whenever the binary is newer than the cache.
# Exit status is nonzero if any non-stretch case fails.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

ORB=/Applications/OrbStack.app/Contents/MacOS/bin/orb
ENGINE=build/xiaoji
NODE_JS=build/wasm-node/xiaoji.js
ORACLE=build/oracle/guest
OUT=build/tests/out

mkdir -p "$ORACLE" "$OUT"

# Extra arguments per test program (the name without -O0/-O2).
args_for() {
    case "$1" in
    args) echo "alpha beta" ;;
    *) echo "" ;;
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
    sh-script) ARGV=(/system/bin/sh -c 'echo "#!/system/bin/sh" > /data/local/tmp/s.sh; echo "echo script ran \$1" >> /data/local/tmp/s.sh; chmod 755 /data/local/tmp/s.sh; /data/local/tmp/s.sh arg1') ;;
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

pass=0
fail=0
xfail=0

# Every binary in build/tests/guest/*-O0 and *-O2 is a case. glibc_hello* is a
# stretch case: failure is XFAIL, not FAIL.

# Creates build/oracle/guest/<name>.{out,rc} by running the binary natively in
# the builder VM, if the cache is missing or older than the binary.
make_oracle() {
    local name=$1 bin=$2 args=$3
    local out="$ORACLE/$name.out" rc="$ORACLE/$name.rc"
    if [ -f "$rc" ] && [ "$rc" -nt "$bin" ]; then
        return 0
    fi
    "$ORB" -m builder sh -c "cd '$ROOT' && '$bin' $args > '$out' 2>/dev/null; echo \$? > '$rc'" \
        >/dev/null 2>&1
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
        pass=$((pass + 1))
        return 0
    fi
    local why="stdout/exit differ: exit $rc, expected $expect_rc"
    if [ -s "$err" ]; then
        why="$why; stderr: $(tail -n 1 "$err")"
    fi
    if [ "$stretch" = 1 ]; then
        echo "XFAIL $tag ($why)"
        xfail=$((xfail + 1))
    else
        echo "FAIL  $tag ($why)"
        diff "$expect_out" "$out" | head -n 6 | sed 's/^/        /'
        fail=$((fail + 1))
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
        pass=$((pass + 1))
    else
        echo "FAIL  $name icount (jit on $on, jit off $off)"
        fail=$((fail + 1))
    fi
}

run_case() {
    local name=$1 stretch=$2
    local bin="build/tests/guest/$name" args
    args=$(args_for "${name%-O?}")
    if [ ! -x "$bin" ]; then
        echo "FAIL  $name (not built; run make guest-tests)"
        fail=$((fail + 1))
        return
    fi
    if ! make_oracle "$name" "$bin" "$args"; then
        echo "FAIL  $name (could not create the native oracle; is the builder VM running?)"
        fail=$((fail + 1))
        return
    fi
    check engine engine "$name" "$bin" "$args" "$stretch"
    if [ -f "$NODE_JS" ]; then
        check node node "$name" "$bin" "$args" "$stretch"
        case "${name%-O?}" in
        jitstress | jitthreads | control | arith | memory | signals | simd) check_icount "$name" "$bin" "$args" ;;
        esac
    fi
}

if [ ! -x "$ENGINE" ]; then
    echo "xiaoji: $ENGINE not built; run make first" >&2
    exit 2
fi
if [ ! -f "$NODE_JS" ]; then
    echo "note: $NODE_JS not built, skipping Node runs (make wasm-node)"
fi

cases=0
for bin in build/tests/guest/*-O0 build/tests/guest/*-O2; do
    [ -e "$bin" ] || continue
    name=$(basename "$bin")
    stretch=0
    case "$name" in glibc_hello*) stretch=1 ;; esac
    # prop-* programs need the engine's property service, which the native run lacks:
    # they are expectation cases (PROP_CASES below), not oracle cases.
    case "$name" in prop-*|initsock-*) continue ;; esac
    cases=$((cases + 1))
    run_case "$name" "$stretch"
done
if [ "$cases" = 0 ]; then
    echo "FAIL  no guest binaries in build/tests/guest (run make guest-tests)"
    fail=$((fail + 1))
fi


# Runs one rootfs case through one runner and compares stdout and the exit code
# with the oracle. The guest sees the root build/rootfs.
check_rootfs() {
    local label=$1 runner=$2 name=$3
    local tag="$name $label"
    local out="$OUT/$name.$label.out" err="$OUT/$name.$label.err"
    local expect_out="$O_ORACLE/$name.out" expect_rc_file="$O_ORACLE/$name.rc"
    if [ ! -f "$expect_out" ] || [ ! -f "$expect_rc_file" ]; then
        echo "FAIL  $tag (no oracle output; run tools/oracle.sh $name)"
        fail=$((fail + 1))
        return 1
    fi
    local expect_rc
    expect_rc=$(cat "$expect_rc_file")
    if [ "$runner" = engine ]; then
        "$ENGINE" --root build/rootfs "${GUEST_ENV_ARGS[@]}" -- "${ARGV[@]}" >"$out" 2>"$err"
    else
        node "$NODE_JS" --root build/rootfs "${GUEST_ENV_ARGS[@]}" -- "${ARGV[@]}" >"$out" 2>"$err"
    fi
    local rc=$?
    if cmp -s "$out" "$expect_out" && [ "$rc" = "$expect_rc" ]; then
        echo "PASS  $tag"
        pass=$((pass + 1))
        return 0
    fi
    local why="stdout/exit differ: exit $rc, expected $expect_rc"
    if [ -s "$err" ]; then
        why="$why; stderr: $(tail -n 1 "$err")"
    fi
    case " $ROOTFS_PENDING " in
    *" $name "*)
        echo "XFAIL $tag ($why)"
        xfail=$((xfail + 1))
        return 1
        ;;
    esac
    echo "FAIL  $tag ($why)"
    diff "$expect_out" "$out" | head -n 6 | sed 's/^/        /'
    fail=$((fail + 1))
    return 1
}

rootfs_cases=0
if [ -d build/rootfs/system/bin ]; then
    O_ORACLE=build/oracle
    for name in $ROOTFS_CASES; do
        rootfs_argv "$name"
        rootfs_cases=$((rootfs_cases + 1))
        check_rootfs engine engine "$name"
        if [ -f "$NODE_JS" ]; then
            check_rootfs node node "$name"
        fi
    done
else
    echo "note: build/rootfs missing, skipping rootfs cases (tools/extract-rootfs.sh)"
fi

# Property cases (B2): expectation tests. The builder VM has no property service,
# so there is no oracle; stdout must equal tests/expect/<name>.out and the exit code
# must be 0. Properties live in the engine process, so each case is one sh -c, where
# setprop and getprop share the store.
# Binder (B3): the real servicemanager is the context manager. A binder case starts it in
# the background, waits until "service check manager" finds it (up to about 10 s), runs,
# then kills it and waits, so the engine exits. $! is the servicemanager pid.
SM_WAIT='i=0; until /system/bin/service check manager 2>/dev/null | grep -q found; do i=$((i+1)); [ $i -gt 100 ] && break; sleep 0.1; done'
PROP_CASES="prop-get prop-set prop-ro prop-list prop-wait evt binder-sm"
prop_argv() {
    case "$1" in
    prop-get) ARGV=(/system/bin/sh -c '/system/bin/getprop ro.build.version.release; /system/bin/getprop ro.product.cpu.abi; /system/bin/getprop ro.hardware') ;;
    prop-set) ARGV=(/system/bin/sh -c '/system/bin/setprop debug.xiaoji.test hello; /system/bin/getprop debug.xiaoji.test') ;;
    prop-ro) ARGV=(/system/bin/sh -c '/system/bin/setprop ro.xiaoji.once a; echo rc=$?; /system/bin/setprop ro.xiaoji.once b 2>/dev/null; echo rc=$?; /system/bin/getprop ro.xiaoji.once; /system/bin/setprop ro.hardware x 2>/dev/null; echo rc=$?') ;;
    prop-list) ARGV=(/system/bin/sh -c 'n=$(/system/bin/getprop | /system/bin/toybox grep -c "^\["); names=$(/system/bin/getprop | /system/bin/toybox sed "s/^\[\([^]]*\)\].*/\1/"); sorted=$(printf "%s\n" "$names" | /system/bin/toybox sort); if [ "$n" -gt 100 ] && [ "$names" = "$sorted" ]; then echo above 100 and sorted; fi') ;;
    prop-wait) ARGV=(/data/local/tmp/prop-wait) ;;
    evt) ARGV=(/data/local/tmp/evt) ;;
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
        fail=$((fail + 1))
        return 1
    fi
    if [ "$runner" = engine ]; then
        "$ENGINE" --root build/rootfs "${GUEST_ENV_ARGS[@]}" -- "${ARGV[@]}" >"$out" 2>"$err"
    else
        node "$NODE_JS" --root build/rootfs "${GUEST_ENV_ARGS[@]}" -- "${ARGV[@]}" >"$out" 2>"$err"
    fi
    local rc=$?
    if cmp -s "$out" "$expect" && [ "$rc" = 0 ]; then
        echo "PASS  $tag"
        pass=$((pass + 1))
        return 0
    fi
    echo "FAIL  $tag (exit $rc)"
    diff "$expect" "$out" | head -n 6 | sed 's/^/        /'
    fail=$((fail + 1))
    return 1
}

if [ -d build/rootfs/system/bin ]; then
    # The futex program (tests/guest/prop-wait.c) runs from the guest: copy it in.
    if [ -f build/tests/guest/prop-wait-O0 ]; then
        mkdir -p build/rootfs/data/local/tmp
        cp build/tests/guest/prop-wait-O0 build/rootfs/data/local/tmp/prop-wait
    fi
    # Event objects (B3): eventfd, timerfd, epoll and ppoll (tests/guest/evt.c).
    if [ -f build/tests/guest/evt-O0 ]; then
        mkdir -p build/rootfs/data/local/tmp
        cp build/tests/guest/evt-O0 build/rootfs/data/local/tmp/evt
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
BINDER_TEST=/data/nativetest64/binderDriverInterfaceTest/binderDriverInterfaceTest
BINDER_SH="/system/bin/servicemanager & $SM_WAIT; $BINDER_TEST --gtest_brief=1; rc=\$?; kill \$!; wait; exit \$rc"
if [ -f "build/rootfs$BINDER_TEST" ]; then
    for runner in engine node; do
        if [ "$runner" = engine ]; then
            "$ENGINE" --root build/rootfs -- /system/bin/sh -c "$BINDER_SH" >"$OUT/binder.$runner.out" 2>&1
        elif [ -f "$NODE_JS" ]; then
            node "$NODE_JS" --root build/rootfs -- /system/bin/sh -c "$BINDER_SH" >"$OUT/binder.$runner.out" 2>&1
        else
            continue
        fi
        if grep -q '\[  PASSED  \] 17 tests\.' "$OUT/binder.$runner.out"; then
            echo "PASS  binder-driver 17 cases $runner"
            pass=$((pass + 1))
        else
            echo "FAIL  binder-driver 17 cases $runner"
            fail=$((fail + 1))
        fi
    done
fi

# Init (B4a): the built-in init (--init) starts servicemanager and a oneshot that lists the
# services, then stops init through the xiaoji.exit property. Stdout must equal
# tests/expect/init-smoke.out and the exit code must be 0. Engine and Node.
INIT_SMOKE="--init tests/init/smoke.rc --root build/rootfs --init-timeout 60 --init-stdio host"
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
            pass=$((pass + 1))
        else
            echo "FAIL  init-smoke $runner (exit $rc)"
            diff tests/expect/init-smoke.out "$OUT/init-smoke.$runner.out" | head -n 6 | sed 's/^/        /'
            fail=$((fail + 1))
        fi
    done
fi

# Init classpaths (B4a): the four classpath variables init derives from the classpath fragments
# must equal the guest's derive_classpath output (tests/expect/init-classpath.out, made by running
# /apex/com.android.sdkext/bin/derive_classpath in the engine). Engine and Node.
if [ -d build/rootfs/system/bin ]; then
    for runner in engine node; do
        if [ "$runner" = engine ]; then
            "$ENGINE" --init tests/init/classpath.rc --root build/rootfs --init-timeout 60 --init-stdio host >"$OUT/init-classpath.$runner.out" 2>"$OUT/init-classpath.$runner.err"
        elif [ -f "$NODE_JS" ]; then
            node "$NODE_JS" --init tests/init/classpath.rc --root build/rootfs --init-timeout 60 --init-stdio host >"$OUT/init-classpath.$runner.out" 2>"$OUT/init-classpath.$runner.err"
        else
            continue
        fi
        rc=$?
        if [ "$rc" = 0 ] && cmp -s "$OUT/init-classpath.$runner.out" tests/expect/init-classpath.out; then
            echo "PASS  init-classpath $runner"
            pass=$((pass + 1))
        else
            echo "FAIL  init-classpath $runner (exit $rc)"
            diff tests/expect/init-classpath.out "$OUT/init-classpath.$runner.out" | head -n 6 | sed 's/^/        /'
            fail=$((fail + 1))
        fi
    done
fi

# Init stdio (B4b): with the default --init-stdio null, a service's stdin, stdout and stderr are
# /dev/null, so the probe writes its report to a file. The report (fdnames, tests/guest/fdnames.c)
# must equal tests/expect/init-stdio.out. Engine and Node.
if [ -d build/rootfs/system/bin ] && [ -f build/tests/guest/fdnames-O0 ]; then
    mkdir -p build/rootfs/data/local/tmp
    cp build/tests/guest/fdnames-O0 build/rootfs/data/local/tmp/fdnames
    for runner in engine node; do
        rm -f build/rootfs/data/local/tmp/fdnames-init.txt
        if [ "$runner" = engine ]; then
            "$ENGINE" --init tests/init/stdio.rc --root build/rootfs --init-timeout 60 >"$OUT/init-stdio.$runner.out" 2>"$OUT/init-stdio.$runner.err"
        elif [ -f "$NODE_JS" ]; then
            node "$NODE_JS" --init tests/init/stdio.rc --root build/rootfs --init-timeout 60 >"$OUT/init-stdio.$runner.out" 2>"$OUT/init-stdio.$runner.err"
        else
            continue
        fi
        rc=$?
        if [ "$rc" = 0 ] && [ ! -s "$OUT/init-stdio.$runner.out" ] &&
            cmp -s build/rootfs/data/local/tmp/fdnames-init.txt tests/expect/init-stdio.out; then
            echo "PASS  init-stdio $runner"
            pass=$((pass + 1))
        else
            echo "FAIL  init-stdio $runner (exit $rc)"
            diff tests/expect/init-stdio.out build/rootfs/data/local/tmp/fdnames-init.txt | head -n 6 | sed 's/^/        /'
            fail=$((fail + 1))
        fi
    done
fi

# Init cases (B4a): each config runs under --init and must print tests/expect/<name>.out, exit 0.
# initsock is the program the socket case runs (tests/guest/initsock.c), copied into the guest.
if [ -d build/rootfs/system/bin ] && [ -f build/tests/guest/initsock-O0 ]; then
    mkdir -p build/rootfs/data/local/tmp
    cp build/tests/guest/initsock-O0 build/rootfs/data/local/tmp/initsock
fi
for case_name in init-sockets init-ctl; do
    case "$case_name" in
    init-sockets) init_rc=tests/init/sockets.rc ;;
    init-ctl) init_rc=tests/init/ctl.rc ;;
    esac
    for runner in engine node; do
        if [ "$runner" = engine ]; then
            "$ENGINE" --init "$init_rc" --root build/rootfs --init-timeout 90 --init-stdio host >"$OUT/$case_name.$runner.out" 2>"$OUT/$case_name.$runner.err"
        elif [ -f "$NODE_JS" ]; then
            node "$NODE_JS" --init "$init_rc" --root build/rootfs --init-timeout 90 --init-stdio host >"$OUT/$case_name.$runner.out" 2>"$OUT/$case_name.$runner.err"
        else
            continue
        fi
        rc=$?
        if [ "$rc" = 0 ] && cmp -s "$OUT/$case_name.$runner.out" "tests/expect/$case_name.out"; then
            echo "PASS  $case_name $runner"
            pass=$((pass + 1))
        else
            echo "FAIL  $case_name $runner (exit $rc)"
            diff "tests/expect/$case_name.out" "$OUT/$case_name.$runner.out" | head -n 6 | sed 's/^/        /'
            fail=$((fail + 1))
        fi
    done
done

# Zygote (B4a, B4b), opt-in: XIAOJI_BOOT=1 boots the zygote of image/xiaoji.rc. The log must show
# the class preload ("...preloaded N classes") and the fork of system_server (zygote's own
# "System server process N has been created", B4b milestone c). About 50 s natively. Not in the
# default run, which is kept short; the boot is stopped by --init-timeout.
if [ "${XIAOJI_BOOT:-0}" = 1 ] && [ -d build/rootfs/system/bin ]; then
    XIAOJI_STAMP=1 timeout 200 "$ENGINE" --logcat --init image/xiaoji.rc --root build/rootfs --init-timeout 90 \
        >"$OUT/boot.out" 2>"$OUT/boot.err"
    if grep -q '\.\.\.preloaded [0-9][0-9]* classes' "$OUT/boot.err" &&
        grep -q 'System server process [0-9][0-9]* has been created' "$OUT/boot.err"; then
        echo "PASS  zygote preload and system_server fork (XIAOJI_BOOT)"
        pass=$((pass + 1))
    else
        echo "FAIL  zygote boot (XIAOJI_BOOT): no preload or no system_server fork line in $OUT/boot.err"
        fail=$((fail + 1))
    fi
fi

# binderLibTest (B3): libbinder end to end, with the real servicemanager and the forked
# servers. Report only, not a gate (a stretch goal, docs/boot.md B3): each case runs in its
# own process (tests/binderlib.sh), the per-case lines go to build/tests/out/binderlib.txt,
# and the counts are printed. Engine only, and it never changes the fail count. Off by
# default (XIAOJI_BINDERLIB=1 turns it on): a case that hangs in teardown is left to the
# per-case watchdog, and the whole run takes long.
BL_TEST=/data/nativetest64/binderLibTest/binderLibTest
if [ -f "build/rootfs$BL_TEST" ] && [ "${XIAOJI_BINDERLIB:-0}" != 1 ]; then
    echo "note  binderLibTest report skipped (set XIAOJI_BINDERLIB=1; it is slow, docs/boot.md B3)"
elif [ -f "build/rootfs$BL_TEST" ]; then
    mkdir -p build/rootfs/data/local/tmp
    cp tests/binderlib.sh build/rootfs/data/local/tmp/binderlib.sh
    BL_SH="/system/bin/servicemanager & $SM_WAIT; LIMIT=20 /system/bin/sh /data/local/tmp/binderlib.sh; kill \$!; wait \$!; exit 0"
    "$ENGINE" --root build/rootfs -- /system/bin/sh -c "$BL_SH" >"$OUT/binderlib.txt" 2>&1
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

echo
echo "summary: $pass passed, $fail failed, $xfail xfail"
[ "$fail" = 0 ]
