#include "test_interp.h"
#include "enki/app.h"
#include "enki/eval.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/nat.h"
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
