/* scev-cores/game-boy — DMG + CGB on RVVM bare-metal.
 *
 * Boot:
 *   1. Standard rvvm-hal init: UART, FDT, PCI, I2C, time, gfx, HID.
 *   2. Audio: HDA + audio_pcm channel for the APU's stereo stream.
 *   3. NVMe: read the cart ROM (up to 8 MB) into a static buffer.
 *   4. binjgb: emulator_new(EmulatorInit{rom, audio frequency}).
 *   5. Frame loop: poll HID → emulator_run_until(next_vblank_ticks)
 *                  → blit framebuffer → push audio → wfi-pace.
 *
 * Audio is 44.1 kHz stereo, fed by the APU's resampler. Video is
 * 160×144 at native res, scaled ×4 to 640×576 for our Bochs surface.
 *
 * No-controller, no-savestate, no-ext-RAM-to-disk yet — all v1
 * scope-cuts. Plumbing for those is sketched in TODO comments. */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "pci.h"
#include "i2c.h"
#include "hid.h"
#include "nvme.h"
#include "gfx.h"
#include "audio.h"
#include "audio_pcm.h"
#include "rvvm.h"

#include "binjgb/common.h"
#include "binjgb/emulator.h"

#include <stddef.h>

extern char __bss_start[], __bss_end[];

void *memcpy(void *, const void *, unsigned long);

/* binjgb produces 160×144 frames; we scale ×4 → 640×576. Border-free —
 * Game Boy has no equivalent of the Speccy's overscan. */
#define GB_W           SCREEN_WIDTH
#define GB_H           SCREEN_HEIGHT
#define GB_SCALE       4
#define DISPLAY_W      (GB_W * GB_SCALE)
#define DISPLAY_H      (GB_H * GB_SCALE)

#define AUDIO_HZ       44100u
/* binjgb wants frames-per-emulator-call. One PPU frame = 70224 ticks =
 * 1/59.7 s. At 44.1 kHz, that's 738.6 audio frames. Round up so we
 * never under-fill: 768 = 2^8 * 3. Stereo, u8 samples → 1536 bytes. */
#define AUDIO_FRAMES   768u

/* On-host PCM ring fed from the APU's per-frame buffer. Match
 * binjgb's native sample rate (44100) so we don't pitch-shift +9 %
 * by replaying 44.1 kHz samples through a 48 kHz PCM channel. RVVM's
 * HDA accepts 44100 directly. 4410 frames = 100 ms ring at 44.1 kHz;
 * round to a multiple of 5 (the BDL entry count) → 4400. */
#define PCM_RING_FRAMES   4400u
#define PCM_BDL_ENTRIES   5u
#define PCM_SAMPLE_RATE   44100u

__attribute__((aligned(128)))
static int16_t pcm_ring[PCM_RING_FRAMES * 2];   /* stereo */

/* Static cart ROM staging buffer. 8 MB max (MBC5 limit). NVMe DMA
 * needs page alignment. */
__attribute__((aligned(NVME_PAGE_SIZE)))
static uint8_t  cart_rom[MAXIMUM_ROM_SIZE];

/* Save (ext-RAM) staging buffer. 128 KB covers MBC5 max. Page-aligned
 * for NVMe DMA. The same buffer is used for both the boot-time read
 * and periodic write-back. */
#define SAVE_BUF_BYTES   (128u * 1024u)
#define SAVE_PERIOD_PPU  300u    /* ~5 s at 59.7 Hz */

__attribute__((aligned(NVME_PAGE_SIZE)))
static uint8_t  save_buf[SAVE_BUF_BYTES];

static nvme_t   save_disk;
static bool     save_attached    = false;
static uint32_t save_size_bytes  = 0;
static uint32_t save_size_lbas   = 0;
static uint32_t last_save_ppu_frame = 0;

static gfx_t          g;
static hid_keyboard_t kb;
static audio_pcm_t    pcm;
static Emulator      *emu = NULL;
static JoypadButtons  joyp;

extern size_t binjgb_shim_used_bytes(void);
extern size_t binjgb_shim_pool_bytes(void);

/* HID usage → JoypadButtons mapping. Standard SNES-controller-on-
 * keyboard layout: arrows for D-pad, Z=A, X=B, Enter=Start, Right
 * Shift=Select. */
