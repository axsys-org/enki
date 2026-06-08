#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "enki/allocator.h"

typedef struct enki_arena {
  unsigned char* ptr;
  size_t cap_s;
  size_t off_o;
  enki_allocator our_a;
  enki_allocator allocator_a;
  bool mmap_f;
} enki_arena;

enki_arena* enki_arena_create(const enki_allocator* parent_a, size_t cap_s);
enki_arena* enki_arena_create_overcommit(size_t cap_s);
void enki_arena_destroy(enki_arena* a);
void* enki_arena_alloc(void* ctx, size_t size_s);
void enki_arena_free(void* ctx, void* ptr); // no-op_b
void enki_arena_reset(enki_arena* a);
const enki_allocator* enki_arena_as_allocator(enki_arena* a);
void* enki_arena_start(enki_arena* a);
void* enki_arena_end(enki_arena* a);
void* enki_arena_alloc_aligned(void* ctx, size_t size_s, size_t align_s);
