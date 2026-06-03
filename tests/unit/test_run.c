#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/run.h"
#include "enki/run_ops.h"

#include <criterion/criterion.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_S8(a, b, c, d, e, f, g, h) \
    ((er_val)(PLAN_S7(a, b, c, d, e, f, g) | (PLAN_CH(h) << 56u)))

static void* fail_alloc(void* ctx, size_t size_s)
{
    (void)ctx;
    (void)size_s;
    return NULL;
}

static void fail_free(void* ctx, void* ptr)
{
    (void)ctx;
    free(ptr);
}

static void free_system(void* ptr)
{
    const enki_allocator* allocator = enki_allocator_system();
    allocator->free(allocator->ctx, ptr);
}

static er_tank* assert_tank_value(er_val value_v, const char* msg_c)
{
    cr_assert(er_is_tank(value_v), "expected er_tank tag 0xFE, got 0x%02x",
              (unsigned int)er_get_tag(value_v));
    er_tank* tank = er_outt(er_tag_tank, value_v);
    cr_assert_not_null(tank);
    if (msg_c != NULL) {
        cr_assert_not_null(tank->msg_c);
        cr_assert_str_eq(tank->msg_c, msg_c);
    }
    return tank;
}

Test(run_alloc, thunk_alloc_initializes_header)
{
    const size_t arg_s = 3;
    er_thk* thk = er_thk_alloc(enki_allocator_system(), arg_s);
    cr_assert_not_null(thk);

    cr_assert_eq(thk->hed.siz_s, sizeof(er_thk) + arg_s * sizeof(er_val));
    cr_assert_eq(thk->hed.raw.fwd_f, 0);
    cr_assert_eq(thk->hed.raw.nf_f, 0);
    cr_assert_eq(thk->fun, ER_XDONE);
    cr_assert_eq(thk->arg_s, arg_s);

    free_system(thk);
}

Test(run_alloc, thunk_init_copies_all_arguments)
{
    enum { ARG_S = 9 };
    er_val args_v[ARG_S];
    for (size_t k = 0; k < ARG_S; k++) {
        args_v[k] = UINT64_C(0x0102030405060708) + (er_val)k * UINT64_C(0x0001000100010001);
    }

    er_thk* thk = er_thk_alloc(enki_allocator_system(), ARG_S);
    cr_assert_not_null(thk);

    er_val value_v = er_thk_init(thk, ER_CALL, ARG_S, args_v);
    cr_assert_eq(er_get_tag(value_v), er_tag_thk);
    cr_assert_eq(er_outa(value_v), thk);
    cr_assert_eq(thk->fun, ER_CALL);
    cr_assert_eq(thk->arg_s, ARG_S);
    for (size_t k = 0; k < ARG_S; k++) {
        cr_assert_eq(thk->arg_v[k], args_v[k]);
    }

    free_system(thk);
}

Test(run_alloc, thunk_init_rejects_too_many_arguments)
{
    er_val args_v[] = {11, 22};
    er_thk* thk = er_thk_alloc(enki_allocator_system(), 1);
    cr_assert_not_null(thk);

    cr_assert_eq(er_thk_init(thk, ER_CALL, 2, args_v), 0);
    cr_assert_eq(thk->arg_s, 1);

    er_val value_v = er_thk_init(thk, ER_CALL, 1, args_v);
    cr_assert_eq(er_get_tag(value_v), er_tag_thk);
    cr_assert_eq(thk->arg_v[0], args_v[0]);

    free_system(thk);
}

Test(run_alloc, thunk_alloc_returns_null_on_alloc_failure)
{
    const enki_allocator fail_allocator = {
        .ctx = NULL,
        .alloc = fail_alloc,
        .realloc = NULL,
        .free = fail_free,
    };

    cr_assert_null(er_thk_alloc(&fail_allocator, 1));
}

Test(run_alloc, tank_uses_error_tag_and_records_payload)
{
    er_val tank_v = er_tank_make(enki_allocator_system(), 42, "boom");

    cr_assert_eq(er_get_tag(tank_v), er_tag_tank);
    cr_assert_not(er_is_good(tank_v));
    er_tank* tank = assert_tank_value(tank_v, "boom");
    cr_assert_eq(tank->val_v, 42);

    free_system(tank);
}

static size_t test_code_len(const er_op* code_v)
{
    cr_assert_not_null(code_v);
    for (size_t k = 0; k < 4096; k++) {
        if (code_v[k].tag == OP_RET) {
            return k + 1;
        }
    }
    cr_assert_fail("test bytecode label is missing OP_RET");
    return 0;
}

Test(run_alloc, law_alloc_init_records_label_table)
{
    er_op label0[] = {{.tag = OP_RET}};
    er_op label1[] = {{.tag = OP_RET}};
    er_op label2[] = {{.tag = OP_RET}};
    er_op label3[] = {{.tag = OP_RET}};
    er_op* labels_v[] = {label0, label1, label2, label3};
    size_t label_len_v[] = {1, 1, 1, 1};
    er_law* law = er_law_alloc(enki_allocator_system(), 4, 4);
    cr_assert_not_null(law);

    er_val law_v = er_law_init(law, 11, 22, 2, 3, 4, labels_v, label_len_v);

    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    cr_assert_eq(er_outa(law_v), law);
    cr_assert_eq(law->h.siz_s & ~(size_t)0x3, law->code_o + 4 * sizeof(er_op));
    cr_assert_eq(law->h.raw.nf_f, 0);
    cr_assert_eq(law->name_v, 11);
    cr_assert_eq(law->body_v, 22);
    cr_assert_eq(law->ari_d, 2);
    cr_assert_eq(law->let_d, 3);
    cr_assert_eq(law->bc_s, 4);
    cr_assert_eq(law->op_s, 4);
    for (size_t k = 0; k < 4; k++) {
        er_op* label_code_v = er_law_label_code(law, k);
        cr_assert_not_null(label_code_v);
        cr_assert_neq(label_code_v, labels_v[k]);
        cr_assert_eq(law->bc_v[k].op_s, 1);
        cr_assert_eq(label_code_v[0].tag, OP_RET);
    }

    free_system(law);
}

Test(run_alloc, law_init_rejects_missing_let_labels)
{
    er_law* law = er_law_alloc(enki_allocator_system(), 1, 1);
    cr_assert_not_null(law);

    cr_assert_eq(er_law_init(law, 0, 0, 2, 1, 1, NULL, NULL), 0);

    free_system(law);
}

static er_val make_law(uint32_t arity_d, er_op* entry_v, size_t n_lets,
    er_op* const let_code_v[])
{
    size_t bc_s = n_lets + 1;
    er_op** labels_v = calloc(bc_s, sizeof(er_op*));
    cr_assert_not_null(labels_v);
    size_t* label_len_v = calloc(bc_s, sizeof(size_t));
    cr_assert_not_null(label_len_v);
    labels_v[0] = entry_v;
    label_len_v[0] = test_code_len(entry_v);
    for (size_t k = 0; k < n_lets; k++) {
        labels_v[k + 1] = let_code_v[k];
        label_len_v[k + 1] = test_code_len(let_code_v[k]);
    }

    size_t op_s = 0;
    for (size_t k = 0; k < bc_s; k++) {
        op_s += label_len_v[k];
    }
    er_law* law = er_law_alloc(enki_allocator_system(), bc_s, op_s);
    cr_assert_not_null(law);
    er_val law_v = er_law_init(law, 0, 0, arity_d, (uint32_t)n_lets, bc_s, labels_v,
                               label_len_v);
    free(label_len_v);
    free(labels_v);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    return law_v;
}

static er_val make_law_with_len(uint32_t arity_d, er_op* entry_v, size_t entry_len_s)
{
    er_op* labels_v[] = {entry_v};
    size_t label_len_v[] = {entry_len_s};
    er_law* law = er_law_alloc(enki_allocator_system(), 1, entry_len_s);
    cr_assert_not_null(law);
    er_val law_v = er_law_init(law, 0, 0, arity_d, 0, 1, labels_v, label_len_v);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    return law_v;
}

static er_val make_prim66(void)
{
    er_pin* pin = er_pin_alloc(enki_allocator_system(), 0);
    cr_assert_not_null(pin);
    er_val pin_v = er_pin_init(pin, NULL, 66, 0, NULL);
    cr_assert_eq(er_get_tag(pin_v), er_tag_pin);
    return pin_v;
}

static er_val make_prim0(void)
{
    er_pin* pin = er_pin_alloc(enki_allocator_system(), 0);
    cr_assert_not_null(pin);
    er_val pin_v = er_pin_init(pin, NULL, 0, 0, NULL);
    cr_assert_eq(er_get_tag(pin_v), er_tag_pin);
    return pin_v;
}

