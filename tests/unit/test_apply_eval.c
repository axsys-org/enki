#include "enki/allocator.h"
#include "test_interp.h"
#include "enki/app.h"
#include "enki/eval.h"
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

static enki_value make_law(size_t arity_s, uint8_t* bc_b, size_t bc_len_s)
{
    return enki_law_alloc(fixture_interp->gc, arity_s, 0, 0, bc_len_s, 0, bc_b, NULL);
}

static enki_value make_law_with_consts(size_t arity_s, uint8_t* bc_b, size_t bc_len_s, enki_value* consts_v, size_t n_const_s)
{
    return enki_law_alloc(fixture_interp->gc, arity_s, 0, 0, bc_len_s, n_const_s, bc_b, consts_v);
}

static void run_until_base_frame(void)
{
    while(fixture_interp->cp > 0 && !fixture_interp->halted) {
        enki_interp_step(fixture_interp);
    }
}

TestSuite(apply_eval, .init = setup, .fini = teardown);

Test(apply_eval, exact_law_application_runs_law_body)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_PICK, 1,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    enki_value law = make_law(2, bc_b, sizeof(bc_b));

    fixture_interp->stack_v[0] = law;
    fixture_interp->stack_v[1] = 20;
    fixture_interp->stack_v[2] = 22;
    fixture_interp->sp = 3;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_app_apply(fixture_interp, 2);
    run_until_base_frame();

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 42);
}

Test(apply_eval, under_application_returns_app)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_PICK, 1,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    enki_value law = make_law(2, bc_b, sizeof(bc_b));

    fixture_interp->stack_v[0] = law;
    fixture_interp->stack_v[1] = 10;
    fixture_interp->sp = 2;

    enki_app_apply(fixture_interp, 1);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert(IS_PTR(fixture_interp->stack_v[0]));
    enki_app* app = ENKI_AS(enki_app, fixture_interp->stack_v[0]);
    cr_assert_eq(app->fn_v, law);
    cr_assert_eq(app->n_args_s, 1);
    cr_assert_eq(app->args_v[0], 10);
}

Test(apply_eval, partial_app_can_be_completed_later)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_PICK, 1,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    enki_value law = make_law(2, bc_b, sizeof(bc_b));

    fixture_interp->stack_v[0] = law;
    fixture_interp->stack_v[1] = 20;
    fixture_interp->sp = 2;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_app_apply(fixture_interp, 1);

    fixture_interp->stack_v[1] = 22;
    fixture_interp->sp = 2;

    enki_app_apply(fixture_interp, 1);
    run_until_base_frame();

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 42);
}

