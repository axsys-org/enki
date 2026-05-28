#include "enki/allocator.h"
#include "test_interp.h"
#include "enki/app.h"
#include "enki/eval.h"
#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/nat.h"
#include "enki/op66.h"
#include "enki/pin.h"
#include "enki/value.h"

#include <criterion/criterion.h>
#include <stdint.h>

static enki_interpreter* fixture_interp;

typedef void (*throw_fn)(void*);

static void setup(void)
{
    fixture_interp = enki_test_interp_create(1024, 0);
    cr_assert_not_null(fixture_interp);
}

static void teardown(void)
{
    enki_test_interp_destroy(fixture_interp);
    fixture_interp = NULL;
}

static enki_value app1(enki_value fn_v, enki_value a)
{
    size_t base_s = fixture_interp->sp;
    fixture_interp->stack_v[fixture_interp->sp++] = fn_v;
    fixture_interp->stack_v[fixture_interp->sp++] = a;
    enki_value value_v = enki_app_alloc(fixture_interp->gc, fn_v, 1);
    enki_app* app = ENKI_AS(enki_app, value_v);
    app->fn_v = fixture_interp->stack_v[base_s];
    app->args_v[0] = fixture_interp->stack_v[base_s + 1];
    fixture_interp->sp = base_s;
    return value_v;
}

static enki_value app2(enki_value fn_v, enki_value a, enki_value b)
{
    size_t base_s = fixture_interp->sp;
    fixture_interp->stack_v[fixture_interp->sp++] = fn_v;
    fixture_interp->stack_v[fixture_interp->sp++] = a;
    fixture_interp->stack_v[fixture_interp->sp++] = b;
    enki_value value_v = enki_app_alloc(fixture_interp->gc, fn_v, 2);
    enki_app* app = ENKI_AS(enki_app, value_v);
    app->fn_v = fixture_interp->stack_v[base_s];
    app->args_v[0] = fixture_interp->stack_v[base_s + 1];
    app->args_v[1] = fixture_interp->stack_v[base_s + 2];
    fixture_interp->sp = base_s;
    return value_v;
}

static enki_value app3(enki_value fn_v, enki_value a, enki_value b, enki_value c)
{
    size_t base_s = fixture_interp->sp;
    fixture_interp->stack_v[fixture_interp->sp++] = fn_v;
    fixture_interp->stack_v[fixture_interp->sp++] = a;
    fixture_interp->stack_v[fixture_interp->sp++] = b;
    fixture_interp->stack_v[fixture_interp->sp++] = c;
    enki_value value_v = enki_app_alloc(fixture_interp->gc, fn_v, 3);
    enki_app* app = ENKI_AS(enki_app, value_v);
    app->fn_v = fixture_interp->stack_v[base_s];
    app->args_v[0] = fixture_interp->stack_v[base_s + 1];
    app->args_v[1] = fixture_interp->stack_v[base_s + 2];
    app->args_v[2] = fixture_interp->stack_v[base_s + 3];
    fixture_interp->sp = base_s;
    return value_v;
}

static void stress_alloc(size_t n)
{
    for(size_t k = 0; k < n; k++) {
        enki_value value_v = enki_nat_bex(fixture_interp->gc, 70 + (k % 8));
        (void)value_v;
    }
    enki_gc_collect(fixture_interp->gc);
}

static void assert_leaf(enki_value value_v, enki_value fn_v, enki_value arg)
{
    cr_assert(IS_PTR(value_v));
    enki_value_header* h = ENKI_AS(enki_value_header, value_v);
    cr_assert_eq(h->kind_b, ENKI_APP);
    enki_app* app = ENKI_AS(enki_app, value_v);
    cr_assert_eq(app->fn_v, fn_v);
    cr_assert_eq(app->n_args_s, 1);
    cr_assert_eq(app->args_v[0], arg);
}

static void run_until_base_frame(void)
{
    while(fixture_interp->cp > 0 && !fixture_interp->halted) {
        enki_interp_step(fixture_interp);
    }
}

static int capture_throw(throw_fn fn, void* ctx)
{
    fixture_interp->has_error_jmp = true;
    if(setjmp(fixture_interp->error_jmp) == 0) {
        fn(ctx);
        fixture_interp->has_error_jmp = false;
        enki_arena_reset(fixture_interp->scratch_a);
        return ENKI_ERROR_OK;
    }
    fixture_interp->has_error_jmp = false;
    enki_arena_reset(fixture_interp->scratch_a);
    return fixture_interp->error_code;
}

static void collect_while_locked(void* ctx)
{
    (void)ctx;
    enki_gc_lock(fixture_interp->gc);
    enki_gc_collect(fixture_interp->gc);
}