static void on_key(uint8_t usage, bool pressed, void *ctx) {
    (void)ctx;
    switch (usage) {
    case 0x52: joyp.up     = pressed; break;   /* Up arrow */
    case 0x51: joyp.down   = pressed; break;   /* Down */
    case 0x50: joyp.left   = pressed; break;   /* Left */
    case 0x4F: joyp.right  = pressed; break;   /* Right */
    case 0x1D: joyp.A      = pressed; break;   /* Z */
    case 0x1B: joyp.B      = pressed; break;   /* X */
    case 0x28: joyp.start  = pressed; break;   /* Enter */
    case 0xE5: joyp.select = pressed; break;   /* RShift */
    default:                                     return;
    }
}

static uintptr_t fdt_addr_of(const fdt_t *fdt, const char *compat,
                             uintptr_t fallback) {
    uint32_t off = fdt_find_compatible(fdt, compat);
    if (off == UINT32_MAX) return fallback;
    uint64_t addr = 0;
    if (!fdt_node_reg64(fdt, off, 0, &addr, NULL)) return fallback;
    return (uintptr_t)addr;
}

/* Cart-header ext-RAM size — byte $149. binjgb has its own internal
 * decode but doesn't expose it cleanly; this is small enough to
 * mirror. Returns 0 if cart has no battery-backed RAM. */
static uint32_t cart_ext_ram_size(const uint8_t *rom) {
    switch (rom[0x149]) {
        case 0x00: return 0;
        case 0x01: return  2u * 1024u;
        case 0x02: return  8u * 1024u;
        case 0x03: return 32u * 1024u;
        case 0x04: return 128u * 1024u;
        case 0x05: return 64u * 1024u;
        default:   return 0;
    }
}

/* Try to attach NVMe controller 1 as the save disk, populate ext-RAM
 * from its contents, and arm periodic write-back. Silent no-op if no
 * disk 1 is attached or the cart has no ext-RAM. The save file should
 * be at least the cart's ext-RAM size — Makefile pre-creates a 128 KB
 * zero-filled file when SAVE= is set, which covers any MBC. */
static void init_save(Emulator *e, const uint8_t *rom) {
    save_size_bytes = cart_ext_ram_size(rom);
    if (save_size_bytes == 0) {
        uart_puts("save: cart has no ext-RAM (no battery)\n");
        return;
    }
    if (save_size_bytes > SAVE_BUF_BYTES) {
        uart_printf("save: cart wants %u bytes ext-RAM, buf is %u; skipping\n",
                    (uint64_t)save_size_bytes, (uint64_t)SAVE_BUF_BYTES);
        return;
    }
    if (!nvme_init_nth(&save_disk, 1)) {
        uart_puts("save: no NVMe disk 1 attached "
                  "(saves won't persist; pass SAVE=… to make run)\n");
        return;
    }

    save_size_lbas = (save_size_bytes + NVME_LBA_SIZE - 1) / NVME_LBA_SIZE;
    if (save_size_lbas > save_disk.num_lbas) {
        uart_printf("save: disk too small (have %u LBAs, need %u); skipping\n",
                    (uint64_t)save_disk.num_lbas, (uint64_t)save_size_lbas);
        return;
    }

    /* Read existing save bytes. NVMe rounds up to LBA granularity;
     * the trailing slack inside the last sector is harmless because
     * binjgb only consumes save_size_bytes. */
    uint32_t got = nvme_read(&save_disk, 0, save_buf, save_size_lbas);
    if (got != save_size_lbas) {
        uart_printf("save: read short (%u/%u LBAs); starting fresh\n",
                    (uint64_t)got, (uint64_t)save_size_lbas);
        for (uint32_t i = 0; i < save_size_bytes; i++) save_buf[i] = 0;
    }

    FileData fd = { .data = save_buf, .size = save_size_bytes };
    if (emulator_read_ext_ram(e, &fd) != OK) {
        uart_puts("save: emulator_read_ext_ram failed; skipping\n");
        return;
    }

    save_attached = true;
    uart_printf("save: %u bytes loaded from NVMe disk 1 "
                "(autosave every %u s)\n",
                (uint64_t)save_size_bytes,
                (uint64_t)(SAVE_PERIOD_PPU / 60));
}

