#include "axsys/vector.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define EXPECT(condition)                                                      \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, __LINE__,   \
              #condition);                                                     \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (0)

static int run_vector_unit_checks(void) {
  ax_vector* vector = ax_vector_create(ax_allocator_system());
  EXPECT(vector != NULL);
  EXPECT(ax_vector_len(vector) == 0);
  EXPECT(ax_vector_capacity(vector) == 0);

  EXPECT(ax_vector_push(vector, (void*)(uintptr_t)1u) == AX_OK);
  EXPECT(ax_vector_push(vector, (void*)(uintptr_t)2u) == AX_OK);
  EXPECT(ax_vector_len(vector) == 2);
  EXPECT(ax_vector_get(vector, 0) == (void*)(uintptr_t)1u);
  EXPECT(ax_vector_set(vector, 1, (void*)(uintptr_t)3u) == AX_OK);
  EXPECT(ax_vector_get(vector, 1) == (void*)(uintptr_t)3u);

  size_t capacity_s = ax_vector_capacity(vector);
  EXPECT(ax_vector_reserve(vector, 1) == AX_OK);
  EXPECT(ax_vector_capacity(vector) == capacity_s);
  EXPECT(ax_vector_shrink(vector) == AX_OK);
  EXPECT(ax_vector_capacity(vector) == ax_vector_len(vector));

  EXPECT(ax_vector_pop(vector) == (void*)(uintptr_t)3u);
  EXPECT(ax_vector_pop(vector) == (void*)(uintptr_t)1u);
  EXPECT(ax_vector_pop(vector) == NULL);

  ax_vector_destroy(vector);
  return EXIT_SUCCESS;
}

int main(void) {
  return run_vector_unit_checks();
}