static er_val make_bat(size_t lim_s, const uint64_t lim_q[])
{
    er_bat* bat = er_bat_alloc(enki_allocator_system(), lim_s);
    cr_assert_not_null(bat);
    er_val bat_v = er_bat_init(bat, lim_s, lim_q);
    cr_assert_eq(er_get_tag(bat_v), er_tag_bat);
    return bat_v;
}

static er_val make_app_value(er_val fn_v, size_t arg_s, const er_val arg_v[])
{
    er_app* app = er_app_alloc(enki_allocator_system(), arg_s);
    cr_assert_not_null(app);
    er_val app_v = er_app_init(app, fn_v, arg_s, arg_v);
    cr_assert_eq(er_get_tag(app_v), er_tag_app);
    return app_v;
}

static er_app* assert_app_value(er_val value_v, er_val fn_v, size_t arg_s)
{
    er_app* app = er_outt(er_tag_app, value_v);
    cr_assert_not_null(app);
    cr_assert_eq(app->fn_v, fn_v);
    cr_assert_eq(app->arg_s, arg_s);
    return app;
}

Test(run_ops, elim_app_case_receives_init_and_last)
{
    er_val app_args_v[] = {20, 30};
    er_val app_v = make_app_value(10, 2, app_args_v);

    er_val elim_v = eo_elim(enki_allocator_system(), 1, 2, 111, 4, 5, app_v);

    er_thk* thunk = er_outt(er_tag_thk, elim_v);
    cr_assert_not_null(thunk);
    cr_assert_eq(thunk->fun, ER_XUNK_APP);
    cr_assert_eq(thunk->arg_s, 3);
    cr_assert_eq(thunk->arg_v[0], 111);
    cr_assert_eq(thunk->arg_v[2], 30);

    er_app* init = er_outt(er_tag_app, thunk->arg_v[1]);
    cr_assert_not_null(init);
    cr_assert_eq(init->fn_v, 10);
    cr_assert_eq(init->arg_s, 1);
    cr_assert_eq(init->arg_v[0], 20);
}

Test(run_ops, row_helpers_match_reference_fallbacks)
{
    const enki_allocator* loc_a = enki_allocator_system();
    er_val row_args_v[] = {11, 22};
    er_val row_v = make_app_value(77, 2, row_args_v);

    cr_assert_eq(eo_hd(row_v), 77);
    cr_assert_eq(eo_hd(123), 123);
    cr_assert_eq(eo_coup(loc_a, 44, 123), 44);

    cr_assert_eq(eo_slice(loc_a, 0, 0, row_v), 0);
    cr_assert_eq(eo_slice(loc_a, 2, 1, row_v), 0);
    er_app* sliced = assert_app_value(eo_slice(loc_a, row_v, 1, row_v), 0, 1);
    cr_assert_eq(sliced->arg_v[0], 11);

    cr_assert_eq(eo_up(loc_a, 9, 99, row_v), row_v);
    cr_assert_eq(eo_up(loc_a, 0, 99, 123), 123);

    er_app* updated = assert_app_value(eo_up(loc_a, row_v, 99, row_v), 77, 2);
    cr_assert_eq(updated->arg_v[0], 99);
    cr_assert_eq(updated->arg_v[1], 22);
}

Test(run_ops, weld_uses_head_zero_and_empty_non_app_rows)
{
    const enki_allocator* loc_a = enki_allocator_system();
    er_val x_args_v[] = {1, 2};
    er_val y_args_v[] = {3};
    er_val x_v = make_app_value(9, 2, x_args_v);
    er_val y_v = make_app_value(8, 1, y_args_v);

    er_app* welded = assert_app_value(eo_weld(loc_a, x_v, y_v), 0, 3);
    cr_assert_eq(welded->arg_v[0], 1);
    cr_assert_eq(welded->arg_v[1], 2);
    cr_assert_eq(welded->arg_v[2], 3);

    welded = assert_app_value(eo_weld(loc_a, 123, y_v), 0, 1);
    cr_assert_eq(welded->arg_v[0], 3);

    welded = assert_app_value(eo_weld(loc_a, x_v, 456), 0, 2);
    cr_assert_eq(welded->arg_v[0], 1);
    cr_assert_eq(welded->arg_v[1], 2);

    assert_app_value(eo_weld(loc_a, 123, 456), 0, 0);
}

Test(run_ops, div_mod_zero_divisor_is_bad_after_nat_coercion)
{
    const enki_allocator* loc_a = enki_allocator_system();
    er_val app_args_v[] = {1};
    er_val app_v = make_app_value(0, 1, app_args_v);
    uint64_t zero_q[] = {0};
    er_val big_zero_v = make_bat(1, zero_q);

    assert_tank_value(eo_div(loc_a, 10, 0), "division by zero");
    assert_tank_value(eo_mod(loc_a, 10, 0), "division by zero");
    assert_tank_value(eo_div(loc_a, 10, app_v), "division by zero");
    assert_tank_value(eo_mod(loc_a, 10, big_zero_v), "division by zero");
    cr_assert_eq(eo_div(loc_a, app_v, 5), 0);
    cr_assert_eq(eo_mod(loc_a, app_v, 5), 0);
}

Test(run_ops, comparisons_coerce_non_nats_to_zero)
{
    er_val a_args_v[] = {1};
    er_val b_args_v[] = {2};
    er_val app_a_v = make_app_value(10, 1, a_args_v);
    er_val app_b_v = make_app_value(20, 1, b_args_v);

    cr_assert_eq(eo_eq(app_a_v, app_b_v), 1);
    cr_assert_eq(eo_eq(app_a_v, 0), 1);
    cr_assert_eq(eo_eq(7, app_b_v), 0);
    cr_assert_eq(eo_cmp(app_a_v, app_b_v), 1);
    cr_assert_eq(eo_cmp(app_a_v, 7), 0);
    cr_assert_eq(eo_cmp(7, app_b_v), 2);
    cr_assert_eq(eo_le(app_a_v, app_b_v), 1);
    cr_assert_eq(eo_le(app_a_v, 7), 1);
    cr_assert_eq(eo_le(7, app_b_v), 0);
}

Test(run_ops, primitive_dispatch_bad_arity_is_bad)
{
    const enki_allocator* loc_a = enki_allocator_system();
    er_val arg_v[] = {1};

    assert_tank_value(eo_exec_op66(loc_a, OP66_ADD, 1, arg_v), "bad primitive arity");
    assert_tank_value(eo_exec_op0(loc_a, OP0_LAW, 1, arg_v), "bad primitive arity");
}

Test(run_ops, row_style_primitive_errors_do_not_fall_through)
{
    const enki_allocator* loc_a = enki_allocator_system();
    er_val add_args_v[] = {PLAN_S3('A', 'd', 'd')};
    er_val add_row_v = make_app_value(0, 1, add_args_v);
    er_val bad_args_v[] = {PLAN_S5('B', 'o', 'g', 'u', 's'), 1};
    er_val bad_row_v = make_app_value(0, 2, bad_args_v);
    er_val div_args_v[] = {PLAN_S3('D', 'i', 'v'), 10, 0};
    er_val div_row_v = make_app_value(0, 3, div_args_v);
    er_val law_args_v[] = {OP0_LAW, 2};
    er_val law_row_v = make_app_value(0, 2, law_args_v);

    assert_tank_value(eo_exec_op66_app(loc_a, add_row_v), "bad primitive arity");
    assert_tank_value(eo_exec_op66_app(loc_a, bad_row_v), "bad primitive tag");
    assert_tank_value(eo_exec_op66_app(loc_a, div_row_v), "division by zero");
    assert_tank_value(eo_exec_op0_app(loc_a, law_row_v), "bad primitive arity");
}

typedef struct er_gc_test_root {
    er_val val_v;
} er_gc_test_root;

static void trace_er_gc_test_root(enki_gc* gc, void* root_p)
{
    er_gc_test_root* root = root_p;
    root->val_v = enki_gc_copy(gc, root->val_v);
}

static er_val make_gc_app_value(const enki_allocator* loc_a, er_val fn_v, size_t arg_s,
                                const er_val arg_v[])
{
    er_app* app = er_app_alloc(loc_a, arg_s);
    cr_assert_not_null(app);
    er_val app_v = er_app_init(app, fn_v, arg_s, arg_v);
    cr_assert_eq(er_get_tag(app_v), er_tag_app);
    return app_v;
}

