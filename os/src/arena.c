#include "axsys/arena.h"

#include <stdint.h>
#include <stdlib.h>

static ax_arena* arena_create(const ax_allocator* allocator, size_t cap) {
  if (allocator == NULL || cap > SIZE_MAX - sizeof(ax_arena))
    return NULL;
  size_t total = sizeof(ax_arena) + cap;
  ax_arena* arena = allocator->alloc(allocator->ctx, total);
  if (arena == NULL)
    return NULL;
  arena->off_o = sizeof(ax_arena);
  arena->ptr = (unsigned char*)arena;
  arena->cap_s = total;
  arena->our_a = *allocator;
  arena->mmap_f = false;
  arena->allocator_a = (ax_allocator){
      .ctx = arena,
      .alloc = ax_arena_alloc,
      .realloc = NULL,
      .free = ax_arena_free,
  };
  return arena;
}

ax_arena* ax_arena_create(const ax_allocator* allocator, size_t cap) {
  return arena_create(allocator, cap);
}

ax_arena* ax_arena_create_overcommit(size_t cap) {
  /* Hosted Enki reserves enormous sparse mappings for printers. Bare metal
   * has no demand paging, so cap those scratch reservations. The persistent
   * store's explicit 128 MiB request remains unchanged. */
  const size_t scratch_cap = (size_t)1 << 26;
  if (cap > ((size_t)1 << 27))
    cap = scratch_cap;
  return arena_create(ax_allocator_system(), cap);
}

void ax_arena_destroy(ax_arena* arena) {
  if (arena != NULL)
    arena->our_a.free(arena->our_a.ctx, arena);
}

void* ax_arena_alloc(void* context, size_t size) {
  ax_arena* arena = context;
  if (arena == NULL)
    return NULL;
  size_t alignment = _Alignof(max_align_t);
  size_t offset = (arena->off_o + alignment - 1) & ~(alignment - 1);
  if (size > arena->cap_s - offset)
    return NULL;
  arena->off_o = offset + size;
  return arena->ptr + offset;
}

void* ax_arena_alloc_aligned(void* context, size_t size, size_t alignment) {
  ax_arena* arena = context;
  if (arena == NULL || alignment == 0 || (alignment & (alignment - 1)) != 0)
    return NULL;
  size_t offset = (arena->off_o + alignment - 1) & ~(alignment - 1);
  if (size > arena->cap_s - offset)
    return NULL;
  arena->off_o = offset + size;
  return arena->ptr + offset;
}

void ax_arena_free(void* context, void* pointer) {
  (void)context;
  (void)pointer;
}

void ax_arena_reset(ax_arena* arena) {
  if (arena != NULL)
    arena->off_o = sizeof(ax_arena);
}

const ax_allocator* ax_arena_as_allocator(ax_arena* arena) {
  return arena == NULL ? NULL : &arena->allocator_a;
}

void* ax_arena_start(ax_arena* arena) {
  return arena == NULL ? NULL : arena->ptr + sizeof(ax_arena);
}

void* ax_arena_end(ax_arena* arena) {
  return arena == NULL ? NULL : arena->ptr + arena->cap_s;
}
