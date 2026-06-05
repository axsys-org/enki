#include "enki/vector.h"

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
  enki_vector* vector = enki_vector_create(enki_allocator_system());
  EXPECT(vector != NULL);
  EXPECT(enki_vector_len(vector) == 0);
  EXPECT(enki_vector_capacity(vector) == 0);

  EXPECT(enki_vector_push(vector, (void*)(uintptr_t)1u) == ENKI_OK);
  EXPECT(enki_vector_push(vector, (void*)(uintptr_t)2u) == ENKI_OK);
  EXPECT(enki_vector_len(vector) == 2);
  EXPECT(enki_vector_get(vector, 0) == (void*)(uintptr_t)1u);
  EXPECT(enki_vector_set(vector, 1, (void*)(uintptr_t)3u) == ENKI_OK);
  EXPECT(enki_vector_get(vector, 1) == (void*)(uintptr_t)3u);

  size_t capacity_s = enki_vector_capacity(vector);
  EXPECT(enki_vector_reserve(vector, 1) == ENKI_OK);
  EXPECT(enki_vector_capacity(vector) == capacity_s);
  EXPECT(enki_vector_shrink(vector) == ENKI_OK);
  EXPECT(enki_vector_capacity(vector) == enki_vector_len(vector));

  EXPECT(enki_vector_pop(vector) == (void*)(uintptr_t)3u);
  EXPECT(enki_vector_pop(vector) == (void*)(uintptr_t)1u);
  EXPECT(enki_vector_pop(vector) == NULL);

  enki_vector_destroy(vector);
  return EXIT_SUCCESS;
}

int main(void) {
  return run_vector_unit_checks();
}
