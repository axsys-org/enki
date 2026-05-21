#include "enki/allocator.h"
#include "enki/interp.h"
#include "enki/value.h"

#include <criterion/criterion.h>

static enki_interpreter* fixture_interp;

static void setup(void)
{
    fixture_interp = enki_create_interp(enki_allocator_system(), 1024 * 1024, 0);
    cr_assert_not_null(fixture_interp);
}

static void teardown(void)
{
    enki_destroy(fixture_interp);
    fixture_interp = NULL;
}

TestSuite(nat, .init = setup, .fini = teardown);

Test(nat, small_arithmetic_roundtrips)
{
    cr_assert_eq(enki_nat_inc(fixture_interp->gc, 41), 42);
    cr_assert_eq(enki_nat_dec(fixture_interp->gc, 42), 41);
    cr_assert_eq(enki_nat_add(fixture_interp->gc, 2, 3), 5);
    cr_assert_eq(enki_nat_sub(fixture_interp->gc, 9, 4), 5);
    cr_assert_eq(enki_nat_mul(fixture_interp->gc, 6, 7), 42);
    cr_assert_eq(enki_nat_div(fixture_interp->gc, 20, 4), 5);
    cr_assert_eq(enki_nat_mod(fixture_interp->gc, 22, 5), 2);
}

Test(nat, comparisons_return_boolean_nats)
{
    cr_assert_eq(enki_nat_cmp(3, 3), 0);
    cr_assert_eq(enki_nat_cmp(2, 3), -1);
    cr_assert_eq(enki_nat_cmp(4, 3), 1);
    cr_assert_eq(enki_nat_eq(3, 3), 1);
    cr_assert_eq(enki_nat_ne(3, 4), 1);
    cr_assert_eq(enki_nat_lt(2, 3), 1);
    cr_assert_eq(enki_nat_le(3, 3), 1);
    cr_assert_eq(enki_nat_gt(4, 3), 1);
    cr_assert_eq(enki_nat_ge(3, 3), 1);
}

Test(nat, shifts_and_bits_handle_big_values)
{
    enki_value big = enki_nat_lsh(fixture_interp->gc, 1, 70);

    cr_assert(IS_PTR(big));
    cr_assert_eq(enki_nat_bits(fixture_interp->gc, big), 71);
    cr_assert_eq(enki_nat_test(fixture_interp->gc, 70, big), 1);
    cr_assert_eq(enki_nat_test(fixture_interp->gc, 69, big), 0);
    cr_assert_eq(enki_nat_rsh(fixture_interp->gc, big, 70), 1);
}

Test(nat, set_clear_test_modify_bits)
{
    enki_value x = enki_nat_set(fixture_interp->gc, 9, 0);
    cr_assert_eq(enki_nat_test(fixture_interp->gc, 9, x), 1);

    x = enki_nat_clear(fixture_interp->gc, 9, x);
    cr_assert_eq(enki_nat_test(fixture_interp->gc, 9, x), 0);
}

Test(nat, byte_and_trunc_ops)
{
    enki_value x = enki_nat_store8(fixture_interp->gc, 1, 0xAB, 0);

    cr_assert_eq(enki_nat_load8(fixture_interp->gc, 0, x), 0);
    cr_assert_eq(enki_nat_load8(fixture_interp->gc, 1, x), 0xAB);
    cr_assert_eq(enki_nat_trunc8(fixture_interp->gc, 0x123), 0x23);
    cr_assert_eq(enki_nat_nib(fixture_interp->gc, 0, 0xAB), 0xB);
    cr_assert_eq(enki_nat_nib(fixture_interp->gc, 1, 0xAB), 0xA);
}
