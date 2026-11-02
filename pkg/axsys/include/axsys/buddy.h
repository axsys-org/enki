#ifndef AX_BUDDY_H
#define AX_BUDDY_H
#include <stddef.h>
#include <stdint.h>

/* Offset allocator. The caller supplies memory and synchronization. Both
 * capacity and minimum block size must be powers of two. */
typedef struct ax_buddy {
  size_t capacity, minimum;
  unsigned order;
  int8_t* tree;
} ax_buddy;

bool ax_buddy_init(ax_buddy* b, size_t capacity, size_t minimum);
void ax_buddy_destroy(ax_buddy* b);
/* SIZE_MAX on exhaustion/overflow; actual receives the rounded block size. */
size_t ax_buddy_alloc(ax_buddy* b, size_t bytes, size_t* actual);
void ax_buddy_free(ax_buddy* b, size_t offset, size_t actual);
#endif
