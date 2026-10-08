TARGET	= input_receiver
# SPDX-License-Identifier: GPL-3.0-or-later
OBJS	= src/input_receiver.o src/pad_wire.o

LIBS	= -lSceSysmemForDriver_stub -lSceThreadmgrForDriver_stub \
	-lSceCpuForDriver_stub -lSceUdcdForDriver_stub \
	-lSceCtrlForDriver_stub -lSceKernelSuspendForDriver_stub \
	-lSceSysclibForDriver_stub -lSceIofilemgrForDriver_stub -ltaihenForKernel_stub

# Build mode:
#   standalone (default) - self-contained taiHEN kplugin, hooks ksceUdcdRegister
#   integrated           - compiled into a patched vita-udcd-uvc tree (see
#                          integration/vita-udcd-uvc-input.patch; that build
#                          uses the vita-udcd-uvc Makefile, not this one)
MODE	?= standalone
ifeq ($(MODE), integrated)
CFLAGS	+= -DINPUT_RECEIVER_INTEGRATED
endif

ifeq ($(DEBUG), 1)
CFLAGS	+= -DDEBUG
endif

PREFIX	= arm-vita-eabi
CC	= $(PREFIX)-gcc
# -std=gnu17: taihen.h's TAI_CONTINUE casts to unprototyped fn pointers '()';
# under GCC 15's default C23 those are zero-arg prototypes and fail to compile.
CFLAGS	+= -std=gnu17 -Wl,-q -Wall -O2 -nostartfiles -mcpu=cortex-a9 -mthumb-interwork -Iinclude
DEPS	= $(OBJS:.o=.d)

all: $(TARGET).skprx

%.skprx: %.velf
	vita-make-fself -c $< $@

%.velf: %.elf
	vita-elf-create -e $(TARGET).yml $< $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ---------------------------------------------------------------------------
# Host-side test of the pure wire decoder (pad_wire). This does NOT build the
# kernel plugin. On Windows run it through WSL (see README.md):
#   wsl.exe -d Ubuntu -e bash -lc 'make host-test'
# ---------------------------------------------------------------------------
HOSTCC	?= cc
HOSTCCFLAGS ?= -std=c99 -Wall -Wextra -Werror -O1

host-test:
	$(HOSTCC) $(HOSTCCFLAGS) -Iinclude -o pad_wire_test tests/pad_wire_test.c src/pad_wire.c
	./pad_wire_test

.PHONY: clean host-test

clean:
	@rm -rf $(TARGET).skprx $(TARGET).velf $(TARGET).elf $(OBJS) $(DEPS) pad_wire_test

-include $(DEPS)