Test(run_gc, collect_moves_nested_er_values)
{
    enki_gc* gc = enki_gc_create(enki_allocator_system(), 4096, NULL);
    cr_assert_not_null(gc);
    const enki_allocator* loc_a = enki_gc_as_allocator(gc);

    er_val child_arg_v[] = {11, 22};
    er_val child_v = make_gc_app_value(loc_a, 7, 2, child_arg_v);
    er_app* old_child = er_outt(er_tag_app, child_v);
    er_val root_arg_v[] = {child_v, 33};
    er_val root_v = make_gc_app_value(loc_a, 9, 2, root_arg_v);
    er_app* old_root = er_outt(er_tag_app, root_v);

    er_gc_test_root root = {.val_v = root_v};
    enki_gc_set_trace_root(gc, &root, trace_er_gc_test_root);
    enki_gc_collect(gc);

    er_app* moved_root = er_outt(er_tag_app, root.val_v);
    cr_assert_not_null(moved_root);
    cr_assert_neq(moved_root, old_root);
    cr_assert_eq(moved_root->fn_v, 9);
    cr_assert_eq(moved_root->arg_s, 2);
    cr_assert_eq(moved_root->arg_v[1], 33);

    er_app* moved_child = er_outt(er_tag_app, moved_root->arg_v[0]);
    cr_assert_not_null(moved_child);
    cr_assert_neq(moved_child, old_child);
    cr_assert_eq(moved_child->fn_v, 7);
    cr_assert_eq(moved_child->arg_s, 2);
    cr_assert_eq(moved_child->arg_v[0], 11);
    cr_assert_eq(moved_child->arg_v[1], 22);

    enki_gc_destroy(gc);
}

Test(run_gc, collect_moves_tank_payload)
{
    enki_gc* gc = enki_gc_create(enki_allocator_system(), 4096, NULL);
    cr_assert_not_null(gc);
    const enki_allocator* loc_a = enki_gc_as_allocator(gc);

    er_val child_arg_v[] = {11};
    er_val child_v = make_gc_app_value(loc_a, 7, 1, child_arg_v);
    er_app* old_child = er_outt(er_tag_app, child_v);
    er_val tank_v = er_tank_make(loc_a, child_v, "boom");
    er_tank* old_tank = assert_tank_value(tank_v, "boom");

    er_gc_test_root root = {.val_v = tank_v};
    enki_gc_set_trace_root(gc, &root, trace_er_gc_test_root);
    enki_gc_collect(gc);

    er_tank* moved_tank = assert_tank_value(root.val_v, "boom");
    cr_assert_neq(moved_tank, old_tank);
    er_app* moved_child = er_outt(er_tag_app, moved_tank->val_v);
    cr_assert_not_null(moved_child);
    cr_assert_neq(moved_child, old_child);
    cr_assert_eq(moved_child->fn_v, 7);
    cr_assert_eq(moved_child->arg_s, 1);
    cr_assert_eq(moved_child->arg_v[0], 11);

    enki_gc_destroy(gc);
}

Test(run_gc, eval_gc_forces_moved_thunk)
{
    enki_gc* gc = enki_gc_create(enki_allocator_system(), 4096, NULL);
    cr_assert_not_null(gc);
    const enki_allocator* loc_a = enki_gc_as_allocator(gc);

    er_val answer_v = 1234;
    er_thk* thk = er_thk_alloc(loc_a, 1);
    cr_assert_not_null(thk);
    er_val thk_v = er_thk_init(thk, ER_XDONE, 1, &answer_v);
    cr_assert_eq(er_get_tag(thk_v), er_tag_thk);

    er_gc_test_root root = {.val_v = thk_v};
    enki_gc_set_trace_root(gc, &root, trace_er_gc_test_root);
    enki_gc_collect(gc);

    cr_assert_eq(er_eval_gc(gc, root.val_v), answer_v);
    enki_gc_destroy(gc);
}

Test(run_gc, compiled_law_survives_collection_before_eval)
{
    enki_gc* gc = enki_gc_create(enki_allocator_system(), 16384, NULL);
    cr_assert_not_null(gc);
    const enki_allocator* loc_a = enki_gc_as_allocator(gc);

    er_val add_v = er_law_make(loc_a, PLAN_S3('A', 'd', 'd'), 0, 2);
    cr_assert_eq(er_get_tag(add_v), er_tag_law);

    er_val add_arg_v[] = {add_v, 10};
    er_val add_10_v = make_gc_app_value(loc_a, 0, 2, add_arg_v);
    er_val body_arg_v[] = {add_10_v, 32};
    er_val body_v = make_gc_app_value(loc_a, 0, 2, body_arg_v);
    er_val law_v = er_law_make(loc_a, 0, body_v, 0);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);

    er_gc_test_root root = {.val_v = law_v};
    enki_gc_set_trace_root(gc, &root, trace_er_gc_test_root);
    enki_gc_collect(gc);

    er_val call_arg_v[] = {root.val_v};
    er_thk* thk = er_thk_alloc(loc_a, 1);
    cr_assert_not_null(thk);
    er_val call_v = er_thk_init(thk, ER_CALL, 1, call_arg_v);
    cr_assert_eq(er_get_tag(call_v), er_tag_thk);

    cr_assert_eq(er_eval_gc(gc, call_v), 42);
    enki_gc_destroy(gc);
}

static er_val make_plan_call_expr(er_val f_v, er_val x_v)
{
    er_val arg_v[] = {f_v, x_v};
    return make_app_value(0, 2, arg_v);
}

static er_val make_law_quote_expr(er_val val_v)
{
    return make_app_value(0, 1, &val_v);
}

static er_val make_prim_law(er_val name_v, uint32_t arity_d)
{
    er_val law_v = er_law_make(enki_allocator_system(), name_v, 0, arity_d);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    return law_v;
}

static er_val make_call(er_val fun_v, size_t frame_s)
{
    er_thk* thk = er_thk_alloc(enki_allocator_system(), frame_s);
    cr_assert_not_null(thk);
    er_val* args_v = calloc(frame_s, sizeof(er_val));
    cr_assert_not_null(args_v);
    args_v[0] = fun_v;
    er_val call_v = er_thk_init(thk, ER_CALL, frame_s, args_v);
    free(args_v);
    cr_assert_eq(er_get_tag(call_v), er_tag_thk);
    return call_v;
}

static er_val make_done_thunk(er_val val_v)
{
    er_thk* thk = er_thk_alloc(enki_allocator_system(), 1);
    cr_assert_not_null(thk);
    er_val thunk_v = er_thk_init(thk, ER_XDONE, 1, &val_v);
    cr_assert_eq(er_get_tag(thunk_v), er_tag_thk);
    return thunk_v;
}

static er_val make_hole_thunk(void)
{
    er_thk* thk = er_thk_alloc(enki_allocator_system(), 0);
    cr_assert_not_null(thk);
    er_val thunk_v = er_thk_init(thk, ER_HOLE, 0, NULL);
    cr_assert_eq(er_get_tag(thunk_v), er_tag_thk);
    return thunk_v;
}

static er_val run_vm_mode(er_op* code, er_val root_v, er_eval_mode mode)
{
    er_val dstack_v[256] = {0};
    er_kon kstack_v[512] = {0};
    er_vm vm = {
        .code = code,
        .loc_a = enki_allocator_system(),
        .dstack = dstack_v,
        .dsp = dstack_v,
        .kbase = kstack_v,
        .ksp = kstack_v,
    };
    return plan_eval(&vm, root_v, mode);
}

static er_val run_vm(er_op* code, er_val root_v)
{
    return run_vm_mode(code, root_v, ER_EVAL_WHNF);
}

static er_val run_code(er_op* code)
{
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);
    return run_vm(code, call_v);
}

static er_val run_prim66_row(er_val tag_v, size_t arg_s, const er_val arg_v[])
{
    er_val prim66_v = make_prim66();
    er_val row_v = make_app_value(tag_v, arg_s, arg_v);
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = row_v},
        [2] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [3] = {.tag = OP_EVAL},
        [4] = {.tag = OP_RET},
    };
    return run_code(code);
}

static er_val run_prim0_row(er_val tag_v, size_t arg_s, const er_val arg_v[])
{
    er_val prim0_v = make_prim0();
    er_val row_v = make_app_value(tag_v, arg_s, arg_v);
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim0_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = row_v},
        [2] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [3] = {.tag = OP_EVAL},
        [4] = {.tag = OP_RET},
    };
    return run_code(code);
}

static er_val make_op_descriptor(er_val name_v, er_val word_v)
{
    return make_app_value(name_v, 1, &word_v);
}

static er_val run_prim66_inc(er_val input_v)
{
    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('I', 'n', 'c')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = input_v},
        [3] = {.tag = OP_MK_APP, .as.u32 = 2},
        [4] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [5] = {.tag = OP_EVAL},
        [6] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);
    return run_vm(code, call_v);
}

static er_val run_prim66_arity(er_val input_v)
{
    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S5('A', 'r', 'i', 't', 'y')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = input_v},
        [3] = {.tag = OP_MK_APP, .as.u32 = 2},
        [4] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [5] = {.tag = OP_EVAL},
        [6] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);
    return run_vm(code, call_v);
}

