# VITA5 — dock payload (anonymous producer ring + loopback IPC + USB workers).
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the PS5 payload ELFs with prospero-clang (prospero.mk), the same
# toolchain path used by the rest of the payload. dev/usb headers resolve via the SDK's
# default sysroot (target/include).
#
#   make VITA5-payload.elf   full dock payload (ring + producers + pad fwd)
#   make VITA5-padtest.elf   scripted pad-report sender for end-to-end input
#                            testing without the native app
#
# Usage (WSL):
#   export PS5_PAYLOAD_SDK=/mnt/c/ps5-payload-sdk/ps5-payload-sdk
#   make -C payload

SHELL := /bin/bash

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CC       := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
CORE_SRC := ../core/src/usb_vita.c ../core/src/uvc_protocol.c \
	../core/src/uvc_stream.c ../core/src/usb_transfer.c \
	../core/src/uac_audio.c ../core/src/pad_passthrough.c
CORE_HDR := $(wildcard ../core/include/*.h)

PAYLOAD  := VITA5-payload.elf
PADTEST  := VITA5-padtest.elf
EPTEST   := VITA5-eptest.elf
TTTEST   := VITA5-touchtest.elf
TSSWIPE  := VITA5-touchswipe.elf
SDIAG    := VITA5-streamdiag.elf
ISOLATED := VITA5-streamdiag-video-main.elf VITA5-streamdiag-video-worker.elf \
	VITA5-streamdiag-audio-main.elf VITA5-streamdiag-audio-worker.elf

CFLAGS  += -D__PROSPERO__ -std=gnu11 -Wall -Wextra -Werror -g -O2 -fPIC \
	-fno-stack-protector -ffunction-sections -fdata-sections \
	-I../core/include -I.
LDFLAGS += -Wl,--gc-sections,--build-id=sha1
VD_BUILD_REV := $(shell git rev-parse --short HEAD 2>/dev/null || printf unknown)
CFLAGS += -DVD_BUILD_REV='"$(VD_BUILD_REV)"'
LDLIBS  := -lkernel_sys

all: $(PAYLOAD) $(PADTEST) $(EPTEST) $(TTTEST) $(TSSWIPE) $(SDIAG)

$(PAYLOAD): main.c dock_status.c dock_ipc.c ../core/src/vd_ipc.c vd_shm_prod.c video_producer.c audio_producer.c \
		pad_forwarder.c $(wildcard *.h) $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ main.c dock_status.c dock_ipc.c ../core/src/vd_ipc.c vd_shm_prod.c video_producer.c \
		audio_producer.c pad_forwarder.c $(CORE_SRC) $(LDFLAGS) $(LDLIBS)
	@echo "built $@"

$(PADTEST): padtest.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ padtest.c $(CORE_SRC) $(LDFLAGS) $(LDLIBS)
	@echo "built $@"

$(EPTEST): eptest.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ eptest.c $(CORE_SRC) $(LDFLAGS) $(LDLIBS)
	@echo "built $@"

$(TTTEST): touchtest.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ touchtest.c $(CORE_SRC) $(LDFLAGS) $(LDLIBS)
	@echo "built $@"

$(TSSWIPE): touchswipe.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ touchswipe.c $(CORE_SRC) $(LDFLAGS) $(LDLIBS)
	@echo "built $@"

$(SDIAG): streamdiag.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ streamdiag.c $(CORE_SRC) $(LDFLAGS) $(LDLIBS)
	@echo "built $@"

$(ISOLATED): streamdiag.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -DVD_USB_ENUM_TRACE=1 $(if $(findstring video,$@),-DDIAG_VIDEO_ONLY=1,-DDIAG_AUDIO_ONLY=1) \
		$(if $(findstring worker,$@),-DDIAG_WORKER=1,-DDIAG_WORKER=0) \
		-o $@ streamdiag.c $(CORE_SRC) $(LDFLAGS) $(LDLIBS)
	@echo "built $@"

clean:
	rm -f $(PAYLOAD) $(PADTEST) $(EPTEST) $(TTTEST) $(TSSWIPE) $(SDIAG) $(ISOLATED)

.PHONY: all clean
