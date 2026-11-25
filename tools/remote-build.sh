#!/bin/sh
# Build xiaoji's trimmed AOSP product on the remote build machine.
#   tools/remote-build.sh [host]     (default host: $XIAOJI_REMOTE_HOST)
# Copies aosp/device/xiaoji into the remote tree (~/xiaoji-aosp) and starts
# `m` for xiaoji_arm64-userdebug in the background; log: ~/xiaoji-aosp/xiaoji-build.log
#
# AOSP's Calculator (docs/demo.md) left the AOSP manifest after Android 9, so the 13 tree has no copy, and this
# repository does not carry Android sources: its last platform release is cloned at a pinned commit into
# device/xiaoji/apps/ExactCalculator/upstream on the build machine (kept across syncs), without its Android.mk and
# CleanSpec.mk (device/xiaoji/apps/ExactCalculator/Android.bp is the build rule).
# XIAOJI_INSTALLCLEAN=1 runs `m installclean` first: an incremental build leaves the files of removed packages in
# out/target/product, and tools/fetch-remote-rootfs.sh copies them.
set -eu
cd "$(dirname "$0")/.."
HOST=${1:-${XIAOJI_REMOTE_HOST:?pass the build host or set XIAOJI_REMOTE_HOST}}
TREE='~/xiaoji-aosp'
CALC_URL=https://android.googlesource.com/platform/packages/apps/ExactCalculator
CALC_BRANCH=pie-platform-release
CALC_COMMIT=076857e77474b03ab8e82734dd8390ea8cc7c693
rsync -a --delete --exclude /apps/ExactCalculator/upstream/ aosp/device/xiaoji/ "$HOST:xiaoji-aosp/device/xiaoji/"
ssh "$HOST" "set -e; d=xiaoji-aosp/device/xiaoji/apps/ExactCalculator/upstream
if [ \"\$(git -C \$d rev-parse HEAD 2>/dev/null)\" != $CALC_COMMIT ]; then
    rm -rf \$d
    git clone -q --depth 1 -b $CALC_BRANCH $CALC_URL \$d
    test \"\$(git -C \$d rev-parse HEAD)\" = $CALC_COMMIT || { echo 'ExactCalculator: not the pinned commit' >&2; exit 1; }
fi
rm -f \$d/Android.mk \$d/CleanSpec.mk"
ssh "$HOST" "cd $TREE && cat > xiaoji-build.sh <<'EOS'
#!/bin/bash
cd ~/xiaoji-aosp
renice -n 5 \$\$ >/dev/null
source build/envsetup.sh
# The build's user and host go into build.prop (ro.build.user, ro.build.host, the build number Settings shows) and
# from there into the public snapshot: fixed names, not the build machine's.
export BUILD_USERNAME=xiaoji BUILD_HOSTNAME=xiaoji
lunch xiaoji_arm64-userdebug
echo \"== build start \$(date -u)\"
if [ \"\$1\" = 1 ]; then m installclean; fi
time m -j20
echo \"== build exit \$? \$(date -u)\"
EOS
chmod +x xiaoji-build.sh && nohup ./xiaoji-build.sh ${XIAOJI_INSTALLCLEAN:-0} > xiaoji-build.log 2>&1 < /dev/null &
echo started"
