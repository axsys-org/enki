#include "axsys/vector.h"
#include "fff.h"

#include <criterion/criterion.h>
#include <criterion/parameterized.h>
#include <criterion/theories.h>
#include <stdint.h>
#include <stdlib.h>

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(void*, fake_alloc, void*, size_t);
FAKE_VALUE_FUNC(void*, fake_realloc, void*, void*, size_t);
FAKE_VOID_FUNC(fake_free, void*, void*);

static ax_vector* fixture_vector;

static void setup(void) {
  fixture_vector = ax_vector_create(ax_allocator_system());
  cr_assert_not_null(fixture_vector);
}

static void teardown(void) {
  ax_vector_destroy(fixture_vector);
  fixture_vector = NULL;
}

TestSuite(vector, .init = setup, .fini = teardown);

static const ax_allocator* fake_allocator(void) {
  static const ax_allocator allocator_a = {
      .ctx = NULL,
      .alloc = fake_alloc,
      .realloc = fake_realloc,
      .free = fake_free,
  };
  return &allocator_a;
}

static void reset_allocator_fakes(void) {
  RESET_FAKE(fake_alloc);
  RESET_FAKE(fake_realloc);
  RESET_FAKE(fake_free);
}

static void* malloc_fake(void* ctx, size_t size_s) {
  (void)ctx;
  return malloc(size_s);
}

static void* realloc_fake(void* ctx, void* ptr, size_t size_s) {
  (void)ctx;
  return realloc(ptr, size_s);
}

static void free_fake(void* ctx, void* ptr) {
  (void)ctx;
  free(ptr);
}

static void* fail_second_alloc(void* ctx, size_t size_s) {
  (void)ctx;
  if (fake_alloc_fake.call_count >= 2u) {
    (void)size_s;
    return NULL;
  }

  return malloc(size_s);
}

static void* always_fail_realloc(void* ctx, void* ptr, size_t size_s) {
  (void)ctx;
  (void)ptr;
  (void)size_s;
  return NULL;
}

Test(vector, starts_empty) {
  cr_assert_eq(ax_vector_len(fixture_vector), 0);
  cr_assert_eq(ax_vector_capacity(fixture_vector), 0);
  cr_assert_null(ax_vector_pop(fixture_vector));
}

Test(vector, pushes_gets_sets_and_pops) {
  uintptr_t one = (uintptr_t)0x1u;
  uintptr_t two = (uintptr_t)0x2u;
  uintptr_t three = (uintptr_t)0x3u;

  cr_assert_eq(ax_vector_push(fixture_vector, (void*)one), AX_OK);
  cr_assert_eq(ax_vector_push(fixture_vector, (void*)two), AX_OK);
  cr_assert_eq(ax_vector_len(fixture_vector), 2);
  cr_assert_eq(ax_vector_get(fixture_vector, 0), (void*)one);
  cr_assert_eq(ax_vector_set(fixture_vector, 1, (void*)three), AX_OK);
  cr_assert_eq(ax_vector_get(fixture_vector, 1), (void*)three);
  cr_assert_eq(ax_vector_pop(fixture_vector), (void*)three);
  cr_assert_eq(ax_vector_pop(fixture_vector), (void*)one);
  cr_assert_null(ax_vector_pop(fixture_vector));
}

Test(vector, rejects_invalid_arguments) {
  cr_assert_null(ax_vector_create(&(const ax_allocator){0}));
  cr_assert_eq(ax_vector_push(NULL, NULL), AX_ERR_INVALID);
  cr_assert_eq(ax_vector_set(NULL, 0, NULL), AX_ERR_INVALID);
  cr_assert_eq(ax_vector_reserve(NULL, 1), AX_ERR_INVALID);
  cr_assert_eq(ax_vector_shrink(NULL), AX_ERR_INVALID);
  cr_assert_eq(ax_vector_set(fixture_vector, 0, NULL), AX_ERR_BOUNDS);
  cr_assert_null(ax_vector_get(fixture_vector, 0));
}

Test(vector, allocator_alloc_failure_is_reported_on_create_and_growth) {
  reset_allocator_fakes();
  fake_alloc_fake.return_val = NULL;

  cr_assert_null(ax_vector_create(fake_allocator()));
  cr_assert_eq(fake_alloc_fake.call_count, 1);

  reset_allocator_fakes();
  fake_alloc_fake.custom_fake = fail_second_alloc;
  fake_free_fake.custom_fake = free_fake;

  ax_vector* vector = ax_vector_create(fake_allocator());
  cr_assert_not_null(vector);
  cr_assert_eq(ax_vector_push(vector, (void*)(uintptr_t)1u), AX_ERR_ALLOC);
  cr_assert_eq(ax_vector_len(vector), 0);

  ax_vector_destroy(vector);
  cr_assert_geq(fake_free_fake.call_count, 1);
}

Test(vector, allocator_realloc_failure_keeps_existing_storage) {
  reset_allocator_fakes();
  fake_alloc_fake.custom_fake = malloc_fake;
  fake_realloc_fake.custom_fake = realloc_fake;
  fake_free_fake.custom_fake = free_fake;

  ax_vector* vector = ax_vector_create(fake_allocator());
  cr_assert_not_null(vector);
  cr_assert_eq(ax_vector_reserve(vector, 8), AX_OK);
  cr_assert_eq(ax_vector_push(vector, (void*)(uintptr_t)1u), AX_OK);

  size_t capacity_s = ax_vector_capacity(vector);
  fake_realloc_fake.custom_fake = always_fail_realloc;

  cr_assert_eq(ax_vector_shrink(vector), AX_ERR_ALLOC);
  cr_assert_eq(ax_vector_len(vector), 1);
  cr_assert_eq(ax_vector_capacity(vector), capacity_s);
  cr_assert_eq(ax_vector_get(vector, 0), (void*)(uintptr_t)1u);

  fake_realloc_fake.custom_fake = realloc_fake;
  ax_vector_destroy(vector);
}

typedef struct reserve_case {
  size_t requested;
  size_t expected;
} reserve_case;

ParameterizedTestParameters(vector, reserve_tracks_capacity) {
  static reserve_case cases[] = {
      {.requested = 0, .expected = 0},
      {.requested = 1, .expected = 1},
      {.requested = 8, .expected = 8},
      {.requested = 32, .expected = 32},
  };

  return cr_make_param_array(reserve_case, cases,
                             sizeof(cases) / sizeof(cases[0]));
}

ParameterizedTest(reserve_case* param, vector, reserve_tracks_capacity) {
  ax_vector* vector = ax_vector_create(ax_allocator_system());
  cr_assert_not_null(vector);
  cr_assert_eq(ax_vector_reserve(vector, param->requested), AX_OK);
  cr_assert_geq(ax_vector_capacity(vector), param->expected);
  cr_assert_eq(ax_vector_len(vector), 0);
  ax_vector_destroy(vector);
}

TheoryDataPoints(vector, set_get_roundtrip) = {
    DataPoints(uintptr_t, (uintptr_t)0u, (uintptr_t)1u, (uintptr_t)255u,
               UINTPTR_MAX),
};

Theory((uintptr_t raw), vector, set_get_roundtrip) {
  ax_vector* vector = ax_vector_create(ax_allocator_system());
  cr_assert_not_null(vector);
  cr_assert_eq(ax_vector_push(vector, NULL), AX_OK);
  cr_assert_eq(ax_vector_set(vector, 0, (void*)raw), AX_OK);
  cr_assert_eq(ax_vector_get(vector, 0), (void*)raw);
  ax_vector_destroy(vector);
}
