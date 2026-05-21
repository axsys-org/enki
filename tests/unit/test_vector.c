#include "enki/vector.h"
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

static enki_vector* fixture_vector;

static void setup(void)
{
    fixture_vector = enki_vector_create(enki_allocator_system());
    cr_assert_not_null(fixture_vector);
}

static void teardown(void)
{
    enki_vector_destroy(fixture_vector);
    fixture_vector = NULL;
}

TestSuite(vector, .init = setup, .fini = teardown);

static enki_allocator fake_allocator(void)
{
    return (enki_allocator){
        .ctx = NULL,
        .alloc = fake_alloc,
        .realloc = fake_realloc,
        .free = fake_free,
    };
}

static void reset_allocator_fakes(void)
{
    RESET_FAKE(fake_alloc);
    RESET_FAKE(fake_realloc);
    RESET_FAKE(fake_free);
}

static void* malloc_fake(void* ctx, size_t size_s)
{
    (void)ctx;
    return malloc(size_s);
}

static void* realloc_fake(void* ctx, void* ptr, size_t size_s)
{
    (void)ctx;
    return realloc(ptr, size_s);
}

static void free_fake(void* ctx, void* ptr)
{
    (void)ctx;
    free(ptr);
}

static void* fail_second_alloc(void* ctx, size_t size_s)
{
    (void)ctx;
    if (fake_alloc_fake.call_count >= 2u) {
        (void)size_s;
        return NULL;
    }

    return malloc(size_s);
}

static void* always_fail_realloc(void* ctx, void* ptr, size_t size_s)
{
    (void)ctx;
    (void)ptr;
    (void)size_s;
    return NULL;
}

Test(vector, starts_empty)
{
    cr_assert_eq(enki_vector_len(fixture_vector), 0);
    cr_assert_eq(enki_vector_capacity(fixture_vector), 0);
    cr_assert_null(enki_vector_pop(fixture_vector));
}

Test(vector, pushes_gets_sets_and_pops)
{
    uintptr_t one = (uintptr_t)0x1u;
    uintptr_t two = (uintptr_t)0x2u;
    uintptr_t three = (uintptr_t)0x3u;

    cr_assert_eq(enki_vector_push(fixture_vector, (void*)one), ENKI_OK);
    cr_assert_eq(enki_vector_push(fixture_vector, (void*)two), ENKI_OK);
    cr_assert_eq(enki_vector_len(fixture_vector), 2);
    cr_assert_eq(enki_vector_get(fixture_vector, 0), (void*)one);
    cr_assert_eq(enki_vector_set(fixture_vector, 1, (void*)three), ENKI_OK);
    cr_assert_eq(enki_vector_get(fixture_vector, 1), (void*)three);
    cr_assert_eq(enki_vector_pop(fixture_vector), (void*)three);
    cr_assert_eq(enki_vector_pop(fixture_vector), (void*)one);
    cr_assert_null(enki_vector_pop(fixture_vector));
}

Test(vector, rejects_invalid_arguments)
{
    cr_assert_null(enki_vector_create((enki_allocator){0}));
    cr_assert_eq(enki_vector_push(NULL, NULL), ENKI_ERR_INVALID);
    cr_assert_eq(enki_vector_set(NULL, 0, NULL), ENKI_ERR_INVALID);
    cr_assert_eq(enki_vector_reserve(NULL, 1), ENKI_ERR_INVALID);
    cr_assert_eq(enki_vector_shrink(NULL), ENKI_ERR_INVALID);
    cr_assert_eq(enki_vector_set(fixture_vector, 0, NULL), ENKI_ERR_BOUNDS);
    cr_assert_null(enki_vector_get(fixture_vector, 0));
}

Test(vector, allocator_alloc_failure_is_reported_on_create_and_growth)
{
    reset_allocator_fakes();
    fake_alloc_fake.return_val = NULL;

    cr_assert_null(enki_vector_create(fake_allocator()));
    cr_assert_eq(fake_alloc_fake.call_count, 1);

    reset_allocator_fakes();
    fake_alloc_fake.custom_fake = fail_second_alloc;
    fake_free_fake.custom_fake = free_fake;

    enki_vector* vector = enki_vector_create(fake_allocator());
    cr_assert_not_null(vector);
    cr_assert_eq(enki_vector_push(vector, (void*)(uintptr_t)1u), ENKI_ERR_ALLOC);
    cr_assert_eq(enki_vector_len(vector), 0);

    enki_vector_destroy(vector);
    cr_assert_geq(fake_free_fake.call_count, 1);
}

Test(vector, allocator_realloc_failure_keeps_existing_storage)
{
    reset_allocator_fakes();
    fake_alloc_fake.custom_fake = malloc_fake;
    fake_realloc_fake.custom_fake = realloc_fake;
    fake_free_fake.custom_fake = free_fake;

    enki_vector* vector = enki_vector_create(fake_allocator());
    cr_assert_not_null(vector);
    cr_assert_eq(enki_vector_reserve(vector, 8), ENKI_OK);
    cr_assert_eq(enki_vector_push(vector, (void*)(uintptr_t)1u), ENKI_OK);

    size_t capacity_s = enki_vector_capacity(vector);
    fake_realloc_fake.custom_fake = always_fail_realloc;

    cr_assert_eq(enki_vector_shrink(vector), ENKI_ERR_ALLOC);
    cr_assert_eq(enki_vector_len(vector), 1);
    cr_assert_eq(enki_vector_capacity(vector), capacity_s);
    cr_assert_eq(enki_vector_get(vector, 0), (void*)(uintptr_t)1u);

    fake_realloc_fake.custom_fake = realloc_fake;
    enki_vector_destroy(vector);
}

typedef struct reserve_case {
    size_t requested;
    size_t expected;
} reserve_case;

ParameterizedTestParameters(vector, reserve_tracks_capacity)
{
    static reserve_case cases[] = {
        {.requested = 0, .expected = 0},
        {.requested = 1, .expected = 1},
        {.requested = 8, .expected = 8},
        {.requested = 32, .expected = 32},
    };

    return cr_make_param_array(reserve_case, cases, sizeof(cases) / sizeof(cases[0]));
}

ParameterizedTest(reserve_case* param, vector, reserve_tracks_capacity)
{
    enki_vector* vector = enki_vector_create(enki_allocator_system());
    cr_assert_not_null(vector);
    cr_assert_eq(enki_vector_reserve(vector, param->requested), ENKI_OK);
    cr_assert_geq(enki_vector_capacity(vector), param->expected);
    cr_assert_eq(enki_vector_len(vector), 0);
    enki_vector_destroy(vector);
}

TheoryDataPoints(vector, set_get_roundtrip) = {
    DataPoints(uintptr_t, (uintptr_t)0u, (uintptr_t)1u, (uintptr_t)255u, UINTPTR_MAX),
};

Theory((uintptr_t raw), vector, set_get_roundtrip)
{
    enki_vector* vector = enki_vector_create(enki_allocator_system());
    cr_assert_not_null(vector);
    cr_assert_eq(enki_vector_push(vector, NULL), ENKI_OK);
    cr_assert_eq(enki_vector_set(vector, 0, (void*)raw), ENKI_OK);
    cr_assert_eq(enki_vector_get(vector, 0), (void*)raw);
    enki_vector_destroy(vector);
}
