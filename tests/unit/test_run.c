#include "enki/run.h"

#include <criterion/criterion.h>
#include <stdint.h>
#include <stdlib.h>

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

Test(run_alloc, law_alloc_init_records_let_offsets)
{
    uint32_t let_off_d[] = {10, 25, 40};
    er_law* law = er_law_alloc(enki_allocator_system(), 3);
    cr_assert_not_null(law);

    er_val law_v = er_law_init(law, 11, 22, 2, 6, 3, let_off_d);

    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    cr_assert_eq(er_outa(law_v), law);
    cr_assert_eq(law->h.siz_s & ~(size_t)0x3, sizeof(er_law) + 3 * sizeof(uint32_t));
    cr_assert_eq(law->h.raw.nf_f, 1);
    cr_assert_eq(law->name_v, 11);
    cr_assert_eq(law->body_v, 22);
    cr_assert_eq(law->ari_d, 2);
    cr_assert_eq(law->frame_d, 6);
    for (size_t k = 0; k < 3; k++) {
        cr_assert_eq(law->let_off_d[k], let_off_d[k]);
    }

    free_system(law);
}

Test(run_alloc, law_init_rejects_frame_let_count_mismatch)
{
    er_law* law = er_law_alloc(enki_allocator_system(), 1);
    cr_assert_not_null(law);

    cr_assert_eq(er_law_init(law, 0, 0, 2, 6, 1, NULL), 0);

    free_system(law);
}

static er_val make_law(uint32_t arity_d, uint32_t start_d, uint32_t frame_d, size_t n_lets,
    const uint32_t let_off_d[])
{
    er_law* law = er_law_alloc(enki_allocator_system(), n_lets);
    cr_assert_not_null(law);
    er_val law_v = er_law_init(law, 0, 0, arity_d, frame_d, n_lets, let_off_d);
    cr_assert_eq(er_get_tag(law_v), er_tag_law);
    law->start_d = start_d;
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

static er_val run_vm(er_op* code, er_val root_v)
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
    return plan_eval(&vm, root_v);
}

Test(run_vm, strict_let_prelude_forces_suspension_and_discards_value)
{
    enum {
        LAW_START = 0,
        LET_START = 5,
    };
    er_op code[] = {
        [0] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [1] = {.tag = OP_FORCE},
        [2] = {.tag = OP_DROP},
        [3] = {.tag = OP_PUSH_LIT, .as.lit_v = 42},
        [4] = {.tag = OP_RET},
        [5] = {.tag = OP_PUSH_LIT, .as.lit_v = 7},
        [6] = {.tag = OP_RET},
    };
    uint32_t let_off_d[] = {LET_START};
    er_val law_v = make_law(0, LAW_START, 2, 1, let_off_d);
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

Test(run_vm, strict_let_cycle_traps_as_bad_value)
{
    enum {
        LAW_START = 0,
        LET_START = 3,
    };
    er_op code[] = {
        [0] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [1] = {.tag = OP_FORCE},
        [2] = {.tag = OP_RET},
        [3] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [4] = {.tag = OP_FORCE},
        [5] = {.tag = OP_RET},
    };
    uint32_t let_off_d[] = {LET_START};
    er_val law_v = make_law(0, LAW_START, 2, 1, let_off_d);
    er_val call_v = make_call(law_v, 2);

    cr_assert_eq(run_vm(code, call_v), er_bad);
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
    er_val law_v = make_law(0, 0, 1, 0, NULL);
    er_val call_v = make_call(law_v, 1);

    er_val result_v = run_vm(code, call_v);

    er_app* app = er_outt(er_tag_app, result_v);
    cr_assert_not_null(app);
    cr_assert_eq(app->fn_v, 100);
    cr_assert_eq(app->arg_s, 2);
    cr_assert_eq(app->arg_v[0], 10);
    cr_assert_eq(app->arg_v[1], 20);
}

Test(run_vm, recursive_factorial_uses_bytecode_calls_and_primop_set)
{
    enum {
        BASE_PC = 24,
    };
    er_val prim66_v = make_prim66();
    er_op code[] = {
        [0] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [1] = {.tag = OP_FORCE},
        [2] = {.tag = OP_JUMP_IF_ZERO, .as.u32 = BASE_PC},

        [3] = {.tag = OP_PUSH_VAR, .as.slot = 0},
        [4] = {.tag = OP_PUSH_LIT, .as.lit_v = 0}, // patched to prim66
        [5] = {.tag = OP_PUSH_LIT, .as.lit_v = PLAN_S3('S', 'u', 'b')},
        [6] = {.tag = OP_PUSH_VAR, .as.slot = 1},
        [7] = {.tag = OP_FORCE},
        [8] = {.tag = OP_PUSH_LIT, .as.lit_v = 1},
        [9] = {.tag = OP_MK_APP, .as.u32 = 3},
        [10] = {.tag = OP_MK_CALL, .as.u32 = 2},
        [11] = {.tag = OP_FORCE},

        [12] = {.tag = OP_PUSH_LIT, .as.lit_v = 0}, // patched to prim66
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
    };
    code[4].as.lit_v = prim66_v;
    code[12].as.lit_v = prim66_v;
    er_val fact_v = make_law(2, 0, 3, 0, NULL);
    er_val call_v = make_call(fact_v, 3);
    er_thk* call = er_outt(er_tag_thk, call_v);
    call->arg_v[1] = 8;
    call->arg_v[2] = 1;

    er_val result_v = run_vm(code, call_v);
    cr_assert_eq(result_v, 40320, "got %llu", (unsigned long long)result_v);
}
