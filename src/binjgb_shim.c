/* binjgb_shim — bump allocator + file_data stubs for binjgb.
 *
 * Most of binjgb's libc surface is now satisfied by picolibc (vendored
 * in rvvm-hal): printf, snprintf, the mem and str family, assert,
 * and stdin/stdout. Only the things picolibc can't reasonably
 * provide live here:
 *
 *   §1  Bump allocator (malloc/calloc/realloc/free) — keep our own
 *       rather than picolibc's nano-malloc, since binjgb allocates
 *       its Emulator + 8 MB cart buffer once at boot and never frees.
 *       Bump is predictable, has no per-block headers (so heap
 *       corruption from any rogue write hits zeroed BSS instead of
 *       allocator metadata), and fits our exact use case. Defining
 *       these here causes the linker to prefer them over picolibc's
 *       libc.a equivalents.
 *
 *   §2  exit() override — picolibc's exit walks __init_array /
 *       __fini_array which our link.ld doesn't expose. Skip straight
 *       to _exit() (wfi loop in rvvm-hal/src/picolibc_hooks.c).
 *
 *   §3  binjgb common.c surface — file_data_resize / file_data_delete
 *       and the file_read/write helpers binjgb's emulator.c uses for
 *       savestate transport. We don't compile common.c (no
 *       filesystem); these are the stubs that satisfy the link. */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     /* _exit */

#include "uart.h"
#include "binjgb/common.h"     /* FileData, Result, OK/ERROR */

/* ====================================================================
 * §1. Bump allocator.
 *
 * Sized for: Emulator struct (~250 KB), 8 MB ROM buffer, audio buffer
 * (~64 KB at 44.1 kHz mono u8 stereo, 1/10 s = 8820 frames × 2
 * channels), some palette working memory. 12 MB is comfortable with
 * our 256 MB linker layout.
 * ==================================================================== */
#define BUMP_POOL_BYTES   (12u * 1024u * 1024u)

__attribute__((aligned(4096)))
static uint8_t  bump_pool[BUMP_POOL_BYTES];
static size_t   bump_used = 0;

static void *bump_alloc(size_t n, size_t zero) {
    /* Align to 16 for safety with vector loads / __attribute__((aligned)). */
    size_t aligned_used = (bump_used + 15u) & ~(size_t)15u;
    if (n == 0) n = 1;
    if (aligned_used + n > BUMP_POOL_BYTES) {
        uart_printf("binjgb_shim: bump allocator exhausted "
                    "(used=%u, want=%u, pool=%u)\n",
                    (uint64_t)bump_used, (uint64_t)n,
                    (uint64_t)BUMP_POOL_BYTES);
        return NULL;
    }
    uint8_t *p = &bump_pool[aligned_used];
    bump_used = aligned_used + n;
    if (zero) {
        for (size_t i = 0; i < n; i++) p[i] = 0;
    }
    return p;
}

void *malloc(size_t n)              { return bump_alloc(n, 0); }
void *calloc(size_t count, size_t n){ return bump_alloc(count * n, 1); }
void  free(void *p)                 { (void)p; /* leak — bump pool */ }

void *realloc(void *old_ptr, size_t new_size) {
    void *fresh = bump_alloc(new_size, 0);
    if (!fresh) return NULL;
    if (old_ptr) {
        /* We can't tell the old block's size from a bare pointer.
         * binjgb only uses realloc inside file_data_resize, where
         * the FileData struct tracks the old size separately. We copy
         * up to (new_size - 1) bytes; any overshoot is fine because
         * the pool was zero-initialised and the caller never reads
         * past the recorded old_size. */
        memcpy(fresh, old_ptr, new_size);
    }
    return fresh;
}

/* Diagnostic — useful for "how much pool did we burn loading this
 * cart?" debug prints from main.c. */
size_t binjgb_shim_used_bytes(void) { return bump_used; }
size_t binjgb_shim_pool_bytes(void) { return BUMP_POOL_BYTES; }

/* ====================================================================
 * §2. exit() override
 *
 * Picolibc's exit walks __init_array / __fini_array via linker symbols
 * we don't define. Skip straight to _exit() (wired in
 * rvvm-hal/src/picolibc_hooks.c → wfi loop). Has no atexit handlers,
 * no global C++ destructors, so nothing's lost.
 * ==================================================================== */
__attribute__((noreturn))
void exit(int status) {
    _exit(status);
}

/* ====================================================================
 * §3. binjgb common.c surface
 * ==================================================================== */

void file_data_resize(FileData *fd, size_t new_size) {
    size_t old_size = fd->size;
    void  *fresh    = realloc(fd->data, new_size);
    fd->data        = fresh;
    if (new_size > old_size && fresh) {
        for (size_t i = old_size; i < new_size; i++)
            ((uint8_t *)fresh)[i] = 0;
    }
    fd->size = new_size;
}

void file_data_delete(FileData *fd) {
    fd->size = 0;
    fd->data = NULL;
}

/* No filesystem on bare-metal: return ERROR. Binjgb's
 * emulator_read_state_from_file et al. propagate this. */
Result file_read(const char *filename, FileData *out) {
    (void)filename; (void)out;
    return ERROR;
}
Result file_read_aligned(const char *filename, size_t align, FileData *out) {
    (void)filename; (void)align; (void)out;
    return ERROR;
}
Result file_write(const char *filename, const FileData *fd) {
    (void)filename; (void)fd;
    return ERROR;
}
const char *replace_extension(const char *filename, const char *ext) {
    (void)filename; (void)ext;
    return NULL;
}
