# xiaoji: one command per job. Build outputs go to build/ (git-ignored).

EMSDK ?= $(HOME)/emsdk
EMENV := . $(EMSDK)/emsdk_env.sh >/dev/null 2>&1 &&
CC    ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
R8_VERSION := 8.2.47

ENGINE_SRC := $(wildcard engine/*.c)
ENGINE_HDR := $(wildcard engine/*.h)

.PHONY: all engine wasm image oracle clean help

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
	rm -rf build/xiaoji build/wasm build/hello
