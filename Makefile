# xiaoji: one command per job. Build outputs go to build/ (git-ignored).

EMSDK ?= $(HOME)/emsdk
EMENV := . $(EMSDK)/emsdk_env.sh >/dev/null 2>&1 &&
CC    ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
R8_VERSION := 8.2.47
ORB   := /Applications/OrbStack.app/Contents/MacOS/bin/orb
GUEST_FLAGS := -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -march=armv8-a -no-pie -mno-outline-atomics

ENGINE_SRC := $(wildcard engine/*.c)
ENGINE_HDR := $(wildcard engine/*.h)
# The block translator is wasm-only: the native build leaves engine/jit.c out.
NATIVE_SRC := $(filter-out engine/jit.c,$(ENGINE_SRC))
JIT_LIB := engine/jit_lib.js
.DEFAULT_GOAL := all
# The GL decoders, generated from gfxstream's protocol specs (Apache-2.0, vendored in third_party/gfxstream;
# tools/gen-glproto.py; docs/graphics.md, G1). tools/fetch-remote-protocols.sh checks them against the guest encoders.
GEN_SRC := build/gen/renderControl_dec.c build/gen/gles2_dec.c build/gen/gles1_dec.c build/gen/gl_defaults.c
GEN_INC := -Ibuild/gen

$(GEN_SRC): build/gen/gen_report.txt

GL_SPECS := $(wildcard third_party/gfxstream/*/*)

build/gen/gen_report.txt: tools/gen-glproto.py $(GL_SPECS)
	python3 -I tools/gen-glproto.py third_party/gfxstream build/gen

.PHONY: all engine wasm wasm-node guest-tests test test-web serve image oracle clean reset-data help

all: engine     ## default: native engine

help:           ## list targets
	@grep -E '^[a-z][a-zA-Z0-9_/.-]*:.*## ' $(MAKEFILE_LIST) | sed 's/:.*## /\t/'

engine: build/xiaoji          ## native engine -> build/xiaoji

build/xiaoji: $(NATIVE_SRC) $(GEN_SRC) $(ENGINE_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(GEN_INC) -o $@ $(NATIVE_SRC) $(GEN_SRC)

wasm: build/wasm/xiaoji.mjs   ## wasm engine -> build/wasm/xiaoji.{mjs,wasm}

# The web build: a module for a Web Worker (web/worker.js), with the guest
# filesystem built by the worker in MEMFS (createLazyFile over HTTP).
build/wasm/xiaoji.mjs: $(ENGINE_SRC) $(GEN_SRC) $(ENGINE_HDR) $(JIT_LIB)
	@mkdir -p build/wasm
	$(EMENV) emcc -O3 -std=c11 $(GEN_INC) -DXIAOJI_LAZY_FS=1 -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web,worker \
	    -sINVOKE_RUN=0 -sFORCE_FILESYSTEM -sEXPORTED_RUNTIME_METHODS=FS,callMain -sEXIT_RUNTIME=1 \
	    -sALLOW_MEMORY_GROWTH -sALLOW_TABLE_GROWTH -sMAXIMUM_MEMORY=4GB \
	    --js-library $(JIT_LIB) -o $@ $(ENGINE_SRC) $(GEN_SRC)

wasm-node: build/wasm-node/xiaoji.js   ## wasm engine for Node (reads host files directly)

build/wasm-node/xiaoji.js: $(ENGINE_SRC) $(GEN_SRC) $(ENGINE_HDR) $(JIT_LIB)
	@mkdir -p build/wasm-node
	$(EMENV) emcc -O3 -std=c11 $(GEN_INC) -sNODERAWFS=1 -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH \
	    -sALLOW_TABLE_GROWTH -sMAXIMUM_MEMORY=4GB -sEXIT_RUNTIME=1 \
	    --js-library $(JIT_LIB) -o $@ $(ENGINE_SRC) $(GEN_SRC)

guest-tests:    ## freestanding test programs, built in the builder VM -> build/tests/guest
	$(ORB) -m builder sh -c 'cd $(CURDIR) && mkdir -p build/tests/guest && \
	    for f in tests/guest/*.c; do n=$$(basename $$f .c); \
	        [ "$$n" = glibc_hello ] && continue; \
	        for O in O0 O2; do \
	            gcc -$$O $(GUEST_FLAGS) -o build/tests/guest/$$n-$$O $$f || exit 1; \
	        done; \
	    done && \
	    gcc -O2 -static -march=armv8-a -o build/tests/guest/glibc_hello-O2 tests/guest/glibc_hello.c \
	    || echo "glibc_hello: stretch build failed"'

test: engine wasm-node guest-tests build/rootfs/data/hello.dex   ## run the guest tests natively and in Node (tests/run.sh)
	tests/run.sh

build/web/manifest.json: tools/mkmanifest.py build/rootfs/data/hello.dex
	@mkdir -p build/web
	tools/mkmanifest.py build/rootfs $@

test-web: wasm build/web/manifest.json   ## run the web build in headless Chromium (tests/web.mjs)
	node tests/web.mjs hello-jit hello-int

serve: wasm build/web/manifest.json   ## dev server for the web page (web/serve.mjs), port 8080
	node web/serve.mjs --port 8080

image:          ## download + extract the Android 13 arm64 guest -> build/rootfs
	tools/fetch-image.sh
	tools/extract-rootfs.sh

oracle:         ## reference runs in the builder VM -> build/oracle
	tools/oracle.sh

build/tools/r8.jar:
	@mkdir -p build/tools
	curl -fsSL -o $@ https://dl.google.com/android/maven2/com/android/tools/r8/$(R8_VERSION)/r8-$(R8_VERSION).jar

build/hello/classes.dex: tests/hello/Hello.java build/tools/r8.jar   ## test program for ART
	@mkdir -p build/hello
	javac --release 8 -nowarn -d build/hello $<
	java -cp build/tools/r8.jar com.android.tools.r8.D8 --min-api 33 --output build/hello build/hello/Hello.class

build/rootfs/data/hello.dex: build/hello/classes.dex   ## the test dex, where the guest's /data/hello.dex is read
	@mkdir -p build/rootfs/data
	cp $< $@

reset-data:     ## clear the product tree's data/ and its metadata records (B8) -> build/rootfs-xiaoji
	python3 -I tools/reset-data.py build/rootfs-xiaoji

clean:          ## remove engine builds (keeps build/rootfs and build/image)
	rm -rf build/xiaoji build/wasm build/wasm-node build/web build/tests build/hello
