# scev-cores/game-boy — DMG + CGB on RVVM bare-metal.
#
# Consumes rvvm-hal (vendor/rvvm-hal as a git submodule) for device
# drivers; vendors binjgb (Ben Smith, MIT) under vendor/binjgb/ for the
# Game Boy core. ~5300 lines of binjgb's emulator.c, plus ~600 lines of
# scev-specific glue + freestanding shims under src/.
#
# Build: `make`        produces firmware.bin (~1 MB; 8 MB cart staging
#                      buffer + 12 MB binjgb shim pool dominate)
# Run:   `make run ROM=roms/foo.gb`
#                      boots under RVVM with -bochs_display -hda_test
# Clean: `make clean`

HAL      := vendor/rvvm-hal
BINJGB   := vendor/binjgb
PICOLIBC := $(HAL)/vendor/picolibc-build/min/install
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

RVVM     ?= $(shell command -v rvvm 2>/dev/null || \
                    echo /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64)

# Include order: picolibc first (so <stdio.h>, <string.h>, <assert.h>
# resolve to the vendored libc; HAL_PICOLIBC=min is the slim variant —
# integer printf, no float, no posix-io). Then src/, HAL, vendor/.
# binjgb_shim.c keeps its bump allocator, overriding picolibc's
# malloc/free at link time.
CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable \
            -Wno-unused-function -Wno-unused-variable \
            -DHAL_PICOLIBC \
            -isystem $(PICOLIBC)/include \
            -Isrc -I$(HAL)/include -Ivendor

# --gc-sections trims unreferenced picolibc objects per-firmware so
# we only pay for symbols binjgb actually calls (printf + a handful
# of mem*/str* + assert).
LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld -Wl,--gc-sections

# Speccy-style: our glue + shim + binjgb's emulator.c.
# We deliberately do NOT compile vendor/binjgb/common.c or memory.c —
# binjgb_shim.c provides the surface those would have linked.
SCEV_OBJS  := build/main.o build/binjgb_shim.o
BINJGB_OBJS := build/emulator.o
OBJS := $(SCEV_OBJS) $(BINJGB_OBJS)

all: firmware.bin

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# binjgb's emulator.c uses #include "common.h" without namespacing —
# add -I$(BINJGB) for that one TU only so its sibling headers resolve.
build/emulator.o: $(BINJGB)/emulator.c
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(BINJGB) -c -o $@ $<

$(PICOLIBC)/lib/libc.a:
	$(MAKE) -C $(HAL) picolibc-min

$(HAL)/libhal.a: $(PICOLIBC)/lib/libc.a
	$(MAKE) -C $(HAL) HAL_PICOLIBC=min

firmware.elf: $(OBJS) $(HAL)/libhal.a $(PICOLIBC)/lib/libc.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a $(PICOLIBC)/lib/libc.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

ROM ?= roms/test.gb

# Convenience: if SAVE=… is set and the file is missing, pre-create
# a 128 KB zero-filled file (covers any MBC's ext-RAM size). Then
# attach it as NVMe disk 1; the firmware reads it at boot, calls
# emulator_read_ext_ram, and writes back every 5 seconds.
#
# Without SAVE=, no -nvme disk 1 is attached and ext-RAM lives only
# in the Emulator struct's BSS — saves work in-session but vanish
# on RVVM exit.
define ENSURE_SAVE
	@if [ -n "$(SAVE)" ] && [ ! -f "$(SAVE)" ]; then \
	    mkdir -p $$(dirname "$(SAVE)"); \
	    dd if=/dev/zero of="$(SAVE)" bs=1024 count=128 status=none; \
	    echo "save: created empty $(SAVE) (128 KB)"; \
	fi
endef

run: firmware.bin
	@test -f "$(ROM)" || { echo "missing $(ROM); set ROM=path/to/cart.gb"; exit 1; }
	$(ENSURE_SAVE)
	$(RVVM) firmware.bin -bochs_display -nonet -hda_test -nvme $(ROM) $(if $(SAVE),-nvme $(SAVE))

run-headless: firmware.bin
	@test -f "$(ROM)" || { echo "missing $(ROM); set ROM=path/to/cart.gb"; exit 1; }
	$(ENSURE_SAVE)
	$(RVVM) firmware.bin -nogui -nonet -hda_test -nvme $(ROM) $(if $(SAVE),-nvme $(SAVE))

clean:
	rm -rf build firmware.elf firmware.bin
	$(MAKE) -C $(HAL) clean

.PHONY: all run run-headless clean
