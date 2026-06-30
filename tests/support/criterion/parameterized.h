#pragma once

#ifdef ENKI_WASM

#include <stddef.h>
#include <stdint.h>

typedef struct cr_param_array {
  void* data;
  size_t size;
  size_t length;
} cr_param_array;

#define cr_make_param_array(type, values, count)                               \
  ((cr_param_array){.data = (values), .size = sizeof(type), .length = (count)})

#define ParameterizedTestParameters(suite, name)                               \
  static cr_param_array suite##_##name##_params(void)

#define ParameterizedTest(param_decl, suite, name)                             \
  static void suite##_##name##_impl(param_decl);                               \
  Test(suite, name) {                                                          \
    cr_param_array params = suite##_##name##_params();                         \
    for (size_t cr_i = 0; cr_i < params.length; cr_i++)                        \
      suite##_##name##_impl((void*)((char*)params.data + cr_i * params.size)); \
  }                                                                            \
  static void suite##_##name##_impl(param_decl)

#else
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-include-next"
#endif
#include_next <criterion/parameterized.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
