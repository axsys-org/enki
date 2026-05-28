#include "test_interp.h"
#include "enki/allocator.h"
#include "enki/app.h"
#include "enki/eval.h"
#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/nat.h"
#include "enki/op66.h"
#include "enki/pin.h"
#include "enki/value.h"
#include "enki/vector.h"

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

static enki_value make_law_with_consts(size_t arity_s, uint8_t* bc_b, size_t bc_len_s,
    enki_value* consts_v, size_t n_const_s)
{
    return enki_law_alloc(
        fixture_interp->gc, arity_s, 0, 0, bc_len_s, n_const_s, bc_b, consts_v);
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

static void stress_alloc_big_nats(size_t n)
{
    for(size_t k = 0; k < n; k++) {
        (void)enki_nat_bex(fixture_interp->gc, 70 + (k % 8));
    }
    enki_gc_collect(fixture_interp->gc);
}

static void run_until_base_frame(void)
{
    while(fixture_interp->cp > 0 && !fixture_interp->halted) {
        enki_interp_step(fixture_interp);
        enki_arena_reset(fixture_interp->scratch_a);
    }
}

TestSuite(runtime_stats, .init = setup, .fini = teardown);

Test(runtime_stats, small_nat_inc_stays_on_immediate_fast_path)
{
    enki_stats_reset(fixture_interp);

    for(enki_value k = 0; k < 1024; k++) {
        cr_assert_eq(enki_nat_inc(fixture_interp->gc, k), k + 1);
    }

    cr_log_info(
        "small inc stats: tmp_alloc=%llu tmp_bytes=%llu heap_alloc=%llu "
        "heap_bytes=%llu immediate_results=%llu big_results=%llu",
        (unsigned long long)fixture_interp->stats.nat_tmp_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_tmp_bytes_s,
        (unsigned long long)fixture_interp->stats.nat_heap_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_heap_bytes_s,
        (unsigned long long)fixture_interp->stats.nat_immediate_result_s,
        (unsigned long long)fixture_interp->stats.nat_big_result_s);

    cr_assert_eq(fixture_interp->stats.nat_tmp_alloc_s, 0);
    cr_assert_eq(fixture_interp->stats.nat_tmp_bytes_s, 0);
    cr_assert_eq(fixture_interp->stats.nat_heap_alloc_s, 0);
    cr_assert_eq(fixture_interp->stats.nat_big_result_s, 0);
}

Test(runtime_stats, big_nat_fallback_still_records_scratch_and_heap_churn)
{
    enki_value big = enki_nat_lsh(fixture_interp->gc, 1, 70);
    cr_assert(IS_PTR(big));

    enki_stats_reset(fixture_interp);
    enki_value bigger = enki_nat_inc(fixture_interp->gc, big);
    cr_assert(IS_PTR(bigger));

    cr_log_info(
        "big inc stats: tmp_alloc=%llu tmp_bytes=%llu heap_alloc=%llu "
        "heap_bytes=%llu immediate_results=%llu big_results=%llu",
        (unsigned long long)fixture_interp->stats.nat_tmp_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_tmp_bytes_s,
        (unsigned long long)fixture_interp->stats.nat_heap_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_heap_bytes_s,
        (unsigned long long)fixture_interp->stats.nat_immediate_result_s,
        (unsigned long long)fixture_interp->stats.nat_big_result_s);

    cr_assert_gt(fixture_interp->stats.nat_tmp_alloc_s, 0);
    cr_assert_gt(fixture_interp->stats.nat_heap_alloc_s, 0);
    cr_assert_gt(fixture_interp->stats.nat_big_result_s, 0);
}

Test(runtime_stats, bytecode_law_inc_records_apply_eval_and_op_counts)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value law = make_law(1, bc_b, sizeof(bc_b));

    enki_stats_reset(fixture_interp);

    fixture_interp->stack_v[0] = law;
    fixture_interp->stack_v[1] = 41;
    fixture_interp->sp = 2;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_app_apply(fixture_interp, 1);
    run_until_base_frame();

    cr_log_info(
        "law inc stats: steps=%llu law_enter=%llu apply=%llu exact=%llu "
        "op66_inc=%llu nat_tmp_alloc=%llu",
        (unsigned long long)fixture_interp->stats.interp_step_s,
        (unsigned long long)fixture_interp->stats.law_enter_s,
        (unsigned long long)fixture_interp->stats.apply_s,
        (unsigned long long)fixture_interp->stats.apply_exact_s,
        (unsigned long long)fixture_interp->stats.op66_s[OP66_INC],
        (unsigned long long)fixture_interp->stats.nat_tmp_alloc_s);

    cr_assert_eq(fixture_interp->stack_v[0], 42);
    cr_assert_eq(fixture_interp->stats.apply_exact_s, 1);
    cr_assert_eq(fixture_interp->stats.law_enter_s, 1);
    cr_assert_gt(fixture_interp->stats.interp_step_s, 0);
    cr_assert_eq(fixture_interp->stats.op66_s[OP66_INC], 1);
    cr_assert_eq(fixture_interp->stats.nat_tmp_alloc_s, 0);
}

