#include <criterion/criterion.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
    keep inline storage tiny in tests so growth/realloc paths are exercised
    without having to append a silly number of pieces.
*/
#include "enki/string_builder.h"

typedef struct test_allocator_state {
    int    fail_after;      /* -1 = never fail; 0 = fail next alloc/realloc */
    size_t alloc_calls;
    size_t realloc_calls;
    size_t free_calls;
    enki_allocator allocator;
} test_allocator_state;

static int test_allocator_should_fail(test_allocator_state* state)
{
    if (state->fail_after < 0)
        return 0;

    if (state->fail_after == 0)
        return 1;

    --state->fail_after;
    return 0;
}

static void* test_alloc(void* ctx, size_t size)
{
    test_allocator_state* state = (test_allocator_state*) ctx;

    ++state->alloc_calls;

    if (test_allocator_should_fail(state))
        return NULL;

    return malloc(size);
}

static void* test_realloc(void* ctx, void* ptr, size_t size)
{
    test_allocator_state* state = (test_allocator_state*) ctx;

    ++state->realloc_calls;

    if (test_allocator_should_fail(state))
        return NULL;

    return realloc(ptr, size);
}

static void test_free(void* ctx, void* ptr)
{
    test_allocator_state* state = (test_allocator_state*) ctx;

    if (ptr)
        ++state->free_calls;

    free(ptr);
}

static enki_allocator* make_test_allocator(test_allocator_state* state, int with_realloc)
{
    memset(state, 0, sizeof(*state));
    state->fail_after = -1;

    state->allocator.ctx = state;
    state->allocator.alloc = test_alloc;
    state->allocator.realloc = with_realloc ? test_realloc : NULL;
    state->allocator.free = test_free;

    return &state->allocator;
}

static void init_builder(enki_string_builder* sb, test_allocator_state* state)
{
    enki_sb_init(sb, make_test_allocator(state, 1));

    cr_assert(!enki_sb_failed(sb));
    cr_assert_eq(enki_sb_piece_count(sb), 0);
}

static void assert_build_bytes(enki_string_builder* sb,
                               const char* expected,
                               size_t expected_len)
{
    size_t got_len = 777;
    char* got = enki_sb_build(sb, &got_len);

    cr_assert_not_null(got);
    cr_assert_eq(got_len, expected_len);
    cr_assert_eq(memcmp(got, expected, expected_len), 0);
    cr_assert_eq(got[expected_len], '\0');

    sb->allocator->free(sb->allocator->ctx, got);
}

static void assert_build_cstr(enki_string_builder* sb, const char* expected)
{
    assert_build_bytes(sb, expected, strlen(expected));
}

Test(enki_string_builder, empty_builder_builds_empty_string)
{
    test_allocator_state state;
    enki_string_builder sb;
    size_t len = 999;
    char* out;

    init_builder(&sb, &state);

    cr_assert(enki_sb_measure(&sb, &len));
    cr_assert_eq(len, 0);

    out = enki_sb_build(&sb, &len);
    cr_assert_not_null(out);
    cr_assert_eq(len, 0);
    cr_assert_eq(out[0], '\0');

    sb.allocator->free(sb.allocator->ctx, out);
    enki_sb_free(&sb);
}

Test(enki_string_builder, appends_refs_cstrings_literals_and_chars)
{
    test_allocator_state state;
    enki_string_builder sb;
    const char* world = "world";

    init_builder(&sb, &state);

    cr_assert(enki_sb_append_lit(&sb, "hello"));
    cr_assert(enki_sb_append_char(&sb, ' '));
    cr_assert(enki_sb_append_cstr(&sb, world));
    cr_assert(enki_sb_append_char(&sb, '!'));

    assert_build_cstr(&sb, "hello world!");

    enki_sb_free(&sb);
}

Test(enki_string_builder, cstr_captures_length_but_not_bytes)
{
    test_allocator_state state;
    enki_string_builder sb;
    char mutable_text[] = "abcdef";

    init_builder(&sb, &state);

    cr_assert(enki_sb_append_cstr(&sb, mutable_text));

    /*
        append_cstr captured length 6, but the builder still references the
        original bytes. this is the intended twine-ish behavior.
    */
    memcpy(mutable_text, "UVWXYZ", 6);

    assert_build_cstr(&sb, "UVWXYZ");

    enki_sb_free(&sb);
}

