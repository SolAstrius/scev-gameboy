# scev-cores/game-boy

A **Game Boy / Game Boy Color** emulator running as bare-metal RISC-V
firmware under [RVVM](https://github.com/LekKit/RVVM). No SBI, no
kernel — the firmware boots straight into M-mode, brings up its own
device stack, loads a cart from NVMe, and runs binjgb's DMG+CGB core
against RVVM's PCI display, HID-over-I²C keyboard, and HDA audio.

`firmware.bin` is ~72 KB. About 600 lines of glue + freestanding shims
in `src/` plus binjgb's ~5,300-line `emulator.c` do the work.

## Layout

```
src/
  main.c            boot, cart load, frame loop, gfx blit, audio push
  binjgb_shim.c     freestanding libc replacement: bump allocator,
                    printf/fprintf → uart_printf, file-I/O stubs,
                    file_data_resize impl, str* helpers, assert handler
  stub-libc/        minimal headers: stdio.h, stdlib.h, string.h,
                    assert.h, inttypes.h — declarations binjgb needs
                    in its TUs without dragging musl in

vendor/rvvm-hal     git submodule → SolAstrius/rvvm-hal v0.6.1
vendor/binjgb       copied-in (not a submodule), see "vendoring" below
roms/               .gb / .gbc files — pass via `make run ROM=...`
flake.nix           zig + llvm-bintools + libasound for `make run`
```

## Vendored code

### `vendor/rvvm-hal/` — git submodule

Pinned to **v0.6.1** (LENGTH=256M linker layout — the Game Boy's 8 MB
ROM staging buffer + 12 MB shim allocator pool fit comfortably; chip-8
and zx-spectrum, both consumers at ~50 KB, are unaffected).

### `vendor/binjgb/` — copied in

[binji/binjgb](https://github.com/binji/binjgb) by Ben Smith,
MIT-licensed. We vendor the core only:

| File | Lines | Compiled? |
|---|---|---|
| `emulator.c`   | 5,279 | yes |
| `emulator.h`   | 251 | header |
| `common.h`     | 123 | header |
| `common.c`     | 82  | **NO** — replaced by `src/binjgb_shim.c` |
| `memory.h`     | 48  | header |
| `memory.c`     | 48  | **NO** — `xmalloc`/`xcalloc`/etc. expand to `malloc`/`calloc` (the default `TRACE_MEMORY=0` path), which `binjgb_shim.c` provides |

**Local modifications:** one. `vendor/binjgb/emulator.c:12` —
`#include <emscripten.h>` is wrapped in `#ifdef __EMSCRIPTEN__` so the
freestanding RISC-V build doesn't choke on a missing host header.
That's it; everything else is the freestanding shim layer doing its
job around an unmodified core. Tagged `/* SCEV PATCH */`.

The implicit "patch" is that we don't compile `common.c` or
`memory.c` — instead, `binjgb_shim.c` provides:

- `malloc`/`calloc`/`realloc`/`free` as a 12 MB static-pool bump
  allocator (no reclaim — binjgb allocates the Emulator + audio
  buffer once and never frees during run, so a bump strategy
  is sufficient)
- `printf`/`fprintf`/`snprintf` routed to `uart_printf`
- `exit`/`abort`/`__assert_fail` as panic + wfi loop
- `str{len,chr,rchr,cmp,ncmp}` and `memchr` (HAL only ships
  mem{cpy,set,move,cmp})
- `file_data_resize` and `file_data_delete` (used by the cart
  loader)
- `file_read`/`file_write`/`replace_extension` as no-op stubs
  returning `ERROR` — the savestate / ext-RAM-from-file paths in
  binjgb call them but we don't expose those in v1

## Build

Toolchain: `zig cc -target riscv64-freestanding-none` plus
`llvm-objcopy`. Both come from the Nix flake:

```sh
direnv allow         # or: nix develop
make                 # → firmware.bin (~72 KB code; ~21 MB BSS at runtime)
make run ROM=roms/foo.gb       # bochs display + HDA audio
make run-headless ROM=roms/foo.gb       # no GUI, UART only
```

## How it runs

`make run` launches RVVM with `-bochs_display -nonet -hda_test -nvme
$ROM`. The firmware:

1. Walks the FDT, brings up UART / PCI / I²C / HID / NVMe / display.
2. Brings up HDA + opens an audio_pcm channel at 48 kHz stereo.
3. Reads up to 8 MB from the NVMe disk into a static cart buffer.
4. Calls `emulator_new` with that buffer; binjgb auto-detects DMG vs
   CGB from the cart header byte at `0x143`.
5. Frame loop: poll HID → `emulator_set_joypad_buttons` →
   `emulator_run_until` (one PPU frame, 70,224 ticks) → blit 160×144
   → push APU samples → wfi-pace at 59.7 Hz.

## Joypad mapping

Standard SNES-on-keyboard layout (host HID → Game Boy):

| Host | GB |
|---|---|
| ↑ ↓ ← → | D-pad |
| Z | A |
| X | B |
| Enter | Start |
| Right Shift | Select |

## Memory footprint

Static reservations (BSS):

```
  12 MB   shim allocator pool (Emulator struct ~250 KB + audio + slack)
   8 MB   cart staging buffer  (MBC5 max)
  19 KB   PCM ring (4800 stereo frames @ 48 kHz)
   ~1 MB  rvvm-hal + binjgb code, palettes, strings, .data
   64 KB  stack
  -----
  ~21 MB total
```

RVVM's default `-mem 256M` covers it. If you point RVVM at a smaller
machine, lower `BUMP_POOL_BYTES` in `binjgb_shim.c` and the static
`cart_rom` size in `main.c` accordingly.

## v1 scope cuts

- **No savestates.** binjgb's in-memory savestate API
  (`emulator_read_state` / `emulator_write_state`) is wired up at the
  binjgb layer; the firmware just doesn't expose it. Adding a
  second NVMe disk for save/load is straightforward — same pattern as
  the Speccy's snapshot disk.
- **No ext-RAM persistence.** Cart ext-RAM lives in the Emulator
  struct's BSS for the duration of the run. Battery-backed saves
  (Pokémon, Zelda) work in-session but are lost on RVVM exit.
- **No SGB**. binjgb supports SGB enhancements; we ignore them and
  render the standard 160×144 frame.
- **No rewind.** binjgb's rewind buffer is an optional feature we
  don't enable.
- **Audio is 48 kHz** (RVVM HDA's preferred rate), not binjgb's
  native 44.1 kHz. binjgb's APU resamples internally based on
  `EmulatorInit.audio_frequency` (we set 44100 there to match its
  internal pipeline) and we copy u8→s16 on the way out.

## License

- `src/` — public-domain reference code (treat as such).
- `vendor/rvvm-hal/` — MIT-ish, see the submodule's README.
- `vendor/binjgb/` — MIT, see `vendor/binjgb/LICENSE`.

RVVM itself isn't redistributed here; install separately from
[LekKit/RVVM](https://github.com/LekKit/RVVM).
