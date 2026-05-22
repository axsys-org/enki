#include "enki/allocator.h"
#include "enki/apply.h"
#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/op66.h"
#include "enki/value.h"

#include <criterion/criterion.h>
#include <stdint.h>

static enki_interpreter* fixture_interp;

static void setup(void)
{
    fixture_interp = enki_create_interp(enki_allocator_system(), 1024, 0);
    cr_assert_not_null(fixture_interp);
}

static void teardown(void)
{
    enki_destroy(fixture_interp);
    fixture_interp = NULL;
}

static enki_value app1(enki_value fn_v, enki_value a)
{
    enki_value value_v = enki_alloc_app(fixture_interp->gc, fn_v, 1);
    enki_app* app = (enki_app*)ENKI_TO_PTR(value_v);
    app->args_v[0] = a;
    return value_v;
}

static enki_value app3(enki_value fn_v, enki_value a, enki_value b, enki_value c)
{
    enki_value value_v = enki_alloc_app(fixture_interp->gc, fn_v, 3);
    enki_app* app = (enki_app*)ENKI_TO_PTR(value_v);
    app->args_v[0] = a;
    app->args_v[1] = b;
    app->args_v[2] = c;
    return value_v;
}

static void stress_alloc(size_t n)
{
    for(size_t k = 0; k < n; k++) {
        enki_value value_v = enki_alloc_app(fixture_interp->gc, (enki_value)k, 2);
        if(value_v != 0) {
            enki_app* app = (enki_app*)ENKI_TO_PTR(value_v);
            app->args_v[0] = (enki_value)k;
            app->args_v[1] = (enki_value)(k + 1);
        }
    }
}

static void assert_leaf(enki_value value_v, enki_value fn_v, enki_value arg)
{
    cr_assert(IS_PTR(value_v));
    enki_value_header* h = (enki_value_header*)ENKI_TO_PTR(value_v);
    cr_assert_eq(h->kind_b, ENKI_APP);
    enki_app* app = (enki_app*)ENKI_TO_PTR(value_v);
    cr_assert_eq(app->fn_v, fn_v);
    cr_assert_eq(app->n_args_s, 1);
    cr_assert_eq(app->args_v[0], arg);
}

TestSuite(gc_runtime, .init = setup, .fini = teardown);

Test(gc_runtime, row_rep_preserves_pointer_args_across_gc)
{
    enki_value leaf = app1(77, 88);
    fixture_interp->stack_v[0] = 0;
    fixture_interp->stack_v[1] = leaf;
    fixture_interp->stack_v[2] = 3;
    fixture_interp->sp = 3;

    stress_alloc(40);
    op66_rep(fixture_interp);
    stress_alloc(40);
    enki_gc_collect(fixture_interp->gc);

    enki_value row_value = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(row_value));
    enki_app* row_v = (enki_app*)ENKI_TO_PTR(row_value);
    cr_assert_eq(row_v->n_args_s, 3);
    assert_leaf(row_v->args_v[0], 77, 88);
    assert_leaf(row_v->args_v[1], 77, 88);
    assert_leaf(row_v->args_v[2], 77, 88);
}

Test(gc_runtime, slice_weld_and_up_preserve_pointer_args_across_gc)
{
    enki_value a = app1(10, 11);
    enki_value b = app1(20, 21);
    enki_value c = app1(30, 31);
    enki_value row_v = app3(0, a, b, c);
    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 2;
    fixture_interp->stack_v[2] = row_v;
    fixture_interp->sp = 3;

    stress_alloc(40);
    op66_slice(fixture_interp);
    stress_alloc(40);
    enki_gc_collect(fixture_interp->gc);

    enki_value slice_v = fixture_interp->stack_v[0];
    enki_app* slice_app = (enki_app*)ENKI_TO_PTR(slice_v);
    cr_assert_eq(slice_app->n_args_s, 2);
    assert_leaf(slice_app->args_v[0], 20, 21);
    assert_leaf(slice_app->args_v[1], 30, 31);

    a = app1(10, 11);
    b = app1(20, 21);
    c = app1(30, 31);
    enki_value x = app3(0, a, b, c);
    enki_value y = app3(0, b, c, a);
    fixture_interp->stack_v[0] = x;
    fixture_interp->stack_v[1] = y;
    fixture_interp->sp = 2;
    stress_alloc(40);
    op66_weld(fixture_interp);
    stress_alloc(40);
    enki_gc_collect(fixture_interp->gc);

    enki_value welded_v = fixture_interp->stack_v[0];
    enki_app* welded_app = (enki_app*)ENKI_TO_PTR(welded_v);
    cr_assert_eq(welded_app->n_args_s, 6);
    assert_leaf(welded_app->args_v[0], 10, 11);
    assert_leaf(welded_app->args_v[3], 20, 21);

    a = app1(10, 11);
    b = app1(20, 21);
    c = app1(30, 31);
    enki_value d = app1(40, 41);
    row_v = app3(0, a, b, c);
    fixture_interp->stack_v[0] = 2;
    fixture_interp->stack_v[1] = d;
    fixture_interp->stack_v[2] = row_v;
    fixture_interp->sp = 3;
    stress_alloc(40);
    op66_up(fixture_interp);
    stress_alloc(40);
    enki_gc_collect(fixture_interp->gc);

    enki_value updated_v = fixture_interp->stack_v[0];
    enki_app* updated_app = (enki_app*)ENKI_TO_PTR(updated_v);
    cr_assert_eq(updated_app->n_args_s, 3);
    assert_leaf(updated_app->args_v[0], 10, 11);
    assert_leaf(updated_app->args_v[1], 20, 21);
    assert_leaf(updated_app->args_v[2], 40, 41);
}

Test(gc_runtime, law_application_survives_gc_during_primitive_allocation)
{
    uint8_t bc_b[] = {
        OP_PICK, 1,
        OP_OP66, OP66_BEX,
        OP_RETURN,
    };
    enki_value law = enki_alloc_law(fixture_interp->gc, 1, 0, 0, sizeof(bc_b), 0, bc_b, NULL);

    fixture_interp->stack_v[0] = law;
    fixture_interp->stack_v[1] = 70;
    fixture_interp->sp = 2;
    fixture_interp->fp = 0;
    fixture_interp->halted = false;

    stress_alloc(40);
    enki_apply(fixture_interp, 1);
    while(fixture_interp->law != NULL && !fixture_interp->halted) {
        enki_step(fixture_interp);
    }

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(enki_nat_bits(fixture_interp->gc, fixture_interp->stack_v[0]), 71);
}

Test(gc_runtime, eval_whnf_survives_gc_during_thunk_evaluation)
{
    uint8_t bc_b[] = {
        OP_PICK, 1,
        OP_OP66, OP66_BEX,
        OP_RETURN,
    };
    enki_value law = enki_alloc_law(fixture_interp->gc, 1, 0, 0, sizeof(bc_b), 0, bc_b, NULL);
    enki_value thunk_v = enki_alloc_app(fixture_interp->gc, law, 1);
    enki_app* app = (enki_app*)ENKI_TO_PTR(thunk_v);
    app->h.state_b = THUNK;
    app->args_v[0] = 70;
    fixture_interp->stack_v[0] = thunk_v;
    fixture_interp->sp = 1;

    stress_alloc(40);
    thunk_v = fixture_interp->stack_v[0];
    enki_value result_v = enki_eval_whnf(fixture_interp, thunk_v);

    cr_assert_eq(enki_nat_bits(fixture_interp->gc, result_v), 71);
}
