#include "test_interp.h"

#include "enki/allocator.h"
#include "enki/app.h"
#include "enki/eval.h"
#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/nat.h"
#include "enki/op66.h"
#include "enki/profile.h"
#include "enki/run.h"
#include "enki/value.h"
#include "enki/wisp.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char* name_c;
    const char* axis_c;
    const char* catches_c;
} profile_workload_info;

static const profile_workload_info workload_info_v[] = {
    {"bytecode_fac", "bytecode recursion + apply/eval + force",
     "call-frame churn, WHNF forcing, thunk cache misses, small nat ops"},
    {"planvm_fac", "PLAN VM recursive factorial using %66 primop-set calls",
     "PLAN VM call frames, direct bytecode recursion, primitive Sub/Mul dispatch"},
    {"bytecode_tinyops", "bytecode tiny primitive loop",
     "dispatch overhead when nat ops are immediate fast paths"},
    {"bytecode_thunk_fresh", "fresh oversaturated thunk each iteration",
     "THUNK creation, force path, leftover argument handling"},
    {"bytecode_thunk_cached", "same oversaturated thunk repeatedly forced",
     "IND/cache-hit cost after the first force"},
    {"nat_big", "direct bignat arithmetic",
     "GMP/scratch/gc behavior without interpreter dispatch noise"},
    {"op66_rows", "direct row-building op66 surface",
     "row allocation, slice/weld/up/ix object churn"},
    {"wisp_plan_fib", "Wisp parser/macro/compiler + Plan evaluator recursion",
     "the current Wisp path, which does not use bytecode eval.c"},
};

static double now_s(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
    return (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
}

static enki_value make_law(enki_interpreter* i, size_t arity_s, uint8_t* bc_b, size_t bc_len_s)
{
    return enki_law_alloc(i->gc, arity_s, 0, 0, bc_len_s, 0, bc_b, NULL);
}

static enki_value make_law_with_consts(enki_interpreter* i, size_t arity_s, uint8_t* bc_b,
    size_t bc_len_s, enki_value* consts_v, size_t n_const_s)
{
    return enki_law_alloc(i->gc, arity_s, 0, 0, bc_len_s, n_const_s, bc_b, consts_v);
}

static void run_until_base_frame(enki_interpreter* i)
{
    while(i->cp > 0 && !i->halted) {
        enki_interp_step(i);
        enki_arena_reset(i->scratch_a);
    }
}

static enki_value build_add_law(enki_interpreter* i)
{
    uint8_t add_bc[] = {
        OP_PICK, 0,
        OP_PICK, 1,
        OP_OP66, OP66_ADD,
        OP_RETURN,
    };
    return make_law(i, 2, add_bc, sizeof(add_bc));
}

static enki_value build_tinyops_law(enki_interpreter* i)
{
    uint8_t bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_INC,
        OP_PUSH_CONST, 0,
        OP_OP66, OP66_ADD,
        OP_PUSH_CONST, 1,
        OP_OP66, OP66_MUL,
        OP_PUSH_CONST, 2,
        OP_OP66, OP66_SUB,
        OP_PUSH_CONST, 3,
        OP_OP66, OP66_TRUNC,
        OP_RETURN,
    };
    enki_value consts_v[] = {3, 5, 7, 63};
    return make_law_with_consts(i, 1, bc, sizeof(bc), consts_v, 4);
}

