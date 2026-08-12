#include "os/sha256.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>

#include "axsys/sha256.h"

static const uint32_t constants[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static uint32_t rotr(uint32_t value, unsigned count) {
  return (value >> count) | (value << (32 - count));
}

static void transform(os_sha256_ctx* context, const uint8_t block[64]) {
  uint32_t words[64];
  for (size_t i = 0; i < 16; i++)
    words[i] = (uint32_t)block[i * 4] << 24 |
               (uint32_t)block[i * 4 + 1] << 16 |
               (uint32_t)block[i * 4 + 2] << 8 |
               block[i * 4 + 3];
  for (size_t i = 16; i < 64; i++) {
    uint32_t s0 = rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
    uint32_t s1 = rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
  }
  uint32_t a = context->state[0], b = context->state[1];
  uint32_t c = context->state[2], d = context->state[3];
  uint32_t e = context->state[4], f = context->state[5];
  uint32_t g = context->state[6], h = context->state[7];
  for (size_t i = 0; i < 64; i++) {
    uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t choose = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + choose + constants[i] + words[i];
    uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + majority;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  context->state[0] += a; context->state[1] += b;
  context->state[2] += c; context->state[3] += d;
  context->state[4] += e; context->state[5] += f;
  context->state[6] += g; context->state[7] += h;
}

void os_sha256_init(os_sha256_ctx* context) {
  *context = (os_sha256_ctx){
      .state = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19},
  };
}

void os_sha256_update(os_sha256_ctx* context, const void* data, size_t size) {
  const uint8_t* bytes = data;
  context->bits += (uint64_t)size * 8;
  while (size != 0) {
    size_t available = 64 - context->used;
    size_t take = size < available ? size : available;
    memcpy(context->block + context->used, bytes, take);
    context->used += take;
    bytes += take;
    size -= take;
    if (context->used == 64) {
      transform(context, context->block);
      context->used = 0;
    }
  }
}

void os_sha256_final(os_sha256_ctx* context, uint8_t out[32]) {
  uint64_t bits = context->bits;
  context->block[context->used++] = 0x80;
  if (context->used > 56) {
    memset(context->block + context->used, 0, 64 - context->used);
    transform(context, context->block);
    context->used = 0;
  }
  memset(context->block + context->used, 0, 56 - context->used);
  for (size_t i = 0; i < 8; i++)
    context->block[63 - i] = (uint8_t)(bits >> (i * 8));
  transform(context, context->block);
  for (size_t i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(context->state[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(context->state[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(context->state[i] >> 8);
    out[i * 4 + 3] = (uint8_t)context->state[i];
  }
}

unsigned char* SHA256(const unsigned char* data, size_t size,
                      unsigned char out[32]) {
  os_sha256_ctx context;
  os_sha256_init(&context);
  os_sha256_update(&context, data, size);
  os_sha256_final(&context, out);
  return out;
}

void ax_sha256(const uint8_t* data, size_t size, uint8_t out[32]) {
  (void)SHA256(data, size, out);
}

struct EVP_MD_CTX { os_sha256_ctx sha; };
struct EVP_MD { int kind; };
static const EVP_MD sha256_md = {.kind = 256};

EVP_MD_CTX* EVP_MD_CTX_new(void) { return calloc(1, sizeof(EVP_MD_CTX)); }
void EVP_MD_CTX_free(EVP_MD_CTX* context) { free(context); }
const EVP_MD* EVP_sha256(void) { return &sha256_md; }
int EVP_DigestInit_ex(EVP_MD_CTX* context, const EVP_MD* type, void* impl) {
  (void)impl;
  if (context == NULL || type != &sha256_md) return 0;
  os_sha256_init(&context->sha);
  return 1;
}
int EVP_DigestUpdate(EVP_MD_CTX* context, const void* data, size_t size) {
  if (context == NULL) return 0;
  os_sha256_update(&context->sha, data, size);
  return 1;
}
int EVP_DigestFinal_ex(EVP_MD_CTX* context, unsigned char* out,
                       unsigned int* out_size) {
  if (context == NULL || out == NULL) return 0;
  os_sha256_final(&context->sha, out);
  if (out_size != NULL) *out_size = 32;
  return 1;
}
