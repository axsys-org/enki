#ifndef ENKI_ALLOCATOR_H
#define ENKI_ALLOCATOR_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocates a block of memory.
 *
 * The implementation must return NULL on failure. A request size_s is always
 * greater than zero in calls made by enki.
 */
typedef void* (*enki_alloc_fn)(void* ctx, size_t size_s);

/**
 * Resizes a previously allocated block of memory.
 *
 * The pointer may be NULL, in which case the function should behave like
 * alloc. The implementation must return NULL on failure without releasing the
 * original allocation.
 */
typedef void* (*enki_realloc_fn)(void* ctx, void* ptr, size_t size_s);

/**
 * Releases a block previously returned by the same allocator_a.
 *
 * The pointer may be NULL.
 */
typedef void (*enki_free_fn)(void* ctx, void* ptr);

/**
 * Function-pointer allocator_a used by all enki containers.
 *
 * The alloc and free callbacks are required. The realloc callback is optional;
 * when it is NULL, enki grows storage with alloc, copy, and free.
 */
typedef struct enki_allocator {
  void* ctx;
  enki_alloc_fn alloc;
  enki_realloc_fn realloc;
  enki_free_fn free;
} enki_allocator;

/**
 * Returns an allocator_a backed by malloc, realloc, and free.
 */
const enki_allocator* enki_allocator_system(void);

extern const enki_allocator sys_a;

static inline void* ea_alloc_zero(const enki_allocator* loc_a, size_t elem_s,
                                  size_t count_s) {
  if (count_s != 0 && elem_s > ((size_t)-1) / count_s) {
    return NULL;
  }

  size_t size_s = elem_s * count_s;
  if (size_s == 0) {
    size_s = 1;
  }

  void* ptr = loc_a->alloc(loc_a->ctx, size_s);
  if (ptr != NULL) {
    memset(ptr, 0, size_s);
  }
  return ptr;
}

#define ea_calloc(loc_a, typ, count)                                           \
  (typ*)ea_alloc_zero((loc_a), sizeof(typ), (count))
#define ea_free(loc_a, ptr) ((loc_a)->free((loc_a)->ctx, ptr))

#ifdef __cplusplus
}
#endif

#endif
