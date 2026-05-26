#include "enki/allocator.h"
#include "test_interp.h"
#include "enki/app.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/value.h"
#include "enki/vector.h"

#include <criterion/criterion.h>
#include <stdbool.h>
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

static enki_value app2(enki_value fn_v, enki_value a, enki_value b)
{
    enki_value value_v = enki_app_alloc(fixture_interp->gc, fn_v, 2);
    enki_app* app = ENKI_AS(enki_app, value_v);
    app->args_v[0] = a;
    app->args_v[1] = b;
    return value_v;
}

static enki_value app1(enki_value fn_v, enki_value a)
{
    enki_value value_v = enki_app_alloc(fixture_interp->gc, fn_v, 1);
    enki_app* app = ENKI_AS(enki_app, value_v);
    app->args_v[0] = a;
    return value_v;
}

static enki_value compile_body(enki_value body_v)
{
    enki_vector* bc_b = enki_vector_create_sized(enki_allocator_system(), sizeof(uint8_t));
    enki_vector* consts_v = enki_vector_create_sized(enki_allocator_system(), sizeof(enki_value));
    cr_assert_not_null(bc_b);
    cr_assert_not_null(consts_v);

    enki_law_compile(fixture_interp, body_v, 0, bc_b, consts_v);
    enki_value law = enki_law_alloc(
        fixture_interp->gc,
        0,
        0,
        body_v,
        enki_vector_len(bc_b),
        enki_vector_len(consts_v),
        (uint8_t*)enki_vector_data(bc_b),
        (enki_value*)enki_vector_data(consts_v));

    enki_vector_destroy(bc_b);
    enki_vector_destroy(consts_v);
    return law;
}

static bool bytecode_contains_wide(uint8_t* bc_b, size_t bc_len_s, uint8_t op_b, uint16_t operand)
{
    for(size_t k = 0; k + 2 < bc_len_s; k++) {
        if(bc_b[k] == op_b && bc_b[k + 1] == (uint8_t)operand
            && bc_b[k + 2] == (uint8_t)(operand >> 8)) {
            return true;
        }
    }
    return false;
}

static void run_compiled(enki_value law)
{
    fixture_interp->frame[0].law = law;
    fixture_interp->frame[0].pc = 0;
    fixture_interp->frame[0].res_base_s = 0;
    fixture_interp->frame[0].arg_base_s = 0;
    fixture_interp->frame[0].cont_v = 0;
    fixture_interp->sp = 0;
    fixture_interp->fp = 0;
    fixture_interp->halted = false;

    enki_interp_run(fixture_interp);
}

TestSuite(compiler, .init = setup, .fini = teardown);

Test(compiler, compile_law_emits_literal_return)
{
    enki_vector* bc_b = enki_vector_create_sized(enki_allocator_system(), sizeof(uint8_t));
    enki_vector* consts_v = enki_vector_create_sized(enki_allocator_system(), sizeof(enki_value));

    enki_law_compile(fixture_interp, 0, 0, bc_b, consts_v);

    cr_assert_eq(enki_vector_len(bc_b), 3);
    cr_assert_eq(((uint8_t*)enki_vector_data(bc_b))[0], OP_PUSH_CONST);
    cr_assert_eq(((uint8_t*)enki_vector_data(bc_b))[1], 0);
    cr_assert_eq(((uint8_t*)enki_vector_data(bc_b))[2], OP_RETURN);
    cr_assert_eq(enki_vector_len(consts_v), 1);
    cr_assert_eq(((enki_value*)enki_vector_data(consts_v))[0], 0);

    enki_vector_destroy(bc_b);
    enki_vector_destroy(consts_v);
}

Test(compiler, compiled_law_runs_letrec_body)
{
    enki_value quote_v = app1(0, 42);
    enki_value body_v = app2(1, quote_v, 1);
    enki_value law = compile_body(body_v);

    run_compiled(law);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 42);
}

Test(compiler, emits_push_const_wide_for_const_indices_above_u8)
{
    enki_value body_v = 0;
    for(size_t k = 0; k < 257; k++) {
        enki_value quote_v = app1(0, (enki_value)(1000 + k));
        body_v = app2(1, quote_v, body_v);
    }
    enki_vector* bc_b = enki_vector_create_sized(enki_allocator_system(), sizeof(uint8_t));
    enki_vector* consts_v = enki_vector_create_sized(enki_allocator_system(), sizeof(enki_value));
    cr_assert_not_null(bc_b);
    cr_assert_not_null(consts_v);

    enki_law_compile(fixture_interp, body_v, 0, bc_b, consts_v);

    cr_assert_gt(enki_vector_len(consts_v), 256);
    cr_assert(bytecode_contains_wide(
        (uint8_t*)enki_vector_data(bc_b), enki_vector_len(bc_b), OP_PUSH_CONST_WIDE, 256));

    enki_vector_destroy(bc_b);
    enki_vector_destroy(consts_v);
}

Test(compiler, emits_pick_wide_for_stack_indices_above_u8)
{
    enki_vector* bc_b = enki_vector_create_sized(enki_allocator_system(), sizeof(uint8_t));
    enki_vector* consts_v = enki_vector_create_sized(enki_allocator_system(), sizeof(enki_value));
    cr_assert_not_null(bc_b);
    cr_assert_not_null(consts_v);

    enki_law_compile(fixture_interp, 1, 300, bc_b, consts_v);

    cr_assert(bytecode_contains_wide(
        (uint8_t*)enki_vector_data(bc_b), enki_vector_len(bc_b), OP_PICK_WIDE, 299));

    enki_vector_destroy(bc_b);
    enki_vector_destroy(consts_v);
}

Test(compiler, emits_apply_wide_for_generic_apps_above_u8_arity)
{
    uint8_t id_bc[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    enki_value id = enki_law_alloc(
        fixture_interp->gc, 256, 0, 0, sizeof(id_bc), 0, id_bc, NULL);
    enki_value app_v = enki_app_alloc(fixture_interp->gc, id, 256);
    enki_app* app = ENKI_AS(enki_app, app_v);
    for(size_t k = 0; k < 256; k++) {
        app->args_v[k] = (enki_value)k;
    }
    enki_vector* bc_b = enki_vector_create_sized(enki_allocator_system(), sizeof(uint8_t));
    enki_vector* consts_v = enki_vector_create_sized(enki_allocator_system(), sizeof(enki_value));
    cr_assert_not_null(bc_b);
    cr_assert_not_null(consts_v);

    enki_law_compile(fixture_interp, app_v, 0, bc_b, consts_v);

    cr_assert(bytecode_contains_wide(
        (uint8_t*)enki_vector_data(bc_b), enki_vector_len(bc_b), OP_APPLY_WIDE, 256));

    enki_vector_destroy(bc_b);
    enki_vector_destroy(consts_v);
}
