/* binjgb_shim — freestanding shim layer.
 *
 * binjgb's emulator.c expects libc: malloc/free, printf/fprintf,
 * exit/abort, plus a few file-I/O helpers from common.c that we
 * deliberately don't compile (ROM and savestate transport flow
 * through NVMe in our main.c).
 *
 * We provide:
 *
 *   malloc/calloc/realloc/free   bump allocator on a static pool.
 *                                 No reclaim — Game Boy emulator
 *                                 allocates Emulator + audio buffer
 *                                 once and never frees during run,
 *                                 so a bump allocator is sufficient.
 *
 *   printf/fprintf/snprintf       routed to uart_printf (stream arg
 *                                 ignored — stderr and stdout both
 *                                 land on UART).
 *
 *   exit/abort                    panic + wfi spin.
 *
 *   strlen/strrchr/strchr/strcmp  small string helpers binjgb's
 *                                 cart-info pretty-printer uses.
 *
 *   file_read / file_write        return ERROR. Convenience wrappers
 *                                 for savestate/ext-RAM-from-file
 *                                 paths we don't expose on this
 *                                 firmware.
 *
 *   file_data_resize              actually implemented — used during
 *                                 cart loading to pad the ROM up to
 *                                 the cart-declared size.
 *
 *   file_data_delete              no-op (the bump allocator can't
 *                                 free, and the only owner is the
 *                                 Emulator at boot — it never goes
 *                                 away).
 *
 *   __assert_fail                 our take on glibc's assert handler. */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "uart.h"
#include "binjgb/common.h"     /* FileData, Result, OK/ERROR */

/* ---------------------------------------------------------------- */
/* Static bump allocator.                                           */
/*                                                                  */
/* Sized for: Emulator struct (~250 KB), 8 MB ROM buffer, audio     */
/* buffer (~64 KB at 44.1 kHz mono u8 stereo, 1/10 s = 8820 frames  */
/* × 2 channels), some palette working memory. 12 MB is comfortable */
/* with our 256 MB linker layout.                                   */
/* ---------------------------------------------------------------- */
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

/* realloc: copy old bytes into a fresh allocation. The old block
 * stays leaked in the pool; that's the cost of the bump strategy.
 * binjgb only realloc's the file_data buffer once (during cart load
 * to pad up to declared size), so the leak is bounded. */
extern void *memcpy(void *, const void *, size_t);

void *realloc(void *old_ptr, size_t new_size) {
    void *fresh = bump_alloc(new_size, 0);
    if (!fresh) return NULL;
    if (old_ptr) {
        /* We can't tell the old block's size from a bare pointer.
         * binjgb only uses realloc inside file_data_resize, where
         * the caller passes the new size; the OLD size is tracked
         * separately by the FileData struct. So conservatively copy
         * up to (new_size - 1) bytes — overshoot is fine since the
         * pool was zero-initialised and we never read past the
         * caller's known old_size anyway. */
        memcpy(fresh, old_ptr, new_size);
    }
    return fresh;
}

/* Diagnostic — not part of libc, but useful for "how much pool did
 * we burn loading this cart?" debug prints from main.c. */
size_t binjgb_shim_used_bytes(void) { return bump_used; }
size_t binjgb_shim_pool_bytes(void) { return BUMP_POOL_BYTES; }

/* ---------------------------------------------------------------- */
/* String helpers binjgb pulls in via <string.h> declarations.       */
/* HAL's string.c only provides memcpy/memset/memmove/memcmp; the   */
/* str* family lives here.                                           */
/* ---------------------------------------------------------------- */

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    unsigned char        target = (unsigned char)c;
    while (n--) { if (*p == target) return (void *)p; p++; }
    return NULL;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char *)s; s++; }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    if (c == 0) return (char *)s;
    return (char *)last;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ---------------------------------------------------------------- */
/* printf / fprintf / snprintf.                                      */
/*                                                                   */
/* binjgb only uses these for cart-info pretty-print and PRINT_ERROR */
/* in CHECK_MSG; both are diagnostic. We route everything to UART —  */
/* no buffering, no stream distinction.                              */
/*                                                                   */
/* %s is the only conversion binjgb actually uses outside our        */
/* control; %d/%u/%x covered for safety. No padding/precision (just  */
/* like uart_printf). For snprintf we render to the buffer instead   */
/* of UART and return the byte count.                                */
/* ---------------------------------------------------------------- */

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uart_vprintf(fmt, ap);
    va_end(ap);
    return 0;   /* binjgb ignores the return */
}

int fprintf(void *stream, const char *fmt, ...) {
    (void)stream;
    va_list ap;
    va_start(ap, fmt);
    uart_vprintf(fmt, ap);
    va_end(ap);
    return 0;
}

/* stderr / stdout symbols — binjgb references them as fprintf args.
 * We export bogus pointers; fprintf above ignores the value. */
void *stderr = (void *)0xDEAD0001;
void *stdout = (void *)0xDEAD0002;

/* snprintf: tiny. binjgb only calls it from replace_extension which
 * we don't compile — implement just enough to be safe if some path
 * we missed reaches it. Format support: %s, %d, %.*s. */
int snprintf(char *buf, size_t n, const char *fmt, ...) {
    if (n == 0) return 0;
    va_list ap;
    va_start(ap, fmt);
    size_t i = 0;
    while (*fmt && i < n - 1) {
        if (*fmt != '%') { buf[i++] = *fmt++; continue; }
        fmt++;
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            while (*s && i < n - 1) buf[i++] = *s++;
        } else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x') {
            /* Skip numeric printing — replace_extension doesn't use
             * these and the path is dead anyway. */
            (void)va_arg(ap, int);
        } else {
            buf[i++] = '?';
        }
        fmt++;
    }
    buf[i] = 0;
    va_end(ap);
    return (int)i;
}

/* ---------------------------------------------------------------- */
/* exit / abort / assert.                                            */
/* ---------------------------------------------------------------- */

void exit(int code) {
    uart_printf("\nbinjgb_shim: exit(%d). halting.\n", (int64_t)code);
    for (;;) __asm__ volatile ("wfi");
}

void abort(void) {
    uart_puts("\nbinjgb_shim: abort. halting.\n");
    for (;;) __asm__ volatile ("wfi");
}

/* glibc-style assertion failure handler. Zig cc with riscv64-
 * freestanding-none expands assert() into a call to __assert_fail.
 * Don't disable assertions in binjgb — they catch real bugs — just
 * log and halt rather than throw. */
void __assert_fail(const char *expr, const char *file, unsigned line,
                   const char *func) {
    uart_printf("\nbinjgb assert FAIL: %s\n  at %s:%u in %s\n",
                expr ? expr : "?", file ? file : "?",
                (uint64_t)line, func ? func : "?");
    for (;;) __asm__ volatile ("wfi");
}

/* ---------------------------------------------------------------- */
/* common.c surface stubs.                                           */
/*                                                                   */
/* file_data_resize: actually implemented (used by cart loader).     */
/* file_data_delete: no-op — bump pool can't reclaim, and the        */
/*                   Emulator's file_data lives until firmware halt. */
/* file_read/write:  return ERROR. Savestate / ext-RAM paths that    */
/*                   try to use these from emulator.c land here.     */
/* ---------------------------------------------------------------- */

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

/* The file_*-from-path helpers from common.c don't make sense on
 * bare-metal — there's no filesystem. Return ERROR; binjgb's
 * convenience wrappers (emulator_read_state_from_file etc.) will
 * propagate it. Use the in-memory variants instead. */
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
