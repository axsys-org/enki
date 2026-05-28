#include "enki/allocator.h"
#include "test_interp.h"
#include "enki/app.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/op66.h"
#include "enki/pin.h"
#include "enki/value.h"

#include <criterion/criterion.h>
#include <stdint.h>

static enki_interpreter* fixture_interp;

static void setup(void)
{
    fixture_interp = enki_test_interp_create(1024 * 1024, 0);
    cr_assert_not_null(fixture_interp);
}

static void teardown(void)
{
    enki_test_interp_destroy(fixture_interp);
    fixture_interp = NULL;
}

TestSuite(op66, .init = setup, .fini = teardown);

static enki_value app2(enki_value fn_v, enki_value a, enki_value b)
{
    enki_value value_v = enki_app_alloc(fixture_interp->gc, fn_v, 2);
    enki_app* app = ENKI_AS(enki_app, value_v);
    app->args_v[0] = a;
    app->args_v[1] = b;
    return value_v;
}

Test(op66, direct_add_replaces_two_stack_values_with_sum)
{
    fixture_interp->stack_v[0] = 2;
    fixture_interp->stack_v[1] = 3;
    fixture_interp->sp = 2;

    op66_add(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 5);
}

Test(op66, structural_eq_compares_small_and_big_nats)
{
    enki_value big_a = enki_nat_lsh(fixture_interp->gc, 1, 70);
    enki_value big_b = enki_nat_lsh(fixture_interp->gc, 1, 70);

    cr_assert_eq(op66_structural_eq(fixture_interp, 42, 42), 1);
    cr_assert_eq(op66_structural_eq(fixture_interp, 42, 43), 0);
    cr_assert_eq(op66_structural_eq(fixture_interp, big_a, big_b), 1);
    cr_assert_eq(op66_structural_eq(fixture_interp, big_a, 42), 0);
}

Test(op66, structural_eq_compares_apps_laws_and_pins)
{
    enki_value app_a = app2(99, 10, 20);
    enki_value app_b = app2(99, 10, 20);
    enki_value app_c = app2(99, 10, 21);

    cr_assert_eq(op66_structural_eq(fixture_interp, app_a, app_b), 1);
    cr_assert_eq(op66_structural_eq(fixture_interp, app_a, app_c), 0);

    uint8_t bc_b[] = { OP_RETURN };
    enki_value law_a = enki_law_alloc(fixture_interp->gc, 2, 11, 22, sizeof(bc_b), 0, bc_b, NULL);
    enki_value law_b = enki_law_alloc(fixture_interp->gc, 2, 11, 22, sizeof(bc_b), 0, bc_b, NULL);
    enki_value law_c = enki_law_alloc(fixture_interp->gc, 1, 11, 22, sizeof(bc_b), 0, bc_b, NULL);

    cr_assert_eq(op66_structural_eq(fixture_interp, law_a, law_b), 1);
    cr_assert_eq(op66_structural_eq(fixture_interp, law_a, law_c), 0);

    uint8_t hash_b[32] = {0};
    enki_value subpins_a[] = { 7 };
    enki_value subpins_b[] = { 7 };
    enki_value pin_a = enki_pin_alloc(fixture_interp->gc, hash_b, 42, 1, subpins_a);
    enki_value pin_b = enki_pin_alloc(fixture_interp->gc, hash_b, 42, 1, subpins_b);

    cr_assert_eq(op66_structural_eq(fixture_interp, pin_a, pin_b), 1);
}

Test(op66, equal_forces_before_structural_compare)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value inc = enki_law_alloc(fixture_interp->gc, 1, 0, 0, sizeof(bc_b), 0, bc_b, NULL);
    enki_value thunk_v = enki_app_alloc(fixture_interp->gc, inc, 1);
    enki_app* app = ENKI_AS(enki_app, thunk_v);
    app->h.state_b = THUNK;
    app->args_v[0] = 41;

    fixture_interp->stack_v[0] = thunk_v;
    fixture_interp->stack_v[1] = 42;
    fixture_interp->sp = 2;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    op66_equal(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 1);
}

Test(op66, eq_is_nat_equality_without_forcing)
{
    fixture_interp->stack_v[0] = 42;
    fixture_interp->stack_v[1] = 42;
    fixture_interp->sp = 2;

    op66_eq(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 1);
}

Test(op66, cmp_maps_ordering_to_plan_nat_shape)
{
    fixture_interp->stack_v[0] = 2;
    fixture_interp->stack_v[1] = 3;
    fixture_interp->sp = 2;
    op66_cmp(fixture_interp);
    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 0);

    fixture_interp->stack_v[0] = 3;
    fixture_interp->stack_v[1] = 3;
    fixture_interp->sp = 2;
    op66_cmp(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 1);

    fixture_interp->stack_v[0] = 4;
    fixture_interp->stack_v[1] = 3;
    fixture_interp->sp = 2;
    op66_cmp(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 2);
}

Test(op66, direct_inc_and_dec_update_top_of_stack)
{
    fixture_interp->stack_v[0] = 41;
    fixture_interp->sp = 1;

    op66_inc(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 42);

    op66_dec(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 41);
}

Test(op66, direct_type_and_is_nat_treat_immediates_as_nats)
{
    fixture_interp->stack_v[0] = 123;
    fixture_interp->sp = 1;

    op66_type(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 0);

    fixture_interp->stack_v[0] = 123;
    op66_is_nat(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 1);
}

Test(op66, direct_rep_sz_and_ix_read_app_args)
{
    fixture_interp->stack_v[0] = 0;
    fixture_interp->stack_v[1] = 7;
    fixture_interp->stack_v[2] = 3;
    fixture_interp->sp = 3;

    op66_rep(fixture_interp);
    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert(IS_PTR(fixture_interp->stack_v[0]));

    enki_value app = fixture_interp->stack_v[0];
    fixture_interp->stack_v[0] = app;
    op66_sz(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 3);

    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = app;
    fixture_interp->sp = 2;
    op66_ix(fixture_interp);
    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 7);
}

Test(op66, bytecode_dispatch_runs_op66_add)
{
    uint8_t bc_b[] = {
        OP_PUSH_CONST, 0,
        OP_PUSH_CONST, 1,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    enki_value consts_v[] = {2, 3};
    enki_value law = enki_law_alloc(
        fixture_interp->gc, 0, 0, 0, sizeof(bc_b), 2, bc_b, consts_v);

    fixture_interp->halted = false;
    fixture_interp->stack_v[0] = law;
    fixture_interp->sp = 1;
    fixture_interp->cp = 0;

    enki_law_enter(0, law, fixture_interp);
    enki_interp_run(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 5);
}