static void locked_alloc_too_large(void* ctx)
{
    (void)ctx;
    (void)enki_gc_alloc_locked(
        fixture_interp->gc, fixture_interp->gc->cap_s + 1, _Alignof(enki_value_header));
}

static void normal_alloc_too_large_while_locked(void* ctx)
{
    (void)ctx;
    enki_gc_lock(fixture_interp->gc);
    (void)fixture_interp->gc->alloc(
        fixture_interp->gc, fixture_interp->gc->cap_s + 1, _Alignof(enki_value_header));
}

TestSuite(gc_runtime, .init = setup, .fini = teardown);

Test(gc_runtime, collect_while_locked_reports_oom_without_swapping_arenas)
{
    enki_arena* active_a = fixture_interp->gc->active_a;
    enki_arena* idle_a = fixture_interp->gc->idle_a;

    cr_assert_eq(capture_throw(collect_while_locked, NULL), ENKI_ERROR_OOM);

    fixture_interp->gc->lock_depth = 0;
    cr_assert_eq(fixture_interp->gc->active_a, active_a);
    cr_assert_eq(fixture_interp->gc->idle_a, idle_a);
}

Test(gc_runtime, locked_alloc_reports_oom_without_swapping_arenas_or_leaking_lock)
{
    enki_arena* active_a = fixture_interp->gc->active_a;
    enki_arena* idle_a = fixture_interp->gc->idle_a;

    cr_assert_eq(capture_throw(locked_alloc_too_large, NULL), ENKI_ERROR_OOM);

    cr_assert_eq(fixture_interp->gc->lock_depth, 0);
    cr_assert_eq(fixture_interp->gc->active_a, active_a);
    cr_assert_eq(fixture_interp->gc->idle_a, idle_a);
}

Test(gc_runtime, normal_alloc_while_locked_reports_oom_before_collecting)
{
    enki_arena* active_a = fixture_interp->gc->active_a;
    enki_arena* idle_a = fixture_interp->gc->idle_a;

    cr_assert_eq(capture_throw(normal_alloc_too_large_while_locked, NULL), ENKI_ERROR_OOM);

    fixture_interp->gc->lock_depth = 0;
    cr_assert_eq(fixture_interp->gc->active_a, active_a);
    cr_assert_eq(fixture_interp->gc->idle_a, idle_a);
}

Test(gc_runtime, trace_copies_all_runtime_tags)
{
    enki_value big_v = enki_nat_bex(fixture_interp->gc, 70);
    fixture_interp->stack_v[0] = big_v;
    fixture_interp->sp = 1;
    enki_value app_v = app1(77, big_v);
    fixture_interp->stack_v[1] = app_v;
    fixture_interp->sp = 2;
    uint8_t bc_b[] = { OP_RETURN };
    enki_value consts_v[] = {0};
    enki_value law_v = enki_law_alloc(fixture_interp->gc, 1, 0, 0, sizeof(bc_b), 1, bc_b, consts_v);
    fixture_interp->stack_v[2] = law_v;
    fixture_interp->sp = 3;
    enki_law* law = ENKI_AS(enki_law, law_v);
    law->name_v = fixture_interp->stack_v[1];
    law->body_v = fixture_interp->stack_v[0];
    ENKI_LAW_CONSTS(law)[0] = fixture_interp->stack_v[1];
    uint8_t hash_b[32] = {0};
    enki_value subpins_v[] = {0};
    enki_value pin_v = enki_pin_alloc(fixture_interp->gc, hash_b, 0, 1, subpins_v);
    fixture_interp->stack_v[3] = pin_v;
    fixture_interp->sp = 4;
    enki_pin* pin = ENKI_AS(enki_pin, pin_v);
    pin->inner_v = fixture_interp->stack_v[2];
    pin->subpins_v[0] = fixture_interp->stack_v[1];
    enki_value cont_args_v[] = {0, 0, 0, 0};
    enki_value cont_v = enki_app_cont_alloc(fixture_interp->gc, 4, cont_args_v);
    enki_cont* cont = ENKI_AS(enki_cont, cont_v);
    cont->args_v[0] = fixture_interp->stack_v[3];
    cont->args_v[1] = fixture_interp->stack_v[2];
    cont->args_v[2] = fixture_interp->stack_v[1];
    cont->args_v[3] = fixture_interp->stack_v[0];

    fixture_interp->stack_v[0] = cont->args_v[0];
    fixture_interp->stack_v[1] = cont->args_v[1];
    fixture_interp->stack_v[2] = cont->args_v[2];
    fixture_interp->stack_v[3] = cont->args_v[3];
    fixture_interp->stack_v[4] = cont_v;
    fixture_interp->sp = 5;
    fixture_interp->cp = 0;

    stress_alloc(80);
    enki_gc_collect(fixture_interp->gc);

    cr_assert(IS_PTR(fixture_interp->stack_v[0]));
    cr_assert(IS_PTR(fixture_interp->stack_v[1]));
    cr_assert(IS_PTR(fixture_interp->stack_v[2]));
    cr_assert(IS_PTR(fixture_interp->stack_v[3]));
    cr_assert_eq((ENKI_AS(enki_value_header, fixture_interp->stack_v[0]))->kind_b, ENKI_PIN);
    cr_assert_eq((ENKI_AS(enki_value_header, fixture_interp->stack_v[1]))->kind_b, ENKI_LAW);
    cr_assert_eq((ENKI_AS(enki_value_header, fixture_interp->stack_v[2]))->kind_b, ENKI_APP);
    cr_assert_eq((ENKI_AS(enki_value_header, fixture_interp->stack_v[3]))->kind_b, ENKI_NAT);
    cr_assert(IS_PTR(fixture_interp->stack_v[4]));
    cr_assert_eq((ENKI_AS(enki_value_header, fixture_interp->stack_v[4]))->kind_b, ENKI_CONT);
}