static enki_value build_fac(enki_interpreter* i)
{
    uint8_t id_bc[] = {
        OP_PICK, 0,
        OP_RETURN,
    };
    enki_value id = make_law(i, 1, id_bc, sizeof(id_bc));

    uint8_t dec_bc[] = {
        OP_PICK, 0,
        OP_OP66, OP66_DEC,
        OP_RETURN,
    };
    enki_value dec = make_law(i, 1, dec_bc, sizeof(dec_bc));

    uint8_t else_bc[] = {
        OP_PUSH_CONST, 0,
        OP_PUSH_CONST, 1,
        OP_PICK, 0,
        OP_APPLY, 1,
        OP_APPLY, 1,
        OP_OP66, OP66_FORCE,
        OP_PICK, 0,
        OP_OP66, OP66_MUL,
        OP_RETURN,
    };
    enki_value else_consts[] = {0, dec};
    enki_value else_law = make_law_with_consts(i, 1, else_bc, sizeof(else_bc), else_consts, 2);

    uint8_t fac_bc[] = {
        OP_PICK, 0,
        OP_PUSH_CONST, 0,
        OP_OP66, OP66_EQ,
        OP_PUSH_CONST, 1,
        OP_PUSH_CONST, 2,
        OP_PUSH_CONST, 3,
        OP_PICK, 0,
        OP_APPLY, 2,
        OP_OP66, OP66_IF,
        OP_RETURN,
    };
    enki_value fac_consts[] = {0, 1, id, else_law};
    enki_value fac = make_law_with_consts(i, 1, fac_bc, sizeof(fac_bc), fac_consts, 4);
    ENKI_LAW_CONSTS(ENKI_AS(enki_law, else_law))[0] = fac;
    return fac;
}

static enki_value run_law1(enki_interpreter* i, enki_value law_v, enki_value arg_v)
{
    i->stack_v[0] = law_v;
    i->stack_v[1] = arg_v;
    i->sp = 2;
    i->cp = 0;
    i->hp = 0;
    i->halted = false;

    enki_app_apply(i, 1);
    run_until_base_frame(i);
    return i->stack_v[0];
}

static enki_value run_fac(enki_interpreter* i, enki_value fac_v, enki_value n_v)
{
    ENKI_PROFILE_ZONE("profile_run_bytecode_fac");
    return enki_eval_whnf(i, run_law1(i, fac_v, n_v));
}

typedef struct {
    uint8_t* data_b;
    size_t off_s;
    size_t cap_s;
    enki_allocator allocator;
} planvm_arena;

typedef struct {
    er_op code_v[27];
    er_val fact_v;
    er_val prim66_v;
} planvm_fac_program;

static void* planvm_arena_alloc(void* ctx, size_t size_s)
{
    planvm_arena* arena = (planvm_arena*)ctx;
    size_s = (size_s + 7u) & ~(size_t)7u;
    if (arena->off_s > arena->cap_s || size_s > arena->cap_s - arena->off_s) {
        return NULL;
    }
    void* ptr = arena->data_b + arena->off_s;
    arena->off_s += size_s;
    return ptr;
}

static void planvm_arena_free(void* ctx, void* ptr)
{
    (void)ctx;
    (void)ptr;
}

static bool planvm_arena_init(planvm_arena* arena, size_t cap_s)
{
    arena->data_b = malloc(cap_s);
    if (arena->data_b == NULL) {
        return false;
    }
    arena->off_s = 0;
    arena->cap_s = cap_s;
    arena->allocator = (enki_allocator){
        .ctx = arena,
        .alloc = planvm_arena_alloc,
        .realloc = NULL,
        .free = planvm_arena_free,
    };
    return true;
}

static void planvm_arena_reset(planvm_arena* arena)
{
    arena->off_s = 0;
}

static void planvm_arena_destroy(planvm_arena* arena)
{
    free(arena->data_b);
    arena->data_b = NULL;
    arena->off_s = 0;
    arena->cap_s = 0;
}

static er_val planvm_make_prim66(void)
{
    er_pin* pin = er_pin_alloc(enki_allocator_system(), 0);
    if (pin == NULL) {
        return 0;
    }
    return er_pin_init(pin, NULL, 66, 0, NULL);
}

static er_val planvm_make_law(uint32_t arity_d, uint32_t start_d, uint32_t frame_d)
{
    er_law* law = er_law_alloc(enki_allocator_system(), 0);
    if (law == NULL) {
        return 0;
    }
    er_val law_v = er_law_init(law, 0, 0, arity_d, frame_d, 0, NULL);
    if (law_v != 0) {
        law->start_d = start_d;
    }
    return law_v;
}

