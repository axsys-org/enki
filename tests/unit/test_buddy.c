#include "test.h"
#include "axsys/buddy.h"
#include <stdint.h>

TEST(buddy, splits_exhausts_and_coalesces) {
  ax_buddy b;
  ASSERT(ax_buddy_init(&b, 4096, 64));
  size_t blocks[64], actual;
  for (size_t i = 0; i < 64; i++) {
    blocks[i] = ax_buddy_alloc(&b, 33, &actual);
    ASSERT_EQ(actual, 64);
    ASSERT_EQ(blocks[i], i * 64);
  }
  ASSERT_EQ(ax_buddy_alloc(&b, 1, &actual), SIZE_MAX);
  for (size_t i = 0; i < 64; i += 2)
    ax_buddy_free(&b, blocks[i], 64);
  ASSERT_EQ(ax_buddy_alloc(&b, 128, &actual), SIZE_MAX);
  for (size_t i = 1; i < 64; i += 2)
    ax_buddy_free(&b, blocks[i], 64);
  ASSERT_EQ(ax_buddy_alloc(&b, 4096, &actual), 0);
  ASSERT_EQ(actual, 4096);
  ax_buddy_free(&b, 0, actual);
  ax_buddy_destroy(&b);
}

TEST(buddy, mixed_sizes_never_overlap) {
  ax_buddy b;
  ASSERT(ax_buddy_init(&b, 65536, 64));
  size_t offset[64], size[64] = {0};
  uint32_t rng = 7;
  for (unsigned step = 0; step < 10000; step++) {
    rng = rng * 1664525u + 1013904223u;
    unsigned k = (rng >> 16) % 64;
    if (size[k]) {
      ax_buddy_free(&b, offset[k], size[k]);
      size[k] = 0;
    } else {
      size_t actual;
      size_t p = ax_buddy_alloc(&b, 1 + (rng % 8192), &actual);
      if (p == SIZE_MAX)
        continue;
      ASSERT_EQ(p % actual, 0);
      for (unsigned i = 0; i < 64; i++)
        ASSERT(!size[i] || p + actual <= offset[i] || offset[i] + size[i] <= p);
      size[k] = actual;
      offset[k] = p;
    }
  }
  for (unsigned k = 0; k < 64; k++)
    if (size[k])
      ax_buddy_free(&b, offset[k], size[k]);
  size_t actual;
  ASSERT_EQ(ax_buddy_alloc(&b, 65536, &actual), 0);
  ax_buddy_destroy(&b);
}

TEST(buddy, invalid_sizes_and_overflow) {
  ax_buddy b;
  ASSERT_FALSE(ax_buddy_init(&b, 1023, 64));
  ASSERT_FALSE(ax_buddy_init(&b, 1024, 63));
  ASSERT(ax_buddy_init(&b, 1024, 64));
  size_t actual;
  ASSERT_EQ(ax_buddy_alloc(&b, 0, &actual), SIZE_MAX);
  ASSERT_EQ(ax_buddy_alloc(&b, SIZE_MAX, &actual), SIZE_MAX);
  ax_buddy_destroy(&b);
}