static void assert_bat_limbs(er_val val_v, size_t lim_s, const uint64_t lim_q[])
{
    er_bat* bat = er_outt(er_tag_bat, val_v);
    cr_assert_not_null(bat);
    cr_assert_eq(bat->lim_s, lim_s);
    for (size_t k = 0; k < lim_s; k++) {
        cr_assert_eq(bat->lim_q[k], lim_q[k], "limb %zu", k);
    }
}

Test(run_vm, strict_let_prelude_forces_suspension_and_discards_value)
{
    enum {
        LAW_START = 0,
        LET_START = 5,
    };
    er_op code[] = {
        [0] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [1] = {.tag = OP_EVAL},
        [2] = {.tag = OP_DROP},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = 42},
        [4] = {.tag = OP_RET},
        [5] = {.tag = OP_PUSH_LIT, .as.lit_v = 7},
        [6] = {.tag = OP_RET},
    };
    er_op* let_code_v[] = {code + LET_START};
    er_val law_v = make_law(0, code + LAW_START, 1, let_code_v);
    er_val call_v = make_call(law_v, 2);
    er_thk* frame = er_outt(er_tag_thk, call_v);

    er_val result_v = run_vm(code, call_v);

    cr_assert_eq(result_v, 42);
    cr_assert_eq(frame->fun, ER_XDONE);
    cr_assert_eq(frame->arg_v[0], 42);
    er_thk* susp = er_outt(er_tag_thk, frame->arg_v[1]);
    cr_assert_not_null(susp);
    cr_assert_eq(susp->fun, ER_XDONE);
    cr_assert_eq(susp->arg_v[0], 7);
}

Test(run_vm, strict_let_cycle_traps_as_tank)
{
    enum {
        LAW_START = 0,
        LET_START = 3,
    };
    er_op code[] = {
        [0] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [1] = {.tag = OP_EVAL},
        [2] = {.tag = OP_RET},
        [3] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [4] = {.tag = OP_EVAL},
        [5] = {.tag = OP_RET},
    };
    er_op* let_code_v[] = {code + LET_START};
    er_val law_v = make_law(0, code + LAW_START, 1, let_code_v);
    er_val call_v = make_call(law_v, 2);

    assert_tank_value(run_vm(code, call_v), "thunk hole");
}

Test(run_vm, mk_app_uses_operand_count_and_leaves_single_stack_value)
{
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = 100},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = 10},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = 20},
        [3] = {.tag = OP_MK_APP, .as.u32 = 3},
        [4] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    er_val result_v = run_vm(code, call_v);

    er_app* app = er_outt(er_tag_app, result_v);
    cr_assert_not_null(app);
    cr_assert_eq(app->fn_v, 100);
    cr_assert_eq(app->arg_s, 2);
    cr_assert_eq(app->arg_v[0], 10);
    cr_assert_eq(app->arg_v[1], 20);
}

Test(run_vm, rotate_reorders_top_n_stack_values)
{
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = 1},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = 2},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = 3},
        [3] = {.tag = OP_ROTATE, .as.u32 = 3},
        [4] = {.tag = OP_MK_APP, .as.u32 = 3},
        [5] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    er_val result_v = run_vm(code, call_v);

    er_app* app = er_outt(er_tag_app, result_v);
    cr_assert_not_null(app);
    cr_assert_eq(app->fn_v, 2);
    cr_assert_eq(app->arg_s, 2);
    cr_assert_eq(app->arg_v[0], 3);
    cr_assert_eq(app->arg_v[1], 1);
}

Test(run_vm, plan_eval_whnf_leaves_app_children_suspended)
{
    er_val child_v = make_done_thunk(42);
    er_val app_v = make_app_value(0, 1, &child_v);

    er_val result_v = run_vm_mode(NULL, app_v, ER_EVAL_WHNF);

    er_app* app = er_outt(er_tag_app, result_v);
    cr_assert_not_null(app);
    cr_assert_eq(result_v, app_v);
    cr_assert_eq(app->h.raw.nf_f, 0);
    cr_assert_eq(app->arg_v[0], child_v);
}

Test(run_vm, plan_eval_nf_forces_children_inside_whnf_app)
{
    er_val child_v = make_done_thunk(42);
    er_val app_v = make_app_value(0, 1, &child_v);

    er_val result_v = run_vm_mode(NULL, app_v, ER_EVAL_NF);

    er_app* app = er_outt(er_tag_app, result_v);
    cr_assert_not_null(app);
    cr_assert_eq(result_v, app_v);
    cr_assert_eq(app->h.raw.nf_f, 1);
    cr_assert_eq(app->arg_v[0], 42);
}

Test(run_vm, op_force_reduces_stack_value_to_nf)
{
    er_val child_v = make_done_thunk(42);
    er_val app_v = make_app_value(0, 1, &child_v);
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = app_v},
        [1] = {.tag = OP_FORCE},
        [2] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    er_val result_v = run_vm(code, call_v);

    er_app* app = er_outt(er_tag_app, result_v);
    cr_assert_not_null(app);
    cr_assert_eq(result_v, app_v);
    cr_assert_eq(app->h.raw.nf_f, 1);
    cr_assert_eq(app->arg_v[0], 42);
}

Test(run_vm, plan_eval_nf_forces_pin_inner_and_subpins)
{
    er_val inner_v = make_done_thunk(42);
    er_val sub_v[] = {make_done_thunk(10)};
    er_pin* pin = er_pin_alloc(enki_allocator_system(), 1);
    cr_assert_not_null(pin);
    er_val pin_v = er_pin_init(pin, NULL, inner_v, 1, sub_v);
    cr_assert_eq(er_get_tag(pin_v), er_tag_pin);

    er_val result_v = run_vm_mode(NULL, pin_v, ER_EVAL_NF);

    pin = er_outt(er_tag_pin, result_v);
    cr_assert_not_null(pin);
    cr_assert_eq(result_v, pin_v);
    cr_assert_eq(pin->hed.raw.nf_f, 1);
    cr_assert_eq(pin->val_v, 42);
    cr_assert_eq(pin->sub_v[0], 10);
}

Test(run_vm, plan_eval_nf_forces_law_name_and_body)
{
    er_val name_v = make_done_thunk(10);
    er_val body_v = make_done_thunk(42);
    er_op code[] = {{.tag = OP_RET}};
    er_op* labels_v[] = {code};
    size_t label_len_v[] = {1};
    er_law* law = er_law_alloc(enki_allocator_system(), 1, 1);
    cr_assert_not_null(law);
    er_val law_v = er_law_init(law, name_v, body_v, 0, 0, 1, labels_v, label_len_v);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);

    er_val result_v = run_vm_mode(NULL, law_v, ER_EVAL_NF);

    law = er_outt(er_tag_law, result_v);
    cr_assert_not_null(law);
    cr_assert_eq(result_v, law_v);
    cr_assert_eq(law->h.raw.nf_f, 1);
    cr_assert_eq(law->name_v, 10);
    cr_assert_eq(law->body_v, 42);
}

Test(run_vm, direct_primop_bytecode_uses_data_stack)
{
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = 10},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = 32},
        [2] = {.tag = OP_ADD},
        [3] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    cr_assert_eq(run_vm(code, call_v), 42);
}

Test(run_vm, primop0_law_forces_arguments_to_whnf_and_increments_arity)
{
    er_val prim0_v = make_prim0();
    er_val arity_v = make_done_thunk(2);
    er_val name_v = make_done_thunk(11);
    er_val body_arg_v[] = {make_done_thunk(42)};
    er_val body_const_v = make_app_value(99, 1, body_arg_v);
    er_val body_v = make_done_thunk(body_const_v);
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim0_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = OP0_LAW},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = arity_v},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = name_v},
        [4] = {.tag = OP_PUSH_LIT, .as.lit_v = body_v},
        [5] = {.tag = OP_MK_APP, .as.u32 = 4},
        [6] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [7] = {.tag = OP_EVAL},
        [8] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    er_val result_v = run_vm(code, call_v);

    er_law* made_law = er_outt(er_tag_law, result_v);
    cr_assert_not_null(made_law);
    cr_assert_eq(made_law->ari_d, 3);
    cr_assert_eq(made_law->name_v, 11);
    cr_assert_eq(made_law->body_v, body_const_v);
    er_app* body_app = er_outt(er_tag_app, made_law->body_v);
    cr_assert_not_null(body_app);
    cr_assert_eq(body_app->h.raw.nf_f, 0);
    cr_assert_eq(body_app->arg_v[0], body_arg_v[0]);
}