static er_val planvm_make_call2(const enki_allocator* allocator, er_val fun_v, er_val a_v,
    er_val b_v)
{
    er_val args_v[] = {fun_v, a_v, b_v};
    er_thk* thk = er_thk_alloc(allocator, 3);
    if (thk == NULL) {
        return 0;
    }
    return er_thk_init(thk, ER_CALL, 3, args_v);
}

static bool planvm_build_fac(planvm_fac_program* program)
{
    enum {
        BASE_PC = 24,
    };
    er_val prim66_v = planvm_make_prim66();
    er_val fact_v = planvm_make_law(2, 0, 3);
    if (prim66_v == 0 || fact_v == 0) {
        return false;
    }

    *program = (planvm_fac_program){
        .code_v = {
            [0] = {.tag = OP_PUSH_VAR, .as.slot = 1},
            [1] = {.tag = OP_FORCE},
            [2] = {.tag = OP_JUMP_IF_ZERO, .as.u32 = BASE_PC},

            [3] = {.tag = OP_PUSH_VAR, .as.slot = 0},
            [4] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
            [5] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('S', 'u', 'b')},
            [6] = {.tag = OP_PUSH_VAR, .as.slot = 1},
            [7] = {.tag = OP_FORCE},
            [8] = {.tag = OP_PUSH_LIT, .as.lit_v = 1},
            [9] = {.tag = OP_MK_APP, .as.u32 = 3},
            [10] = {.tag = OP_MK_CALL, .as.u32 = 2},
            [11] = {.tag = OP_FORCE},

            [12] = {.tag = OP_PUSH_LIT, .as.lit_v = prim66_v},
            [13] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('M', 'u', 'l')},
            [14] = {.tag = OP_PUSH_VAR, .as.slot = 2},
            [15] = {.tag = OP_FORCE},
            [16] = {.tag = OP_PUSH_VAR, .as.slot = 1},
            [17] = {.tag = OP_FORCE},
            [18] = {.tag = OP_MK_APP, .as.u32 = 3},
            [19] = {.tag = OP_MK_CALL, .as.u32 = 2},
            [20] = {.tag = OP_FORCE},

            [21] = {.tag = OP_MK_CALL, .as.u32 = 3},
            [22] = {.tag = OP_FORCE},
            [23] = {.tag = OP_RET},

            [24] = {.tag = OP_PUSH_VAR, .as.slot = 2},
            [25] = {.tag = OP_FORCE},
            [26] = {.tag = OP_RET},
        },
        .fact_v = fact_v,
        .prim66_v = prim66_v,
    };
    return true;
}

static er_val planvm_run_fac(planvm_fac_program* program, planvm_arena* arena, er_val n_v)
{
    er_val dstack_v[1024] = {0};
    er_kon kstack_v[4096] = {0};
    er_val call_v = planvm_make_call2(&arena->allocator, program->fact_v, n_v, 1);
    if (call_v == 0) {
        return er_bad;
    }
    er_vm vm = {
        .code = program->code_v,
        .loc_a = &arena->allocator,
        .dstack = dstack_v,
        .dsp = dstack_v,
        .kbase = kstack_v,
        .ksp = kstack_v,
    };
    return plan_eval(&vm, call_v);
}

static enki_value make_oversat_thunk(enki_interpreter* i, enki_value add_v, enki_value x_v)
{
    i->stack_v[0] = add_v;
    i->stack_v[1] = x_v;
    i->stack_v[2] = 4;
    i->stack_v[3] = 5;
    i->sp = 4;
    i->cp = 0;
    i->hp = 0;
    i->halted = false;
    enki_app_apply(i, 3);
    return i->stack_v[0];
}

static void maybe_collect_interp(enki_interpreter* i)
{
    if(i->gc->active_a->off_o > (16 * 1024 * 1024)) {
        enki_gc_collect(i->gc);
    }
}