/* Periodic write-back. Called every emulator frame; only does work
 * once per SAVE_PERIOD_PPU PPU frames. Cheap when it doesn't fire,
 * and the actual write is one ~17 KB NVMe op for an 8 KB cart, ~256
 * KB for a 128 KB cart — both well under one frame's slack. */
static void tick_save(Emulator *e, uint32_t ppu_frame_count) {
    if (!save_attached) return;
    if (ppu_frame_count - last_save_ppu_frame < SAVE_PERIOD_PPU) return;

    FileData fd = { .data = save_buf, .size = save_size_bytes };
    if (emulator_write_ext_ram(e, &fd) != OK) return;
    nvme_write(&save_disk, 0, save_buf, save_size_lbas);
    last_save_ppu_frame = ppu_frame_count;
}

/* Read up to MAXIMUM_ROM_SIZE bytes from NVMe controller 0 into
 * cart_rom. Returns the number of bytes read, or 0 on failure. */
static uint32_t load_cart_from_nvme(void) {
    static nvme_t cart_disk;
    if (!nvme_init_nth(&cart_disk, 0)) {
        uart_puts("FATAL: no NVMe disk attached. "
                  "Start RVVM with -nvme path/to/cart.gb\n");
        return 0;
    }

    /* Cart size = min(disk size, MAXIMUM_ROM_SIZE). NVMe IDENTIFY
     * gave us num_lbas; multiply by 512 B per LBA. */
    uint64_t disk_bytes = (uint64_t)cart_disk.num_lbas * NVME_LBA_SIZE;
    if (disk_bytes > MAXIMUM_ROM_SIZE) disk_bytes = MAXIMUM_ROM_SIZE;
    if (disk_bytes < MINIMUM_ROM_SIZE) {
        uart_printf("FATAL: cart too small (%u bytes, need >= %u)\n",
                    disk_bytes, (uint64_t)MINIMUM_ROM_SIZE);
        return 0;
    }
    uint32_t lbas = (uint32_t)(disk_bytes / NVME_LBA_SIZE);

    /* binjgb's MBC code expects the cart at offset 0 of the buffer. */
    uint32_t got = nvme_read(&cart_disk, 0, cart_rom, lbas);
    if (got != lbas) {
        uart_printf("FATAL: NVMe read short (%u/%u LBAs)\n",
                    (uint64_t)got, (uint64_t)lbas);
        return 0;
    }
    uart_printf("cart: loaded %u bytes from NVMe (%u LBAs)\n",
                disk_bytes, (uint64_t)lbas);
    return (uint32_t)disk_bytes;
}

/* Direct-to-vram blit, ×4 nearest-neighbour. Bypasses gfx_pixel's
 * per-call format check by hoisting it once at the top, hoists the
 * 4 destination row pointers once per source scanline, and unrolls
 * the 4×4 source-pixel block into 16 explicit stores.
 *
 * Pixel format note: binjgb's RGBA packs as 0xAABBGGRR — R in the
 * low byte. Bochs Display's XRGB8888 wants R in the HIGH byte
 * (0x00RRGGBB). So we must swap R↔B when the surface is XRGB; when
 * the surface is XBGR (simplefb sometimes), binjgb's bytes already
 * line up and no swap is needed. (My previous version had this
 * conditional inverted, producing the famous "blue Zelda" bug.) */
static void blit_frame(const RGBA *fb, uint32_t x_off, uint32_t y_off) {
    uint32_t *vram   = g.vram;
    uint32_t  stride = g.stride_px;
    bool      need_swap = (g.format == GFX_FMT_XRGB8888);

    for (uint32_t y = 0; y < GB_H; y++) {
        uint32_t  base = (y_off + y * GB_SCALE) * stride + x_off;
        uint32_t *r0   = &vram[base];
        uint32_t *r1   = &vram[base + stride];
        uint32_t *r2   = &vram[base + 2 * stride];
        uint32_t *r3   = &vram[base + 3 * stride];
        const RGBA *src = &fb[y * GB_W];

        for (uint32_t x = 0; x < GB_W; x++) {
            uint32_t c = (uint32_t)src[x] & 0x00FFFFFFu;
            if (need_swap) {
                uint32_t r = c        & 0xFF;
                uint32_t b = (c >> 16) & 0xFF;
                c = (c & 0x0000FF00U) | (r << 16) | b;
            }
            uint32_t dx = x * GB_SCALE;
            r0[dx] = r0[dx+1] = r0[dx+2] = r0[dx+3] = c;
            r1[dx] = r1[dx+1] = r1[dx+2] = r1[dx+3] = c;
            r2[dx] = r2[dx+1] = r2[dx+2] = r2[dx+3] = c;
            r3[dx] = r3[dx+1] = r3[dx+2] = r3[dx+3] = c;
        }
    }
}

