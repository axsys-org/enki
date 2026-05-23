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

static enki_value app2(enki_value fn, enki_value a, enki_value b)
{
    enki_value value = enki_alloc_app(fixture_interp->gc, fn, 2);
    enki_app* app = (enki_app*)ENKI_TO_PTR(value);
    app->args[0] = a;
    app->args[1] = b;
    return value;
}

static enki_value app1(enki_value fn, enki_value a)
{
    enki_value value = enki_alloc_app(fixture_interp->gc, fn, 1);
    enki_app* app = (enki_app*)ENKI_TO_PTR(value);
    app->args[0] = a;
    return value;
}

static enki_value compile_body(enki_value body)
{
    enki_vector* bc = enki_vector_create_sized(enki_allocator_system(), sizeof(uint8_t));
    enki_vector* consts = enki_vector_create_sized(enki_allocator_system(), sizeof(enki_value));
    cr_assert_not_null(bc);
    cr_assert_not_null(consts);

    enki_compile_law(body, 0, bc, consts);
    enki_value law = enki_alloc_law(
        fixture_interp->gc,
        0,
        0,
        body,
        enki_vector_len(bc),
        enki_vector_len(consts),
        (uint8_t*)enki_vector_data(bc),
        (enki_value*)enki_vector_data(consts));

    enki_vector_destroy(bc);
    enki_vector_destroy(consts);
    return law;
}

static void run_compiled(enki_value law)
{
    fixture_interp->frame[0].law = law;
    fixture_interp->frame[0].pc = 0;
    fixture_interp->frame[0].res_base = 0;
    fixture_interp->frame[0].arg_base = 0;
    fixture_interp->frame[0].cont = 0;
    fixture_interp->sp = 0;
    fixture_interp->fp = 0;
    fixture_interp->halted = false;

    enki_run(fixture_interp);
}

TestSuite(compiler, .init = setup, .fini = teardown);

Test(compiler, compile_law_emits_judge)
{
    enki_vector* bc = enki_vector_create_sized(enki_allocator_system(), sizeof(uint8_t));
    enki_vector* consts = enki_vector_create_sized(enki_allocator_system(), sizeof(enki_value));

    enki_compile_law(0, 0, bc, consts);

    cr_assert_eq(enki_vector_len(bc), 2);
    cr_assert_eq(((uint8_t*)enki_vector_data(bc))[0], OP_JUDGE);
    cr_assert_eq(((uint8_t*)enki_vector_data(bc))[1], OP_RETURN);
    cr_assert_eq(enki_vector_len(consts), 0);

    enki_vector_destroy(bc);
    enki_vector_destroy(consts);
}

Test(compiler, compiled_law_runs_letrec_body)
{
    enki_value quote = app1(0, 42);
    enki_value body = app2(1, quote, 1);
    enki_value law = compile_body(body);

    run_compiled(law);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack[0], 42);
}
