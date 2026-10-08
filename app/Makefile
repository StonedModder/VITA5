# VITA5 native display app — build entry points.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This app is the display/UX half of VITA5: it renders the Vita's USB
# video stream, plays its audio and forwards controller input, on top of the
# ps5-native-app-boilerplate build that owns this repository (kept stock).
#
#   make deps    fetch + verify the pinned ps5-opengl SDK, stage link inputs
#   make init    set a fresh application identity in sce_sys/param.json
#   make         build the PS5 application folder via the root build
#   make test    host tests for the shared-memory protocol and converters
#   make lint    root lint
#
# The kit (draw list, components, themes, sounds, PS5 platform layer) is
# vendored unchanged under vendor/kit/ (see vendor/kit/REVISION.txt).

SHELL := /bin/bash
.DEFAULT_GOAL := app

ROOT := $(abspath ..)
HERE := $(abspath .)
# Paths the root build resolves relative to the repository root.
APP_REL := app
KIT_REL := app/vendor/kit
GENERATED_REL := app/.generated
APP_DEFINITIONS := GL_GLEXT_PROTOTYPES=1 VD_APP_REV=$(shell git rev-parse --short HEAD)
APP_INCLUDE_PATHS := $(APP_REL) $(KIT_REL) core/include \
	.deps/ps5-opengl/current/include
APP_STATIC_ARCHIVES := $(GENERATED_REL)/libps5opengl-group.a
# The process-lifetime heap (vendor/kit/runtime/app_heap.c) owns the malloc
# family; the splash picture is held until the first frame is presented
# (vendor/kit/runtime/runtime_shims.c). Both follow the kit's own build.
APP_WRAP_SYMBOLS := malloc calloc realloc free posix_memalign malloc_usable_size \
	sceSystemServiceHideSplashScreen

export APP_DEFINITIONS APP_INCLUDE_PATHS APP_STATIC_ARCHIVES APP_WRAP_SYMBOLS

HOST_CXX := $(shell command -v clang++-18 2>/dev/null || command -v clang++ 2>/dev/null || echo clang++)
HOST_CC := $(shell command -v clang-18 2>/dev/null || command -v clang 2>/dev/null || echo clang)

# New input modules need no build-list edit in the PS5 build: the root build
# (tools/build.sh) compiles every *.c/*.cc/*.cpp under app/, so src/vd/
# touch_bridge.cpp and src/screen_inputtest.cpp are picked up the moment they
# land. The host test link below is the only explicit source list; touch_bridge
# joins it when present so pad_bridge's calls resolve there too.
# src/screen_inputtest.cpp stays out on purpose: it is a kit-UI Screen TU and
# the host tests never link the screens.
HOST_TEST_OPTIONAL_SOURCES := $(wildcard $(HERE)/src/vd/touch_bridge.cpp)

.PHONY: all app deps opengl init test lint doctor clean help

all: app

## deps: fetch and verify the pinned SDKs and stage the link inputs
deps: opengl

opengl:
	@printf '%s\n' '==> [opengl] Preparing the ps5-opengl SDK link group'
	@bash $(HERE)/tools/prepare-opengl.sh

## app: build the PS5 application folder through the stock root build
app: opengl
	@$(MAKE) -C $(ROOT) app \
		APP_SOURCE_DIR=$(APP_REL) \
		APP_PARAM=$(APP_REL)/sce_sys/param.json \
		APP_SCE_SYS=$(APP_REL)/sce_sys \
		APP_ASSETS=$(APP_REL)/assets \
		APP_DEFINITIONS="$(APP_DEFINITIONS)" \
		APP_INCLUDE_PATHS="$(APP_INCLUDE_PATHS)" \
		APP_STATIC_ARCHIVES="$(APP_STATIC_ARCHIVES)" \
		APP_WRAP_SYMBOLS="$(APP_WRAP_SYMBOLS)"

## init: write a fresh identity into app/sce_sys/param.json
## usage: make init TITLE_ID=PPSA12345 APP_NAME="VITA5"
init:
	@command -v python3 >/dev/null
	@TITLE_ID="$(TITLE_ID)" APP_NAME="$(APP_NAME)" APP_CATEGORY="$(or $(APP_CATEGORY),game)" \
		CONTENT_SUFFIX="$(CONTENT_SUFFIX)" \
		bash $(ROOT)/tools/init-project.sh $(HERE)/sce_sys/param.json

## test: host-only checks for the shared-memory protocol and converters
test:
	@mkdir -p $(HERE)/.generated
	@$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -c \
		-I$(ROOT)/core/include $(HERE)/src/vd/core_pad.c -o $(HERE)/.generated/core_pad.o
	@$(HOST_CC) -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Wpedantic -Werror -c \
		-I$(ROOT)/core/include $(HERE)/src/vd/core_ipc.c -o $(HERE)/.generated/core_ipc.o
	@$(HOST_CXX) -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread \
		-DVD5_HOST_TESTS -I$(HERE)/src -I$(ROOT)/core/include -I$(HERE)/vendor/kit \
		$(HERE)/tests/vd_tests.cpp $(HERE)/src/vd/vd_shm.cpp \
		$(HERE)/src/vd/nv12.cpp $(HERE)/src/vd/pad_bridge.cpp \
		$(HERE)/src/vd_settings.cpp $(HERE)/vendor/kit/core/save_file.cpp \
		$(HOST_TEST_OPTIONAL_SOURCES) \
		$(HERE)/.generated/core_pad.o $(HERE)/.generated/core_ipc.o -o $(HERE)/.generated/vd_tests
	@$(HERE)/.generated/vd_tests
	@bash $(ROOT)/tools/test-upscaler-fallback.sh
	@bash $(ROOT)/tools/test-dock-ipc.sh
	@bash $(ROOT)/tools/repro-dock-shutdown.sh

lint:
	@$(MAKE) -C $(ROOT) lint

doctor:
	@$(MAKE) -C $(ROOT) doctor

clean:
	@$(MAKE) -C $(ROOT) clean
	@rm -rf $(HERE)/.generated

help:
	@sed -n '1,20p' $(MAKEFILE_LIST)
