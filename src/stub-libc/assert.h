#ifndef _STUB_ASSERT_H
#define _STUB_ASSERT_H

void __assert_fail(const char *expr, const char *file, unsigned line,
                   const char *func) __attribute__((noreturn));

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__, __func__))
#endif

#endif
