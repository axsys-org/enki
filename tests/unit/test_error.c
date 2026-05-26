#include "test_interp.h"
#include "enki/error.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/op66.h"
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

static int run_law(uint8_t* bc_b, size_t bc_len_s, enki_value* consts_v, size_t n_const_s)
{
    enki_value law = enki_law_alloc(
        fixture_interp->gc, 0, 0, 0, bc_len_s, n_const_s, bc_b, consts_v);
    fixture_interp->frame[0].law = law;
    fixture_interp->frame[0].pc = 0;
    fixture_interp->frame[0].res_base_s = 0;
    fixture_interp->frame[0].arg_base_s = 0;
    fixture_interp->frame[0].cont_v = 0;
    fixture_interp->sp = 0;
    fixture_interp->fp = 0;
    fixture_interp->halted = false;

    return enki_interp_run(fixture_interp);
}

TestSuite(error, .init = setup, .fini = teardown);

Test(error, interp_run_catches_divide_by_zero)
{
    uint8_t bc_b[] = {
        OP_PUSH_CONST, 0,
        OP_PUSH_CONST, 1,
        OP_OP66, OP66_DIV,
        OP_RETURN,
    };
    enki_value consts_v[] = {10, 0};

    cr_assert_eq(run_law(bc_b, sizeof(bc_b), consts_v, 2), 1);
    cr_assert_eq(fixture_interp->error_code, ENKI_ERROR_DIV_ZERO);
}

Test(error, interp_run_catches_stack_underflow)
{
    uint8_t bc_b[] = {
        OP_DUP,
        OP_RETURN,
    };

    cr_assert_eq(run_law(bc_b, sizeof(bc_b), NULL, 0), 1);
    cr_assert_eq(fixture_interp->error_code, ENKI_ERROR_BOUNDS);
}

Test(error, interp_run_catches_bad_opcode)
{
    uint8_t bc_b[] = {
        0x7F,
        OP_RETURN,
    };

    cr_assert_eq(run_law(bc_b, sizeof(bc_b), NULL, 0), 1);
    cr_assert_eq(fixture_interp->error_code, ENKI_ERROR_BAD_TAG);
}

Test(error, interp_run_resets_scratch_after_throw)
{
    uint8_t bc_b[] = {
        OP_PUSH_CONST, 0,
        OP_PUSH_CONST, 1,
        OP_OP66, OP66_DIV,
        OP_RETURN,
    };
    enki_value consts_v[] = {10, 0};
    (void)enki_arena_alloc(fixture_interp->scratch_a, 128);

    cr_assert_gt(fixture_interp->scratch_a->off_o, sizeof(enki_arena));
    cr_assert_eq(run_law(bc_b, sizeof(bc_b), consts_v, 2), 1);
    cr_assert_eq(fixture_interp->scratch_a->off_o, sizeof(enki_arena));
}
