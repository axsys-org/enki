#ifndef ENKI_ALLOCATOR_H
#define ENKI_ALLOCATOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocates a block of memory.
 *
 * The implementation must return NULL on failure. A request size is always
 * greater than zero in calls made by enki.
 */
typedef void* (*enki_alloc_fn)(void* ctx, size_t size);

/**
 * Resizes a previously allocated block of memory.
 *
 * The pointer may be NULL, in which case the function should behave like
 * alloc. The implementation must return NULL on failure without releasing the
 * original allocation.
 */
typedef void* (*enki_realloc_fn)(void* ctx, void* ptr, size_t size);

/**
 * Releases a block previously returned by the same allocator.
 *
 * The pointer may be NULL.
 */
typedef void (*enki_free_fn)(void* ctx, void* ptr);

/**
 * Function-pointer allocator used by all enki containers.
 *
 * The alloc and free callbacks are required. The realloc callback is optional;
 * when it is NULL, enki grows storage with alloc, copy, and free.
 */
typedef struct enki_allocator {
    void*           ctx;
    enki_alloc_fn   alloc;
    enki_realloc_fn realloc;
    enki_free_fn    free;
} enki_allocator;

/**
 * Returns an allocator backed by malloc, realloc, and free.
 */
enki_allocator enki_allocator_system(void);

#ifdef __cplusplus
}
#endif

#endif