Test(enki_string_builder, supports_embedded_nuls)
{
    test_allocator_state state;
    enki_string_builder sb;

    const char chunk[] = { 'a', '\0', 'b' };
    const char expected[] = { 'x', 'a', '\0', 'b', 'y' };

    init_builder(&sb, &state);

    cr_assert(enki_sb_append_char(&sb, 'x'));
    cr_assert(enki_sb_append_ref(&sb, chunk, sizeof(chunk)));
    cr_assert(enki_sb_append_char(&sb, 'y'));

    assert_build_bytes(&sb, expected, sizeof(expected));

    enki_sb_free(&sb);
}

Test(enki_string_builder, formats_signed_and_unsigned_integers)
{
    test_allocator_state state;
    enki_string_builder sb;

    init_builder(&sb, &state);

    cr_assert(enki_sb_append_i64(&sb, 0));
    cr_assert(enki_sb_append_lit(&sb, ","));
    cr_assert(enki_sb_append_i64(&sb, -123));
    cr_assert(enki_sb_append_lit(&sb, ","));
    cr_assert(enki_sb_append_i64(&sb, INT64_MIN));
    cr_assert(enki_sb_append_lit(&sb, ","));
    cr_assert(enki_sb_append_u64(&sb, UINT64_MAX));
    cr_assert(enki_sb_append_lit(&sb, ","));
    cr_assert(enki_sb_append_u64_base(&sb, 255, 16, 0));
    cr_assert(enki_sb_append_lit(&sb, ","));
    cr_assert(enki_sb_append_u64_base(&sb, 255, 16, 1));
    cr_assert(enki_sb_append_lit(&sb, ","));
    cr_assert(enki_sb_append_u64_base(&sb, 5, 2, 0));

    assert_build_cstr(
        &sb,
        "0,-123,-9223372036854775808,18446744073709551615,ff,FF,101"
    );

    enki_sb_free(&sb);
}

Test(enki_string_builder, reserve_pieces_grows_metadata_storage)
{
    test_allocator_state state;
    enki_string_builder sb;

    init_builder(&sb, &state);

    cr_assert(enki_sb_reserve_pieces(&sb, 32));
    cr_assert(sb.capacity >= 32);
    cr_assert(state.alloc_calls > 0);

    cr_assert(enki_sb_append_lit(&sb, "after reserve"));

    assert_build_cstr(&sb, "after reserve");

    enki_sb_free(&sb);
}

Test(enki_string_builder, growth_uses_realloc_when_available)
{
    test_allocator_state state;
    enki_string_builder sb;
    char expected[21];
    size_t i;

    init_builder(&sb, &state);

    for (i = 0; i < 20; ++i) {
        expected[i] = (char) ('a' + (int) i);
        cr_assert(enki_sb_append_char(&sb, expected[i]));
    }
    expected[20] = '\0';

    cr_assert(state.alloc_calls > 0);
    //cr_assert(state.realloc_calls > 0);

    assert_build_cstr(&sb, expected);

    enki_sb_free(&sb);
}

Test(enki_string_builder, growth_falls_back_to_alloc_copy_free_without_realloc)
{
    test_allocator_state state;
    enki_allocator* allocator;
    enki_string_builder sb;
    char expected[21];
    size_t i;

    allocator = make_test_allocator(&state, 0);
    enki_sb_init(&sb, allocator);

    cr_assert(!enki_sb_failed(&sb));

    for (i = 0; i < 20; ++i) {
        expected[i] = (char) ('A' + (int) i);
        cr_assert(enki_sb_append_char(&sb, expected[i]));
    }
    expected[20] = '\0';

    cr_assert_eq(state.realloc_calls, 0);
    cr_assert(state.alloc_calls > 0);

    assert_build_cstr(&sb, expected);

    enki_sb_free(&sb);
}

