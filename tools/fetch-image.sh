#!/bin/sh
# Download Google's prebuilt AOSP Android 13 arm64 emulator image (the
# "default" variant: no Google APIs) and verify it. Output: build/image/.
set -eu
cd "$(dirname "$0")/.."

URL=https://dl.google.com/android/repository/sys-img/android/arm64-v8a-33_r02.zip
SHA1=69883e92b77f8705af135ee86e1d87589f8f113e
ZIP=build/image/arm64-v8a-33_r02.zip

mkdir -p build/image
if [ ! -f "$ZIP" ] || ! echo "$SHA1  $ZIP" | shasum -a 1 -c - >/dev/null 2>&1; then
    curl -fL --progress-bar -o "$ZIP.part" "$URL"
    echo "$SHA1  $ZIP.part" | shasum -a 1 -c -
    mv "$ZIP.part" "$ZIP"
fi
echo "$ZIP ok"