static void print_interp_result(const char* workload_c, size_t n, enki_value last_v,
    size_t iterations_s, enki_interpreter* i)
{
    printf("profile_runtime: workload=%s n=%zu result=%llu iterations=%zu "
           "steps=%llu apply=%llu exact=%llu under=%llu over=%llu row=%llu op=%llu whnf=%llu "
           "gc_alloc=%llu gc_locked=%llu gc_collect=%llu gc_bytes=%llu\n",
        workload_c,
        n,
        (unsigned long long)last_v,
        iterations_s,
        (unsigned long long)i->stats.interp_step_s,
        (unsigned long long)i->stats.apply_s,
        (unsigned long long)i->stats.apply_exact_s,
        (unsigned long long)i->stats.apply_under_s,
        (unsigned long long)i->stats.apply_over_s,
        (unsigned long long)i->stats.apply_row_s,
        (unsigned long long)i->stats.apply_op_s,
        (unsigned long long)i->stats.whnf_s,
        (unsigned long long)i->stats.gc_alloc_s,
        (unsigned long long)i->stats.gc_locked_alloc_s,
        (unsigned long long)i->stats.gc_collect_s,
        (unsigned long long)i->stats.gc_alloc_bytes_s);
}

static int run_bytecode_fac(size_t n, double seconds)
{
    enki_interpreter* i = enki_test_interp_create(128 * 1024 * 1024, 0);
    enki_value fac_v = build_fac(i);
    size_t iterations_s = 0;
    enki_value last_v = 0;
    double end_s = now_s() + seconds;

    while(now_s() < end_s) {
        ENKI_PROFILE_ZONE("profile_iteration");
        last_v = run_fac(i, fac_v, (enki_value)n);
        iterations_s++;
        maybe_collect_interp(i);
        ENKI_PROFILE_FRAME("profile_runtime");
        ENKI_PROFILE_PLOT_I("iterations", (int64_t)iterations_s);
    }

    print_interp_result("bytecode_fac", n, last_v, iterations_s, i);
    enki_test_interp_destroy(i);
    return 0;
}

static int run_planvm_fac(size_t n, double seconds)
{
    planvm_fac_program program;
    planvm_arena arena;
    if (!planvm_build_fac(&program)) {
        fprintf(stderr, "failed to build planvm factorial program\n");
        return 1;
    }
    if (!planvm_arena_init(&arena, 64 * 1024 * 1024)) {
        fprintf(stderr, "failed to allocate planvm arena\n");
        return 1;
    }

    size_t iterations_s = 0;
    er_val last_v = 0;
    size_t peak_s = 0;
    double end_s = now_s() + seconds;

    while(now_s() < end_s) {
        ENKI_PROFILE_ZONE("profile_iteration");
        planvm_arena_reset(&arena);
        last_v = planvm_run_fac(&program, &arena, (er_val)n);
        if(arena.off_s > peak_s) {
            peak_s = arena.off_s;
        }
        iterations_s++;
        ENKI_PROFILE_FRAME("profile_runtime");
        ENKI_PROFILE_PLOT_I("iterations", (int64_t)iterations_s);
    }

    printf("profile_runtime: workload=planvm_fac n=%zu result=%llu iterations=%zu "
           "arena_peak_bytes=%zu\n",
        n,
        (unsigned long long)last_v,
        iterations_s,
        peak_s);
    planvm_arena_destroy(&arena);
    return last_v == er_bad ? 1 : 0;
}

static int run_bytecode_tinyops(size_t n, double seconds)
{
    enki_interpreter* i = enki_test_interp_create(128 * 1024 * 1024, 0);
    enki_value law_v = build_tinyops_law(i);
    size_t iterations_s = 0;
    enki_value last_v = 0;
    double end_s = now_s() + seconds;

    while(now_s() < end_s) {
        ENKI_PROFILE_ZONE("profile_iteration");
        last_v = run_law1(i, law_v, (enki_value)(n + (iterations_s & 255)));
        iterations_s++;
        ENKI_PROFILE_FRAME("profile_runtime");
        ENKI_PROFILE_PLOT_I("iterations", (int64_t)iterations_s);
    }

    print_interp_result("bytecode_tinyops", n, last_v, iterations_s, i);
    enki_test_interp_destroy(i);
    return 0;
}

