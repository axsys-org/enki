#ifndef ENKI_UTIL_H
#define ENKI_UTIL_H
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "stb_ds.h"

#define shr_ceil(x, n) ((x >> n) + ((x & ((1 << n) - 1)) != 0))
#define max(x, y)      (x > y ? x : y)
#define min(x, y)      (x > y ? y : x)
#define die(msg)       assert((msg && 0))
#define UNUSED(x)      ((void)x)

#include <stdio.h>
#include <stdlib.h>

#define ea_assertf(cond, ...)                                                  \
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

#define ea_abort(...) ea_assertf(false, __VA_ARGS__)

#define ea_s1(a)          ((a))
#define ea_s2(a, b)       (((b) << 8) | ea_s1(a))
#define ea_s3(a, b, c)    (((c) << 16) | ea_s2(a, b))
#define ea_s4(a, b, c, d) (((d) << 24) | ea_s3(a, b, c))

#define ea_s5(a, b, c, d, e) (((uint64_t)ea_s1(e) << 32ULL) | ea_s4(a, b, c, d))
#define ea_s6(a, b, c, d, e, f)                                                \
  (((uint64_t)ea_s2(e, f) << 32ULL) | ea_s4(a, b, c, d))
#define ea_s7(a, b, c, d, e, f, g)                                             \
  (((uint64_t)ea_s3(e, f, g) << 32ULL) | ea_s4(a, b, c, d))
#define ea_s8(a, b, c, d, e, f, g, h)                                          \
  (((uint64_t)ea_s4(e, f, g, h) << 32ULL) | ea_s4(a, b, c, d))

#define WILD              ((uint64_t)-1)
#define ARG_EQ(slot, pat) ((pat) == WILD || (slot) == (pat))

#define match_app1more(app, hd, one)                                           \
     (app).hd_v == (hd) &&                            \
     ARG_EQ((app)->args_v[0], one) &&                  \
     ARG_EQ((app)->args_v[1], two) &&                  \
     ARG_EQ((app)->args_v[2], three))

#define match_app2(app, hd, one, two)                                          \
  ((app)->n_args_s == 2 && (app)->fn_v == (hd) &&                              \
   ARG_EQ((app)->args_v[0], one) && ARG_EQ((app)->args_v[1], two))

#define match_app3(app, hd, one, two, three)                                   \
  ((app)->n_args_s == 3 && (app)->fn_v == (hd) &&                              \
   ARG_EQ((app)->args_v[0], one) && ARG_EQ((app)->args_v[1], two) &&           \
   ARG_EQ((app)->args_v[2], three))
#endif
