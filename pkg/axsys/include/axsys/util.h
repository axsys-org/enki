#ifndef AX_UTIL_H
#define AX_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define AX_UNUSED(x) ((void)(x))

#define ax_assertf(cond, ...)                                                  \
  do {                                                                         \
    if (__builtin_expect(!(cond), 0)) {                                        \
      fprintf(stderr, "%s:%d: %s: Assertion `%s' failed", __FILE__, __LINE__,  \
              __func__, #cond);                                                \
      __VA_OPT__(fprintf(stderr, ": " __VA_ARGS__);)                           \
      fputc('\n', stderr);                                                     \
      fflush(stderr);                                                          \
      abort();                                                                 \
    }                                                                          \
  } while (0)

#define ax_abort(...) ax_assertf(false, __VA_ARGS__)

/* Pack short byte strings into a little-endian word (mote). */
#define ax_s1(a)                   ((uint64_t)(uint8_t)(a))
#define ax_s2(a, b)                ((ax_s1(b) << 8) | ax_s1(a))
#define ax_s3(a, b, c)             ((ax_s1(c) << 16) | ax_s2(a, b))
#define ax_s4(a, b, c, d)          ((ax_s1(d) << 24) | ax_s3(a, b, c))
#define ax_s5(a, b, c, d, e)       ((ax_s1(e) << 32) | ax_s4(a, b, c, d))
#define ax_s6(a, b, c, d, e, f)    ((ax_s2(e, f) << 32) | ax_s4(a, b, c, d))
#define ax_s7(a, b, c, d, e, f, g) ((ax_s3(e, f, g) << 32) | ax_s4(a, b, c, d))
#define ax_s8(a, b, c, d, e, f, g, h)                                          \
  ((ax_s4(e, f, g, h) << 32) | ax_s4(a, b, c, d))

#endif