static int run_bytecode_thunk_fresh(size_t n, double seconds)
{
    enki_interpreter* i = enki_test_interp_create(128 * 1024 * 1024, 0);
    enki_value add_v = build_add_law(i);
    size_t iterations_s = 0;
    enki_value last_v = 0;
    double end_s = now_s() + seconds;

    while(now_s() < end_s) {
        ENKI_PROFILE_ZONE("profile_iteration");
        enki_value thunk_v = make_oversat_thunk(i, add_v, (enki_value)(n + (iterations_s & 31)));
        last_v = enki_eval_whnf(i, thunk_v);
        iterations_s++;
        maybe_collect_interp(i);
        ENKI_PROFILE_FRAME("profile_runtime");
        ENKI_PROFILE_PLOT_I("iterations", (int64_t)iterations_s);
    }

    print_interp_result("bytecode_thunk_fresh", n, last_v, iterations_s, i);
    enki_test_interp_destroy(i);
    return 0;
}

static int run_bytecode_thunk_cached(size_t n, double seconds)
{
    enki_interpreter* i = enki_test_interp_create(128 * 1024 * 1024, 0);
    enki_value add_v = build_add_law(i);
    enki_value thunk_v = make_oversat_thunk(i, add_v, (enki_value)n);
    i->stack_v[0] = thunk_v;
    i->sp = 1;

    size_t iterations_s = 0;
    enki_value last_v = 0;
    double end_s = now_s() + seconds;

    while(now_s() < end_s) {
        ENKI_PROFILE_ZONE("profile_iteration");
        last_v = enki_eval_whnf(i, i->stack_v[0]);
        iterations_s++;
        ENKI_PROFILE_FRAME("profile_runtime");
        ENKI_PROFILE_PLOT_I("iterations", (int64_t)iterations_s);
    }

    print_interp_result("bytecode_thunk_cached", n, last_v, iterations_s, i);
    enki_test_interp_destroy(i);
    return 0;
}

static int run_nat_big(size_t n, double seconds)
{
    enki_interpreter* i = enki_test_interp_create(256 * 1024 * 1024, 0);
    i->stack_v[0] = enki_nat_lsh(i->gc, (enki_value)(n + 1), (enki_value)96);
    i->stack_v[1] = enki_nat_lsh(i->gc, (enki_value)(n + 3), (enki_value)80);
    i->stack_v[2] = 0;
    i->sp = 3;

    size_t iterations_s = 0;
    double end_s = now_s() + seconds;
    while(now_s() < end_s) {
        ENKI_PROFILE_ZONE("profile_iteration");
        enki_value a_v = i->stack_v[0];
        enki_value b_v = i->stack_v[1];
        i->stack_v[2] = enki_nat_mul(i->gc, a_v, b_v);
        i->stack_v[2] = enki_nat_add(i->gc, i->stack_v[2], b_v);
        i->stack_v[2] = enki_nat_rsh(i->gc, i->stack_v[2], (enki_value)17);
        enki_arena_reset(i->scratch_a);
        iterations_s++;
        maybe_collect_interp(i);
        ENKI_PROFILE_FRAME("profile_runtime");
        ENKI_PROFILE_PLOT_I("iterations", (int64_t)iterations_s);
    }

    print_interp_result("nat_big", n, i->stack_v[2], iterations_s, i);
    enki_test_interp_destroy(i);
    return 0;
}

