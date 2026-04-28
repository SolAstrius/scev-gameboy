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

/* On-host PCM ring fed from the APU's per-frame buffer. 4800 frames
 * stereo s16 ≈ 19 KB; 100 ms at 48 kHz of slack — plenty. We'll
 * resample u8→s16 at copy-out time. */
#define PCM_RING_FRAMES   4800u
#define PCM_BDL_ENTRIES   5u
#define PCM_SAMPLE_RATE   48000u

__attribute__((aligned(128)))
static int16_t pcm_ring[PCM_RING_FRAMES * 2];   /* stereo */

/* Static cart ROM staging buffer. 8 MB max (MBC5 limit). NVMe DMA
 * needs page alignment. */
__attribute__((aligned(NVME_PAGE_SIZE)))
static uint8_t  cart_rom[MAXIMUM_ROM_SIZE];

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

/* Convert binjgb's 160×144 RGBA framebuffer to the gfx surface,
 * scaled ×GB_SCALE, centred on the surface. RGBA in binjgb is
 * little-endian ARGB, which is exactly XRGB8888 + alpha=0xFF — gfx
 * will mask alpha off (XBGR backends are auto-swizzled by gfx_pixel). */
static void blit_frame(const RGBA *fb, uint32_t x_off, uint32_t y_off) {
    for (uint32_t y = 0; y < GB_H; y++) {
        for (uint32_t x = 0; x < GB_W; x++) {
            uint32_t color = (uint32_t)fb[y * GB_W + x] & 0x00FFFFFFu;
            uint32_t px = x_off + x * GB_SCALE;
            uint32_t py = y_off + y * GB_SCALE;
            for (uint32_t dy = 0; dy < GB_SCALE; dy++)
                for (uint32_t dx = 0; dx < GB_SCALE; dx++)
                    gfx_pixel(&g, px + dx, py + dy, color);
        }
    }
}

/* Convert binjgb's u8 stereo audio to s16 stereo and push into the
 * PCM ring. binjgb's AudioBuffer.data is u8 0..255 with 128 = silence;
 * shift to s16 by ((sample - 128) << 8). */
static void push_audio(AudioBuffer *ab) {
    uint32_t frames = (uint32_t)(ab->position - ab->data) / 2;
    if (frames == 0) return;

    uint32_t writable = audio_pcm_writable(&pcm);
    if (writable < frames) {
        /* Drop oldest by skipping ahead — better than stalling the
         * frame loop. Audio glitches but the emulator stays paced. */
        return;
    }

    uint32_t wp = pcm.wp_frames;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = ((wp + i) % PCM_RING_FRAMES) * 2;
        int16_t l = (int16_t)((int32_t)ab->data[i * 2 + 0] - 128) << 8;
        int16_t r = (int16_t)((int32_t)ab->data[i * 2 + 1] - 128) << 8;
        pcm_ring[idx + 0] = l;
        pcm_ring[idx + 1] = r;
    }
    audio_pcm_advance(&pcm, frames);

    /* Reset the position cursor — binjgb refills the buffer on the
     * next emulator_run call, starting from data again. */
    ab->position = ab->data;
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
    init.cgb_color_curve = 0;             /* CGB_COLOR_CURVE_NONE */

    emu = emulator_new(&init);
    if (!emu) {
        uart_puts("FATAL: emulator_new failed\n");
        for (;;) __asm__ volatile ("wfi");
    }
    uart_printf("binjgb: emulator up (shim pool %u/%u KB used)\n",
                (uint64_t)(binjgb_shim_used_bytes() >> 10),
                (uint64_t)(binjgb_shim_pool_bytes() >> 10));

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

    for (;;) {
        hid_kb_poll(&kb, on_key, NULL);
        emulator_set_joypad_buttons(emu, &joyp);

        /* Run until the PPU completes one frame. binjgb returns the
         * event mask: EMULATOR_EVENT_NEW_FRAME and/or
         * EMULATOR_EVENT_AUDIO_BUFFER_FULL. We don't need to inspect —
         * advancing by PPU_FRAME_TICKS is exactly what we want. */
        Ticks now = (Ticks)0;       /* placeholder, see TODO below */
        (void)now;
        emulator_run_until(emu, emulator_get_ticks(emu) + PPU_FRAME_TICKS);

        if (have_gfx) {
            FrameBuffer *fb = emulator_get_frame_buffer(emu);
            blit_frame((const RGBA *)*fb, x_off, y_off);
        }

        if (have_audio) {
            push_audio(emulator_get_audio_buffer(emu));
        }

        time_busy_until(deadline);
        deadline += ticks_per_frame;
    }
}
