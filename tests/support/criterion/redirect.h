#pragma once

#ifdef ENKI_WASM
static inline void cr_redirect_stderr(void) {}
static inline void cr_redirect_stdout(void) {}
#else
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-include-next"
#endif
#include_next <criterion/redirect.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
