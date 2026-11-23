set -eu
cd "$(dirname "$0")/.."
HOST=${1:-build-host}
mkdir -p build/spec/ids
rsync -a "$HOST:xiaoji-aosp/system/core/libcutils/include/private/android_filesystem_config.h" build/spec/ids/
rsync -a "$HOST:xiaoji-aosp/bionic/libc/kernel/uapi/linux/capability.h" build/spec/ids/
rsync -a "$HOST:xiaoji-aosp/system/core/init/capabilities.cpp" build/spec/ids/
python3 -I tools/gen-ids.py build/spec/ids/android_filesystem_config.h build/spec/ids/capability.h \
    build/spec/ids/capabilities.cpp engine/ids_gen.h