static int run_op66_rows(size_t n, double seconds)
{
    enki_interpreter* i = enki_test_interp_create(256 * 1024 * 1024, 0);
    size_t iterations_s = 0;
    enki_value last_v = 0;
    double end_s = now_s() + seconds;
    enki_value row_size_v = (enki_value)(n == 0 ? 32 : n);

    while(now_s() < end_s) {
        ENKI_PROFILE_ZONE("profile_iteration");
        i->sp = 0;
        i->stack_v[i->sp++] = 0;
        i->stack_v[i->sp++] = (enki_value)(iterations_s & 255);
        i->stack_v[i->sp++] = row_size_v;
        op66_rep(i);
        enki_value row_v = i->stack_v[0];

        i->sp = 0;
        i->stack_v[i->sp++] = 1;
        i->stack_v[i->sp++] = row_size_v > 4 ? 4 : 1;
        i->stack_v[i->sp++] = row_v;
        op66_slice(i);
        enki_value slice_v = i->stack_v[0];

        i->sp = 0;
        i->stack_v[i->sp++] = row_v;
        i->stack_v[i->sp++] = slice_v;
        op66_weld(i);
        enki_value welded_v = i->stack_v[0];

        i->sp = 0;
        i->stack_v[i->sp++] = 0;
        i->stack_v[i->sp++] = (enki_value)(iterations_s & 511);
        i->stack_v[i->sp++] = welded_v;
        op66_up(i);
        last_v = i->stack_v[0];

        i->sp = 0;
        i->stack_v[i->sp++] = last_v;
        op66_ix0(i);
        last_v = i->stack_v[0];

        iterations_s++;
        maybe_collect_interp(i);
        ENKI_PROFILE_FRAME("profile_runtime");
        ENKI_PROFILE_PLOT_I("iterations", (int64_t)iterations_s);
    }

    print_interp_result("op66_rows", n, last_v, iterations_s, i);
    enki_test_interp_destroy(i);
    return 0;
}

static enki_value wisp_eval_text(wisp_rt* rt, const char* input_c)
{
    char* cur_c = (char*)input_c;
    rt->err_f = 1;
    if(setjmp(rt->errjmp) != 0) {
        fprintf(stderr, "wisp error: %s\n", rt->msg_c ? rt->msg_c : "(no message)");
        exit(1);
    }
    enki_value parsed_v = wisp_parse(rt, &cur_c);
    enki_value res_v = wisp_eval(rt, parsed_v);
    rt->err_f = 0;
    return res_v;
}

static void wisp_install_fib_program(wisp_rt* rt)
{
    static const char* defs_v[] = {
        "(#bind inc (#law \"inc\" (inc x) ((#pin \"B\") (\"Inc\" x))))",
        "(#bind if (#law \"if\" (if cond then else) ((#pin \"B\") (\"If\" cond then else))))",
        "(#bind eq (#law \"eq\" (eq a b) ((#pin \"B\") (\"Eq\" a b))))",
        "(#bind dechelp (#law \"dechelp\" (dechelp count x) "
            "(if (eq (inc count) x) count (dechelp (inc count) x))))",
        "(#bind dec (#law \"dec\" (dec x) (dechelp 0 x)))",
        "(#bind add (#law \"add\" (add a b) (if (eq 0 a) b (add (dec a) (inc b)))))",
        "(#bind fib (#law \"fib\" (fib n) "
            "(if (eq n 1) 1 (if (eq n 2) 1 "
            "(add (fib (dec n)) (fib (dec (dec n))))))))",
    };
    for(size_t k = 0; k < sizeof(defs_v) / sizeof(defs_v[0]); k++) {
        (void)wisp_eval_text(rt, defs_v[k]);
    }
}

