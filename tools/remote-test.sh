#!/bin/sh
# Runs the guest suite (tests/run.sh) on the build machine (XIAOJI_REMOTE_HOST: Ubuntu 20.04, x86_64,
# gcc 9.4, no clang, no node), in ~/xiaoji-remote. Nothing outside that directory is written, except the
# user-local Node install under ~/xiaoji-remote/tools/node. ~/xiaoji-aosp is not touched.
#
#   tools/remote-test.sh [sync|build|run|all]     (default: all)
#
#   sync   the tracked sources (git ls-files of the working tree; files no longer tracked are removed
#          with the manifest ~/xiaoji-remote/.sources), build/gen (the generated GL decoders), the guest
#          root trees build/rootfs and build/rootfs-xiaoji (deltas only after the first sync; their data/
#          keeps only the static inputs data/hello.dex and data/nativetest64, and the metadata sidecar is
#          not copied: runs write the rest), the guest test
#          binaries build/tests/guest, the recorded oracle outputs build/oracle/*.{out,rc}, and the Node
#          build of the engine build/wasm-node (built on the Mac: no emscripten on the remote).
#   build  Node, user-local, under ~/xiaoji-remote/tools/node (the version of the local `node`, from
#          nodejs.org, checked against SHASUMS256.txt; no sudo, no shell profile change); then the
#          native engine with gcc (make engine CC=gcc, a fresh build), warnings counted.
#   run    tests/run.sh with XIAOJI_ORACLE=recorded (the builder-VM oracle is never called) and
#          XIAOJI_JOBS=$XIAOJI_REMOTE_JOBS (default 16). The mode variables XIAOJI_JIT, XIAOJI_BOOT,
#          XIAOJI_ROOT and XIAOJI_BINDERLIB are passed through when they are set on the Mac. The report
#          comes back to build/tests/remote-out/report.txt, and the outputs of the failing cases are copied
#          to build/tests/remote-out/ too. The exit status is that of tests/run.sh.
#
# Environment: XIAOJI_REMOTE_HOST (required), XIAOJI_REMOTE_JOBS (default 16),
# XIAOJI_REMOTE_SKIP (default exec: the guest programs left out, passed as XIAOJI_SKIP; empty runs all of them).
# Stays on the Mac: the oracle refresh (tools/oracle.sh, make oracle, which needs the OrbStack builder VM),
# make test-web (headless Chromium), and the wasm builds (make wasm-node, make wasm).
set -eu

cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
HOST=${XIAOJI_REMOTE_HOST:?set XIAOJI_REMOTE_HOST to the build host}
JOBS=${XIAOJI_REMOTE_JOBS:-16}
RDIR=xiaoji-remote # relative to the remote home directory
# ssh's key-exchange warnings would end up in the report (stderr of the run): LogLevel=ERROR keeps them out.
RSYNC_RSH="ssh -o LogLevel=ERROR"
export RSYNC_RSH
ssh() { command ssh -o LogLevel=ERROR "$@"; }
LOCAL_OUT=build/tests/remote-out
STEP=${1:-all}
case "$STEP" in sync | build | run | all) ;; *)
    echo "usage: tools/remote-test.sh [sync|build|run|all]" >&2
    exit 2
    ;;
