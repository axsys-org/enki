#include "enki/allocator.h"
#include "enki/apply.h"
#include "enki/interp.h"
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

static void run_law(uint8_t* bc_b, size_t bc_len_s, enki_value* consts_v, size_t n_const_s)
{
    enki_value law = enki_alloc_law(
        fixture_interp->gc, 0, 0, 0, bc_len_s, n_const_s, bc_b, consts_v);

    fixture_interp->fp = 0;
    enki_load_frame(fixture_interp, (enki_law*)ENKI_TO_PTR(law));

    enki_run(fixture_interp);
}

TestSuite(interp_bytecode, .init = setup, .fini = teardown);

Test(interp_bytecode, runs_multiple_op66_ops_in_one_law)
{
    uint8_t bc_b[] = {
        OP_PUSH_CONST, 0,
        OP_PUSH_CONST, 1,
        OP_OP66, OP66_ADD,
        OP_PUSH_CONST, 2,
        OP_OP66, OP66_MUL,
        OP_RETURN,
    };
    enki_value consts_v[] = {2, 3, 4};

    fixture_interp->sp = 0;
    run_law(bc_b, sizeof(bc_b), consts_v, 3);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 20);
}

Test(interp_bytecode, dup_and_pop_leave_expected_stack)
{
    uint8_t bc_b[] = {
        OP_PUSH_CONST, 0,
        OP_DUP,
        OP_PUSH_CONST, 1,
        OP_POP,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    enki_value consts_v[] = {7, 99};

    fixture_interp->sp = 0;
    run_law(bc_b, sizeof(bc_b), consts_v, 2);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 14);
}

Test(interp_bytecode, pick_zero_reads_self)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    enki_value law = enki_alloc_law(
        fixture_interp->gc, 0, 0, 0, sizeof(bc_b), 0, bc_b, NULL);

    fixture_interp->stack_v[0] = law;
    fixture_interp->sp = 1;
    fixture_interp->fp = 0;
    fixture_interp->halted = false;

    enki_apply(fixture_interp, 0);
    while (fixture_interp->law != NULL && !fixture_interp->halted) {
        enki_step(fixture_interp);
    }

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], law);
}

Test(interp_bytecode, pick_one_reads_first_arg)
{
    uint8_t bc_b[] = {
        OP_PICK, 1,
        OP_RETURN,
    };
    enki_value law = enki_alloc_law(
        fixture_interp->gc, 1, 0, 0, sizeof(bc_b), 0, bc_b, NULL);

    fixture_interp->stack_v[0] = law;
    fixture_interp->stack_v[1] = 1234;
    fixture_interp->sp = 2;
    fixture_interp->fp = 0;
    fixture_interp->halted = false;

    enki_apply(fixture_interp, 1);
    while (fixture_interp->law != NULL && !fixture_interp->halted) {
        enki_step(fixture_interp);
    }

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 1234);
}

Test(interp_bytecode, op_apply_calls_law_from_bytecode)
{
    uint8_t inc_bc[] = {
        OP_PICK, 1,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value inc = enki_alloc_law(
        fixture_interp->gc, 1, 0, 0, sizeof(inc_bc), 0, inc_bc, NULL);
    uint8_t bc_b[] = {
        OP_PUSH_CONST, 0,
        OP_PUSH_CONST, 1,
        OP_APPLY, 1,
        OP_RETURN,
    };
    enki_value consts_v[] = {inc, 41};

    fixture_interp->sp = 0;
    run_law(bc_b, sizeof(bc_b), consts_v, 2);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 42);
}
