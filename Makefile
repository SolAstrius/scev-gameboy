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
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

RVVM     ?= $(shell command -v rvvm 2>/dev/null || \
                    echo /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64)

# Include order: src/ first (for our shims overriding nothing yet),
# then HAL headers, then binjgb headers (binjgb does `#include
# "common.h"` so we need the binjgb dir on the path; we put it under
# `binjgb/` in the include path so the user code says
# `#include "binjgb/emulator.h"` — keeps namespacing tidy and avoids
# clashes with HAL's `common.h`-shaped names).
CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable \
            -Wno-unused-function -Wno-unused-variable \
            -Isrc/stub-libc -Isrc -I$(HAL)/include -Ivendor

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld

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

$(HAL)/libhal.a:
	$(MAKE) -C $(HAL)

firmware.elf: $(OBJS) $(HAL)/libhal.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

ROM ?= roms/test.gb

run: firmware.bin
	@test -f "$(ROM)" || { echo "missing $(ROM); set ROM=path/to/cart.gb"; exit 1; }
	$(RVVM) firmware.bin -bochs_display -nonet -hda_test -nvme $(ROM)

run-headless: firmware.bin
	@test -f "$(ROM)" || { echo "missing $(ROM); set ROM=path/to/cart.gb"; exit 1; }
	$(RVVM) firmware.bin -nogui -nonet -hda_test -nvme $(ROM)

clean:
	rm -rf build firmware.elf firmware.bin
	$(MAKE) -C $(HAL) clean

.PHONY: all run run-headless clean