esac
TMP=$(mktemp -d "${TMPDIR:-/tmp}/xiaoji-remote.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# Copies the sources (tracked files) and the build inputs that are not in git.
do_sync() {
    echo "sync: tracked sources"
    ssh "$HOST" "mkdir -p $RDIR/build $RDIR/tools"
    git ls-files >"$TMP/new"
    ssh "$HOST" "cat $RDIR/.sources 2>/dev/null || true" >"$TMP/old"
    sort "$TMP/new" >"$TMP/new.s"
    sort "$TMP/old" >"$TMP/old.s"
    comm -23 "$TMP/old.s" "$TMP/new.s" >"$TMP/stale"
    if [ -s "$TMP/stale" ]; then
        echo "sync: removing $(wc -l <"$TMP/stale" | tr -d ' ') files no longer tracked"
        tr '\n' '\0' <"$TMP/stale" | ssh "$HOST" "cd $RDIR && xargs -0 rm -f --"
    fi
    rsync -a --files-from="$TMP/new" ./ "$HOST:$RDIR/"
    ssh "$HOST" "cat > $RDIR/.sources" <"$TMP/new.s"

    echo "sync: build/gen"
    rsync -a build/gen/ "$HOST:$RDIR/build/gen/"

    echo "sync: guest root trees (data/ keeps only the static inputs: hello.dex and nativetest64/)"
    for tree in rootfs rootfs-xiaoji; do
        [ -d "build/$tree" ] || {
            echo "sync: build/$tree missing, skipped"
            continue
        }
        # --delete removes the files of an older tree (the excluded data/ and sidecar are kept: rsync protects them).
        rsync -a --delete --include='data/' --include='data/hello.dex' --include='data/nativetest64/' \
            --include='data/nativetest64/**' --exclude='data/*' \
            --exclude='.xiaoji-meta' --exclude='.xiaoji-meta.tmp' \
            "build/$tree/" "$HOST:$RDIR/build/$tree/"
    done

    echo "sync: guest test binaries and recorded oracle outputs"
    rsync -a build/tests/guest/ "$HOST:$RDIR/build/tests/guest/"
    rsync -a --include='*/' --include='*.out' --include='*.rc' --exclude='*' \
        build/oracle/ "$HOST:$RDIR/build/oracle/"

    echo "sync: wasm Node build"
    rsync -a build/wasm-node/ "$HOST:$RDIR/build/wasm-node/"
}

# Node (user-local) and the gcc build of the native engine.
do_build() {
    NODE_VER=$(node --version)
    echo "build: node $NODE_VER (user-local, ~/$RDIR/tools/node)"
    ssh "$HOST" "sh -s" <<EOF
set -e
cd \$HOME/$RDIR/tools
if [ -x node/bin/node ] && [ "\$(node/bin/node --version)" = "$NODE_VER" ]; then
    exit 0
fi
name=node-$NODE_VER-linux-x64.tar.xz
rm -rf node.new && mkdir node.new
curl -fsSL "https://nodejs.org/dist/$NODE_VER/\$name" -o "\$name"
curl -fsSL "https://nodejs.org/dist/$NODE_VER/SHASUMS256.txt" -o SHASUMS256.txt
grep " \$name\$" SHASUMS256.txt | sha256sum -c - >/dev/null
tar -xJf "\$name" -C node.new --strip-components=1
rm -rf node && mv node.new node && rm -f "\$name" SHASUMS256.txt
EOF
    echo "build: engine with gcc"
    ssh "$HOST" "sh -s" >"$TMP/gcc.rc" <<EOF || true
cd \$HOME/$RDIR
mkdir -p build
rm -f build/xiaoji
rc=0
make engine CC=gcc >build/gcc-engine.log 2>&1 || rc=\$?
echo "rc=\$rc warnings=\$(grep -c 'warning:' build/gcc-engine.log || true) gcc=\$(gcc -dumpfullversion)"
EOF
    cat "$TMP/gcc.rc"
    if ! grep -q '^rc=0 warnings=0 ' "$TMP/gcc.rc"; then
        echo "build: the gcc build failed or warned; see ~/$RDIR/build/gcc-engine.log on the remote" >&2
        return 1
    fi
}

# Runs tests/run.sh on the remote, and collects the report and the failing outputs.
do_run() {
    pass_env=""
    for v in XIAOJI_JIT XIAOJI_BOOT XIAOJI_ROOT XIAOJI_BINDERLIB; do
        eval "set_now=\${$v+x}"
        if [ -n "$set_now" ]; then
            eval "val=\${$v}"
            pass_env="$pass_env $v='$val'"
        fi
    done
    # exec: its expected output names the checkout's absolute path (argv[0] and /proc/self/exe of the guest
    # program), and it needs a /tmp without the files a previous run left. Skipped on the remote unless
    # XIAOJI_REMOTE_SKIP says otherwise (empty: run it). See docs/testing.md.
    skip=${XIAOJI_REMOTE_SKIP-exec}
    pass_env="$pass_env XIAOJI_SKIP='$skip'"
    mkdir -p "$LOCAL_OUT"
    rm -rf "$LOCAL_OUT"/*
    echo "run: tests/run.sh on $HOST, XIAOJI_JOBS=$JOBS XIAOJI_ORACLE=recorded$pass_env"
    # The guest programs use fixed /tmp/xiaoji-* paths, and the engine makes /tmp/xiaoji-tmpfs-<pid>-<n> for tmpfs
    # mounts (a boot killed at its timeout leaves them): start from none, and leave none behind.
    ssh "$HOST" "rm -rf /tmp/xiaoji-*" 2>/dev/null || true
    rc=0
    ssh "$HOST" "cd $RDIR && env PATH=\$HOME/$RDIR/tools/node/bin:\$PATH XIAOJI_ORACLE=recorded XIAOJI_JOBS=$JOBS$pass_env tests/run.sh" \
        >"$LOCAL_OUT/report.txt" 2>&1 || rc=$?
    ssh "$HOST" "rm -rf /tmp/xiaoji-*" 2>/dev/null || true
    cat "$LOCAL_OUT/report.txt"

    # The outputs of the failing cases: the names in the FAIL lines (binder-driver's files are binder.*).
    awk '/^FAIL  /{print $2}' "$LOCAL_OUT/report.txt" | sort -u >"$TMP/fail-names"
    ssh "$HOST" "ls -1 $RDIR/build/tests/out" >"$TMP/remote-files" 2>/dev/null || true
    awk -v names="$TMP/fail-names" '
        BEGIN { while ((getline n < names) > 0) { if (n == "binder-driver") pre["binder."] = 1; else pre[n "."] = 1 } }
        { for (p in pre) if (index($0, p) == 1) { print; break } }' "$TMP/remote-files" >"$TMP/fail-files"
    if [ -s "$TMP/fail-files" ]; then
        ssh "$HOST" "cd $RDIR/build/tests/out && tar -cf - -T -" <"$TMP/fail-files" |
            tar -xf - -C "$LOCAL_OUT"
        echo "run: $(wc -l <"$TMP/fail-files" | tr -d ' ') output files of the failing cases copied to $LOCAL_OUT"
    fi
    return "$rc"
}

case "$STEP" in
sync) do_sync ;;
build) do_build ;;
run) do_run ;;
all)
    do_sync
    do_build
    do_run
    ;;
esac
