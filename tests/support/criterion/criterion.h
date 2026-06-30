#pragma once

#ifdef ENKI_WASM

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*cr_test_fn)(void);

typedef struct cr_wasm_test {
  const char* suite;
  const char* name;
  cr_test_fn fn;
} cr_wasm_test;

static cr_wasm_test cr_wasm_tests[512];
static size_t cr_wasm_test_count;
static size_t cr_wasm_failures;

static void cr_wasm_register(const char* suite, const char* name,
                             cr_test_fn fn) {
  if (cr_wasm_test_count >= sizeof(cr_wasm_tests) / sizeof(cr_wasm_tests[0])) {
    fputs("too many tests\n", stderr);
    abort();
  }
  cr_wasm_tests[cr_wasm_test_count++] =
      (cr_wasm_test){.suite = suite, .name = name, .fn = fn};
}

static void cr_wasm_fail_at(const char* file, int line, const char* expr,
                            const char* fmt, ...) {
  cr_wasm_failures++;
  fprintf(stderr, "%s:%d: assertion failed: %s", file, line, expr);
  if (fmt != NULL && fmt[0] != '\0') {
    fputs(": ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
  fputc('\n', stderr);
}

static int cr_wasm_run_all(int argc, char** argv) {
  (void)argc;
  (void)argv;
  for (size_t i = 0; i < cr_wasm_test_count; i++) {
    fprintf(stderr, "[wasm-test] %s/%s\n", cr_wasm_tests[i].suite,
            cr_wasm_tests[i].name);
    cr_wasm_tests[i].fn();
  }
  return cr_wasm_failures == 0 ? 0 : 1;
}

#define CR_WASM_JOIN2(a, b) a##b
#define CR_WASM_JOIN(a, b)  CR_WASM_JOIN2(a, b)

#define Test(suite, name, ...)                                                 \
  static void CR_WASM_JOIN(suite, CR_WASM_JOIN(_, name))(void);                \
  __attribute__((constructor)) static void CR_WASM_JOIN(                       \
      suite, CR_WASM_JOIN(_, CR_WASM_JOIN(name, _register)))(void) {           \
    cr_wasm_register(#suite, #name,                                            \
                     CR_WASM_JOIN(suite, CR_WASM_JOIN(_, name)));              \
  }                                                                            \
  static void CR_WASM_JOIN(suite, CR_WASM_JOIN(_, name))(void)

#define TestSuite(suite, ...) typedef int suite##_cr_wasm_testsuite_unused

#define cr_assert(cond, ...)                                                   \
  do {                                                                         \
    if (!(cond))                                                               \
      cr_wasm_fail_at(__FILE__, __LINE__, #cond,                               \
                      "" __VA_OPT__(, ) __VA_ARGS__);                          \
  } while (0)

#define cr_assert_not(cond, ...) cr_assert(!(cond), __VA_ARGS__)

#define cr_assert_fail(...)                                                    \
  do {                                                                         \
    cr_wasm_fail_at(__FILE__, __LINE__, "cr_assert_fail",                      \
                    "" __VA_OPT__(, ) __VA_ARGS__);                            \
    return;                                                                    \
  } while (0)

#define cr_skip_test(...) return

#define cr_assert_eq(a, b, ...)    cr_assert((a) == (b), __VA_ARGS__)
#define cr_assert_neq(a, b, ...)   cr_assert((a) != (b), __VA_ARGS__)
#define cr_assert_lt(a, b, ...)    cr_assert((a) < (b), __VA_ARGS__)
#define cr_assert_gt(a, b, ...)    cr_assert((a) > (b), __VA_ARGS__)
#define cr_assert_geq(a, b, ...)   cr_assert((a) >= (b), __VA_ARGS__)
#define cr_assert_null(a, ...)     cr_assert((a) == NULL, __VA_ARGS__)
#define cr_assert_not_null(a, ...) cr_assert((a) != NULL, __VA_ARGS__)
#define cr_assert_str_eq(a, b, ...)                                            \
  cr_assert((a) != NULL && (b) != NULL && strcmp((a), (b)) == 0, __VA_ARGS__)

int main(int argc, char** argv) {
  return cr_wasm_run_all(argc, argv);
}

#else
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-include-next"
#endif
#include_next <criterion/criterion.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
