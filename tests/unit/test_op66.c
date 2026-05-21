#include "enki/allocator.h"
#include "enki/interp.h"
#include "enki/op66.h"
#include "enki/value.h"

#include <criterion/criterion.h>
#include <stdint.h>

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

TestSuite(op66, .init = setup, .fini = teardown);

Test(op66, direct_add_replaces_two_stack_values_with_sum)
{
    fixture_interp->stack[0] = 2;
    fixture_interp->stack[1] = 3;
    fixture_interp->sp = 2;

    op66_add(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack[0], 5);
}

Test(op66, direct_inc_and_dec_update_top_of_stack)
{
    fixture_interp->stack[0] = 41;
    fixture_interp->sp = 1;

    op66_inc(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 42);

    op66_dec(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 41);
}

Test(op66, direct_type_and_is_nat_treat_immediates_as_nats)
{
    fixture_interp->stack[0] = 123;
    fixture_interp->sp = 1;

    op66_type(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 0);

    fixture_interp->stack[0] = 123;
    op66_is_nat(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 1);
}

Test(op66, direct_rep_sz_and_ix_read_app_args)
{
    fixture_interp->stack[0] = 0;
    fixture_interp->stack[1] = 7;
    fixture_interp->stack[2] = 3;
    fixture_interp->sp = 3;

    op66_rep(fixture_interp);
    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert(IS_PTR(fixture_interp->stack[0]));

    enki_value app = fixture_interp->stack[0];
    fixture_interp->stack[0] = app;
    op66_sz(fixture_interp);
    cr_assert_eq(fixture_interp->stack[0], 3);

    fixture_interp->stack[0] = 1;
    fixture_interp->stack[1] = app;
    fixture_interp->sp = 2;
    op66_ix(fixture_interp);
    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack[0], 7);
}

Test(op66, bytecode_dispatch_runs_op66_add)
{
    uint8_t bc[] = {
        OP_PUSH_CONST, 0,
        OP_PUSH_CONST, 1,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    enki_value consts[] = {2, 3};
    enki_value law = enki_alloc_law(
        fixture_interp->gc, 0, 0, 0, sizeof(bc), 2, bc, consts);

    fixture_interp->frame[0].law = law;
    fixture_interp->halted = false;
    fixture_interp->sp = 0;
    fixture_interp->fp = 0;

    enki_run(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack[0], 5);
}
