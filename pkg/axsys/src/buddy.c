#include "axsys/buddy.h"
#include "axsys/assume.h"
#include <stdlib.h>
#include <string.h>

bool ax_buddy_init(ax_buddy* b, size_t capacity, size_t minimum) {
  memset(b, 0, sizeof(*b));
  if (!minimum || (minimum & (minimum - 1)) || capacity < minimum ||
      (capacity & (capacity - 1)))
    return false;
  size_t leaves = capacity / minimum;
  if (leaves > SIZE_MAX / 2)
    return false;
  b->tree = calloc(2 * leaves, sizeof(*b->tree));
  if (!b->tree)
    return false;
  b->capacity = capacity;
  b->minimum = minimum;
  for (size_t n = leaves; n > 1; n >>= 1)
    b->order++;
  b->tree[1] = (int8_t)b->order;
  return true;
}

void ax_buddy_destroy(ax_buddy* b) {
  free(b->tree);
  memset(b, 0, sizeof(*b));
}

size_t ax_buddy_alloc(ax_buddy* b, size_t bytes, size_t* actual) {
  if (!bytes || bytes > b->capacity)
    return SIZE_MAX;
  unsigned need = 0;
  size_t size = b->minimum;
  while (size < bytes) {
    size <<= 1;
    need++;
  }
  if (b->tree[1] < (int)need)
    return SIZE_MAX;
  size_t node = 1, offset = 0;
  unsigned order = b->order;
  while (order > need) {
    if (b->tree[node] == (int)order) {
      b->tree[node * 2] = (int8_t)(order - 1);
      b->tree[node * 2 + 1] = (int8_t)(order - 1);
    }
    order--;
    node *= 2;
    if (b->tree[node] < (int)need) {
      node++;
      offset += b->minimum << order;
    }
  }
  b->tree[node] = -1;
  while (node > 1) {
    node /= 2;
    int8_t a = b->tree[node * 2], c = b->tree[node * 2 + 1];
    b->tree[node] = a > c ? a : c;
  }
  *actual = size;
  return offset;
}

void ax_buddy_free(ax_buddy* b, size_t offset, size_t actual) {
  ax_assume(actual >= b->minimum && actual <= b->capacity &&
                !(actual & (actual - 1)) && offset % actual == 0 &&
                offset <= b->capacity - actual,
            "invalid buddy block");
  unsigned order = 0;
  for (size_t n = actual / b->minimum; n > 1; n >>= 1)
    order++;
  size_t node = (b->capacity + offset) / actual;
  ax_assume(b->tree[node] == -1, "buddy double free or wrong block size");
  b->tree[node] = (int8_t)order;
  while (node > 1) {
    node /= 2;
    int8_t a = b->tree[node * 2], c = b->tree[node * 2 + 1];
    b->tree[node] = a == (int)order && c == (int)order ? (int8_t)(order + 1)
                                                       : (a > c ? a : c);
    order++;
  }
}