Test(runtime_stats, app_apply_shape_surface_reports_under_exact_and_over)
{
    uint8_t add_bc[] = {
        OP_PICK, 0,
        OP_PICK, 1,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    enki_value add = make_law(2, add_bc, sizeof(add_bc));

    uint8_t inc_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value inc = make_law(1, inc_bc, sizeof(inc_bc));

    uint8_t id_bc[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    enki_value id = make_law(1, id_bc, sizeof(id_bc));

    enki_stats_reset(fixture_interp);

    fixture_interp->stack_v[0] = add;
    fixture_interp->stack_v[1] = 20;
    fixture_interp->sp = 2;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;
    enki_app_apply(fixture_interp, 1);

    enki_value partial_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(partial_v));
    cr_assert_eq(ENKI_AS(enki_app, partial_v)->n_args_s, 1);

    fixture_interp->stack_v[0] = partial_v;
    fixture_interp->stack_v[1] = 22;
    fixture_interp->sp = 2;
    enki_app_apply(fixture_interp, 1);
    run_until_base_frame();
    cr_assert_eq(fixture_interp->stack_v[0], 42);

    fixture_interp->stack_v[0] = id;
    fixture_interp->stack_v[1] = inc;
    fixture_interp->stack_v[2] = 41;
    fixture_interp->sp = 3;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;
    enki_app_apply(fixture_interp, 2);
    enki_value thunk_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(thunk_v));
    cr_assert_eq(ENKI_AS(enki_value_header, thunk_v)->state_b, THUNK);

    fixture_interp->stack_v[0] = thunk_v;
    fixture_interp->sp = 1;
    enki_value forced_v = enki_eval_whnf(fixture_interp, thunk_v);
    cr_assert_eq(forced_v, 42);

    cr_log_info(
        "apply shape stats: apply=%llu row=%llu exact=%llu under=%llu over=%llu "
        "law_enter=%llu steps=%llu whnf=%llu thunk_forces=%llu nat_heap=%llu",
        (unsigned long long)fixture_interp->stats.apply_s,
        (unsigned long long)fixture_interp->stats.apply_row_s,
        (unsigned long long)fixture_interp->stats.apply_exact_s,
        (unsigned long long)fixture_interp->stats.apply_under_s,
        (unsigned long long)fixture_interp->stats.apply_over_s,
        (unsigned long long)fixture_interp->stats.law_enter_s,
        (unsigned long long)fixture_interp->stats.interp_step_s,
        (unsigned long long)fixture_interp->stats.whnf_s,
        (unsigned long long)fixture_interp->stats.whnf_app_thunk_s,
        (unsigned long long)fixture_interp->stats.nat_heap_alloc_s);

    cr_assert_geq(fixture_interp->stats.apply_under_s, 1);
    cr_assert_geq(fixture_interp->stats.apply_exact_s, 2);
    cr_assert_geq(fixture_interp->stats.apply_over_s, 1);
    cr_assert_eq(fixture_interp->stats.nat_heap_alloc_s, 0);
    cr_assert_lt(fixture_interp->stats.apply_s, 32);
    cr_assert_lt(fixture_interp->stats.interp_step_s, 64);
}

