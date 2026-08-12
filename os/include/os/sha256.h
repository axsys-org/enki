#ifndef ENKI_OS_SHA256_H
#define ENKI_OS_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct os_sha256_ctx {
  uint32_t state[8];
  uint64_t bits;
  uint8_t block[64];
  size_t used;
} os_sha256_ctx;
void os_sha256_init(os_sha256_ctx* ctx);
void os_sha256_update(os_sha256_ctx* ctx, const void* data, size_t size);
void os_sha256_final(os_sha256_ctx* ctx, uint8_t out[32]);
#endif