Test(run_vm, primop0_elim_forces_only_scrutinee)
{
    er_val prim0_v = make_prim0();
    er_val bad_v = make_hole_thunk();
    er_val scrutinee_v = make_done_thunk(0);
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim0_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = OP0_ELIM},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = bad_v},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = bad_v},
        [4] = {.tag = OP_PUSH_LIT, .as.lit_v = bad_v},
        [5] = {.tag = OP_PUSH_LIT, .as.lit_v = 42},
        [6] = {.tag = OP_PUSH_LIT, .as.lit_v = bad_v},
        [7] = {.tag = OP_PUSH_LIT, .as.lit_v = scrutinee_v},
        [8] = {.tag = OP_MK_APP, .as.u32 = 7},
        [9] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [10] = {.tag = OP_EVAL},
        [11] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    cr_assert_eq(run_vm(code, call_v), 42);
}

Test(run_vm, primop0_bad_arity_is_tank)
{
    er_val prim0_v = make_prim0();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim0_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = OP0_LAW},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = 2},
        [3] = {.tag = OP_MK_APP, .as.u32 = 2},
        [4] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [5] = {.tag = OP_EVAL},
        [6] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    assert_tank_value(run_vm(code, call_v), "bad primitive arity");
}

Test(run_vm, primop66_eq_returns_nat_boolean)
{
    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S2('E', 'q')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = 42},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = 42},
        [4] = {.tag = OP_MK_APP, .as.u32 = 3},
        [5] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [6] = {.tag = OP_EVAL},
        [7] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    cr_assert_eq(run_vm(code, call_v), 1);

    code[3].as.lit_v = 43;
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 0);
}

Test(run_vm, primop66_sub_forces_operands_and_preserves_order)
{
    er_val arg_v[] = {make_done_thunk(10), make_done_thunk(3)};

    cr_assert_eq(run_prim66_row(PLAN_S3('S', 'u', 'b'), 2, arg_v), 7);
}

Test(run_vm, primop66_rep_forces_count_only)
{
    er_val lazy_item_v = make_hole_thunk();
    er_val arg_v[] = {5, lazy_item_v, make_done_thunk(3)};

    er_val result_v = run_prim66_row(PLAN_S3('R', 'e', 'p'), 3, arg_v);

    er_app* row = er_outt(er_tag_app, result_v);
    cr_assert_not_null(row);
    cr_assert_eq(row->fn_v, 5);
    cr_assert_eq(row->arg_s, 3);
    for (size_t k = 0; k < row->arg_s; k++) {
        cr_assert_eq(row->arg_v[k], lazy_item_v);
    }
}

Test(run_vm, primop66_up_forces_index_and_row_only)
{
    er_val row_arg_v[] = {10, 20, 30};
    er_val row_v = make_app_value(0, 3, row_arg_v);
    er_val replacement_v = make_hole_thunk();
    er_val arg_v[] = {make_done_thunk(1), replacement_v, make_done_thunk(row_v)};

    er_val result_v = run_prim66_row(PLAN_S2('U', 'p'), 3, arg_v);

    er_app* row = er_outt(er_tag_app, result_v);
    cr_assert_not_null(row);
    cr_assert_eq(row->fn_v, 0);
    cr_assert_eq(row->arg_s, 3);
    cr_assert_eq(row->arg_v[0], 10);
    cr_assert_eq(row->arg_v[1], replacement_v);
    cr_assert_eq(row->arg_v[2], 30);
}

Test(run_vm, primop66_or_and_short_circuit_do_not_force_rhs)
{
    er_val or_arg_v[] = {1, make_hole_thunk()};
    er_val and_arg_v[] = {0, make_hole_thunk()};

    cr_assert_eq(run_prim66_row(PLAN_S2('O', 'r'), 2, or_arg_v), 1);
    cr_assert_eq(run_prim66_row(PLAN_S3('A', 'n', 'd'), 2, and_arg_v), 0);
}

Test(run_vm, primop66_store_preserves_stack_order)
{
    er_val store_desc_v = make_op_descriptor(TEST_S8('O', 'P', '_', 'S', 'T', 'O', 'R', 'E'), 0);
    er_val arg_v[] = {1, 4, 10, 0};

    cr_assert_eq(run_prim66_row(store_desc_v, 4, arg_v), 160);
}

Test(run_vm, primop66_word_descriptors_route_correctly)
{
    er_val load_desc_v = make_op_descriptor(PLAN_S7('O', 'P', '_', 'L', 'O', 'A', 'D'), 4);
    er_val load_arg_v[] = {1, 0xAB};
    er_op load_code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = load_arg_v[0]},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = load_arg_v[1]},
        [2] = {.tag = OP_LOADN, .as.lit_v = 4},
        [3] = {.tag = OP_RET},
    };
    cr_assert_eq(run_prim66_row(load_desc_v, 2, load_arg_v), run_code(load_code));

    er_val store_desc_v = make_op_descriptor(TEST_S8('O', 'P', '_', 'S', 'T', 'O', 'R', 'E'), 4);
    er_val store_arg_v[] = {1, 10, 0};
    er_op store_code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = store_arg_v[0]},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = store_arg_v[1]},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = store_arg_v[2]},
        [3] = {.tag = OP_STOREN, .as.lit_v = 4},
        [4] = {.tag = OP_RET},
    };
    cr_assert_eq(run_prim66_row(store_desc_v, 3, store_arg_v), run_code(store_code));

    er_val trunc_desc_v = make_op_descriptor(TEST_S8('O', 'P', '_', 'T', 'R', 'U', 'N', 'C'), 4);
    er_val trunc_arg_v[] = {0xAB};
    er_op trunc_code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = trunc_arg_v[0]},
        [1] = {.tag = OP_TRUNCN, .as.lit_v = 4},
        [2] = {.tag = OP_RET},
    };
    cr_assert_eq(run_prim66_row(trunc_desc_v, 1, trunc_arg_v), run_code(trunc_code));

    er_val met_desc_v = make_op_descriptor(PLAN_S6('O', 'P', '_', 'M', 'E', 'T'), 4);
    er_val met_arg_v[] = {0x10};
    er_op met_code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = met_arg_v[0]},
        [1] = {.tag = OP_MET, .as.lit_v = 4},
        [2] = {.tag = OP_RET},
    };
    cr_assert_eq(run_prim66_row(met_desc_v, 1, met_arg_v), run_code(met_code));
}

Test(run_vm, primop0_leading_zero_descriptor_matches_direct_tag)
{
    er_val direct_arg_v[] = {make_done_thunk(42)};
    er_val leading_arg_v[] = {OP0_PIN, direct_arg_v[0]};

    er_val direct_v = run_prim0_row(OP0_PIN, 1, direct_arg_v);
    er_val leading_v = run_prim0_row(0, 2, leading_arg_v);

    er_pin* direct = er_outt(er_tag_pin, direct_v);
    er_pin* leading = er_outt(er_tag_pin, leading_v);
    cr_assert_not_null(direct);
    cr_assert_not_null(leading);
    cr_assert_eq(direct->val_v, 42);
    cr_assert_eq(leading->val_v, direct->val_v);
}

Test(run_vm, primop66_bad_arity_is_tank)
{
    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('A', 'd', 'd')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = 10},
        [3] = {.tag = OP_MK_APP, .as.u32 = 2},
        [4] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [5] = {.tag = OP_EVAL},
        [6] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    assert_tank_value(run_vm(code, call_v), "bad primitive arity");
}

Test(run_vm, primop66_cmp_identical_non_nat_is_equal)
{
    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('C', 'm', 'p')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [4] = {.tag = OP_MK_APP, .as.u32 = 3},
        [5] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [6] = {.tag = OP_EVAL},
        [7] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    cr_assert_eq(run_vm(code, call_v), 1);
}

Test(run_vm, primop66_arity_reports_only_raw_laws)
{
    er_op raw_code[] = {{.tag = OP_RET}};
    er_val raw_law_v = make_law(3, raw_code, 0, NULL);
    er_val pinned_law_v = er_pin_make(enki_allocator_system(), raw_law_v);
    cr_assert_eq(er_get_tag(pinned_law_v), er_tag_pin);
    er_val primitive_pin_v = make_prim66();
    er_val partial_arg_v = 99;
    er_val partial_app_v = make_app_value(raw_law_v, 1, &partial_arg_v);

    cr_assert_eq(run_prim66_arity(raw_law_v), 3);
    cr_assert_eq(run_prim66_arity(pinned_law_v), 0);
    cr_assert_eq(run_prim66_arity(primitive_pin_v), 0);
    cr_assert_eq(run_prim66_arity(partial_app_v), 0);
    cr_assert_eq(run_prim66_arity(42), 0);
}

Test(run_vm, primop66_init_singleton_returns_head)
{
    er_val row_arg_v[] = {7};
    er_val row_v = make_app_value(42, 1, row_arg_v);

    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S4('I', 'n', 'i', 't')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = row_v},
        [3] = {.tag = OP_MK_APP, .as.u32 = 2},
        [4] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [5] = {.tag = OP_EVAL},
        [6] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    cr_assert_eq(run_vm(code, call_v), 42);
}

