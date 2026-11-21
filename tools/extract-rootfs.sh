#!/bin/sh
# Turn the downloaded image zip into a plain directory tree at build/rootfs:
#   system.img (GPT disk) -> super partition -> system, system_ext, product,
#   vendor (ext4) -> one tree; every APEX payload -> /apex/<name>;
#   xiaoji's linker config -> /linkerconfig.
# Runs on macOS; the ext4 steps run in the OrbStack builder VM (debugfs).
# Needs ~3 GB free while running; intermediate images are deleted at the end.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
ORB=${ORB:-/Applications/OrbStack.app/Contents/MacOS/bin/orb}
VM=${VM:-builder}
vm() { "$ORB" -m "$VM" sh -c "export PATH=\$PATH:/usr/sbin:/sbin; cd '$ROOT' && $1"; }

ZIP=build/image/arm64-v8a-33_r02.zip
[ -f "$ZIP" ] || { echo "run tools/fetch-image.sh first" >&2; exit 1; }
if [ -e build/rootfs ]; then
    echo "build/rootfs already exists; move it away to re-extract" >&2
    exit 1
fi

"$ORB" start "$VM" >/dev/null 2>&1 || true
vm "command -v debugfs >/dev/null || sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q e2fsprogs gdisk >/dev/null"

echo "== unzip"
unzip -o -q "$ZIP" 'arm64-v8a/system.img' -d build/image

# The super partition's start, from the GPT (the emulator image has vbmeta, super).
START=$(vm "sgdisk -p build/image/arm64-v8a/system.img" | awk '$NF == "super" { print $2 }')
[ -n "$START" ] || { echo "no 'super' partition in system.img; layout changed" >&2; exit 1; }

echo "== super partition (sector $START)"
python3 -I tools/lpunpack.py build/image/arm64-v8a/system.img build/image/parts --offset $((START * 512))
for p in system system_ext product vendor; do
    [ -f "build/image/parts/$p.img" ] || { echo "missing logical partition $p" >&2; exit 1; }
done

echo "== partitions -> build/rootfs"
mkdir -p build/rootfs
vm "debugfs -R 'rdump / build/rootfs' build/image/parts/system.img 2>&1 | grep -v '^debugfs' || true"
mkdir -p build/rootfs-parts
for p in system_ext product vendor; do
    mkdir -p "build/rootfs-parts/$p"
    vm "debugfs -R 'rdump / build/rootfs-parts/$p' build/image/parts/$p.img 2>&1 | grep -v '^debugfs' || true"
    mv build/rootfs-parts/$p/* "build/rootfs/$p/"
done

echo "== APEX payloads -> build/rootfs/apex"
python3 -I tools/apexunpack.py build/rootfs/system/apex build/image/apex >/dev/null
for img in build/image/apex/*.img; do
    n=$(basename "$img" .img)
    mkdir -p "build/rootfs/apex/$n"
    vm "debugfs -R 'rdump / build/rootfs/apex/$n' $img 2>&1 | grep -v '^debugfs' || true"
done

# What apexd would write at boot; linkerconfig and nativeloader read it.
{
    echo '<?xml version="1.0" encoding="utf-8"?>'
    echo '<apex-info-list>'
    for d in build/rootfs/apex/*/; do
        n=$(basename "$d")
        src=$(ls build/rootfs/system/apex | grep -E "^$n\.c?apex$" | head -1)
        echo "  <apex-info moduleName=\"$n\" modulePath=\"/system/apex/$src\" preinstalledModulePath=\"/system/apex/$src\" versionCode=\"1\" versionName=\"1\" isFactory=\"true\" isActive=\"true\" provideSharedApexLibs=\"false\" />"
    done
    echo '</apex-info-list>'
} > build/rootfs/apex/apex-info-list.xml

python3 -I tools/gen-ldconfig.py build/rootfs

echo "== cleanup"
rm -f build/image/arm64-v8a/system.img build/image/parts/*.img build/image/apex/*.img build/image/apex/*.manifest.pb
du -sh build/rootfs
test -f build/rootfs/apex/com.android.art/bin/dalvikvm64
test -f build/rootfs/apex/com.android.runtime/bin/linker64
echo "build/rootfs ok"
