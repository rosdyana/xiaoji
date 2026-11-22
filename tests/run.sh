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
    hello-int) ARGV=(/apex/com.android.art/bin/dalvikvm64 -Xint -Ximage:/system/framework/boot.art -cp /data/hello.dex Hello) ;;
    hello-jit) ARGV=(/apex/com.android.art/bin/dalvikvm64 -Ximage:/system/framework/boot.art -cp /data/hello.dex Hello) ;;
    esac
}
ROOTFS_CASES="toybox-echo toybox-ls toybox-uname toybox-wc toybox-sha toybox-lsdir toybox-cat sh-arith hello-int hello-jit"

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

echo
echo "summary: $pass passed, $fail failed, $xfail xfail"
[ "$fail" = 0 ]
