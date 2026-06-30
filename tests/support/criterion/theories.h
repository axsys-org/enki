#pragma once

#ifdef ENKI_WASM

#include <stdint.h>

#define TheoryDataPoints(suite, name) static uintptr_t suite##_##name##_points[]
#define DataPoints(type, ...)         __VA_ARGS__

#define Theory(param_decl, suite, name)                                        \
  static void suite##_##name##_impl param_decl;                                \
  Test(suite, name) {                                                          \
    for (size_t cr_i = 0; cr_i < sizeof(suite##_##name##_points) /             \
                                     sizeof(suite##_##name##_points[0]);       \
         cr_i++)                                                               \
      suite##_##name##_impl((uintptr_t)suite##_##name##_points[cr_i]);         \
  }                                                                            \
  static void suite##_##name##_impl param_decl

#else
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-include-next"
#endif
#include_next <criterion/theories.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
