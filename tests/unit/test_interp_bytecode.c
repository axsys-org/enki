#include "enki/allocator.h"
#include "test_interp.h"
#include "enki/interp.h"
#include "enki/law.h"
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

static void run_law(uint8_t* bc_b, size_t bc_len_s, enki_value* consts_v, size_t n_const_s)
{
    enki_value law = enki_law_alloc(
        fixture_interp->gc, 0, 0, 0, bc_len_s, n_const_s, bc_b, consts_v);

    fixture_interp->stack_v[0] = law;
    fixture_interp->sp = 1;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_law_enter(0, law, fixture_interp);
    enki_interp_run(fixture_interp);
}

static void run_law_with_args(uint8_t* bc_b, size_t bc_len_s, enki_value* consts_v,
    size_t n_const_s, size_t arity_s, enki_value* args_v)
{
    enki_value law = enki_law_alloc(
        fixture_interp->gc, arity_s, 0, 0, bc_len_s, n_const_s, bc_b, consts_v);

    fixture_interp->stack_v[0] = law;
    for(size_t k = 0; k < arity_s; k++) {
        fixture_interp->stack_v[k + 1] = args_v[k];
    }
    fixture_interp->sp = arity_s + 1;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_law_enter(arity_s, law, fixture_interp);
    enki_interp_run(fixture_interp);
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

Test(interp_bytecode, pick_reads_from_arg_base)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_RETURN,
    };

    enki_value args_v[] = {1234};
    run_law_with_args(bc_b, sizeof(bc_b), NULL, 0, 1, args_v);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 1234);
}

Test(interp_bytecode, op_apply_calls_law_from_bytecode)
{
    uint8_t inc_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value inc = enki_law_alloc(
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

Test(interp_bytecode, push_const_wide_reads_two_byte_const_index)
{
    uint8_t bc_b[] = {
        OP_PUSH_CONST_WIDE, 0x00, 0x01,
        OP_RETURN,
    };
    enki_value consts_v[257];
    for(size_t k = 0; k < 257; k++) {
        consts_v[k] = (enki_value)k;
    }
    consts_v[256] = 4242;

    fixture_interp->sp = 0;
    run_law(bc_b, sizeof(bc_b), consts_v, 257);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 4242);
}

Test(interp_bytecode, pick_wide_reads_two_byte_stack_index)
{
    uint8_t bc_b[] = {
        OP_PICK_WIDE, 0x2c, 0x01,
        OP_RETURN,
    };

    enki_value args_v[301] = {0};
    args_v[300] = 9001;
    run_law_with_args(bc_b, sizeof(bc_b), NULL, 0, 301, args_v);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 9001);
}

Test(interp_bytecode, apply_wide_applies_more_than_255_args)
{
    uint8_t id_bc[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    enki_value id = enki_law_alloc(
        fixture_interp->gc, 256, 0, 0, sizeof(id_bc), 0, id_bc, NULL);
    enki_value consts_v[257];
    consts_v[0] = id;
    for(size_t k = 1; k < 257; k++) {
        consts_v[k] = (enki_value)(1000 + k - 1);
    }

    uint8_t bc_b[3 + (255 * 2) + 3 + 3 + 1];
    size_t pc = 0;
    bc_b[pc++] = OP_PUSH_CONST;
    bc_b[pc++] = 0;
    for(size_t k = 1; k < 256; k++) {
        bc_b[pc++] = OP_PUSH_CONST;
        bc_b[pc++] = (uint8_t)k;
    }
    bc_b[pc++] = OP_PUSH_CONST_WIDE;
    bc_b[pc++] = 0x00;
    bc_b[pc++] = 0x01;
    bc_b[pc++] = OP_APPLY_WIDE;
    bc_b[pc++] = 0x00;
    bc_b[pc++] = 0x01;
    bc_b[pc++] = OP_RETURN;

    fixture_interp->sp = 0;
    run_law(bc_b, pc, consts_v, 257);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 1000);
}