Test(runtime_stats, op66_immediate_nat_surface_stays_off_nat_allocators)
{
    enum {
        C_ZERO,
        C_ONE,
        C_TWO,
        C_THREE,
        C_FOUR,
        C_FIVE,
        C_SEVEN,
        C_EIGHT,
        C_NINE,
        C_TEN,
        C_FIFTEEN,
        C_SIXTEEN,
        C_THIRTY_TWO,
        C_BYTE,
        C_WORD,
        C_DONE,
        C_COUNT,
    };
    enki_value consts_v[C_COUNT] = {
        [C_ZERO] = 0,
        [C_ONE] = 1,
        [C_TWO] = 2,
        [C_THREE] = 3,
        [C_FOUR] = 4,
        [C_FIVE] = 5,
        [C_SEVEN] = 7,
        [C_EIGHT] = 8,
        [C_NINE] = 9,
        [C_TEN] = 10,
        [C_FIFTEEN] = 15,
        [C_SIXTEEN] = 16,
        [C_THIRTY_TWO] = 32,
        [C_BYTE] = 0xAB,
        [C_WORD] = 0x1234,
        [C_DONE] = 42,
    };

    uint8_t bc_b[256];
    size_t pc = 0;
#define EMIT(b) bc_b[pc++] = (uint8_t)(b)
#define PUSH(c) do { EMIT(OP_PUSH_CONST); EMIT(c); } while(0)
#define OP1(op, a) do { PUSH(a); EMIT(OP_OP66); EMIT(op); EMIT(OP_POP); } while(0)
#define OP2(op, a, b) do { PUSH(a); PUSH(b); EMIT(OP_OP66); EMIT(op); EMIT(OP_POP); } while(0)
#define OP3(op, a, b, c) do { PUSH(a); PUSH(b); PUSH(c); EMIT(OP_OP66); EMIT(op); EMIT(OP_POP); } while(0)

    OP1(OP66_INC, C_FIVE);
    OP1(OP66_DEC, C_FIVE);
    OP2(OP66_ADD, C_FIVE, C_SEVEN);
    OP2(OP66_SUB, C_TEN, C_THREE);
    OP2(OP66_MUL, C_SEVEN, C_EIGHT);
    OP2(OP66_DIV, C_THIRTY_TWO, C_FOUR);
    OP2(OP66_MOD, C_THIRTY_TWO, C_FIVE);
    OP2(OP66_EQ, C_FIVE, C_FIVE);
    OP2(OP66_NE, C_FIVE, C_SEVEN);
    OP2(OP66_LT, C_FIVE, C_SEVEN);
    OP2(OP66_LE, C_FIVE, C_FIVE);
    OP2(OP66_GT, C_SEVEN, C_FIVE);
    OP2(OP66_GE, C_FIVE, C_FIVE);
    OP2(OP66_CMP, C_SEVEN, C_FIVE);
    OP2(OP66_RSH, C_THIRTY_TWO, C_TWO);
    OP2(OP66_LSH, C_FIVE, C_THREE);
    OP2(OP66_TEST, C_TWO, C_FIVE);
    OP2(OP66_SET, C_THREE, C_FIVE);
    OP2(OP66_CLEAR, C_TWO, C_SEVEN);
    OP1(OP66_BEX, C_FIVE);
    OP1(OP66_BITS, C_WORD);
    OP1(OP66_BYTES, C_WORD);
    OP2(OP66_NIB, C_ONE, C_BYTE);
    OP2(OP66_LOAD8, C_ONE, C_WORD);
    OP3(OP66_STORE8, C_ONE, C_BYTE, C_WORD);
    OP2(OP66_TRUNC, C_EIGHT, C_WORD);
    OP1(OP66_TRUNC8, C_WORD);
    OP1(OP66_TRUNC16, C_WORD);
    OP1(OP66_TRUNC32, C_WORD);
    OP1(OP66_TRUNC64, C_WORD);
    PUSH(C_DONE);
    EMIT(OP_RETURN);

#undef OP3
#undef OP2
#undef OP1
#undef PUSH
#undef EMIT

    enki_value law = make_law_with_consts(0, bc_b, pc, consts_v, C_COUNT);

    enki_stats_reset(fixture_interp);

    fixture_interp->stack_v[0] = law;
    fixture_interp->sp = 1;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_law_enter(0, law, fixture_interp);
    run_until_base_frame();

    cr_log_info(
        "op66 immediate surface stats: steps=%llu nat_tmp=%llu nat_heap=%llu "
        "add=%llu mul=%llu bits=%llu store8=%llu",
        (unsigned long long)fixture_interp->stats.interp_step_s,
        (unsigned long long)fixture_interp->stats.nat_tmp_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_heap_alloc_s,
        (unsigned long long)fixture_interp->stats.op66_s[OP66_ADD],
        (unsigned long long)fixture_interp->stats.op66_s[OP66_MUL],
        (unsigned long long)fixture_interp->stats.op66_s[OP66_BITS],
        (unsigned long long)fixture_interp->stats.op66_s[OP66_STORE8]);

    cr_assert_eq(fixture_interp->stack_v[0], 42);
    cr_assert_eq(fixture_interp->stats.nat_tmp_alloc_s, 0);
    cr_assert_eq(fixture_interp->stats.nat_heap_alloc_s, 0);
    cr_assert_eq(fixture_interp->stats.op66_s[OP66_INC], 1);
    cr_assert_eq(fixture_interp->stats.op66_s[OP66_STORE8], 1);
    cr_assert_lt(fixture_interp->stats.interp_step_s, 256);
}

