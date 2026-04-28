/* Minimal <stdio.h> stub for binjgb's freestanding build.
 * Declarations only — implementations live in binjgb_shim.c, which
 * routes printf/fprintf to UART and stubs file ops with ERROR. */
#ifndef _STUB_STDIO_H
#define _STUB_STDIO_H
#include <stddef.h>

typedef struct _FILE FILE;
extern void *stdout;
extern void *stderr;

int printf(const char *fmt, ...);
int fprintf(void *stream, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);

/* The file_* family in binjgb's common.c uses these. We don't compile
 * common.c, so these declarations exist only to satisfy whatever path
 * accidentally drags them into a TU we DO compile. They never resolve
 * at link time because nothing calls them. */
FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
size_t fread(void *p, size_t s, size_t n, FILE *f);
size_t fwrite(const void *p, size_t s, size_t n, FILE *f);
int   fseek(FILE *f, long off, int whence);
long  ftell(FILE *f);
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif
