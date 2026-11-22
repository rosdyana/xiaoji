#!/bin/sh
# Build xiaoji's trimmed AOSP product on the remote build machine.
#   tools/remote-build.sh [host]     (default host: build-host)
# Copies aosp/device/xiaoji into the remote tree (~/xiaoji-aosp) and starts
# `m` for xiaoji_arm64-userdebug in the background; log: ~/xiaoji-aosp/xiaoji-build.log
set -eu
cd "$(dirname "$0")/.."
HOST=${1:-build-host}
TREE='~/xiaoji-aosp'
rsync -a --delete aosp/device/xiaoji/ "$HOST:xiaoji-aosp/device/xiaoji/"
ssh "$HOST" "cd $TREE && cat > xiaoji-build.sh <<'EOS'
#!/bin/bash
cd ~/xiaoji-aosp
renice -n 5 \$\$ >/dev/null
source build/envsetup.sh
lunch xiaoji_arm64-userdebug
echo \"== build start \$(date -u)\"
time m -j20
echo \"== build exit \$? \$(date -u)\"
EOS
chmod +x xiaoji-build.sh && nohup ./xiaoji-build.sh > xiaoji-build.log 2>&1 < /dev/null &
echo started"