Test(runtime_stats, row_structural_ops_survive_gc_pressure)
{
    enki_value a = app1(10, 11);
    enki_value b = app1(20, 21);
    enki_value c = app1(30, 31);
    enki_value xs = app2(0, c, 0);
    xs = app2(0, b, xs);
    xs = app2(0, a, xs);

    enki_stats_reset(fixture_interp);

    fixture_interp->stack_v[0] = 99;
    fixture_interp->stack_v[1] = 3;
    fixture_interp->stack_v[2] = xs;
    fixture_interp->sp = 3;
    stress_alloc_big_nats(48);
    op66_row(fixture_interp);
    stress_alloc_big_nats(48);

    enki_value row_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(row_v));
    enki_app* row = ENKI_AS(enki_app, row_v);
    cr_assert_eq(row->fn_v, 99);
    cr_assert_eq(row->n_args_s, 3);
    assert_leaf(row->args_v[0], 10, 11);
    assert_leaf(row->args_v[1], 20, 21);
    assert_leaf(row->args_v[2], 30, 31);

    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 2;
    fixture_interp->stack_v[2] = row_v;
    fixture_interp->sp = 3;
    op66_slice(fixture_interp);
    fixture_interp->stack_v[1] = row_v;
    fixture_interp->sp = 2;
    stress_alloc_big_nats(48);

    enki_value slice_v = fixture_interp->stack_v[0];
    row_v = fixture_interp->stack_v[1];
    cr_assert(IS_PTR(slice_v));
    enki_app* slice = ENKI_AS(enki_app, slice_v);
    cr_assert_eq(slice->n_args_s, 2);
    assert_leaf(slice->args_v[0], 20, 21);
    assert_leaf(slice->args_v[1], 30, 31);

    fixture_interp->stack_v[0] = row_v;
    fixture_interp->stack_v[1] = slice_v;
    fixture_interp->sp = 2;
    op66_weld(fixture_interp);
    stress_alloc_big_nats(48);

    enki_value welded_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(welded_v));
    enki_app* welded = ENKI_AS(enki_app, welded_v);
    cr_assert_eq(welded->n_args_s, 5);
    assert_leaf(welded->args_v[0], 10, 11);
    assert_leaf(welded->args_v[3], 20, 21);

    a = app1(10, 11);
    b = app1(20, 21);
    c = app1(30, 31);
    enki_value d = app1(40, 41);
    row_v = app3(0, a, b, c);
    fixture_interp->stack_v[0] = 2;
    fixture_interp->stack_v[1] = d;
    fixture_interp->stack_v[2] = row_v;
    fixture_interp->sp = 3;
    op66_up(fixture_interp);
    stress_alloc_big_nats(48);

    enki_value updated_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(updated_v));
    enki_app* updated = ENKI_AS(enki_app, updated_v);
    cr_assert_eq(updated->n_args_s, 3);
    assert_leaf(updated->args_v[0], 10, 11);
    assert_leaf(updated->args_v[1], 20, 21);
    assert_leaf(updated->args_v[2], 40, 41);

    cr_log_info(
        "row structural gc pressure stats: nat_tmp=%llu nat_heap=%llu "
        "nat_heap_bytes=%llu gc_collect=%llu gc_copy=%llu gc_live=%llu "
        "gc_high=%llu final_sp=%zu",
        (unsigned long long)fixture_interp->stats.nat_tmp_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_heap_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_heap_bytes_s,
        (unsigned long long)fixture_interp->stats.gc_collect_s,
        (unsigned long long)fixture_interp->stats.gc_copy_s,
        (unsigned long long)fixture_interp->stats.gc_live_bytes_s,
        (unsigned long long)fixture_interp->stats.gc_high_water_bytes_s,
        fixture_interp->sp);

    cr_assert_gt(fixture_interp->stats.nat_heap_alloc_s, 0);
    cr_assert_lt(fixture_interp->stats.nat_heap_bytes_s, 64 * 1024);
}

