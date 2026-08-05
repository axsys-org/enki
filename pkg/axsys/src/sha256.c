#include "axsys/sha256.h"

#ifndef ENKI_WASM
#include <openssl/sha.h>

void ax_sha256(const uint8_t* data_b, size_t len_s, uint8_t out_b[32]) {
  SHA256(data_b, len_s, out_b);
}
#else
#include <string.h>

static uint32_t ax_rotr32(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32u - n));
}

static uint32_t ax_load_be32(const uint8_t b[4]) {
  return ((uint32_t)b[0] << 24u) | ((uint32_t)b[1] << 16u) |
         ((uint32_t)b[2] << 8u) | (uint32_t)b[3];
}

static void ax_store_be32(uint8_t b[4], uint32_t x) {
  b[0] = (uint8_t)(x >> 24u);
  b[1] = (uint8_t)(x >> 16u);
  b[2] = (uint8_t)(x >> 8u);
  b[3] = (uint8_t)x;
}

static const uint32_t ax_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static void ax_sha256_block(uint32_t h[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (size_t i = 0; i < 16; i++)
    w[i] = ax_load_be32(block + i * 4u);
  for (size_t i = 16; i < 64; i++) {
    uint32_t s0 =
        ax_rotr32(w[i - 15], 7) ^ ax_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3u);
    uint32_t s1 =
        ax_rotr32(w[i - 2], 17) ^ ax_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10u);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = h[0];
  uint32_t b = h[1];
  uint32_t c = h[2];
  uint32_t d = h[3];
  uint32_t e = h[4];
  uint32_t f = h[5];
  uint32_t g = h[6];
  uint32_t hh = h[7];

  for (size_t i = 0; i < 64; i++) {
    uint32_t s1 = ax_rotr32(e, 6) ^ ax_rotr32(e, 11) ^ ax_rotr32(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + s1 + ch + ax_sha256_k[i] + w[i];
    uint32_t s0 = ax_rotr32(a, 2) ^ ax_rotr32(a, 13) ^ ax_rotr32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

void ax_sha256(const uint8_t* data_b, size_t len_s, uint8_t out_b[32]) {
  uint32_t h[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  size_t total_s = len_s;

  while (len_s >= 64) {
    ax_sha256_block(h, data_b);
    data_b += 64;
    len_s -= 64;
  }

  uint8_t tail[128] = {0};
  memcpy(tail, data_b, len_s);
  tail[len_s] = 0x80u;
  size_t tail_s = len_s + 1u;
  if (tail_s > 56)
    tail_s = 120;
  else
    tail_s = 56;

  uint64_t bit_len = (uint64_t)total_s * 8u;
  for (size_t i = 0; i < 8; i++)
    tail[tail_s + i] = (uint8_t)(bit_len >> ((7u - i) * 8u));

  ax_sha256_block(h, tail);
  if (tail_s == 120)
    ax_sha256_block(h, tail + 64);

  for (size_t i = 0; i < 8; i++)
    ax_store_be32(out_b + i * 4u, h[i]);
}
#endif
