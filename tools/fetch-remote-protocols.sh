#!/bin/sh
# Read the emugen protocol specs of vulkan-cereal from the remote build machine and generate the engine's GL
# decoders (tools/gen-glproto.py):
#   tools/fetch-remote-protocols.sh [host]     (default host: $XIAOJI_REMOTE_HOST)
# The specs go to build/spec/protocols and the generated code to build/gen. Neither is checked in: the
# vulkan-cereal package is marked special licensing (its METADATA), and the license of the specs is not
# settled (docs/graphics.md, G1). make gl-gen runs this when build/spec is missing.
set -eu
cd "$(dirname "$0")/.."
HOST=${1:-${XIAOJI_REMOTE_HOST:?pass the build host or set XIAOJI_REMOTE_HOST}}
# Fetches the A13 guest encoders' opcode headers and checks the vendored specs (third_party/gfxstream) against them.
# The guest encoders: their opcode headers, and the fixed request sizes (sizeWithoutChecksum), are what
# gen-glproto.py checks its numbers and sizes against.
G='xiaoji-aosp/device/generic/goldfish-opengl/system'
mkdir -p build/spec/guest
rsync -a "$HOST:$G/renderControl_enc/renderControl_opcodes.h" build/spec/guest/renderControl_opcodes.h
rsync -a "$HOST:$G/GLESv2_enc/gl2_opcodes.h" build/spec/guest/gles2_opcodes.h
rsync -a "$HOST:$G/GLESv1_enc/gl_opcodes.h" build/spec/guest/gles1_opcodes.h
rsync -a "$HOST:$G/renderControl_enc/renderControl_enc.cpp" build/spec/guest/renderControl_enc.cpp
rsync -a "$HOST:$G/GLESv2_enc/gl2_enc.cpp" build/spec/guest/gl2_enc.cpp
rsync -a "$HOST:$G/GLESv1_enc/gl_enc.cpp" build/spec/guest/gl_enc.cpp
mkdir -p build/gen-check
python3 -I tools/gen-glproto.py third_party/gfxstream build/gen-check --check-guest build/spec/guest