Test(run_vm, primop66_bit_helpers)
{
    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('S', 'e', 't')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = 1},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = 0},
        [4] = {.tag = OP_MK_APP, .as.u32 = 3},
        [5] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [6] = {.tag = OP_EVAL},
        [7] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 2);

    code[1].as.lit_v = PLAN_S5('C', 'l', 'e', 'a', 'r');
    code[3].as.lit_v = 3;
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 1);

    code[1].as.lit_v = PLAN_S3('N', 'i', 'b');
    code[2].as.lit_v = 1;
    code[3].as.lit_v = PLAN_S2('a', 'b');
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 6);
}

Test(run_vm, primop66_ixn_forces_row_argument)
{
    er_val row_arg_v[] = {1, 2, 3};
    er_val row_v = make_app_value(0, 3, row_arg_v);
    er_val lazy_row_v = make_done_thunk(row_v);

    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('I', 'x', '0')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = lazy_row_v},
        [3] = {.tag = OP_MK_APP, .as.u32 = 2},
        [4] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [5] = {.tag = OP_EVAL},
        [6] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    cr_assert_eq(run_vm(code, call_v), 1);
}

Test(run_vm, primop66_case_n_uses_last_branch_as_default)
{
    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S5('C', 'a', 's', 'e', '3')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = make_done_thunk(1)},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = 11},
        [4] = {.tag = OP_PUSH_LIT, .as.lit_v = 22},
        [5] = {.tag = OP_PUSH_LIT, .as.lit_v = 33},
        [6] = {.tag = OP_MK_APP, .as.u32 = 5},
        [7] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [8] = {.tag = OP_EVAL},
        [9] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    cr_assert_eq(run_vm(code, call_v), 22);

    code[2].as.lit_v = 2;
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 33);

    code[2].as.lit_v = 7;
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 33);
}

Test(run_vm, primop66_case10_tag_maps_to_case10_opcode)
{
    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S6('C', 'a', 's', 'e', '1', '0')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = make_done_thunk(8)},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = 100},
        [4] = {.tag = OP_PUSH_LIT, .as.lit_v = 101},
        [5] = {.tag = OP_PUSH_LIT, .as.lit_v = 102},
        [6] = {.tag = OP_PUSH_LIT, .as.lit_v = 103},
        [7] = {.tag = OP_PUSH_LIT, .as.lit_v = 104},
        [8] = {.tag = OP_PUSH_LIT, .as.lit_v = 105},
        [9] = {.tag = OP_PUSH_LIT, .as.lit_v = 106},
        [10] = {.tag = OP_PUSH_LIT, .as.lit_v = 107},
        [11] = {.tag = OP_PUSH_LIT, .as.lit_v = 108},
        [12] = {.tag = OP_PUSH_LIT, .as.lit_v = 109},
        [13] = {.tag = OP_MK_APP, .as.u32 = 12},
        [14] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [15] = {.tag = OP_EVAL},
        [16] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    cr_assert_eq(run_vm(code, call_v), 108);

    code[2].as.lit_v = 9;
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 109);

    code[2].as.lit_v = 99;
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 109);
}

Test(run_vm, primop66_case_forces_index_and_row)
{
    er_val row_arg_v[] = {11, 22, 33};
    er_val row_v = make_app_value(0, 3, row_arg_v);

    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S4('C', 'a', 's', 'e')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = make_done_thunk(2)},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = make_done_thunk(row_v)},
        [4] = {.tag = OP_PUSH_LIT, .as.lit_v = 99},
        [5] = {.tag = OP_MK_APP, .as.u32 = 4},
        [6] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [7] = {.tag = OP_EVAL},
        [8] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    cr_assert_eq(run_vm(code, call_v), 33);

    code[2].as.lit_v = 3;
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 99);
}

Test(run_vm, primop66_row_forces_list_spine_while_copying_lazy_items)
{
    er_val cell4_arg_v[] = {4, 0};
    er_val cell4_v = make_app_value(0, 2, cell4_arg_v);
    er_val cell3_arg_v[] = {3, make_done_thunk(cell4_v)};
    er_val cell3_v = make_app_value(0, 2, cell3_arg_v);
    er_val cell2_arg_v[] = {2, make_done_thunk(cell3_v)};
    er_val cell2_v = make_app_value(0, 2, cell2_arg_v);
    er_val lazy_item_v = make_done_thunk(1);
    er_val cell1_arg_v[] = {lazy_item_v, make_done_thunk(cell2_v)};
    er_val cell1_v = make_app_value(0, 2, cell1_arg_v);

    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('R', 'o', 'w')},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = 3},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = 4},
        [4] = {.tag = OP_PUSH_LIT, .as.lit_v = cell1_v},
        [5] = {.tag = OP_MK_APP, .as.u32 = 4},
        [6] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [7] = {.tag = OP_EVAL},
        [8] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    er_val result_v = run_vm(code, call_v);
    er_app* row = er_outt(er_tag_app, result_v);
    cr_assert_not_null(row);
    cr_assert_eq(row->fn_v, 3);
    cr_assert_eq(row->arg_s, 4);
    cr_assert_eq(row->arg_v[0], lazy_item_v);
    cr_assert_eq(row->arg_v[1], 2);
    cr_assert_eq(row->arg_v[2], 3);
    cr_assert_eq(row->arg_v[3], 4);

    code[3].as.lit_v = 0;
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 3);

    code[2].as.lit_v = prim66_v;
    code[3].as.lit_v = 2;
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    result_v = run_vm(code, call_v);
    row = er_outt(er_tag_app, result_v);
    cr_assert_not_null(row);
    cr_assert_eq(row->fn_v, 0);
    cr_assert_eq(row->arg_s, 2);
    cr_assert_eq(row->arg_v[0], lazy_item_v);
    cr_assert_eq(row->arg_v[1], 2);

    code[2].as.lit_v = 3;
    code[3].as.lit_v = prim66_v;
    law_v = make_law(0, code, 0, NULL);
    call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(code, call_v), 3);
}

Test(run_vm, unknown_application_flattens_existing_app_head)
{
    er_val row_arg_v[] = {1, 2, 3};
    er_val row_v = make_app_value(0, 3, row_arg_v);
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = row_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = 4},
        [2] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [3] = {.tag = OP_EVAL},
        [4] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    er_val result_v = run_vm(code, call_v);
    er_app* row = er_outt(er_tag_app, result_v);
    cr_assert_not_null(row);
    cr_assert_eq(row->fn_v, 0);
    cr_assert_eq(row->arg_s, 4);
    cr_assert_eq(row->arg_v[0], 1);
    cr_assert_eq(row->arg_v[1], 2);
    cr_assert_eq(row->arg_v[2], 3);
    cr_assert_eq(row->arg_v[3], 4);
}

Test(run_vm, unknown_application_flattens_nested_app_spine)
{
    er_val one_v = 1;
    er_val first_v = make_app_value(0, 1, &one_v);
    er_val two_v = 2;
    er_val nested_v = make_app_value(first_v, 1, &two_v);
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = nested_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = 3},
        [2] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [3] = {.tag = OP_EVAL},
        [4] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    er_val result_v = run_vm(code, call_v);
    er_app* row = er_outt(er_tag_app, result_v);
    cr_assert_not_null(row);
    cr_assert_eq(row->fn_v, 0);
    cr_assert_eq(row->arg_s, 3);
    cr_assert_eq(row->arg_v[0], 1);
    cr_assert_eq(row->arg_v[1], 2);
    cr_assert_eq(row->arg_v[2], 3);
}

Test(run_vm, overapplication_reenters_returned_row_with_leftover_args)
{
    er_op returns_row_code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = 0},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = 1},
        [2] = {.tag = OP_MK_APP, .as.u32 = 2},
        [3] = {.tag = OP_RET},
    };
    er_val returns_row_v = make_law(1, returns_row_code, 0, NULL);
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = returns_row_v},
        [1] = {.tag = OP_PUSH_LIT, .as.lit_v = 99},
        [2] = {.tag = OP_PUSH_LIT, .as.lit_v = 2},
        [3] = {.tag = OP_MK_CALL, .as.u32 = 3},
        [4] = {.tag = OP_EVAL},
        [5] = {.tag = OP_RET},
    };
    er_val law_v = make_law(0, code, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    er_val result_v = run_vm(code, call_v);
    er_app* row = er_outt(er_tag_app, result_v);
    cr_assert_not_null(row);
    cr_assert_eq(row->fn_v, 0);
    cr_assert_eq(row->arg_s, 2);
    cr_assert_eq(row->arg_v[0], 1);
    cr_assert_eq(row->arg_v[1], 2);
}

Test(run_vm, compiled_law_emits_direct_primitive_bytecode)
{
    er_val add_v = make_prim_law(PLAN_S3('A', 'd', 'd'), 2);
    er_val body_v = make_plan_call_expr(make_plan_call_expr(add_v, 10), 32);
    er_val law_v = er_law_make(enki_allocator_system(), 0, body_v, 0);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    er_val call_v = make_call(law_v, 1);

    cr_assert_eq(run_vm(NULL, call_v), 42);
}