Test(apply_eval, over_application_builds_flat_thunk)
{
    uint8_t id_bc[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    uint8_t inc_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value id = make_law(1, id_bc, sizeof(id_bc));
    enki_value inc = make_law(1, inc_bc, sizeof(inc_bc));

    fixture_interp->stack_v[0] = id;
    fixture_interp->stack_v[1] = inc;
    fixture_interp->stack_v[2] = 41;
    fixture_interp->sp = 3;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_app_apply(fixture_interp, 2);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert(IS_PTR(fixture_interp->stack_v[0]));
    enki_app* thunk = ENKI_AS(enki_app, fixture_interp->stack_v[0]);
    cr_assert_eq(thunk->h.state_b, THUNK);
    cr_assert_eq(thunk->fn_v, id);
    cr_assert_eq(thunk->n_args_s, 2);
    cr_assert_eq(thunk->args_v[0], inc);
    cr_assert_eq(thunk->args_v[1], 41);
    cr_assert_eq(enki_eval_whnf(fixture_interp, fixture_interp->stack_v[0]), 42);
}

Test(apply_eval, over_application_of_two_arity_law_builds_flat_thunk)
{
    uint8_t choose_bc[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    uint8_t inc_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value choose = make_law(2, choose_bc, sizeof(choose_bc));
    enki_value inc = make_law(1, inc_bc, sizeof(inc_bc));

    fixture_interp->stack_v[0] = choose;
    fixture_interp->stack_v[1] = inc;
    fixture_interp->stack_v[2] = 0;
    fixture_interp->stack_v[3] = 41;
    fixture_interp->sp = 4;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_app_apply(fixture_interp, 3);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert(IS_PTR(fixture_interp->stack_v[0]));
    enki_app* thunk = ENKI_AS(enki_app, fixture_interp->stack_v[0]);
    cr_assert_eq(thunk->h.state_b, THUNK);
    cr_assert_eq(thunk->fn_v, choose);
    cr_assert_eq(thunk->n_args_s, 3);
    cr_assert_eq(thunk->args_v[0], inc);
    cr_assert_eq(thunk->args_v[1], 0);
    cr_assert_eq(thunk->args_v[2], 41);
    cr_assert_eq(enki_eval_whnf(fixture_interp, fixture_interp->stack_v[0]), 42);
}

Test(apply_eval, over_application_of_partial_app_builds_flat_thunk)
{
    uint8_t choose_bc[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    uint8_t inc_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value choose = make_law(2, choose_bc, sizeof(choose_bc));
    enki_value inc = make_law(1, inc_bc, sizeof(inc_bc));
    enki_value partial_v = enki_app_alloc(fixture_interp->gc, choose, 1);
    enki_app* app = ENKI_AS(enki_app, partial_v);
    app->args_v[0] = inc;

    fixture_interp->stack_v[0] = partial_v;
    fixture_interp->stack_v[1] = 0;
    fixture_interp->stack_v[2] = 41;
    fixture_interp->sp = 3;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_app_apply(fixture_interp, 2);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert(IS_PTR(fixture_interp->stack_v[0]));
    enki_app* thunk = ENKI_AS(enki_app, fixture_interp->stack_v[0]);
    cr_assert_eq(thunk->h.state_b, THUNK);
    cr_assert_eq(thunk->fn_v, choose);
    cr_assert_eq(thunk->n_args_s, 3);
    cr_assert_eq(thunk->args_v[0], inc);
    cr_assert_eq(thunk->args_v[1], 0);
    cr_assert_eq(thunk->args_v[2], 41);
    cr_assert_eq(enki_eval_whnf(fixture_interp, fixture_interp->stack_v[0]), 42);
}

Test(apply_eval, flat_thunk_applies_multiple_extra_args_when_forced)
{
    uint8_t id_bc[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    uint8_t add_bc[] = {
        OP_PICK, 0,
        OP_PICK, 1,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    enki_value id = make_law(1, id_bc, sizeof(id_bc));
    enki_value add = make_law(2, add_bc, sizeof(add_bc));

    fixture_interp->stack_v[0] = id;
    fixture_interp->stack_v[1] = add;
    fixture_interp->stack_v[2] = 20;
    fixture_interp->stack_v[3] = 22;
    fixture_interp->sp = 4;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_app_apply(fixture_interp, 3);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert(IS_PTR(fixture_interp->stack_v[0]));
    enki_app* thunk = ENKI_AS(enki_app, fixture_interp->stack_v[0]);
    cr_assert_eq(thunk->h.state_b, THUNK);
    cr_assert_eq(thunk->fn_v, id);
    cr_assert_eq(thunk->n_args_s, 3);
    cr_assert_eq(thunk->args_v[0], add);
    cr_assert_eq(thunk->args_v[1], 20);
    cr_assert_eq(thunk->args_v[2], 22);
    cr_assert_eq(enki_eval_whnf(fixture_interp, fixture_interp->stack_v[0]), 42);
}

Test(apply_eval, eval_whnf_forces_app_thunk_to_outer_value)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value law = make_law(1, bc_b, sizeof(bc_b));
    enki_value app_value = enki_app_alloc(fixture_interp->gc, law, 1);
    enki_app* app = ENKI_AS(enki_app, app_value);
    app->h.state_b = THUNK;
    app->args_v[0] = 41;

    enki_value result_v = enki_eval_whnf(fixture_interp, app_value);

    cr_assert_eq(result_v, 42);
}

Test(apply_eval, eval_whnf_does_not_force_children)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value law = make_law(1, bc_b, sizeof(bc_b));
    enki_value child_value = enki_app_alloc(fixture_interp->gc, law, 1);
    enki_app* child = ENKI_AS(enki_app, child_value);
    child->h.state_b = THUNK;
    child->args_v[0] = 41;

    enki_value row_value = enki_app_alloc(fixture_interp->gc, 0, 1);
    enki_app* row_v = ENKI_AS(enki_app, row_value);
    row_v->h.state_b = WHNF;
    row_v->args_v[0] = child_value;

    enki_value result_v = enki_eval_whnf(fixture_interp, row_value);

    cr_assert_eq(result_v, row_value);
    child = ENKI_AS(enki_app, child_value);
    cr_assert_eq(child->h.state_b, THUNK);
    cr_assert_eq(row_v->args_v[0], child_value);
}

Test(apply_eval, eval_nf_forces_children_inside_whnf_app)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value law = make_law(1, bc_b, sizeof(bc_b));
    enki_value child_value = enki_app_alloc(fixture_interp->gc, law, 1);
    enki_app* child = ENKI_AS(enki_app, child_value);
    child->h.state_b = THUNK;
    child->args_v[0] = 41;

    enki_value row_value = enki_app_alloc(fixture_interp->gc, 0, 1);
    enki_app* row_v = ENKI_AS(enki_app, row_value);
    row_v->h.state_b = WHNF;
    row_v->args_v[0] = child_value;

    enki_value result_v = enki_eval_nf(fixture_interp, row_value);

    row_v = ENKI_AS(enki_app, result_v);
    cr_assert_eq(result_v, row_value);
    cr_assert_eq(row_v->h.state_b, NF);
    cr_assert_eq(row_v->args_v[0], 42);
}

Test(apply_eval, eval_nf_forces_law_name_body_and_consts)
{
    uint8_t inc_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value inc = make_law(1, inc_bc, sizeof(inc_bc));

    enki_value name_v = enki_app_alloc(fixture_interp->gc, inc, 1);
    (ENKI_AS(enki_app, name_v))->h.state_b = THUNK;
    (ENKI_AS(enki_app, name_v))->args_v[0] = 9;

    enki_value body_v = enki_app_alloc(fixture_interp->gc, inc, 1);
    (ENKI_AS(enki_app, body_v))->h.state_b = THUNK;
    (ENKI_AS(enki_app, body_v))->args_v[0] = 41;

    enki_value constant_v = enki_app_alloc(fixture_interp->gc, inc, 1);
    (ENKI_AS(enki_app, constant_v))->h.state_b = THUNK;
    (ENKI_AS(enki_app, constant_v))->args_v[0] = 19;

    uint8_t bc_b[] = { OP_RETURN };
    enki_value consts_v[] = { constant_v };
    enki_value law = make_law_with_consts(0, bc_b, sizeof(bc_b), consts_v, 1);
    enki_law* law_ptr = ENKI_AS(enki_law, law);
    law_ptr->name_v = name_v;
    law_ptr->body_v = body_v;

    enki_value result_v = enki_eval_nf(fixture_interp, law);

    law_ptr = ENKI_AS(enki_law, result_v);
    cr_assert_eq(result_v, law);
    cr_assert_eq(law_ptr->h.state_b, NF);
    cr_assert_eq(law_ptr->name_v, 10);
    cr_assert_eq(law_ptr->body_v, 42);
    cr_assert_eq(ENKI_LAW_CONSTS(law_ptr)[0], 20);
}

Test(apply_eval, eval_nf_forces_pin_inner_and_subpins)
{
    uint8_t inc_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value inc = make_law(1, inc_bc, sizeof(inc_bc));

    enki_value inner_v = enki_app_alloc(fixture_interp->gc, inc, 1);
    (ENKI_AS(enki_app, inner_v))->h.state_b = THUNK;
    (ENKI_AS(enki_app, inner_v))->args_v[0] = 41;

    enki_value subpin = enki_app_alloc(fixture_interp->gc, inc, 1);
    (ENKI_AS(enki_app, subpin))->h.state_b = THUNK;
    (ENKI_AS(enki_app, subpin))->args_v[0] = 9;

    uint8_t hash_b[32] = {0};
    enki_value subpins_v[] = { subpin };
    enki_value pin = enki_pin_alloc(fixture_interp->gc, hash_b, inner_v, 1, subpins_v);

    enki_value result_v = enki_eval_nf(fixture_interp, pin);

    enki_pin* pin_ptr = ENKI_AS(enki_pin, result_v);
    cr_assert_eq(result_v, pin);
    cr_assert_eq(pin_ptr->h.state_b, NF);
    cr_assert_eq(pin_ptr->inner_v, 42);
    cr_assert_eq(pin_ptr->subpins_v[0], 10);
}

Test(apply_eval, force_and_deepseq_use_eval_nf)
{
    enki_value app_value = enki_app_alloc(fixture_interp->gc, 0, 2);
    enki_app* app = ENKI_AS(enki_app, app_value);
    app->h.state_b = WHNF;
    app->args_v[0] = 1;
    app->args_v[1] = 2;

    fixture_interp->stack_v[0] = app_value;
    fixture_interp->sp = 1;
    op66_force(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], app_value);
    cr_assert_eq((ENKI_AS(enki_value_header, app_value))->state_b, NF);

    fixture_interp->stack_v[0] = app_value;
    fixture_interp->stack_v[1] = 99;
    fixture_interp->sp = 2;
    op66_deepseq(fixture_interp);
    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 99);
}

