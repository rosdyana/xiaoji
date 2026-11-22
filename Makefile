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

.PHONY: all engine wasm wasm-node guest-tests test image oracle clean help

all: engine     ## default: native engine

help:           ## list targets
	@grep -E '^[a-z][a-zA-Z0-9_/.-]*:.*## ' $(MAKEFILE_LIST) | sed 's/:.*## /\t/'

engine: build/xiaoji          ## native engine -> build/xiaoji

build/xiaoji: $(ENGINE_SRC) $(ENGINE_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(ENGINE_SRC)

wasm: build/wasm/xiaoji.mjs   ## wasm engine -> build/wasm/xiaoji.{mjs,wasm}

build/wasm/xiaoji.mjs: $(ENGINE_SRC) $(ENGINE_HDR)
	@mkdir -p build/wasm
	$(EMENV) emcc -O3 -std=c11 -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=node,web \
	    -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=4GB -o $@ $(ENGINE_SRC)

wasm-node: build/wasm-node/xiaoji.js   ## wasm engine for Node (reads host files directly)

build/wasm-node/xiaoji.js: $(ENGINE_SRC) $(ENGINE_HDR)
	@mkdir -p build/wasm-node
	$(EMENV) emcc -O3 -std=c11 -sNODERAWFS=1 -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH \
	    -sMAXIMUM_MEMORY=4GB -sEXIT_RUNTIME=1 -o $@ $(ENGINE_SRC)

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

test: engine guest-tests   ## run the guest tests natively and in Node (tests/run.sh)
	tests/run.sh

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

clean:          ## remove engine builds (keeps build/rootfs and build/image)
	rm -rf build/xiaoji build/wasm build/wasm-node build/tests build/hello
