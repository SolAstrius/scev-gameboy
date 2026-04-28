#ifndef _STUB_STDLIB_H
#define _STUB_STDLIB_H
#include <stddef.h>

void *malloc(size_t n);
void *calloc(size_t count, size_t n);
void *realloc(void *p, size_t n);
void  free(void *p);
void  exit(int code) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));

#endif
