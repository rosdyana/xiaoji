#!/bin/sh
# Reference runs: execute the guest's own ARM64 binaries natively in the
# OrbStack builder VM (arm64 Linux) inside a chroot of build/rootfs, under
# strace. Outputs go to build/oracle/<case>.{out,strace,log}; they define
# what the engine must reproduce and which syscalls it needs.
#
# Usage: tools/oracle.sh            (all cases)
#        tools/oracle.sh hello-jit  (one case)
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
R=$ROOT/build/rootfs
O=$ROOT/build/oracle
ORB=${ORB:-/Applications/OrbStack.app/Contents/MacOS/bin/orb}
VM=${VM:-builder}
vm() { "$ORB" -m "$VM" sh -c "$1"; }

[ -d "$R/apex/com.android.art" ] || { echo "run tools/extract-rootfs.sh first" >&2; exit 1; }
make -s build/hello/classes.dex
mkdir -p "$O"
"$ORB" start "$VM" >/dev/null 2>&1 || true
vm "command -v strace >/dev/null || sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q strace >/dev/null"

# The chroot needs /dev, /proc, /sys and a writable /data (tmpfs, per boot).
# They are unmounted on exit so build/rootfs never keeps the VM's real /dev
# mounted inside it (a recursive delete would otherwise reach it).
teardown() {
    vm "for d in data dev proc sys; do mountpoint -q $R/\$d && sudo umount $R/\$d; done
        sudo rm -f /dev/socket/logdw" || true
}
trap teardown EXIT
vm "for d in dev proc sys; do mountpoint -q $R/\$d || sudo mount --bind /\$d $R/\$d; done
    mountpoint -q $R/data || sudo mount -t tmpfs tmpfs $R/data
    sudo mkdir -p $R/data/dalvik-cache $R/data/local/tmp
    sudo cp $ROOT/tools/guest-env.sh $R/data/env.sh
    sudo cp $ROOT/build/hello/classes.dex $R/data/hello.dex"

run() {  # name, command run by the guest's /system/bin/sh
    name=$1; shift
    case "${ONLY:-$name}" in "$name") ;; *) return 0 ;; esac
    vm "sudo python3 $ROOT/tools/fakelogd.py /dev/socket/logdw 5 > $O/$name.log &
        sleep 0.5
        sudo strace -f -qq -o $O/$name.strace chroot $R /system/bin/sh -c '. /data/env.sh; $*' > $O/$name.out 2> $O/$name.err; echo \$? > $O/$name.rc
        wait"
    printf '%-12s rc=%s  %s\n' "$name" "$(cat "$O/$name.rc")" "$(head -c 120 "$O/$name.out" | tr '\n' ' ')"
}

ONLY=${1:-}
run toybox-echo "exec /system/bin/toybox echo hi from bionic"
run toybox-ls   "exec /system/bin/toybox ls /system/bin/linker64"
run toybox-uname "exec /system/bin/toybox uname -m"
run toybox-wc   "exec /system/bin/toybox wc -c /system/build.prop"
run toybox-sha  "exec /system/bin/toybox sha256sum /apex/com.android.runtime/bin/linker64"
run toybox-lsdir "exec /system/bin/toybox ls /apex/com.android.art/bin"
run toybox-cat  "exec /system/bin/toybox cat /system/etc/hosts"
run sh-arith    'echo $((6*7))'
run sh-pipe     'echo hello | /system/bin/toybox tr a-z A-Z'
run sh-pipeline '/system/bin/toybox ls /system/bin | /system/bin/toybox wc -l'
run sh-subshell 'x=$(/system/bin/toybox uname -m); echo "arch=$x"; (exit 3); echo "rc=$?"'
run sh-bg       '/system/bin/toybox sleep 0.1 & wait; echo done'
run sh-loop     'for i in 1 2 3; do /system/bin/toybox echo $i; done | /system/bin/toybox tac'
run sh-script   'echo "#!/system/bin/sh" > /data/local/tmp/s.sh; echo "echo script ran \$1" >> /data/local/tmp/s.sh; chmod 755 /data/local/tmp/s.sh; /data/local/tmp/s.sh arg1'
run hello-int   "exec /apex/com.android.art/bin/dalvikvm64 -Xint -Ximage:/system/framework/boot.art:/system/framework/boot-framework.art -cp /data/hello.dex Hello"
run hello-jit   "exec /apex/com.android.art/bin/dalvikvm64 -Ximage:/system/framework/boot.art:/system/framework/boot-framework.art -cp /data/hello.dex Hello"