Test(gc_runtime, row_preserves_pointer_list_elements_across_gc)
{
    enki_value a = app1(10, 11);
    enki_value b = app1(20, 21);
    enki_value c = app1(30, 31);
    enki_value xs = app2(0, c, 0);
    xs = app2(0, b, xs);
    xs = app2(0, a, xs);

    fixture_interp->stack_v[0] = 99;
    fixture_interp->stack_v[1] = 3;
    fixture_interp->stack_v[2] = xs;
    fixture_interp->sp = 3;

    stress_alloc(40);
    op66_row(fixture_interp);
    stress_alloc(40);
    enki_gc_collect(fixture_interp->gc);

    enki_value row_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(row_v));
    enki_app* row = ENKI_AS(enki_app, row_v);
    cr_assert_eq(row->fn_v, 99);
    cr_assert_eq(row->n_args_s, 3);
    assert_leaf(row->args_v[0], 10, 11);
    assert_leaf(row->args_v[1], 20, 21);
    assert_leaf(row->args_v[2], 30, 31);
}

Test(gc_runtime, init_preserves_fn_and_args_across_gc)
{
    enki_value fn = app1(1, 2);
    enki_value a = app1(10, 11);
    enki_value b = app1(20, 21);
    enki_value c = app1(30, 31);
    enki_value row_v = app3(fn, a, b, c);

    fixture_interp->stack_v[0] = row_v;
    fixture_interp->sp = 1;

    stress_alloc(40);
    op66_init(fixture_interp);
    stress_alloc(40);
    enki_gc_collect(fixture_interp->gc);

    enki_value init_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(init_v));
    enki_app* init = ENKI_AS(enki_app, init_v);
    cr_assert_eq(init->n_args_s, 2);
    assert_leaf(init->fn_v, 1, 2);
    assert_leaf(init->args_v[0], 10, 11);
    assert_leaf(init->args_v[1], 20, 21);
}

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
    enki_app* row_v = ENKI_AS(enki_app, row_value);
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
    enki_app* slice_app = ENKI_AS(enki_app, slice_v);
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
    enki_app* welded_app = ENKI_AS(enki_app, welded_v);
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
    enki_app* updated_app = ENKI_AS(enki_app, updated_v);
    cr_assert_eq(updated_app->n_args_s, 3);
    assert_leaf(updated_app->args_v[0], 10, 11);
    assert_leaf(updated_app->args_v[1], 20, 21);
    assert_leaf(updated_app->args_v[2], 40, 41);
}

Test(gc_runtime, law_application_survives_gc_during_primitive_allocation)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_OP66, OP66_BEX,
        OP_RETURN,
    };
    enki_value law = enki_law_alloc(fixture_interp->gc, 1, 0, 0, sizeof(bc_b), 0, bc_b, NULL);

    fixture_interp->stack_v[0] = law;
    fixture_interp->stack_v[1] = 70;
    fixture_interp->sp = 2;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    stress_alloc(40);
    enki_app_apply(fixture_interp, 1);
    while(fixture_interp->cp > 0 && !fixture_interp->halted) {
        enki_interp_step(fixture_interp);
    }

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(enki_nat_bits(fixture_interp->gc, fixture_interp->stack_v[0]), 71);
}

