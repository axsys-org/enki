#ifndef ENKI_OS_ASSERT_H
#define ENKI_OS_ASSERT_H

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
[[noreturn]] void __assert_fail(const char* expr, const char* file,
                                unsigned line, const char* function);
#define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__, __func__))
#endif

#endif
