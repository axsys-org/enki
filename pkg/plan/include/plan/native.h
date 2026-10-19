#ifndef PL_NATIVE_H
#define PL_NATIVE_H

#include <stdint.h>

/* These are format properties, not address hints. Never relocate an image.
 * The build sets PL_NATIVE_SANITIZER because GCC UBSan has no feature macro. */
#if defined(__linux__) && defined(__aarch64__)
#define PL_NATIVE_BASE UINT64_C(0x100000000)
#if defined(PL_NATIVE_SANITIZER) || defined(__SANITIZE_ADDRESS__) ||           \
    defined(__SANITIZE_THREAD__)
#define PL_NATIVE_BYTES (UINT64_C(1) << 33)
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||     \
    __has_feature(undefined_behavior_sanitizer)
#define PL_NATIVE_BYTES (UINT64_C(1) << 33)
#endif
#endif
#else
#define PL_NATIVE_BASE UINT64_C(0x7a0000000000)
#endif
#ifndef PL_NATIVE_BYTES
#define PL_NATIVE_BYTES (UINT64_C(1) << 38)
#endif
#define PL_NATIVE_GRANULE UINT64_C(65536)

#endif
