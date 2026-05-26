#include "enki/allocator.h"
#include "enki/app.h"
#include "enki/gc.h"
#include "enki/law.h"
#include "enki/nat.h"
#include "enki/pin.h"
#include "enki/print.h"
#include "enki/value.h"

#include <criterion/criterion.h>
#include <stdint.h>
#include <string.h>

static const enki_allocator* fixture_allocator;
static enki_gc* fixture_gc;

static void setup(void)
{
    fixture_allocator = enki_allocator_system();
    fixture_gc = enki_gc_create(fixture_allocator, 1024 * 1024, NULL);
    cr_assert_not_null(fixture_gc);
}

static void teardown(void)
{
    enki_gc_destroy(fixture_gc);
    fixture_gc = NULL;
}

static void assert_prints(enki_value value_v, const char* expected_c)
{
    size_t expected_s = strlen(expected_c);
    size_t out_s = 0;
    char* out_c = enki_print_value(fixture_allocator, value_v, &out_s);

    cr_assert_not_null(out_c);
    fprintf(stderr, "want: %s have %s \n", expected_c, out_c);
    if ( out_s != expected_s ) {
      fprintf(stderr, "len: %lu have %lu \n", expected_s, out_s);
    }
    cr_assert_eq(out_s, expected_s);
    cr_assert_eq(memcmp(out_c, expected_c, expected_s), 0);
    cr_assert_eq(out_c[out_s], '\0');

    fixture_allocator->free(fixture_allocator->ctx, out_c);
}

static enki_value make_app(enki_value fn_v, size_t n_args_s, const enki_value* args_v)
{
    enki_value app_v = enki_app_alloc(fixture_gc, fn_v, n_args_s);
    enki_app* app = (enki_app*)ENKI_TO_PTR(app_v);

    for (size_t i = 0; i < n_args_s; i++) {
        app->args_v[i] = args_v[i];
    }

    return app_v;
}

static enki_value make_law(size_t arity_s, enki_value name_v, enki_value body_v)
{
    return enki_law_alloc(fixture_gc, arity_s, name_v, body_v, 0, 0, NULL, NULL);
}

static enki_value make_big_nat_bytes(const char* bytes_c, size_t bytes_s)
{
    mp_limb_t limbs[4] = {0};
    size_t n_limbs_s = (bytes_s + sizeof(mp_limb_t) - 1u) / sizeof(mp_limb_t);

    cr_assert_gt(bytes_s, 0);
    cr_assert_leq(n_limbs_s, sizeof(limbs) / sizeof(limbs[0]));

    memcpy(limbs, bytes_c, bytes_s);
    return enki_nat_alloc_big(fixture_gc, n_limbs_s, limbs);
}

TestSuite(print, .init = setup, .fini = teardown);

Test(print, immediate_nats_print_printable_bytes_as_quoted_text)
{
    assert_prints(0, "0");
    assert_prints(42, "42");
    assert_prints((UINT64_C(1) << 63) - 1u, "9223372036854775807");
}

Test(print, printable_big_nat_bytes_print_as_quoted_text)
{
    assert_prints(make_big_nat_bytes("hello", strlen("hello")), "\"hello\"");
    assert_prints(make_big_nat_bytes("ABCDEFGHIJKLMNOP", strlen("ABCDEFGHIJKLMNOP")),
                  "\"ABCDEFGHIJKLMNOP\"");
}

Test(print, non_printable_big_nat_prints_as_decimal_text)
{
    mp_limb_t limbs[] = {(mp_limb_t)(UINT64_C(1) << 63)};
    enki_value value_v = enki_nat_alloc_big(fixture_gc, 1, limbs);

    assert_prints(value_v, "9223372036854775808");
}

Test(print, app_prints_function_and_arguments_in_parentheses)
{
    enki_value no_args_v = make_app(7, 0, NULL);
    enki_value args_v[] = {2, 3, 4};
    enki_value app_v = make_app(1, 3, args_v);

    assert_prints(no_args_v, "(7)");
    assert_prints(app_v, "(1 2 3 4)");
}

Test(print, nested_apps_recurse)
{
    enki_value inner_args_v[] = {9};
    enki_value inner_v = make_app(8, 1, inner_args_v);
    enki_value outer_args_v[] = {inner_v};
    enki_value outer_v = make_app(7, 1, outer_args_v);

    assert_prints(outer_v, "(7 (8 9))");
}

Test(print, pins_wrap_printed_inner_value_in_angle_brackets)
{
    uint8_t hash_b[32] = {0};
    enki_value pin_v = enki_pin_alloc(fixture_gc, hash_b, 42, 0, NULL);
    enki_value nested_pin_v = enki_pin_alloc(fixture_gc, hash_b, pin_v, 0, NULL);

    assert_prints(pin_v, "<42>");
    assert_prints(nested_pin_v, "<<42>>");
}

Test(print, law_prints_zero_arity_signature_and_body)
{
    enki_value law_v = make_law(0, 7, 42);

    assert_prints(law_v, "{7 (self)\n#(42#)}");
}

Test(print, law_prints_argument_names_in_signature)
{
    enki_value law_v = make_law(3, 7, 42);

    assert_prints(law_v, "{7 (self a b c)\n#(42#)}");
}

Test(print, unknown_heap_objects_print_placeholder)
{
    enki_value no_args_v = 0;
    enki_value cont_v = enki_app_cont_alloc(fixture_gc, 0, &no_args_v);

    assert_prints(cont_v, "<<>>");
}
