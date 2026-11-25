#!/bin/sh
# Copy xiaoji's own AOSP build (xiaoji_arm64, see tools/remote-build.sh) from
# the remote build machine into a plain rootfs directory on this machine:
#   tools/fetch-remote-rootfs.sh [host] [dest]
#   (defaults: $XIAOJI_REMOTE_HOST, build/rootfs-xiaoji)
# The product's APEXes are flattened (plain directories in /system/apex), so
# each one is copied to /apex/<name> as apexd would bind-mount it. Then the
# same apex-info-list and linker config as tools/extract-rootfs.sh are added.
set -eu
cd "$(dirname "$0")/.."
HOST=${1:-${XIAOJI_REMOTE_HOST:?pass the build host or set XIAOJI_REMOTE_HOST}}
DEST=${2:-build/rootfs-xiaoji}
OUT=xiaoji-aosp/out/target/product/xiaoji_arm64

mkdir -p "$DEST"
echo "== root, system, system_ext, product, vendor"
rsync -a --delete "$HOST:$OUT/root/" "$DEST/" --exclude system --exclude system_ext --exclude product --exclude vendor --exclude apex
for p in system system_ext product vendor; do
    mkdir -p "$DEST/$p"
    rsync -a --delete "$HOST:$OUT/$p/" "$DEST/$p/"
done

echo "== /apex/<name> from the flattened /system/apex"
mkdir -p "$DEST/apex"
# An APEX the product no longer has (com.android.cellbroadcast, docs/demo.md) would stay in /apex and in the list.
# Links (init's /apex/com.android.vndk.v33, image/xiaoji.rc) are not APEX copies and stay.
for d in "$DEST"/apex/*/; do
    n=$(basename "$d")
    if [ ! -L "$DEST/apex/$n" ] && [ ! -d "$DEST/system/apex/$n" ]; then
        echo "   removing /apex/$n (not in /system/apex)"
        rm -rf "${DEST:?}/apex/$n"
    fi
done
for d in "$DEST"/system/apex/*/; do
    n=$(basename "$d")
    rsync -a --delete "$d" "$DEST/apex/$n/"
done
# The VNDK APEX is installed as com.android.vndk.v<N>; Android also exposes it
# as com.android.vndk.current only through apexd, which we don't run.

{
    echo '<?xml version="1.0" encoding="utf-8"?>'
    echo '<apex-info-list>'
    for d in "$DEST"/apex/*/; do
        n=$(basename "$d")
        echo "  <apex-info moduleName=\"$n\" modulePath=\"/system/apex/$n\" preinstalledModulePath=\"/system/apex/$n\" versionCode=\"1\" versionName=\"1\" isFactory=\"true\" isActive=\"true\" provideSharedApexLibs=\"false\" />"
    done
    echo '</apex-info-list>'
} > "$DEST/apex/apex-info-list.xml"

mkdir -p "$DEST/data" "$DEST/linkerconfig" "$DEST/dev" "$DEST/proc" "$DEST/sys"
python3 -I tools/gen-ldconfig.py "$DEST"
test -f "$DEST/apex/com.android.art/bin/dalvikvm64"
du -sh "$DEST"
echo "$DEST ok"