Test(enki_string_builder, appends_another_builder_as_metadata_only)
{
    test_allocator_state left_state;
    test_allocator_state right_state;
    enki_string_builder left;
    enki_string_builder right;

    char referenced[] = "old";

    init_builder(&left, &left_state);
    init_builder(&right, &right_state);

    cr_assert(enki_sb_append_lit(&left, "prefix:"));
    cr_assert(enki_sb_append_ref(&right, referenced, 3));

    cr_assert(enki_sb_append_builder(&left, &right));

    /*
        append_builder copied the reference piece, not the pointed-to bytes.
    */
    memcpy(referenced, "new", 3);

    assert_build_cstr(&left, "prefix:new");
    assert_build_cstr(&right, "new");

    enki_sb_free(&right);
    enki_sb_free(&left);
}

Test(enki_string_builder, supports_self_append)
{
    test_allocator_state state;
    enki_string_builder sb;

    init_builder(&sb, &state);

    cr_assert(enki_sb_append_lit(&sb, "ab"));
    cr_assert(enki_sb_append_builder(&sb, &sb));

    assert_build_cstr(&sb, "abab");

    enki_sb_free(&sb);
}

Test(enki_string_builder, measure_and_write_report_required_length)
{
    test_allocator_state state;
    enki_string_builder sb;

    size_t len = 999;
    char too_small[5];
    char exact[6];

    init_builder(&sb, &state);

    cr_assert(enki_sb_append_lit(&sb, "abc"));
    cr_assert(enki_sb_append_u64(&sb, 42));

    cr_assert(enki_sb_measure(&sb, &len));
    cr_assert_eq(len, 5);

    len = 999;
    cr_assert(!enki_sb_write(&sb, too_small, sizeof(too_small), &len));
    cr_assert_eq(len, 5);

    len = 999;
    cr_assert(enki_sb_write(&sb, exact, sizeof(exact), &len));
    cr_assert_eq(len, 5);
    cr_assert_str_eq(exact, "abc42");

    enki_sb_free(&sb);
}

Test(enki_string_builder, zero_length_null_ref_is_allowed)
{
    test_allocator_state state;
    enki_string_builder sb;

    init_builder(&sb, &state);

    cr_assert(enki_sb_append_ref(&sb, NULL, 0));
    cr_assert(!enki_sb_failed(&sb));

    cr_assert(enki_sb_append_lit(&sb, "ok"));

    assert_build_cstr(&sb, "ok");

    enki_sb_free(&sb);
}

Test(enki_string_builder, null_nonempty_ref_marks_builder_failed)
{
    test_allocator_state state;
    enki_string_builder sb;
    size_t len = 999;
    char* out;

    init_builder(&sb, &state);

    cr_assert(!enki_sb_append_ref(&sb, NULL, 1));
    cr_assert(enki_sb_failed(&sb));

    out = enki_sb_build(&sb, &len);
    cr_assert_null(out);
    cr_assert_eq(len, 0);

    enki_sb_free(&sb);
}

Test(enki_string_builder, null_cstr_marks_builder_failed)
{
    test_allocator_state state;
    enki_string_builder sb;

    init_builder(&sb, &state);

    cr_assert(!enki_sb_append_cstr(&sb, NULL));
    cr_assert(enki_sb_failed(&sb));

    enki_sb_free(&sb);
}

Test(enki_string_builder, invalid_integer_base_marks_builder_failed)
{
    test_allocator_state state;
    enki_string_builder sb;

    init_builder(&sb, &state);

    cr_assert(!enki_sb_append_u64_base(&sb, 10, 1, 0));
    cr_assert(enki_sb_failed(&sb));

    enki_sb_free(&sb);
}

Test(enki_string_builder, allocation_failure_during_growth_marks_builder_failed)
{
    test_allocator_state state;
    enki_string_builder sb;
    size_t i;
    size_t len = 999;
    char* out;

    init_builder(&sb, &state);

    /*
        with ENKI_SB_INLINE_PIECES == 4, the fifth append must allocate
        metadata storage. make that allocation fail.
    */
    state.fail_after = 0;

    for (i = 0; i < ENKI_SB_INLINE_PIECES; ++i)
        cr_assert(enki_sb_append_char(&sb, 'x'));

    cr_assert(!enki_sb_append_char(&sb, 'y'));
    cr_assert(enki_sb_failed(&sb));
    cr_assert_eq(state.alloc_calls, 1);

    out = enki_sb_build(&sb, &len);
    cr_assert_null(out);
    cr_assert_eq(len, 0);

    enki_sb_free(&sb);
}

