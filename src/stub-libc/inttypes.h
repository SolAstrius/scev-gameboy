/* binjgb common.h doesn't pull this directly but emulator.c may.
 * We provide a minimal subset — just the format-string macros that
 * are commonly used. Nothing here is essential at runtime; format
 * strings flow into our printf shim which ignores most modifiers. */
#ifndef _STUB_INTTYPES_H
#define _STUB_INTTYPES_H
#include <stdint.h>

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "lld"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "llu"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "llx"

#endif