Test(apply_eval, seq_forces_first_value_and_returns_second)
{
    enki_value app_value = enki_app_alloc(fixture_interp->gc, 0, 1);
    enki_app* app = ENKI_AS(enki_app, app_value);
    app->h.state_b = WHNF;
    app->args_v[0] = 1;

    fixture_interp->stack_v[0] = app_value;
    fixture_interp->stack_v[1] = 123;
    fixture_interp->sp = 2;

    op66_seq(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 123);
}

Test(apply_eval, seq2_and_seq3_return_last_value)
{
    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 2;
    fixture_interp->stack_v[2] = 3;
    fixture_interp->sp = 3;

    op66_seq2(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 3);

    fixture_interp->stack_v[0] = 4;
    fixture_interp->stack_v[1] = 5;
    fixture_interp->stack_v[2] = 6;
    fixture_interp->stack_v[3] = 7;
    fixture_interp->sp = 4;

    op66_seq3(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 7);
}

Test(apply_eval, sap_forces_arg_applies_function_and_forces_result)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value inc = make_law(1, bc_b, sizeof(bc_b));

    fixture_interp->stack_v[0] = inc;
    fixture_interp->stack_v[1] = 41;
    fixture_interp->sp = 2;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    op66_sap(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 42);
}

Test(apply_eval, sap_can_apply_partial_function)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_PICK, 1,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    enki_value add = make_law(2, bc_b, sizeof(bc_b));
    enki_value partial_v = enki_app_alloc(fixture_interp->gc, add, 1);
    enki_app* app = ENKI_AS(enki_app, partial_v);
    app->args_v[0] = 20;

    fixture_interp->stack_v[0] = partial_v;
    fixture_interp->stack_v[1] = 22;
    fixture_interp->sp = 2;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    op66_sap(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 42);
}

Test(apply_eval, sap2_forces_args_applies_function_twice_and_forces_result)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_PICK, 1,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    enki_value add = make_law(2, bc_b, sizeof(bc_b));

    fixture_interp->stack_v[0] = add;
    fixture_interp->stack_v[1] = 20;
    fixture_interp->stack_v[2] = 22;
    fixture_interp->sp = 3;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    op66_sap2(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 42);
}