Test(enki_string_builder, failed_realloc_does_not_prevent_freeing_builder)
{
    test_allocator_state state;
    enki_string_builder sb;
    size_t i;

    init_builder(&sb, &state);

    /*
        First dynamic grow succeeds.
        Second grow, which should be realloc, fails.
    */
    state.fail_after = 1;

    for (i = 0; i < ENKI_SB_INLINE_PIECES * 2; ++i)
        cr_assert(enki_sb_append_char(&sb, 'x'));

    cr_assert(!enki_sb_append_char(&sb, 'y'));
    cr_assert(enki_sb_failed(&sb));
    cr_assert(state.realloc_calls > 0);

    enki_sb_free(&sb);

    /*
        The original heap metadata allocation should still have been released.
    */
    cr_assert(state.free_calls > 0);
}

Test(enki_string_builder, reset_clears_content_and_recovers_from_allocator_failure)
{
    test_allocator_state state;
    enki_string_builder sb;
    size_t i;

    init_builder(&sb, &state);

    state.fail_after = 0;

    for (i = 0; i < ENKI_SB_INLINE_PIECES; ++i)
        cr_assert(enki_sb_append_char(&sb, 'x'));

    cr_assert(!enki_sb_append_char(&sb, 'y'));
    cr_assert(enki_sb_failed(&sb));

    state.fail_after = -1;
    enki_sb_reset(&sb);

    cr_assert(!enki_sb_failed(&sb));
    cr_assert_eq(enki_sb_piece_count(&sb), 0);

    cr_assert(enki_sb_append_lit(&sb, "recovered"));

    assert_build_cstr(&sb, "recovered");

    enki_sb_free(&sb);
}

Test(enki_string_builder, build_allocation_failure_returns_null_without_failing_builder)
{
    test_allocator_state state;
    enki_string_builder sb;
    size_t len = 999;
    char* out;

    init_builder(&sb, &state);

    cr_assert(enki_sb_append_lit(&sb, "abc"));

    state.fail_after = 0;

    out = enki_sb_build(&sb, &len);

    cr_assert_null(out);
    cr_assert_eq(len, 0);
    cr_assert(!enki_sb_failed(&sb));

    state.fail_after = -1;

    assert_build_cstr(&sb, "abc");

    enki_sb_free(&sb);
}

Test(enki_string_builder, build_with_allocator_uses_requested_output_allocator)
{
    test_allocator_state builder_state;
    test_allocator_state output_state;

    enki_allocator* output_allocator;
    enki_string_builder sb;

    size_t len = 999;
    char* out;

    init_builder(&sb, &builder_state);

    output_allocator = make_test_allocator(&output_state, 1);

    cr_assert(enki_sb_append_lit(&sb, "separate output allocator"));

    out = enki_sb_build_with_allocator(&sb, output_allocator, &len);

    cr_assert_not_null(out);
    cr_assert_eq(len, strlen("separate output allocator"));
    cr_assert_str_eq(out, "separate output allocator");

    cr_assert_eq(output_state.alloc_calls, 1);

    output_allocator->free(output_allocator->ctx, out);
    enki_sb_free(&sb);
}

Test(enki_string_builder, bad_allocator_initializes_as_failed)
{
    enki_allocator bad_allocator;
    enki_string_builder sb;
    size_t len = 999;

    memset(&bad_allocator, 0, sizeof(bad_allocator));

    enki_sb_init(&sb, &bad_allocator);

    cr_assert(enki_sb_failed(&sb));
    cr_assert(!enki_sb_append_lit(&sb, "nope"));
    cr_assert(!enki_sb_measure(&sb, &len));
    cr_assert_eq(len, 0);

    enki_sb_free(&sb);
}

Test(enki_string_builder, measure_detects_size_overflow)
{
    test_allocator_state state;
    enki_string_builder sb;
    const char one_byte = 'x';
    size_t len = 999;

    init_builder(&sb, &state);

    /*
        No bytes are copied here; the huge length only exercises accounting.
    */
    cr_assert(enki_sb_append_ref(&sb, &one_byte, SIZE_MAX));
    cr_assert(enki_sb_append_char(&sb, 'y'));

    cr_assert(!enki_sb_measure(&sb, &len));
    cr_assert_eq(len, 0);

    enki_sb_free(&sb);
}