Test(gc_runtime, eval_whnf_survives_gc_during_thunk_evaluation)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_OP66, OP66_BEX,
        OP_RETURN,
    };
    enki_value law = enki_law_alloc(fixture_interp->gc, 1, 0, 0, sizeof(bc_b), 0, bc_b, NULL);
    enki_value thunk_v = enki_app_alloc(fixture_interp->gc, law, 1);
    enki_app* app = ENKI_AS(enki_app, thunk_v);
    app->h.state_b = THUNK;
    app->args_v[0] = 70;
    fixture_interp->stack_v[0] = thunk_v;
    fixture_interp->sp = 1;

    stress_alloc(40);
    thunk_v = fixture_interp->stack_v[0];
    enki_value result_v = enki_eval_whnf(fixture_interp, thunk_v);

    cr_assert_eq(enki_nat_bits(fixture_interp->gc, result_v), 71);
}

Test(gc_runtime, partial_application_preserves_old_and_new_pointer_args_across_gc)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    enki_value law = enki_law_alloc(fixture_interp->gc, 3, 0, 0, sizeof(bc_b), 0, bc_b, NULL);
    enki_value a = app1(10, 11);

    fixture_interp->stack_v[0] = law;
    fixture_interp->stack_v[1] = a;
    fixture_interp->sp = 2;
    stress_alloc(40);
    enki_app_apply(fixture_interp, 1);

    enki_value partial_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(partial_v));
    enki_app* partial = ENKI_AS(enki_app, partial_v);
    cr_assert_eq(partial->n_args_s, 1);
    assert_leaf(partial->args_v[0], 10, 11);

    enki_value b = app1(20, 21);
    partial_v = fixture_interp->stack_v[0];
    fixture_interp->stack_v[0] = partial_v;
    fixture_interp->stack_v[1] = b;
    fixture_interp->sp = 2;
    stress_alloc(40);
    enki_app_apply(fixture_interp, 1);
    stress_alloc(40);
    enki_gc_collect(fixture_interp->gc);

    enki_value extended_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(extended_v));
    enki_app* extended = ENKI_AS(enki_app, extended_v);
    cr_assert_eq(extended->n_args_s, 2);
    assert_leaf(extended->args_v[0], 10, 11);
    assert_leaf(extended->args_v[1], 20, 21);
}

Test(gc_runtime, flat_thunk_preserves_extra_args_across_gc)
{
    uint8_t id_bc_b[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    uint8_t bex_bc_b[] = {
        OP_PICK, 0,
        OP_OP66, OP66_BEX,
        OP_RETURN,
    };
    enki_value id_law = enki_law_alloc(fixture_interp->gc, 1, 0, 0, sizeof(id_bc_b), 0, id_bc_b, NULL);
    fixture_interp->stack_v[0] = id_law;
    fixture_interp->sp = 1;
    enki_value bex_law = enki_law_alloc(fixture_interp->gc, 1, 0, 0, sizeof(bex_bc_b), 0, bex_bc_b, NULL);

    id_law = fixture_interp->stack_v[0];
    fixture_interp->stack_v[0] = id_law;
    fixture_interp->stack_v[1] = bex_law;
    fixture_interp->stack_v[2] = 70;
    fixture_interp->sp = 3;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    stress_alloc(40);
    enki_app_apply(fixture_interp, 2);
    stress_alloc(40);
    enki_gc_collect(fixture_interp->gc);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert(IS_PTR(fixture_interp->stack_v[0]));
    enki_app* thunk = ENKI_AS(enki_app, fixture_interp->stack_v[0]);
    cr_assert_eq(thunk->h.state_b, THUNK);
    cr_assert_eq(thunk->n_args_s, 2);
    cr_assert(IS_PTR(thunk->fn_v));
    cr_assert(IS_PTR(thunk->args_v[0]));
    cr_assert_eq(thunk->args_v[1], 70);
    cr_assert_eq(enki_nat_bits(fixture_interp->gc,
            enki_eval_whnf(fixture_interp, fixture_interp->stack_v[0])), 71);
}

Test(gc_runtime, nat_pointer_operands_survive_primitive_allocation_gc)
{
    enki_value a = enki_nat_bex(fixture_interp->gc, 70);
    enki_value b = enki_nat_bex(fixture_interp->gc, 69);

    fixture_interp->stack_v[0] = a;
    fixture_interp->stack_v[1] = b;
    fixture_interp->sp = 2;

    stress_alloc(40);
    op66_add(fixture_interp);
    stress_alloc(40);
    enki_gc_collect(fixture_interp->gc);

    enki_value result_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(result_v));
    cr_assert_eq(enki_nat_bits(fixture_interp->gc, result_v), 71);
    cr_assert_eq(enki_nat_test(fixture_interp->gc, 70, result_v), 1);
    cr_assert_eq(enki_nat_test(fixture_interp->gc, 69, result_v), 1);
}