static int run_wisp_plan_fib(size_t n, double seconds)
{
    FILE* devnull_f = freopen("/dev/null", "w", stderr);
    (void)devnull_f;

    wisp_rt* rt = wisp_rt_alloc(&sys_a);
    wisp_install_fib_program(rt);

    char query_c[64];
    snprintf(query_c, sizeof(query_c), "(fib %zu)", n);

    size_t iterations_s = 0;
    enki_value last_v = 0;
    double end_s = now_s() + seconds;
    while(now_s() < end_s) {
        ENKI_PROFILE_ZONE("profile_iteration");
        last_v = wisp_eval_text(rt, query_c);
        enki_arena_reset(rt->i->scratch_a);
        iterations_s++;
        maybe_collect_interp(rt->i);
        ENKI_PROFILE_FRAME("profile_runtime");
        ENKI_PROFILE_PLOT_I("iterations", (int64_t)iterations_s);
    }

    print_interp_result("wisp_plan_fib", n, last_v, iterations_s, rt->i);
    wisp_rt_free(&sys_a, rt);
    return 0;
}

static void wait_for_tracy(void)
{
#if defined(TRACY_ENABLE)
    double deadline_s = now_s() + 10.0;
    while(!TracyCIsConnected && now_s() < deadline_s) {
        usleep(10000);
    }
#endif
}

static void print_workloads(void)
{
    printf("profile_runtime workload matrix:\n");
    for(size_t k = 0; k < sizeof(workload_info_v) / sizeof(workload_info_v[0]); k++) {
        printf("  %-24s axis=%s\n      catches=%s\n",
            workload_info_v[k].name_c,
            workload_info_v[k].axis_c,
            workload_info_v[k].catches_c);
    }
}

static int run_workload(const char* workload_c, size_t n, double seconds)
{
    if(strcmp(workload_c, "bytecode_fac") == 0) return run_bytecode_fac(n, seconds);
    if(strcmp(workload_c, "planvm_fac") == 0) return run_planvm_fac(n, seconds);
    if(strcmp(workload_c, "bytecode_tinyops") == 0) return run_bytecode_tinyops(n, seconds);
    if(strcmp(workload_c, "bytecode_thunk_fresh") == 0) return run_bytecode_thunk_fresh(n, seconds);
    if(strcmp(workload_c, "bytecode_thunk_cached") == 0) return run_bytecode_thunk_cached(n, seconds);
    if(strcmp(workload_c, "nat_big") == 0) return run_nat_big(n, seconds);
    if(strcmp(workload_c, "op66_rows") == 0) return run_op66_rows(n, seconds);
    if(strcmp(workload_c, "wisp_plan_fib") == 0) return run_wisp_plan_fib(n, seconds);
    if(strcmp(workload_c, "all") == 0) {
        int rc = 0;
        for(size_t k = 0; k < sizeof(workload_info_v) / sizeof(workload_info_v[0]); k++) {
            rc |= run_workload(workload_info_v[k].name_c, n, seconds);
        }
        return rc;
    }
    fprintf(stderr, "unknown workload: %s\n", workload_c);
    print_workloads();
    return 2;
}

int main(int argc, char** argv)
{
    ENKI_PROFILE_THREAD("enki profile_runtime");

    const char* workload_c = "bytecode_fac";
    size_t n = 8;
    double seconds = 5.0;
    bool wait = false;

    for(int k = 1; k < argc; k++) {
        if(strcmp(argv[k], "--workload") == 0 && k + 1 < argc) {
            workload_c = argv[++k];
        }
        else if(strcmp(argv[k], "--n") == 0 && k + 1 < argc) {
            n = (size_t)strtoull(argv[++k], NULL, 10);
        }
        else if(strcmp(argv[k], "--seconds") == 0 && k + 1 < argc) {
            seconds = strtod(argv[++k], NULL);
        }
        else if(strcmp(argv[k], "--wait") == 0) {
            wait = true;
        }
        else if(strcmp(argv[k], "--list") == 0) {
            print_workloads();
            return 0;
        }
        else {
            fprintf(stderr,
                "usage: %s [--list] [--workload NAME|all] [--n N] [--seconds S] [--wait]\n",
                argv[0]);
            print_workloads();
            return 2;
        }
    }

    if(wait) wait_for_tracy();
    return run_workload(workload_c, n, seconds);
}