Test(runtime_stats, eval_whnf_ind_cache_prevents_second_thunk_apply)
{
    uint8_t bc_b[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value law = make_law(1, bc_b, sizeof(bc_b));
    enki_value thunk_v = enki_app_alloc(fixture_interp->gc, law, 1);
    enki_app* thunk = ENKI_AS(enki_app, thunk_v);
    thunk->h.state_b = THUNK;
    thunk->args_v[0] = 41;

    enki_stats_reset(fixture_interp);

    enki_value first_v = enki_eval_whnf(fixture_interp, thunk_v);
    uint64_t first_whnf_s = fixture_interp->stats.whnf_s;
    uint64_t first_apply_s = fixture_interp->stats.apply_s;
    uint64_t first_thunk_s = fixture_interp->stats.whnf_app_thunk_s;
    uint64_t first_steps_s = fixture_interp->stats.interp_step_s;

    thunk = ENKI_AS(enki_app, thunk_v);
    cr_assert_eq(first_v, 42);
    cr_assert_eq(thunk->h.kind_b, ENKI_IND);
    cr_assert_eq(thunk->fn_v, 42);
    cr_assert_gt(first_apply_s, 0);
    cr_assert_gt(first_steps_s, 0);
    cr_assert_eq(first_thunk_s, 1);

    enki_value second_v = enki_eval_whnf(fixture_interp, thunk_v);
    uint64_t second_whnf_delta_s = fixture_interp->stats.whnf_s - first_whnf_s;
    uint64_t second_apply_delta_s = fixture_interp->stats.apply_s - first_apply_s;
    uint64_t second_step_delta_s = fixture_interp->stats.interp_step_s - first_steps_s;
    uint64_t second_thunk_delta_s = fixture_interp->stats.whnf_app_thunk_s - first_thunk_s;

    cr_log_info(
        "eval thunk cache stats: first_whnf=%llu first_apply=%llu first_steps=%llu "
        "second_whnf_delta=%llu second_apply_delta=%llu second_step_delta=%llu "
        "second_thunk_delta=%llu",
        (unsigned long long)first_whnf_s,
        (unsigned long long)first_apply_s,
        (unsigned long long)first_steps_s,
        (unsigned long long)second_whnf_delta_s,
        (unsigned long long)second_apply_delta_s,
        (unsigned long long)second_step_delta_s,
        (unsigned long long)second_thunk_delta_s);

    cr_assert_eq(second_v, 42);
    cr_assert_eq(second_apply_delta_s, 0);
    cr_assert_eq(second_step_delta_s, 0);
    cr_assert_eq(second_thunk_delta_s, 0);
    cr_assert_leq(second_whnf_delta_s, 2);
}

Test(runtime_stats, eval_nf_pin_law_app_surface_forces_hashes_and_caches)
{
    uint8_t inc_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value inc = make_law(1, inc_bc, sizeof(inc_bc));

    inc = make_law(1, inc_bc, sizeof(inc_bc));
    enki_value inner_v = enki_app_alloc(fixture_interp->gc, inc, 1);
    enki_app* inner = ENKI_AS(enki_app, inner_v);
    inner->h.state_b = THUNK;
    inner->args_v[0] = 41;

    enki_value subpin_v = enki_app_alloc(fixture_interp->gc, inc, 1);
    enki_app* subpin_app = ENKI_AS(enki_app, subpin_v);
    subpin_app->h.state_b = THUNK;
    subpin_app->args_v[0] = 9;

    uint8_t hash_b[32] = {0};
    enki_value subpins_v[] = { subpin_v };
    enki_value pin_v = enki_pin_alloc(fixture_interp->gc, hash_b, inner_v, 1, subpins_v);

    enki_stats_reset(fixture_interp);

    enki_value result_v = enki_eval_nf(fixture_interp, pin_v);
    enki_pin* pin = ENKI_AS(enki_pin, result_v);

    cr_log_info(
        "eval nf pin/law/app stats: whnf=%llu thunk_forces=%llu apply=%llu "
        "exact=%llu steps=%llu law_enter=%llu nat_tmp=%llu nat_heap=%llu",
        (unsigned long long)fixture_interp->stats.whnf_s,
        (unsigned long long)fixture_interp->stats.whnf_app_thunk_s,
        (unsigned long long)fixture_interp->stats.apply_s,
        (unsigned long long)fixture_interp->stats.apply_exact_s,
        (unsigned long long)fixture_interp->stats.interp_step_s,
        (unsigned long long)fixture_interp->stats.law_enter_s,
        (unsigned long long)fixture_interp->stats.nat_tmp_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_heap_alloc_s);

    cr_assert_eq(result_v, pin_v);
    cr_assert_eq(pin->h.state_b, NF);
    cr_assert_eq(pin->inner_v, 42);
    cr_assert_eq(pin->subpins_v[0], 10);
    cr_assert_eq(fixture_interp->stats.nat_heap_alloc_s, 0);
    cr_assert_lt(fixture_interp->stats.whnf_s, 32);
    cr_assert_lt(fixture_interp->stats.apply_s, 16);
}

Test(runtime_stats, eval_bytecode_factorial_uses_eval_whnf_without_exploding)
{
    uint8_t id_bc[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    enki_value id = make_law(1, id_bc, sizeof(id_bc));

    uint8_t dec_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_DEC,
        OP_RETURN,
    };
    enki_value dec = make_law(1, dec_bc, sizeof(dec_bc));

    uint8_t else_bc[] = {
        OP_PUSH_CONST, 0,          // fac
        OP_PUSH_CONST, 1,          // dec
        OP_PICK, 0,                // n
        OP_APPLY, 1,               // dec n
        OP_APPLY, 1,               // fac (dec n)
        OP_OP66, OP66_FORCE,       // force recursive result
        OP_PICK, 0,                // n
        OP_OP66, OP66_MUL,
        OP_RETURN,
    };
    enki_value else_consts[] = {0, dec};
    enki_value else_law = make_law_with_consts(1, else_bc, sizeof(else_bc), else_consts, 2);

    uint8_t fac_bc[] = {
        OP_PICK, 0,
        OP_PUSH_CONST, 0,          // 0
        OP_OP66, OP66_EQ,
        OP_PUSH_CONST, 1,          // 1
        OP_PUSH_CONST, 2,          // id
        OP_PUSH_CONST, 3,          // else law
        OP_PICK, 0,                // n
        OP_APPLY, 2,               // thunk: id else n
        OP_OP66, OP66_IF,
        OP_RETURN,
    };
    enki_value fac_consts[] = {0, 1, id, else_law};
    enki_value fac = make_law_with_consts(1, fac_bc, sizeof(fac_bc), fac_consts, 4);
    ENKI_LAW_CONSTS(ENKI_AS(enki_law, else_law))[0] = fac;

    enki_stats_reset(fixture_interp);

    fixture_interp->stack_v[0] = fac;
    fixture_interp->stack_v[1] = 8;
    fixture_interp->sp = 2;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;

    enki_app_apply(fixture_interp, 1);
    run_until_base_frame();
    enki_value result_v = enki_eval_whnf(fixture_interp, fixture_interp->stack_v[0]);

    cr_log_info(
        "eval fac 8 stats: result=%llu whnf=%llu whnf_thunk=%llu apply=%llu "
        "exact=%llu under=%llu over=%llu steps=%llu law_enter=%llu op66_force=%llu "
        "op66_mul=%llu nat_tmp=%llu nat_heap=%llu",
        (unsigned long long)result_v,
        (unsigned long long)fixture_interp->stats.whnf_s,
        (unsigned long long)fixture_interp->stats.whnf_app_thunk_s,
        (unsigned long long)fixture_interp->stats.apply_s,
        (unsigned long long)fixture_interp->stats.apply_exact_s,
        (unsigned long long)fixture_interp->stats.apply_under_s,
        (unsigned long long)fixture_interp->stats.apply_over_s,
        (unsigned long long)fixture_interp->stats.interp_step_s,
        (unsigned long long)fixture_interp->stats.law_enter_s,
        (unsigned long long)fixture_interp->stats.op66_s[OP66_FORCE],
        (unsigned long long)fixture_interp->stats.op66_s[OP66_MUL],
        (unsigned long long)fixture_interp->stats.nat_tmp_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_heap_alloc_s);

    cr_assert_eq(result_v, 40320);
    cr_assert_lt(fixture_interp->stats.whnf_s, 10000);
    cr_assert_lt(fixture_interp->stats.apply_s, 10000);
    cr_assert_eq(fixture_interp->stats.nat_heap_alloc_s, 0);
}

Test(runtime_stats, full_surface_one_shot_reports_runtime_oom_perf)
{
    enki_stats_reset(fixture_interp);

    enki_value quote_v = app1(0, 42);
    enki_value body_v = app2(1, quote_v, 1);
    enki_value compiled_law = compile_body(body_v);

    fixture_interp->stack_v[0] = compiled_law;
    fixture_interp->sp = 1;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;
    enki_law_enter(0, compiled_law, fixture_interp);
    run_until_base_frame();
    cr_assert_eq(fixture_interp->stack_v[0], 42);
    cr_log_info("FULL_SURFACE checkpoint: compiler/law");

    uint8_t inc_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_RETURN,
    };
    enki_value inc = make_law(1, inc_bc, sizeof(inc_bc));

    fixture_interp->stack_v[0] = inc;
    fixture_interp->stack_v[1] = 41;
    fixture_interp->sp = 2;
    fixture_interp->cp = 0;
    fixture_interp->halted = false;
    enki_app_apply(fixture_interp, 1);
    run_until_base_frame();
    cr_assert_eq(fixture_interp->stack_v[0], 42);
    cr_log_info("FULL_SURFACE checkpoint: bytecode apply");

    enki_value thunk_v = enki_app_alloc(fixture_interp->gc, inc, 1);
    enki_app* thunk = ENKI_AS(enki_app, thunk_v);
    thunk->h.state_b = THUNK;
    thunk->args_v[0] = 41;
    enki_value first_v = enki_eval_whnf(fixture_interp, thunk_v);
    uint64_t apply_after_first_force_s = fixture_interp->stats.apply_s;
    uint64_t steps_after_first_force_s = fixture_interp->stats.interp_step_s;
    enki_value second_v = enki_eval_whnf(fixture_interp, thunk_v);
    cr_assert_eq(first_v, 42);
    cr_assert_eq(second_v, 42);
    cr_assert_eq(fixture_interp->stats.apply_s, apply_after_first_force_s);
    cr_assert_eq(fixture_interp->stats.interp_step_s, steps_after_first_force_s);
    cr_log_info("FULL_SURFACE checkpoint: thunk cache");

    enki_value row_v = app3(7, 10, 20, 30);
    fixture_interp->stack_v[0] = row_v;
    fixture_interp->sp = 1;
    op66_type(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 3);

    fixture_interp->stack_v[0] = row_v;
    fixture_interp->sp = 1;
    op66_hd(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 7);

    fixture_interp->stack_v[0] = row_v;
    fixture_interp->sp = 1;
    op66_ix2(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 30);

    fixture_interp->stack_v[0] = 2;
    fixture_interp->stack_v[1] = row_v;
    fixture_interp->stack_v[2] = 99;
    fixture_interp->sp = 3;
    op66_case(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 30);

    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 44;
    fixture_interp->stack_v[2] = 55;
    fixture_interp->sp = 3;
    op66_if(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 44);
    cr_log_info("FULL_SURFACE checkpoint: op66 shape/bool");

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
    stress_alloc_big_nats(32);
    op66_row(fixture_interp);
    stress_alloc_big_nats(32);

    row_v = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(row_v));
    enki_app* row = ENKI_AS(enki_app, row_v);
    cr_assert_eq(row->n_args_s, 3);
    assert_leaf(row->args_v[0], 10, 11);
    assert_leaf(row->args_v[2], 30, 31);
    cr_log_info("FULL_SURFACE checkpoint: row/gc pressure");

    inc = make_law(1, inc_bc, sizeof(inc_bc));
    size_t pin_root_s = fixture_interp->sp;
    fixture_interp->stack_v[pin_root_s] = inc;
    fixture_interp->sp = pin_root_s + 1;
    enki_value inner_v = enki_app_alloc(fixture_interp->gc, fixture_interp->stack_v[pin_root_s], 1);
    fixture_interp->stack_v[pin_root_s + 1] = inner_v;
    fixture_interp->sp = pin_root_s + 2;
    enki_app* inner = ENKI_AS(enki_app, inner_v);
    inner->h.state_b = THUNK;
    inner->args_v[0] = 9;
    uint8_t hash_b[32] = {0};
    enki_value pin_v = enki_pin_alloc(fixture_interp->gc, hash_b, fixture_interp->stack_v[pin_root_s + 1], 0, NULL);
    fixture_interp->stack_v[pin_root_s] = pin_v;
    fixture_interp->sp = pin_root_s + 1;
    enki_value nf_pin_v = enki_eval_nf(fixture_interp, pin_v);
    cr_assert_eq(nf_pin_v, pin_v);
    cr_assert_eq(ENKI_AS(enki_pin, nf_pin_v)->inner_v, 10);
    cr_log_info("FULL_SURFACE checkpoint: pin nf");

    fixture_interp->sp = 0;
    enki_value big_a = enki_nat_bex(fixture_interp->gc, 70);
    fixture_interp->stack_v[0] = big_a;
    fixture_interp->sp = 1;
    enki_value big_b = enki_nat_bex(fixture_interp->gc, 69);
    big_a = fixture_interp->stack_v[0];
    fixture_interp->stack_v[0] = big_a;
    fixture_interp->stack_v[1] = big_b;
    fixture_interp->sp = 2;
    op66_add(fixture_interp);
    enki_value big_sum = fixture_interp->stack_v[0];
    cr_assert(IS_PTR(big_sum));
    cr_assert_eq(enki_nat_bits(fixture_interp->gc, big_sum), 71);

    cr_log_info(
        "FULL_SURFACE oom-perf: steps=%llu law_enter=%llu apply=%llu exact=%llu "
        "under=%llu over=%llu whnf=%llu thunk_forces=%llu op66_inc=%llu "
        "op66_add=%llu nat_tmp=%llu nat_tmp_bytes=%llu nat_heap=%llu "
        "nat_heap_bytes=%llu gc_alloc=%llu gc_locked=%llu gc_collect=%llu "
        "gc_copy=%llu gc_live=%llu gc_high=%llu sp=%zu",
        (unsigned long long)fixture_interp->stats.interp_step_s,
        (unsigned long long)fixture_interp->stats.law_enter_s,
        (unsigned long long)fixture_interp->stats.apply_s,
        (unsigned long long)fixture_interp->stats.apply_exact_s,
        (unsigned long long)fixture_interp->stats.apply_under_s,
        (unsigned long long)fixture_interp->stats.apply_over_s,
        (unsigned long long)fixture_interp->stats.whnf_s,
        (unsigned long long)fixture_interp->stats.whnf_app_thunk_s,
        (unsigned long long)fixture_interp->stats.op66_s[OP66_INC],
        (unsigned long long)fixture_interp->stats.op66_s[OP66_ADD],
        (unsigned long long)fixture_interp->stats.nat_tmp_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_tmp_bytes_s,
        (unsigned long long)fixture_interp->stats.nat_heap_alloc_s,
        (unsigned long long)fixture_interp->stats.nat_heap_bytes_s,
        (unsigned long long)fixture_interp->stats.gc_alloc_s,
        (unsigned long long)fixture_interp->stats.gc_locked_alloc_s,
        (unsigned long long)fixture_interp->stats.gc_collect_s,
        (unsigned long long)fixture_interp->stats.gc_copy_s,
        (unsigned long long)fixture_interp->stats.gc_live_bytes_s,
        (unsigned long long)fixture_interp->stats.gc_high_water_bytes_s,
        fixture_interp->sp);

    cr_assert_lt(fixture_interp->stats.interp_step_s, 256);
    cr_assert_lt(fixture_interp->stats.apply_s, 32);
    cr_assert_lt(fixture_interp->stats.whnf_s, 32);
    cr_assert_lt(fixture_interp->stats.nat_heap_bytes_s, 32 * 1024);
    cr_assert_lt(fixture_interp->stats.gc_high_water_bytes_s, 64 * 1024);
}
