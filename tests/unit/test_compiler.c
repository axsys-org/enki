#include "enki/allocator.h"
#include "enki/compiler.h"
#include "enki/interp.h"
#include "enki/value.h"
#include "enki/vector.h"

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

static enki_value app2(enki_value fn_v, enki_value a, enki_value b)
{
    enki_value value_v = enki_alloc_app(fixture_interp->gc, fn_v, 2);
    enki_app* app = (enki_app*)ENKI_TO_PTR(value_v);
    app->args_v[0] = a;
    app->args_v[1] = b;
    return value_v;
}

static enki_value app1(enki_value fn_v, enki_value a)
{
    enki_value value_v = enki_alloc_app(fixture_interp->gc, fn_v, 1);
    enki_app* app = (enki_app*)ENKI_TO_PTR(value_v);
    app->args_v[0] = a;
    return value_v;
}

static enki_value compile_body(enki_value body_v)
{
    enki_vector* bc_b = enki_vector_create_sized(enki_allocator_system(), sizeof(uint8_t));
    enki_vector* consts_v = enki_vector_create_sized(enki_allocator_system(), sizeof(enki_value));
    cr_assert_not_null(bc_b);
    cr_assert_not_null(consts_v);

    enki_compile_law(body_v, 0, bc_b, consts_v);
    enki_value law = enki_alloc_law(
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

    enki_run(fixture_interp);
}

TestSuite(compiler, .init = setup, .fini = teardown);

Test(compiler, compile_law_emits_judge)
{
    enki_vector* bc_b = enki_vector_create_sized(enki_allocator_system(), sizeof(uint8_t));
    enki_vector* consts_v = enki_vector_create_sized(enki_allocator_system(), sizeof(enki_value));

    enki_compile_law(0, 0, bc_b, consts_v);

    cr_assert_eq(enki_vector_len(bc_b), 2);
    cr_assert_eq(((uint8_t*)enki_vector_data(bc_b))[0], OP_JUDGE);
    cr_assert_eq(((uint8_t*)enki_vector_data(bc_b))[1], OP_RETURN);
    cr_assert_eq(enki_vector_len(consts_v), 0);

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