Test(run_vm, compiled_call_preserves_quoted_partial_primitive_args)
{
    er_val prim66_v = make_prim66();
    er_val ix_prim_arg_v[] = {PLAN_S2('I', 'x'), 1, 2};
    er_val ix_prim_row_v = make_app_value(0, 3, ix_prim_arg_v);
    er_val ix_body_v = make_plan_call_expr(prim66_v, ix_prim_row_v);
    er_val ix_law_v = er_law_make(enki_allocator_system(), PLAN_S2('I', 'x'), ix_body_v, 2);
    cr_assert_eq(er_get_tag(ix_law_v), er_tag_law);
    er_val ix_v = er_pin_make(enki_allocator_system(), ix_law_v);
    cr_assert_eq(er_get_tag(ix_v), er_tag_pin);
    er_val ix_arg_v[] = {2};
    er_val ix2_v = make_app_value(ix_v, 1, ix_arg_v);
    er_val quoted_ix2_v = make_law_quote_expr(ix2_v);
    er_val body_v = make_plan_call_expr(quoted_ix2_v, 1);
    er_val law_v = er_law_make(enki_allocator_system(), 0, body_v, 2);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    er_law* law = er_outt(er_tag_law, law_v);
    cr_assert_not_null(law);
    er_op* code = er_law_label_code(law, 0);
    cr_assert_not_null(code);

    bool saw_ix_f = false;
    for (size_t k = 0; k < 16 && code[k].tag != OP_RET; k++) {
        saw_ix_f = saw_ix_f || code[k].tag == OP_IX;
    }
    cr_assert(saw_ix_f);

    er_val row_arg_v[] = {10, 11, 12};
    er_val row_v = make_app_value(0, 3, row_arg_v);
    er_val call_v = make_call(law_v, 3);
    er_thk* call = er_outt(er_tag_thk, call_v);
    cr_assert_not_null(call);
    call->arg_v[1] = row_v;
    call->arg_v[2] = 99;

    cr_assert_eq(run_vm(NULL, call_v), 12);
}

Test(run_vm, compiled_law_can_return_self_as_value)
{
    er_val law_v = er_law_make(enki_allocator_system(), PLAN_S4('s', 'e', 'l', 'f'), 0, 0);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    er_law* law = er_outt(er_tag_law, law_v);
    cr_assert_not_null(law);
    er_op* code = er_law_label_code(law, 0);
    cr_assert_not_null(code);
    cr_assert_eq(code[0].tag, OP_PUSH_VAR);
    cr_assert_eq(code[0].as.slot, 0);

    er_val call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(NULL, call_v), law_v);
}

Test(run_vm, compiled_primitive_evals_lower_stack_argument_before_opcode)
{
    er_val add_v = make_prim_law(PLAN_S3('A', 'd', 'd'), 2);
    er_val lazy_10_v = make_done_thunk(10);
    er_val body_v = make_plan_call_expr(make_plan_call_expr(add_v, lazy_10_v), 32);
    er_val law_v = er_law_make(enki_allocator_system(), 0, body_v, 0);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    er_law* law = er_outt(er_tag_law, law_v);
    cr_assert_not_null(law);
    er_op* code = er_law_label_code(law, 0);
    cr_assert_not_null(code);

    cr_assert_eq(code[2].tag, OP_ROTATE);
    cr_assert_eq(code[2].as.u32, 2);
    cr_assert_eq(code[3].tag, OP_EVAL);
    cr_assert_eq(code[4].tag, OP_ROTATE);
    cr_assert_eq(code[4].as.u32, 2);
    cr_assert_eq(code[5].tag, OP_EVAL);
    cr_assert_eq(code[6].tag, OP_ADD);

    er_val call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(NULL, call_v), 42);
}

Test(run_vm, compiled_law_primitive_forces_arguments_to_whnf_and_increments_arity)
{
    er_val law_prim_v = make_prim_law(PLAN_S3('L', 'a', 'w'), 3);
    er_val name_v = make_done_thunk(11);
    er_val body_arg_v[] = {make_done_thunk(42)};
    er_val body_const_v = make_app_value(99, 1, body_arg_v);
    er_val law_arity_v = make_plan_call_expr(law_prim_v, make_law_quote_expr(0));
    er_val law_named_v = make_plan_call_expr(law_arity_v, name_v);
    er_val body_v = make_plan_call_expr(law_named_v, make_law_quote_expr(body_const_v));
    er_val outer_law_v = er_law_make(enki_allocator_system(), 0, body_v, 0);
    cr_assert_eq(er_get_tag(outer_law_v), er_tag_law);
    er_law* outer_law = er_outt(er_tag_law, outer_law_v);
    cr_assert_not_null(outer_law);
    er_op* code = er_law_label_code(outer_law, 0);
    cr_assert_not_null(code);

    size_t force_s = 0;
    size_t eval_s = 0;
    bool saw_law_f = false;
    for (size_t k = 0; k < 32 && code[k].tag != OP_RET; k++) {
        if (code[k].tag == OP_FORCE) {
            force_s++;
        }
        if (code[k].tag == OP_EVAL) {
            eval_s++;
        }
        if (code[k].tag == OP_LAW) {
            saw_law_f = true;
            break;
        }
    }
    cr_assert_eq(force_s, 0);
    cr_assert_eq(eval_s, 3);
    cr_assert(saw_law_f);

    er_val call_v = make_call(outer_law_v, 1);
    er_val result_v = run_vm(NULL, call_v);
    er_law* made_law = er_outt(er_tag_law, result_v);
    cr_assert_not_null(made_law);
    cr_assert_eq(made_law->ari_d, 1);
    cr_assert_eq(made_law->name_v, 11);
    cr_assert_eq(made_law->body_v, body_const_v);
    er_app* body_app = er_outt(er_tag_app, made_law->body_v);
    cr_assert_not_null(body_app);
    cr_assert_eq(body_app->h.raw.nf_f, 0);
    cr_assert_eq(body_app->arg_v[0], body_arg_v[0]);
}

Test(run_vm, compiled_elim_primitive_forces_only_scrutinee)
{
    er_val elim_v = make_prim_law(PLAN_S4('E', 'l', 'i', 'm'), 6);
    er_val bad_v = make_hole_thunk();
    er_val scrutinee_v = make_done_thunk(0);
    er_val body_v = make_plan_call_expr(
        make_plan_call_expr(
            make_plan_call_expr(
                make_plan_call_expr(
                    make_plan_call_expr(
                        make_plan_call_expr(elim_v, bad_v),
                        bad_v),
                    bad_v),
                42),
            bad_v),
        scrutinee_v);

    er_val law_v = er_law_make(enki_allocator_system(), 0, body_v, 0);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    er_law* law = er_outt(er_tag_law, law_v);
    cr_assert_not_null(law);
    er_op* code = er_law_label_code(law, 0);
    cr_assert_not_null(code);

    size_t eval_s = 0;
    bool saw_elim_f = false;
    for (size_t k = 0; k < 32 && code[k].tag != OP_RET; k++) {
        if (code[k].tag == OP_EVAL) {
            eval_s++;
        }
        if (code[k].tag == OP_ELIM) {
            saw_elim_f = true;
            break;
        }
    }
    cr_assert_eq(eval_s, 1);
    cr_assert(saw_elim_f);

    er_val call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(NULL, call_v), 42);
}

Test(run_vm, compiled_coup_primitive_forces_only_row)
{
    er_val coup_v = make_prim_law(PLAN_S4('C', 'o', 'u', 'p'), 2);
    er_val bad_v = make_hole_thunk();
    er_val row_v = make_done_thunk(0);
    er_val body_v =
        make_plan_call_expr(make_plan_call_expr(coup_v, bad_v), row_v);

    er_val law_v = er_law_make(enki_allocator_system(), 0, body_v, 0);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    er_law* law = er_outt(er_tag_law, law_v);
    cr_assert_not_null(law);
    er_op* code = er_law_label_code(law, 0);
    cr_assert_not_null(code);

    size_t eval_s = 0;
    bool saw_coup_f = false;
    for (size_t k = 0; k < 32 && code[k].tag != OP_RET; k++) {
        if (code[k].tag == OP_EVAL) {
            eval_s++;
        }
        if (code[k].tag == OP_COUP) {
            saw_coup_f = true;
            break;
        }
    }
    cr_assert_eq(eval_s, 1);
    cr_assert(saw_coup_f);
}