/* Convert binjgb's u8 stereo audio to s16 stereo and push into the
 * PCM ring. binjgb's AudioBuffer.data is u8 0..255 with 128 = silence;
 * shift to s16 by ((sample - 128) << 8). */
static void push_audio(AudioBuffer *ab) {
    uint32_t frames = (uint32_t)(ab->position - ab->data) / 2;
    /* Reset the position cursor BEFORE early-out paths — binjgb's
     * audio_buffer_full event fires when position reaches end, and
     * if we leave position at end the PPU stalls indefinitely
     * waiting for the consumer (us). Always claim we drained, even
     * if the host PCM ring couldn't actually fit it. */
    ab->position = ab->data;
    if (frames == 0) return;

    uint32_t writable = audio_pcm_writable(&pcm);
    if (writable < frames) {
        /* Host hasn't drained fast enough — drop this batch. Audio
         * glitches but emulation stays paced. Common when the GB
         * frame rate (59.7 Hz) is faster than wall-clock pacing
         * during JIT warmup. */
        return;
    }

    uint32_t wp = pcm.wp_frames;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = ((wp + i) % PCM_RING_FRAMES) * 2;
        int16_t l = (int16_t)(((int32_t)ab->data[i * 2 + 0] - 128) << 8);
        int16_t r = (int16_t)(((int32_t)ab->data[i * 2 + 1] - 128) << 8);
        pcm_ring[idx + 0] = l;
        pcm_ring[idx + 1] = r;
    }
    audio_pcm_advance(&pcm, frames);
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\nscev-cores/game-boy — DMG/CGB on RVVM\n");
    uart_printf("hartid=%u  fdt=%p  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    /* FDT discovery + driver re-init with discovered addresses. */
    fdt_t fdt;
    bool fdt_ok = fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr);
    if (fdt_ok) {
        uart_init(fdt_addr_of(&fdt, "ns16550a",              RVVM_UART_BASE));
        pci_init(fdt_addr_of(&fdt, "pci-host-ecam-generic",  RVVM_PCI_ECAM_BASE));
        i2c_init(fdt_addr_of(&fdt, "opencores,i2c-ocores",   RVVM_I2C_OC_BASE));
        uint32_t cpus = fdt_find_node_named(&fdt, "cpus");
        uint32_t hz = 0;
        if (cpus != UINT32_MAX) fdt_node_prop_u32(&fdt, cpus, "timebase-frequency", &hz);
        time_init(fdt_addr_of(&fdt, "sifive,clint0", RVVM_CLINT_BASE), hz);
    } else {
        pci_init(0);
        i2c_init(RVVM_I2C_OC_BASE);
        time_init(RVVM_CLINT_BASE, 0);
    }
    hid_kb_init(&kb, RVVM_I2C_HID_KEYBOARD);

    /* Graphics. 160×144 ×4 → 640×576. */
    bool have_gfx = gfx_init_fdt(&g, &fdt, DISPLAY_W, DISPLAY_H);
    if (have_gfx) {
        gfx_fill(&g, 0x00000000);
    } else {
        uart_puts("gfx: no display backend; running blind\n");
    }

    /* Audio. */
    bool have_audio = audio_init();
    if (have_audio) {
        for (uint32_t i = 0; i < PCM_RING_FRAMES * 2; i++) pcm_ring[i] = 0;
        if (!audio_pcm_open(&pcm, pcm_ring, PCM_RING_FRAMES,
                            PCM_BDL_ENTRIES, PCM_SAMPLE_RATE)) {
            uart_puts("audio_pcm_open failed; continuing silently\n");
            have_audio = false;
        }
        /* Pre-peg ring so writable() returns 0 until LPIB advances. */
        if (have_audio) audio_pcm_advance(&pcm, PCM_RING_FRAMES - 1);
    } else {
        uart_puts("audio: backend unavailable, running silently\n");
    }

    /* Cart load. */
    uint32_t cart_size = load_cart_from_nvme();
    if (cart_size == 0) for (;;) __asm__ volatile ("wfi");

    /* binjgb instantiation. EmulatorInit takes a FileData — point it
     * at the static cart_rom buffer. binjgb may call file_data_resize
     * inside emulator_new to pad up to declared cart size; our
     * realloc shim copies into a fresh bump-pool block, so the
     * original cart_rom static array becomes unused after that. */
    EmulatorInit init;
    for (uint32_t i = 0; i < sizeof(init); i++) ((uint8_t *)&init)[i] = 0;
    init.rom.data        = cart_rom;
    init.rom.size        = cart_size;
    init.audio_frequency = AUDIO_HZ;
    init.audio_frames    = AUDIO_FRAMES;
    init.random_seed     = (uint32_t)time_now();
    init.builtin_palette = 0;             /* default greys for DMG */
    init.force_dmg       = 0;
    init.cgb_color_curve = CGB_COLOR_CURVE_SAMEBOY_EMULATE_HARDWARE;
    /* SameBoy's curve models the actual GBC LCD's response: warmer
     * mids, slightly desaturated reds. Cleaner than NONE (raw 5-bit
     * cart RGB) and matches what most modern GBC emulators ship. */

    emu = emulator_new(&init);
    if (!emu) {
        uart_puts("FATAL: emulator_new failed\n");
        for (;;) __asm__ volatile ("wfi");
    }
    uart_printf("binjgb: emulator up (shim pool %u/%u KB used)\n",
                (uint64_t)(binjgb_shim_used_bytes() >> 10),
                (uint64_t)(binjgb_shim_pool_bytes() >> 10));

    /* Save layer — load any existing ext-RAM contents from NVMe
     * disk 1; arm periodic write-back if attached. */
    init_save(emu, cart_rom);

    AudioBuffer *ab = emulator_get_audio_buffer(emu);
    uart_printf("audio buffer: %u Hz, %u frames/run\n",
                (uint64_t)ab->frequency, (uint64_t)ab->frames);

    /* Centre on the surface — bochs mode-sets exactly to DISPLAY_W×H
     * but simplefb may be larger. */
    uint32_t x_off = (have_gfx && g.width  > DISPLAY_W) ? (g.width  - DISPLAY_W) / 2 : 0;
    uint32_t y_off = (have_gfx && g.height > DISPLAY_H) ? (g.height - DISPLAY_H) / 2 : 0;

    uart_puts("Running.\n\n");

    /* Frame loop. Pace at the GB's 59.7 Hz natively. RVVM_TIME_HZ /
     * 59.7 ≈ 167504 ticks/frame. */
    const uint64_t ticks_per_frame = RVVM_TIME_HZ * 10 / 597;
    uint64_t deadline = time_now() + ticks_per_frame;

    /* Profiling. Accumulate per-phase ticks (RVVM_TIME_HZ = 10 MHz,
     * so 1 tick = 100 ns) and call counts; dump every 60 frames =
     * once per second of wall-clock at the GB's 59.7 Hz target.
     *
     * The dump tells us where the budget goes. If `wfi_pace` is
     * close to ticks_per_frame we have headroom; if it's near zero
     * we're at the limit and overshooting. If `run` dominates,
     * binjgb's interpreter is the bottleneck (RVVM JIT warmup /
     * DGB CPU complexity); if `blit` dominates, the framebuffer
     * path needs work. */
    uint64_t prof_run = 0, prof_blit = 0, prof_audio = 0,
             prof_hid = 0, prof_pace = 0;
    uint32_t prof_iters = 0, prof_run_calls = 0,
             prof_audio_drops = 0;
    /* binjgb's authoritative "PPU frames produced" counter — the
     * one that tells us emulator-speed-vs-wall-clock honestly,
     * regardless of whether our loop ever called blit. */
    uint32_t prof_ppu_frame_base = emulator_get_ppu_frame(emu);
    uint64_t prof_window_start   = time_now();

    for (;;) {
        uint64_t t0 = time_now();
        hid_kb_poll(&kb, on_key, NULL);
        emulator_set_joypad_buttons(emu, &joyp);
        uint64_t t1 = time_now();
        prof_hid += t1 - t0;

        /* Drive the emulator forward by exactly one PPU frame's
         * worth of ticks. Each call to run_until may return early on
         * AUDIO_BUFFER_FULL (drain) or UNTIL_TICKS (deadline reached);
         * we re-target each call from the CURRENT tick counter so
         * we always make forward progress.
         *
         * IMPORTANT: events must accumulate across calls. NEW_FRAME
         * typically fires in the call that hits PPU vblank (early
         * in the iteration); the FINAL call usually returns just
         * UNTIL_TICKS. If we only kept the last `ev`, we'd miss
         * NEW_FRAME and never blit. */
        Ticks initial_ticks = emulator_get_ticks(emu);
        Ticks target        = initial_ticks + PPU_FRAME_TICKS;
        EmulatorEvent ev    = 0;
        while (emulator_get_ticks(emu) < target) {
            EmulatorEvent step_ev = emulator_run_until(emu, target);
            ev |= step_ev;
            prof_run_calls++;
            if (have_audio && (step_ev & EMULATOR_EVENT_AUDIO_BUFFER_FULL)) {
                uint64_t a0 = time_now();
                push_audio(emulator_get_audio_buffer(emu));
                prof_audio += time_now() - a0;
            }
        }
        uint64_t t2 = time_now();
        prof_run += t2 - t1;

        if (have_gfx && (ev & EMULATOR_EVENT_NEW_FRAME)) {
            FrameBuffer *fb = emulator_get_frame_buffer(emu);
            blit_frame((const RGBA *)*fb, x_off, y_off);
        }

        /* Periodic ext-RAM write-back to NVMe disk 1. Cheap when not
         * firing, ~256 KB write at most when it does. */
        tick_save(emu, emulator_get_ppu_frame(emu));
        uint64_t t3 = time_now();
        prof_blit += t3 - t2;

        if (have_audio) {
            AudioBuffer *ab = emulator_get_audio_buffer(emu);
            uint32_t buffered = (uint32_t)(ab->position - ab->data) / 2;
            if (buffered && audio_pcm_writable(&pcm) < buffered)
                prof_audio_drops++;
            push_audio(ab);
        }
        uint64_t t4 = time_now();
        prof_audio += t4 - t3;

        time_busy_until(deadline);
        uint64_t t5 = time_now();
        prof_pace += t5 - t4;
        deadline += ticks_per_frame;

        if (++prof_iters >= 60) {
            uint64_t window  = t5 - prof_window_start;
            uint32_t ppu_now = emulator_get_ppu_frame(emu);
            uint32_t ppu_dt  = ppu_now - prof_ppu_frame_base;
            /* Effective emulator speed: PPU frames produced × 1000 /
             * window_ms = real Hz the GB hardware is running at.
             * 59.7 = native; <30 = visibly slow. */
            uint64_t eff_hz_x10 = (uint64_t)ppu_dt * 10000ULL
                                / ((uint64_t)window / 10000ULL);
            #define US(t) ((uint64_t)((t) / 10))
            uart_printf("[prof] iters=%u ppu_frames=%u "
                        "wall=%ums eff=%u.%uHz | "
                        "run=%uus blit=%uus audio=%uus hid=%uus pace=%uus "
                        "(run-calls=%u, drops=%u)\n",
                        (uint64_t)prof_iters,
                        (uint64_t)ppu_dt,
                        US(window) / 1000,
                        eff_hz_x10 / 10, eff_hz_x10 % 10,
                        US(prof_run)   / prof_iters,
                        US(prof_blit)  / prof_iters,
                        US(prof_audio) / prof_iters,
                        US(prof_hid)   / prof_iters,
                        US(prof_pace)  / prof_iters,
                        (uint64_t)prof_run_calls,
                        (uint64_t)prof_audio_drops);
            #undef US
            prof_run = prof_blit = prof_audio = prof_hid = prof_pace = 0;
            prof_iters = prof_run_calls = prof_audio_drops = 0;
            prof_ppu_frame_base = ppu_now;
            prof_window_start   = t5;
        }
    }
}