Test(run_vm, compiled_law_inlines_simple_primitive_wrapper)
{
    er_val add_v = make_prim_law(PLAN_S3('A', 'd', 'd'), 2);
    er_val wrapper_body_v =
        make_plan_call_expr(make_plan_call_expr(make_law_quote_expr(add_v), 1), 2);
    er_val wrapper_v = er_law_make(enki_allocator_system(), PLAN_S3('a', 'd', 'd'),
                                   wrapper_body_v, 2);
    cr_assert_eq(er_get_tag(wrapper_v), er_tag_law);

    er_val body_v = make_plan_call_expr(make_plan_call_expr(wrapper_v, 10), 32);
    er_val law_v = er_law_make(enki_allocator_system(), 0, body_v, 0);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    er_law* law = er_outt(er_tag_law, law_v);
    cr_assert_not_null(law);
    er_op* code = er_law_label_code(law, 0);
    cr_assert_not_null(code);

    cr_assert_eq(code[0].tag, OP_PUSH_LIT);
    cr_assert_eq(code[0].as.lit_v, 10);
    cr_assert_eq(code[1].tag, OP_PUSH_LIT);
    cr_assert_eq(code[1].as.lit_v, 32);
    cr_assert_eq(code[2].tag, OP_ROTATE);
    cr_assert_eq(code[2].as.u32, 2);
    cr_assert_eq(code[3].tag, OP_EVAL);
    cr_assert_eq(code[4].tag, OP_ROTATE);
    cr_assert_eq(code[4].as.u32, 2);
    cr_assert_eq(code[5].tag, OP_EVAL);
    cr_assert_eq(code[6].tag, OP_ADD);
    cr_assert_eq(code[7].tag, OP_RET);

    er_val call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(NULL, call_v), 42);
}

Test(run_vm, compiled_if_evals_condition_before_jump)
{
    er_val if_v = make_prim_law(PLAN_S2('I', 'f'), 3);
    er_val cond_v = make_done_thunk(0);
    er_val body_v =
        make_plan_call_expr(make_plan_call_expr(make_plan_call_expr(if_v, cond_v), 42), 7);
    er_val law_v = er_law_make(enki_allocator_system(), 0, body_v, 0);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    er_law* law = er_outt(er_tag_law, law_v);
    cr_assert_not_null(law);
    er_op* code = er_law_label_code(law, 0);
    cr_assert_not_null(code);

    cr_assert_eq(code[1].tag, OP_EVAL);
    cr_assert_eq(code[2].tag, OP_JUMP_IF);

    er_val call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(NULL, call_v), 7);
}

Test(run_vm, compiled_if_uses_law_local_labels)
{
    er_val if_v = make_prim_law(PLAN_S2('I', 'f'), 3);
    er_val le_v = make_prim_law(PLAN_S2('L', 'e'), 2);
    er_val cond_v = make_plan_call_expr(make_plan_call_expr(le_v, 2), 3);
    er_val body_v =
        make_plan_call_expr(make_plan_call_expr(make_plan_call_expr(if_v, cond_v), 42), 7);
    er_val law_v = er_law_make(enki_allocator_system(), 0, body_v, 0);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    er_law* law = er_outt(er_tag_law, law_v);
    cr_assert_not_null(law);
    cr_assert_eq(law->bc_s, 2);

    er_val call_v = make_call(law_v, 1);
    cr_assert_eq(run_vm(NULL, call_v), 42);
}

Test(run_vm, compiled_let_uses_suspension_label)
{
    er_val let_body_v = 7;
    er_val final_body_v = 1;
    er_val let_arg_v[] = {let_body_v, final_body_v};
    er_val body_v = make_app_value(1, 2, let_arg_v);
    er_val law_v = er_law_make(enki_allocator_system(), 0, body_v, 0);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    er_law* law = er_outt(er_tag_law, law_v);
    cr_assert_not_null(law);
    cr_assert_eq(law->let_d, 1);
    cr_assert_eq(law->bc_s, 2);

    er_val call_v = make_call(law_v, 2);
    cr_assert_eq(run_vm(NULL, call_v), 7);
}

Test(run_vm, lazy_let_suspension_preserves_self_after_frame_update)
{
    enum {
        ENTRY_START = 0,
        REST_START = 5,
    };
    er_op code[] = {
        [0] = {.tag = OP_PUSH_LIT, .as.lit_v = 0},
        [1] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [2] = {.tag = OP_PUSH_VAR, .as.slot = 2},
        [3] = {.tag = OP_MK_APP, .as.u32 = 3},
        [4] = {.tag = OP_RET},
        [5] = {.tag = OP_PUSH_SELF},
        [6] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [7] = {.tag = OP_DEC},
        [8] = {.tag = OP_CALLF, .as.u32 = 1},
        [9] = {.tag = OP_RET},
    };
    er_op* let_code_v[] = {code + REST_START};
    er_val law_v = make_law(1, code + ENTRY_START, 1, let_code_v);
    er_val call_v = make_call(law_v, 3);
    er_thk* call = er_outt(er_tag_thk, call_v);
    cr_assert_not_null(call);
    call->arg_v[1] = 2;

    er_val result_v = run_vm(code, call_v);
    er_app* row = er_outt(er_tag_app, result_v);
    cr_assert_not_null(row);
    cr_assert_eq(row->arg_s, 2);
    cr_assert_eq(row->arg_v[0], 2);

    er_val tail_v = er_eval(enki_allocator_system(), row->arg_v[1]);
    er_app* tail = er_outt(er_tag_app, tail_v);
    cr_assert_not_null(tail);
    cr_assert_eq(tail->arg_s, 2);
    cr_assert_eq(tail->arg_v[0], 1);
}

Test(run_vm, primop66_inc_promotes_max_small_nat_to_big_nat)
{
    uint64_t expected_q[] = {UINT64_C(0x8000000000000000)};

    er_val result_v = run_prim66_inc(UINT64_C(0x7fffffffffffffff));

    assert_bat_limbs(result_v, 1, expected_q);
}

Test(run_vm, primop66_inc_preserves_big_nat_limb_count_without_high_carry)
{
    uint64_t input_q[] = {
        UINT64_C(5),
        UINT64_C(0x123456789abcdef0),
    };
    uint64_t expected_q[] = {
        UINT64_C(6),
        UINT64_C(0x123456789abcdef0),
    };
    er_val input_v = make_bat(2, input_q);

    er_val result_v = run_prim66_inc(input_v);

    assert_bat_limbs(result_v, 2, expected_q);
}

Test(run_vm, primop66_inc_extends_big_nat_when_high_limb_carries)
{
    uint64_t input_q[] = {
        UINT64_MAX,
        UINT64_MAX,
    };
    uint64_t expected_q[] = {
        0,
        0,
        1,
    };
    er_val input_v = make_bat(2, input_q);

    er_val result_v = run_prim66_inc(input_v);

    assert_bat_limbs(result_v, 3, expected_q);
}

Test(run_vm, recursive_factorial_uses_bytecode_calls_and_primop_set)
{
    enum {
        BASE_PC = 24,
    };
    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [1] = {.tag = OP_EVAL},
        [2] = {.tag = OP_JUMP_IF_ZERO, .as.u32 = BASE_PC},

        [3] = {.tag = OP_PUSH_VAR, .as.slot = 0},
        [4] = {.tag = OP_PUSH_LIT, .as.lit_v = 0}, // patched to prim66
        [5] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('S', 'u', 'b')},
        [6] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [7] = {.tag = OP_EVAL},
        [8] = {.tag = OP_PUSH_LIT, .as.lit_v = 1},
        [9] = {.tag = OP_MK_APP, .as.u32 = 3},
        [10] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [11] = {.tag = OP_EVAL},

        [12] = {.tag = OP_PUSH_LIT, .as.lit_v = 0}, // patched to prim66
        [13] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('M', 'u', 'l')},
        [14] = {.tag = OP_PUSH_VAR, .as.slot = 2},
        [15] = {.tag = OP_EVAL},
        [16] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [17] = {.tag = OP_EVAL},
        [18] = {.tag = OP_MK_APP, .as.u32 = 3},
        [19] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [20] = {.tag = OP_EVAL},

        [21] = {.tag = OP_MK_CALL, .as.u32 = 3},
        [22] = {.tag = OP_EVAL},
        [23] = {.tag = OP_RET},

        [24] = {.tag = OP_PUSH_VAR, .as.slot = 2},
        [25] = {.tag = OP_EVAL},
        [26] = {.tag = OP_RET},
    };
    code[4].as.lit_v = prim66_v;
    code[12].as.lit_v = prim66_v;
    er_val fact_v = make_law_with_len(2, code, sizeof(code) / sizeof(code[0]));
    er_val call_v = make_call(fact_v, 3);
    er_thk* call = er_outt(er_tag_thk, call_v);
    call->arg_v[1] = 8;
    call->arg_v[2] = 1;

    er_val result_v = run_vm(code, call_v);
    cr_assert_eq(result_v, 40320, "got %llu", (unsigned long long)result_v);
}
