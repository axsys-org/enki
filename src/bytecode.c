#include "enki/bytecode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bytecode_internal.h"

#include "enki/gc.h"
#include "enki/profile.h"
#include "enki/run_ops.h"

typedef struct er_bc_code {
    const enki_allocator* loc_a;
    er_op* op_v;
    size_t op_s;
    size_t cap_s;
    bool ok_f;
} er_bc_code;

typedef struct er_bc_vals {
    const enki_allocator* loc_a;
    er_val* val_v;
    size_t val_s;
    size_t cap_s;
    er_app** own_app_v;
    size_t own_app_s;
    size_t own_app_cap_s;
    bool ok_f;
} er_bc_vals;

typedef struct er_bc_label {
    er_op* code_v;
    size_t code_s;
    bool set_f;
} er_bc_label;

typedef struct er_bc_compiler {
    const enki_allocator* loc_a;
    er_bc_label* label_v;
    size_t label_s;
    size_t cap_s;
    uint32_t next_label_d;
    bool ok_f;
} er_bc_compiler;

typedef struct er_bc_prim {
    er_val name_v;
    er_optag tag;
    uint32_t ari_d;
    bool if_f;
    er_bc_eval_req arg_eval_v[ER_BC_MAX_PRIM_ARITY];
} er_bc_prim;

#define ER_BC_L ER_BC_EVAL_NONE
#define ER_BC_W ER_BC_EVAL_WHNF
#define ER_BC_N ER_BC_EVAL_NF

#define ER_BC_ROUTE_ARGS(_name, _tag, _ari, ...)                 \
    {                                                            \
        .name_v = (_name),                                       \
        .tag = (_tag),                                           \
        .ari_d = (_ari),                                         \
        .if_f = false,                                           \
        .arg_eval_v = {__VA_ARGS__},                             \
    }
#define ER_BC_ROUTE0(_name, _tag, _ari) \
    ER_BC_ROUTE_ARGS(_name, _tag, _ari, ER_BC_L)
#define ER_BC_ROUTE1(_name, _tag, _ari, _a0) \
    ER_BC_ROUTE_ARGS(_name, _tag, _ari, _a0)
#define ER_BC_ROUTE2(_name, _tag, _ari, _a0, _a1) \
    ER_BC_ROUTE_ARGS(_name, _tag, _ari, _a0, _a1)
#define ER_BC_ROUTE3(_name, _tag, _ari, _a0, _a1, _a2) \
    ER_BC_ROUTE_ARGS(_name, _tag, _ari, _a0, _a1, _a2)
#define ER_BC_ROUTE4(_name, _tag, _ari, _a0, _a1, _a2, _a3) \
    ER_BC_ROUTE_ARGS(_name, _tag, _ari, _a0, _a1, _a2, _a3)
#define ER_BC_ROUTE6(_name, _tag, _ari, _a0, _a1, _a2, _a3, _a4, _a5) \
    ER_BC_ROUTE_ARGS(_name, _tag, _ari, _a0, _a1, _a2, _a3, _a4, _a5)

static const er_bc_prim er_bc_prim_v[] = {
    ER_BC_ROUTE1(PLAN_S3('P', 'i', 'n'), OP_PIN, 1, ER_BC_W),
    ER_BC_ROUTE3(PLAN_S3('L', 'a', 'w'), OP_LAW, 3, ER_BC_W, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE6(PLAN_S4('E', 'l', 'i', 'm'), OP_ELIM, 6, ER_BC_L, ER_BC_L,
                 ER_BC_L, ER_BC_L, ER_BC_L, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S3('N', 'a', 't'), OP_NAT, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S5('A', 'r', 'i', 't', 'y'), OP_ARI, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S4('N', 'a', 'm', 'e'), OP_NAM, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S4('B', 'o', 'd', 'y'), OP_BODY, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S5('U', 'n', 'p', 'i', 'n'), OP_UNPIN, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S2('S', 'z'), OP_SZ, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S4('L', 'a', 's', 't'), OP_LAST, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S4('I', 'n', 'i', 't'), OP_INIT, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S3('I', 'n', 'c'), OP_INC, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S3('D', 'e', 'c'), OP_DEC, 1, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S3('A', 'd', 'd'), OP_ADD, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S3('S', 'u', 'b'), OP_SUB, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S3('R', 's', 'h'), OP_RSH, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S3('L', 's', 'h'), OP_LSH, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S3('D', 'i', 'v'), OP_DIV, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S3('M', 'u', 'l'), OP_MUL, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S3('M', 'o', 'd'), OP_MOD, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S4('T', 'e', 's', 't'), OP_TEST, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE3(PLAN_S7('L', 'o', 'a', 'd', 'V', 'a', 'r'), OP_LOAD, 3, ER_BC_W,
                 ER_BC_W, ER_BC_W),
    ER_BC_ROUTE4(PLAN_S5('S', 't', 'o', 'r', 'e'), OP_STORE, 4, ER_BC_W, ER_BC_W,
                 ER_BC_W, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S5('T', 'r', 'u', 'n', 'c'), OP_TRUNC, 2, ER_BC_W,
                 ER_BC_W),
    ER_BC_ROUTE1(PLAN_S4('B', 'i', 't', 's'), OP_BITS, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S5('B', 'y', 't', 'e', 's'), OP_BYTES, 1, ER_BC_W),
    ER_BC_ROUTE3(PLAN_S3('R', 'e', 'p'), OP_REP, 3, ER_BC_L, ER_BC_L, ER_BC_W),
    ER_BC_ROUTE3(PLAN_S5('S', 'l', 'i', 'c', 'e'), OP_SLICE, 3, ER_BC_W, ER_BC_W,
                 ER_BC_W),
    ER_BC_ROUTE2(PLAN_S4('W', 'e', 'l', 'd'), OP_WELD, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE0(PLAN_S4('E', 'v', 'a', 'l'), OP_EVAL, 1),
    ER_BC_ROUTE0(PLAN_S5('F', 'o', 'r', 'c', 'e'), OP_FORCE, 1),
    ER_BC_ROUTE3(PLAN_S2('U', 'p'), OP_UP, 3, ER_BC_W, ER_BC_L, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S4('C', 'o', 'u', 'p'), OP_COUP, 2, ER_BC_L, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S2('H', 'd'), OP_HD, 1, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S2('I', 'x'), OP_IX, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S3('N', 'i', 'l'), OP_NOT, 1, ER_BC_W),
    ER_BC_ROUTE1(PLAN_S5('T', 'r', 'u', 't', 'h'), OP_TRU, 1, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S2('O', 'r'), OP_OR, 2, ER_BC_W, ER_BC_L),
    ER_BC_ROUTE2(PLAN_S3('A', 'n', 'd'), OP_AND, 2, ER_BC_W, ER_BC_L),
    {.name_v = PLAN_S2('I', 'f'), .tag = OP_COUNT, .ari_d = 3, .if_f = true},
    {.name_v = PLAN_S3('I', 'f', 'z'),
     .tag = OP_JUMP_IF_ZERO,
     .ari_d = 3,
     .if_f = true},
    ER_BC_ROUTE2(PLAN_S2('E', 'q'), OP_EQ, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S2('L', 'e'), OP_LE, 2, ER_BC_W, ER_BC_W),
    ER_BC_ROUTE2(PLAN_S3('C', 'm', 'p'), OP_CMP, 2, ER_BC_W, ER_BC_W),
};

#define ER_BC_PRIM_ROUTE(_tag, _arg_s) \
    { \
        .tag = (_tag), \
        .arg_s = (_arg_s), \
        .valid_f = true, \
        .arg_eval_v = {ER_BC_W, ER_BC_W, ER_BC_W, ER_BC_W, ER_BC_W, ER_BC_W, ER_BC_W, \
                       ER_BC_W}, \
    }

#define ER_BC_PRIM_ROUTE_ARGS(_tag, _arg_s, ...) \
    { \
        .tag = (_tag), \
        .arg_s = (_arg_s), \
        .valid_f = true, \
        .arg_eval_v = {__VA_ARGS__}, \
    }

static const er_bc_prim_route er_bc_prim0_route_v[] = {
    [OP0_PIN]  = ER_BC_PRIM_ROUTE(OP_PIN, 1),
    [OP0_LAW]  = ER_BC_PRIM_ROUTE(OP_LAW, 3),
    [OP0_ELIM] = ER_BC_PRIM_ROUTE_ARGS(OP_ELIM, 6, ER_BC_L, ER_BC_L, ER_BC_L, ER_BC_L,
                                        ER_BC_L, ER_BC_W),
};

static const er_bc_prim_route er_bc_prim66_route_v[ER_OP66_COUNT] = {
    [OP66_INC]      = ER_BC_PRIM_ROUTE(OP_INC, 1),
    [OP66_DEC]      = ER_BC_PRIM_ROUTE(OP_DEC, 1),
    [OP66_ADD]      = ER_BC_PRIM_ROUTE(OP_ADD, 2),
    [OP66_SUB]      = ER_BC_PRIM_ROUTE(OP_SUB, 2),
    [OP66_MUL]      = ER_BC_PRIM_ROUTE(OP_MUL, 2),
    [OP66_DIV]      = ER_BC_PRIM_ROUTE(OP_DIV, 2),
    [OP66_MOD]      = ER_BC_PRIM_ROUTE(OP_MOD, 2),
    [OP66_EQ]       = ER_BC_PRIM_ROUTE(OP_EQ, 2),
    [OP66_LE]       = ER_BC_PRIM_ROUTE(OP_LE, 2),
    [OP66_CMP]      = ER_BC_PRIM_ROUTE(OP_CMP, 2),
    [OP66_RSH]      = ER_BC_PRIM_ROUTE(OP_RSH, 2),
    [OP66_LSH]      = ER_BC_PRIM_ROUTE(OP_LSH, 2),
    [OP66_TEST]     = ER_BC_PRIM_ROUTE(OP_TEST, 2),
    [OP66_BEX]      = ER_BC_PRIM_ROUTE(OP_BEX, 1),
    [OP66_BITS]     = ER_BC_PRIM_ROUTE(OP_BITS, 1),
    [OP66_BYTES]    = ER_BC_PRIM_ROUTE(OP_BYTES, 1),
    [OP66_LOAD8]    = ER_BC_PRIM_ROUTE(OP_LOAD8, 2),
    [OP66_STORE8]   = ER_BC_PRIM_ROUTE(OP_STORE8, 3),
    [OP66_TRUNC]    = ER_BC_PRIM_ROUTE(OP_TRUNC, 2),
    [OP66_TRUNC8]   = ER_BC_PRIM_ROUTE(OP_TRUNC8, 1),
    [OP66_TRUNC16]  = ER_BC_PRIM_ROUTE(OP_TRUNC16, 1),
    [OP66_TRUNC32]  = ER_BC_PRIM_ROUTE(OP_TRUNC32, 1),
    [OP66_TRUNC64]  = ER_BC_PRIM_ROUTE(OP_TRUNC64, 1),
    [OP66_NAT]      = ER_BC_PRIM_ROUTE(OP_NAT, 1),
    [OP66_UNPIN]    = ER_BC_PRIM_ROUTE(OP_UNPIN, 1),
    [OP66_ARITY]    = ER_BC_PRIM_ROUTE(OP_ARI, 1),
    [OP66_NAME]     = ER_BC_PRIM_ROUTE(OP_NAM, 1),
    [OP66_BODY]     = ER_BC_PRIM_ROUTE(OP_BODY, 1),
    [OP66_HD]       = ER_BC_PRIM_ROUTE(OP_HD, 1),
    [OP66_LAST]     = ER_BC_PRIM_ROUTE(OP_LAST, 1),
    [OP66_INIT]     = ER_BC_PRIM_ROUTE(OP_INIT, 1),
    [OP66_REP]      = ER_BC_PRIM_ROUTE_ARGS(OP_REP, 3, ER_BC_L, ER_BC_L, ER_BC_W),
    [OP66_SLICE]    = ER_BC_PRIM_ROUTE(OP_SLICE, 3),
    [OP66_WELD]     = ER_BC_PRIM_ROUTE(OP_WELD, 2),
    [OP66_UP]       = ER_BC_PRIM_ROUTE_ARGS(OP_UP, 3, ER_BC_W, ER_BC_L, ER_BC_W),
    [OP66_UP_UNIQ]  = ER_BC_PRIM_ROUTE_ARGS(OP_UP, 3, ER_BC_W, ER_BC_L, ER_BC_W),
    [OP66_COUP]     = ER_BC_PRIM_ROUTE_ARGS(OP_COUP, 2, ER_BC_L, ER_BC_W),
    [OP66_SZ]       = ER_BC_PRIM_ROUTE(OP_SZ, 1),
    [OP66_IX]       = ER_BC_PRIM_ROUTE(OP_IX, 2),
    [OP66_NIL]      = ER_BC_PRIM_ROUTE(OP_NOT, 1),
    [OP66_TRUTH]    = ER_BC_PRIM_ROUTE(OP_TRU, 1),
    [OP66_OR]       = ER_BC_PRIM_ROUTE_ARGS(OP_OR, 2, ER_BC_W, ER_BC_L),
    [OP66_AND]      = ER_BC_PRIM_ROUTE_ARGS(OP_AND, 2, ER_BC_W, ER_BC_L),
    [OP66_LOAD]     = ER_BC_PRIM_ROUTE(OP_LOAD, 3),
    [ER_OP66_STORE] = ER_BC_PRIM_ROUTE(OP_STORE, 4),
    [ER_OP66_MET]   = ER_BC_PRIM_ROUTE(OP_MET_DYN, 2),
};

bool er_bc_prim_route_strict(er_optag tag, size_t arg_s, er_bc_prim_route* out)
{
    if (out == NULL || arg_s > ER_BC_MAX_PRIM_ARITY) {
        return false;
    }
    *out = (er_bc_prim_route)ER_BC_PRIM_ROUTE(tag, arg_s);
    return true;
}

bool er_bc_prim0_route(size_t op_s, er_bc_prim_route* out)
{
    if (out == NULL || op_s >= sizeof(er_bc_prim0_route_v) / sizeof(er_bc_prim0_route_v[0]) ||
        !er_bc_prim0_route_v[op_s].valid_f) {
        return false;
    }
    *out = er_bc_prim0_route_v[op_s];
    return true;
}

bool er_bc_prim66_route(int op_i, er_bc_prim_route* out)
{
    if (out == NULL || op_i < 0 || (size_t)op_i >= ER_OP66_COUNT ||
        !er_bc_prim66_route_v[op_i].valid_f) {
        return false;
    }
    *out = er_bc_prim66_route_v[op_i];
    return true;
}

static bool er_bc_mul_size(size_t a_s, size_t b_s, size_t* out_s)
{
    if (b_s != 0 && a_s > SIZE_MAX / b_s) {
        return false;
    }
    *out_s = a_s * b_s;
    return true;
}

static bool er_bc_realloc(const enki_allocator* loc_a, void** ptr, size_t old_count_s,
                          size_t new_count_s, size_t elem_s)
{
    size_t old_size_s = 0;
    size_t new_size_s = 0;
    if (!er_bc_mul_size(old_count_s, elem_s, &old_size_s) ||
        !er_bc_mul_size(new_count_s, elem_s, &new_size_s) || new_size_s == 0) {
        return false;
    }
    if (loc_a == NULL || loc_a->alloc == NULL || loc_a->free == NULL) {
        return false;
    }
    if (loc_a->realloc != NULL) {
        void* next = loc_a->realloc(loc_a->ctx, *ptr, new_size_s);
        if (next == NULL) {
            return false;
        }
        *ptr = next;
        return true;
    }
    void* next = loc_a->alloc(loc_a->ctx, new_size_s);
    if (next == NULL) {
        return false;
    }
    if (*ptr != NULL && old_size_s != 0) {
        memcpy(next, *ptr, old_size_s < new_size_s ? old_size_s : new_size_s);
        loc_a->free(loc_a->ctx, *ptr);
    }
    *ptr = next;
    return true;
}

static bool er_bc_code_reserve(er_bc_code* code, size_t need_s)
{
    if (!code->ok_f) {
        return false;
    }
    if (need_s <= code->cap_s) {
        return true;
    }
    size_t next_s = code->cap_s == 0 ? 8 : code->cap_s;
    while (next_s < need_s) {
        if (next_s > SIZE_MAX / 2) {
            code->ok_f = false;
            return false;
        }
        next_s *= 2;
    }
    void* ptr = code->op_v;
    if (!er_bc_realloc(code->loc_a, &ptr, code->cap_s, next_s, sizeof(er_op))) {
        code->ok_f = false;
        return false;
    }
    code->op_v = ptr;
    code->cap_s = next_s;
    return true;
}

static bool er_bc_emit(er_bc_code* code, er_optag tag)
{
    if (!er_bc_code_reserve(code, code->op_s + 1)) {
        return false;
    }
    code->op_v[code->op_s] = (er_op){.tag = tag};
    code->op_s++;
    return true;
}

static bool er_bc_emit_u32(er_bc_code* code, er_optag tag, uint32_t u32)
{
    if (!er_bc_emit(code, tag)) {
        return false;
    }
    code->op_v[code->op_s - 1].as.u32 = u32;
    return true;
}

static bool er_bc_emit_lit(er_bc_code* code, er_val lit_v)
{
    if (!er_bc_emit(code, OP_PUSH_LIT)) {
        return false;
    }
    code->op_v[code->op_s - 1].as.lit_v = lit_v;
    return true;
}

static bool er_bc_emit_var(er_bc_code* code, er_val var_v)
{
    if (!er_bc_emit(code, OP_PUSH_VAR)) {
        return false;
    }
    code->op_v[code->op_s - 1].as.slot = (uintptr_t)var_v;
    return true;
}

static void er_bc_code_free(er_bc_code* code)
{
    if (code->op_v != NULL && code->loc_a != NULL && code->loc_a->free != NULL) {
        code->loc_a->free(code->loc_a->ctx, code->op_v);
    }
    code->op_v = NULL;
    code->op_s = 0;
    code->cap_s = 0;
}

static void er_bc_vals_init(er_bc_vals* vals, const enki_allocator* loc_a)
{
    vals->loc_a = loc_a;
    vals->val_v = NULL;
    vals->val_s = 0;
    vals->cap_s = 0;
    vals->own_app_v = NULL;
    vals->own_app_s = 0;
    vals->own_app_cap_s = 0;
    vals->ok_f = true;
}

static bool er_bc_vals_reserve(er_bc_vals* vals, size_t need_s)
{
    if (!vals->ok_f) {
        return false;
    }
    if (need_s <= vals->cap_s) {
        return true;
    }
    size_t next_s = vals->cap_s == 0 ? 8 : vals->cap_s;
    while (next_s < need_s) {
        if (next_s > SIZE_MAX / 2) {
            vals->ok_f = false;
            return false;
        }
        next_s *= 2;
    }
    void* ptr = vals->val_v;
    if (!er_bc_realloc(vals->loc_a, &ptr, vals->cap_s, next_s, sizeof(er_val))) {
        vals->ok_f = false;
        return false;
    }
    vals->val_v = ptr;
    vals->cap_s = next_s;
    return true;
}

static bool er_bc_vals_push(er_bc_vals* vals, er_val val_v)
{
    if (!er_bc_vals_reserve(vals, vals->val_s + 1)) {
        return false;
    }
    vals->val_v[vals->val_s++] = val_v;
    return true;
}

static bool er_bc_vals_owned_reserve(er_bc_vals* vals, size_t need_s)
{
    if (!vals->ok_f) {
        return false;
    }
    if (need_s <= vals->own_app_cap_s) {
        return true;
    }
    size_t next_s = vals->own_app_cap_s == 0 ? 8 : vals->own_app_cap_s;
    while (next_s < need_s) {
        if (next_s > SIZE_MAX / 2) {
            vals->ok_f = false;
            return false;
        }
        next_s *= 2;
    }
    void* ptr = vals->own_app_v;
    if (!er_bc_realloc(vals->loc_a, &ptr, vals->own_app_cap_s, next_s, sizeof(er_app*))) {
        vals->ok_f = false;
        return false;
    }
    vals->own_app_v = ptr;
    vals->own_app_cap_s = next_s;
    return true;
}

static bool er_bc_vals_push_const(er_bc_vals* vals, er_val val_v)
{
    er_val arg_v[] = {val_v};
    er_app* app = er_app_alloc(vals->loc_a, 1);
    if (app == NULL || !er_bc_vals_owned_reserve(vals, vals->own_app_s + 1)) {
        if (app != NULL && vals->loc_a != NULL && vals->loc_a->free != NULL) {
            vals->loc_a->free(vals->loc_a->ctx, app);
        }
        vals->ok_f = false;
        return false;
    }
    er_val quote_v = er_app_init(app, 0, 1, arg_v);
    if (quote_v == 0) {
        vals->loc_a->free(vals->loc_a->ctx, app);
        vals->ok_f = false;
        return false;
    }
    vals->own_app_v[vals->own_app_s++] = app;
    return er_bc_vals_push(vals, quote_v);
}

static void er_bc_vals_free(er_bc_vals* vals)
{
    if (vals->own_app_v != NULL && vals->loc_a != NULL && vals->loc_a->free != NULL) {
        for (size_t k = 0; k < vals->own_app_s; k++) {
            vals->loc_a->free(vals->loc_a->ctx, vals->own_app_v[k]);
        }
        vals->loc_a->free(vals->loc_a->ctx, vals->own_app_v);
    }
    if (vals->val_v != NULL && vals->loc_a != NULL && vals->loc_a->free != NULL) {
        vals->loc_a->free(vals->loc_a->ctx, vals->val_v);
    }
    vals->val_v = NULL;
    vals->val_s = 0;
    vals->cap_s = 0;
    vals->own_app_v = NULL;
    vals->own_app_s = 0;
    vals->own_app_cap_s = 0;
}

static bool er_bc_compiler_reserve(er_bc_compiler* c, size_t need_s)
{
    if (!c->ok_f) {
        return false;
    }
    if (need_s <= c->cap_s) {
        return true;
    }
    size_t next_s = c->cap_s == 0 ? 8 : c->cap_s;
    while (next_s < need_s) {
        if (next_s > SIZE_MAX / 2) {
            c->ok_f = false;
            return false;
        }
        next_s *= 2;
    }
    void* ptr = c->label_v;
    if (!er_bc_realloc(c->loc_a, &ptr, c->cap_s, next_s, sizeof(er_bc_label))) {
        c->ok_f = false;
        return false;
    }
    c->label_v = ptr;
    for (size_t k = c->cap_s; k < next_s; k++) {
        c->label_v[k] = (er_bc_label){0};
    }
    c->cap_s = next_s;
    return true;
}

static bool er_bc_compiler_set_label(er_bc_compiler* c, uint32_t label_d, er_op* code_v,
                                     size_t code_s)
{
    size_t label_s = (size_t)label_d;
    if (code_v == NULL || code_s == 0 || !er_bc_compiler_reserve(c, label_s + 1)) {
        return false;
    }
    if (label_s >= c->label_s) {
        c->label_s = label_s + 1;
    }
    c->label_v[label_s].code_v = code_v;
    c->label_v[label_s].code_s = code_s;
    c->label_v[label_s].set_f = true;
    return true;
}

static bool er_bc_compiler_fresh(er_bc_compiler* c, uint32_t* out_d)
{
    if (c->next_label_d == UINT32_MAX) {
        c->ok_f = false;
        return false;
    }
    *out_d = c->next_label_d++;
    return er_bc_compiler_reserve(c, (size_t)*out_d + 1);
}

static void er_bc_compiler_free(er_bc_compiler* c, bool free_code_f)
{
    if (c->label_v == NULL || c->loc_a == NULL || c->loc_a->free == NULL) {
        return;
    }
    if (free_code_f) {
        for (size_t k = 0; k < c->label_s; k++) {
            if (c->label_v[k].code_v != NULL) {
                c->loc_a->free(c->loc_a->ctx, c->label_v[k].code_v);
            }
        }
    }
    c->loc_a->free(c->loc_a->ctx, c->label_v);
    c->label_v = NULL;
    c->label_s = 0;
    c->cap_s = 0;
}

static bool er_bc_is_call(er_val val_v, er_val* f_v, er_val* x_v)
{
    er_app* app = er_outt(er_tag_app, val_v);
    if (app == NULL || app->fn_v != 0 || app->arg_s != 2) {
        return false;
    }
    *f_v = app->arg_v[0];
    *x_v = app->arg_v[1];
    return true;
}

static bool er_bc_is_let(er_val val_v, er_val* v_v, er_val* k_v)
{
    er_app* app = er_outt(er_tag_app, val_v);
    if (app == NULL || app->fn_v != 1 || app->arg_s != 2) {
        return false;
    }
    *v_v = app->arg_v[0];
    *k_v = app->arg_v[1];
    return true;
}

static bool er_bc_is_var(size_t depth_s, er_val val_v)
{
    return er_is_cat(val_v) && val_v <= (er_val)depth_s;
}

static er_val er_bc_pull_const(er_val val_v)
{
    er_app* app = er_outt(er_tag_app, val_v);
    if (app != NULL && app->fn_v == 0 && app->arg_s == 1) {
        return app->arg_v[0];
    }
    return val_v;
}

static const er_bc_prim* er_bc_prim_lookup(er_val key_v)
{
    for (size_t k = 0; k < sizeof(er_bc_prim_v) / sizeof(er_bc_prim_v[0]); k++) {
        if (er_bc_prim_v[k].name_v == key_v) {
            return &er_bc_prim_v[k];
        }
    }
    return NULL;
}

static bool er_bc_call_head_with_local_args(er_val body_v, uint32_t ari_d, er_val* head_v)
{
    er_val cur_v = body_v;
    er_val f_v = 0;
    er_val x_v = 0;
    size_t arg_s = 0;
    while (er_bc_is_call(cur_v, &f_v, &x_v)) {
        arg_s++;
        if (arg_s > (size_t)ari_d || x_v != (er_val)((size_t)ari_d + 1u - arg_s)) {
            return false;
        }
        cur_v = f_v;
    }
    if (arg_s != (size_t)ari_d) {
        return false;
    }
    *head_v = er_bc_pull_const(cur_v);
    return true;
}

static bool er_bc_prim_name_from_law_value(er_val prim_v, uint32_t ari_d, er_val* key_v)
{
    er_pin* pin = er_outt(er_tag_pin, prim_v);
    if (pin != NULL) {
        prim_v = pin->val_v;
    }
    er_law* prim_law = er_outt(er_tag_law, prim_v);
    if (prim_law == NULL) {
        return false;
    }

    const er_bc_prim* prim = er_bc_prim_lookup(prim_law->name_v);
    if (prim == NULL || prim->ari_d != ari_d) {
        return false;
    }

    *key_v = prim_law->name_v;
    return true;
}

static bool er_bc_direct_prim_body_key(er_val body_v, uint32_t ari_d, er_val* key_v)
{
    er_val head_v = 0;
    if (er_bc_call_head_with_local_args(body_v, ari_d, &head_v) &&
        er_bc_prim_name_from_law_value(head_v, ari_d, key_v)) {
        return true;
    }

    er_val pin_f_v = 0;
    er_val row_v = 0;
    if (!er_bc_is_call(body_v, &pin_f_v, &row_v)) {
        return false;
    }
    er_pin* pin = er_outt(er_tag_pin, er_bc_pull_const(pin_f_v));
    if (pin == NULL || pin->val_v != 66) {
        return false;
    }

    er_val prim_name_v = 0;
    if (!er_bc_call_head_with_local_args(row_v, ari_d, &prim_name_v)) {
        return false;
    }
    const er_bc_prim* prim = er_bc_prim_lookup(prim_name_v);
    if (prim == NULL || prim->ari_d != ari_d) {
        return false;
    }

    *key_v = prim_name_v;
    return true;
}

static bool er_bc_direct_prim_wrapper_key(er_val law_v, er_val* key_v)
{
    er_law* law = er_outt(er_tag_law, law_v);
    if (law == NULL) {
        return false;
    }
    return er_bc_direct_prim_body_key(law->body_v, law->ari_d, key_v);
}

static uint32_t er_bc_arity(er_val val_v)
{
    er_pin* pin;
    er_app* app;
    er_law* law;
    switch (er_get_tag(val_v)) {
    case er_tag_pin:
        pin = er_outa(val_v);
        if (er_is_cat(pin->val_v)) {
            return 1;
        }
        law = er_outt(er_tag_law, pin->val_v);
        return law == NULL ? 0 : law->ari_d;
    case er_tag_app:
        app = er_outa(val_v);
        {
            uint32_t ari_d = er_bc_arity(app->fn_v);
            return ari_d < app->arg_s ? 0 : ari_d - (uint32_t)app->arg_s;
        }
    case er_tag_law:
        law = er_outa(val_v);
        return law->ari_d;
    default:
        return 0;
    }
}

static bool er_bc_is_prim_pin(er_val val_v)
{
    er_pin* pin = er_outt(er_tag_pin, val_v);
    return pin != NULL && er_is_cat(pin->val_v);
}

static er_val er_bc_prim_key(er_val f_v)
{
    er_pin* pin = er_outt(er_tag_pin, f_v);
    if (pin != NULL) {
        f_v = pin->val_v;
    }

    er_law* law = er_outt(er_tag_law, f_v);
    if (law != NULL) {
        er_val key_v = 0;
        if (er_bc_direct_prim_wrapper_key(f_v, &key_v)) {
            return key_v;
        }
        return law->name_v;
    }

    er_app* app = er_outt(er_tag_app, f_v);
    if (app != NULL) {
        if (app->fn_v == 0 && app->arg_s > 0) {
            return er_bc_prim_key(app->arg_v[0]);
        }
        return er_bc_prim_key(app->fn_v);
    }

    return 0;
}

static const er_bc_prim* er_bc_prim_get(er_val f_v)
{
    return er_bc_prim_lookup(er_bc_prim_key(f_v));
}

static bool er_bc_compile_expr(er_bc_compiler* c, size_t depth_s, uint32_t ari_d, er_val body_v,
                               er_bc_code* code, bool tail_f);
static bool er_bc_compile_value(er_bc_compiler* c, size_t depth_s, uint32_t ari_d, er_val body_v,
                                er_bc_code* code);
static bool er_bc_compile_app_value(er_bc_compiler* c, size_t depth_s, uint32_t ari_d,
                                    er_val body_v, er_bc_code* code);

static bool er_bc_lift_call_inner(er_bc_vals* out, er_val f_v, er_val x_v)
{
    er_val ff_v = 0;
    er_val xx_v = 0;
    if (er_bc_is_call(f_v, &ff_v, &xx_v)) {
        return er_bc_lift_call_inner(out, ff_v, xx_v) && er_bc_vals_push(out, x_v);
    }
    return er_bc_vals_push(out, f_v) && er_bc_vals_push(out, x_v);
}

static bool er_bc_lift_call(er_bc_compiler* c, er_val f_v, er_val x_v, er_bc_vals* out)
{
    er_bc_vals_init(out, c->loc_a);
    if (!er_bc_lift_call_inner(out, f_v, x_v)) {
        return false;
    }
    er_val head_v = out->val_s == 0 ? 0 : er_bc_pull_const(out->val_v[0]);
    er_app* app = er_outt(er_tag_app, head_v);
    if (app == NULL || app->fn_v == 0) {
        return true;
    }

    er_bc_vals expanded;
    er_bc_vals_init(&expanded, c->loc_a);
    bool ok_f = er_bc_vals_push(&expanded, er_bc_pull_const(app->fn_v));
    for (size_t k = 0; ok_f && k < app->arg_s; k++) {
        /*
         * Captured partial-application arguments are runtime values. Keep them
         * quoted so small natural literals are not recompiled as local refs.
         */
        ok_f = er_bc_vals_push_const(&expanded, app->arg_v[k]);
    }
    for (size_t k = 1; ok_f && k < out->val_s; k++) {
        ok_f = er_bc_vals_push(&expanded, out->val_v[k]);
    }
    if (!ok_f) {
        er_bc_vals_free(&expanded);
        return false;
    }
    er_bc_vals_free(out);
    *out = expanded;
    return true;
}

static bool er_bc_compile_args(er_bc_compiler* c, size_t depth_s, uint32_t ari_d,
                               const er_val* arg_v, size_t arg_s, er_bc_code* code)
{
    for (size_t k = 0; k < arg_s; k++) {
        if (!er_bc_compile_value(c, depth_s, ari_d, arg_v[k], code)) {
            return false;
        }
    }
    return true;
}

static bool er_bc_emit_eval_stack_arg(er_bc_code* code, size_t arg_s, size_t arg_i,
                                      er_bc_eval_req eval)
{
    if (eval == ER_BC_EVAL_NONE) {
        return true;
    }
    if (arg_i >= arg_s || arg_s > UINT32_MAX) {
        code->ok_f = false;
        return false;
    }

    size_t span_s = arg_s - arg_i;
    if (span_s > UINT32_MAX) {
        code->ok_f = false;
        return false;
    }

    /*
     * Bring the selected argument to top, evaluate it, then left-rotate the
     * same window enough times to restore the primitive's operand order.
     */
    uint32_t span_d = (uint32_t)span_s;
    if (span_s > 1 && !er_bc_emit_u32(code, OP_ROTATE, span_d)) {
        return false;
    }
    if (!er_bc_emit(code, eval == ER_BC_EVAL_NF ? OP_FORCE : OP_EVAL)) {
        return false;
    }
    for (size_t k = 1; k < span_s; k++) {
        if (!er_bc_emit_u32(code, OP_ROTATE, span_d)) {
            return false;
        }
    }
    return true;
}

static bool er_bc_emit_prim_arg_evals(const er_bc_prim* prim, er_bc_code* code)
{
    if (prim->ari_d > ER_BC_MAX_PRIM_ARITY) {
        code->ok_f = false;
        return false;
    }
    for (size_t k = 0; k < prim->ari_d; k++) {
        if (!er_bc_emit_eval_stack_arg(code, prim->ari_d, k, prim->arg_eval_v[k])) {
            return false;
        }
    }
    return true;
}

bool er_bc_emit_prim_route_fragment(er_op out_v[], size_t cap_s, er_optag tag, size_t arg_s,
                                    const er_bc_eval_req arg_eval_v[], er_val lit_v,
                                    size_t* out_s)
{
    er_bc_code code = {
        .op_v = out_v,
        .op_s = 0,
        .cap_s = cap_s,
        .ok_f = true,
    };
    if (out_v == NULL || arg_eval_v == NULL || out_s == NULL || tag >= OP_COUNT ||
        arg_s > ER_BC_MAX_PRIM_ARITY) {
        return false;
    }
    for (size_t k = 0; k < arg_s; k++) {
        if (!er_bc_emit_eval_stack_arg(&code, arg_s, k, arg_eval_v[k])) {
            return false;
        }
    }
    if (!er_bc_emit(&code, tag)) {
        return false;
    }
    code.op_v[code.op_s - 1u].as.lit_v = lit_v;
    *out_s = code.op_s;
    return true;
}

static bool er_bc_compile_call_tail(er_bc_code* code, uint32_t fun_ari_d, size_t arg_s)
{
    if (arg_s > UINT32_MAX) {
        code->ok_f = false;
        return false;
    }
    if (arg_s > fun_ari_d) {
        return er_bc_emit_u32(code, OP_CALLU, (uint32_t)arg_s);
    }
    if (arg_s < fun_ari_d) {
        return er_bc_emit_u32(code, OP_MK_APP, (uint32_t)arg_s + 1);
    }
    return er_bc_emit_u32(code, OP_CALLF, fun_ari_d);
}

static bool er_bc_compile_label(er_bc_compiler* c, size_t depth_s, uint32_t ari_d,
                                uint32_t label_d, er_val body_v)
{
    ENKI_PROFILE_ZONE("er_bc_compile_label");
    er_bc_code code = {.loc_a = c->loc_a, .ok_f = true};
    if (!er_bc_compile_expr(c, depth_s, ari_d, body_v, &code, true) ||
        !er_bc_emit(&code, OP_RET)) {
        er_bc_code_free(&code);
        c->ok_f = false;
        return false;
    }
    if (!er_bc_compiler_set_label(c, label_d, code.op_v, code.op_s)) {
        er_bc_code_free(&code);
        return false;
    }
    return true;
}

static bool er_bc_compile_if(er_bc_compiler* c, size_t depth_s, uint32_t ari_d,
                             const er_val* arg_v, er_bc_code* code, bool zero_f)
{
    ENKI_PROFILE_ZONE("er_bc_compile_if");
    if (!er_bc_compile_expr(c, depth_s, ari_d, arg_v[0], code, false)) {
        return false;
    }
    uint32_t true_label_d = 0;
    if (!er_bc_compiler_fresh(c, &true_label_d) ||
        !er_bc_compile_label(c, depth_s, ari_d, true_label_d, arg_v[1]) ||
        !er_bc_emit(code, OP_EVAL) ||
        !er_bc_emit_u32(code, zero_f ? OP_JUMP_IF_ZERO : OP_JUMP_IF, true_label_d)) {
        return false;
    }
    return er_bc_compile_expr(c, depth_s, ari_d, arg_v[2], code, true);
}

static bool er_bc_compile_prim_call(er_bc_compiler* c, size_t depth_s, uint32_t ari_d,
                                    const er_bc_prim* prim, er_val f_v, const er_val* arg_v,
                                    size_t arg_s, er_bc_code* code, bool tail_f)
{
    ENKI_PROFILE_ZONE("er_bc_compile_prim_call");
    if (prim->if_f) {
        if (!tail_f || arg_s != prim->ari_d) {
            code->ok_f = false;
            return false;
        }
        return er_bc_compile_if(c, depth_s, ari_d, arg_v, code,
                                prim->tag == OP_JUMP_IF_ZERO);
    }
    if (arg_s == prim->ari_d) {
        return er_bc_compile_args(c, depth_s, ari_d, arg_v, arg_s, code) &&
               er_bc_emit_prim_arg_evals(prim, code) &&
               er_bc_emit(code, prim->tag);
    }
    if (arg_s < prim->ari_d) {
        return er_bc_emit_lit(code, er_bc_pull_const(f_v)) &&
               er_bc_compile_args(c, depth_s, ari_d, arg_v, arg_s, code) &&
               er_bc_emit_u32(code, OP_MK_APP, (uint32_t)arg_s + 1);
    }

    return er_bc_compile_args(c, depth_s, ari_d, arg_v, prim->ari_d, code) &&
           er_bc_emit_prim_arg_evals(prim, code) &&
           er_bc_emit(code, prim->tag) &&
           er_bc_compile_args(c, depth_s, ari_d, arg_v + prim->ari_d, arg_s - prim->ari_d,
                              code) &&
           er_bc_emit_u32(code, OP_CALLU, (uint32_t)(arg_s - prim->ari_d));
}

static bool er_bc_compile_direct_prim_body(er_bc_compiler* c, size_t depth_s, uint32_t ari_d,
                                           er_val body_v, er_bc_code* code, bool* done_f)
{
    *done_f = false;
    er_val key_v = 0;
    if (!er_bc_direct_prim_body_key(body_v, ari_d, &key_v)) {
        return true;
    }

    const er_bc_prim* prim = er_bc_prim_lookup(key_v);
    if (prim == NULL || prim->ari_d != ari_d || prim->ari_d > ER_BC_MAX_PRIM_ARITY) {
        c->ok_f = false;
        return false;
    }

    er_val arg_v[ER_BC_MAX_PRIM_ARITY];
    for (uint32_t k = 0; k < prim->ari_d; k++) {
        arg_v[k] = (er_val)k + 1u;
    }

    *done_f = true;
    return er_bc_compile_prim_call(c, depth_s, ari_d, prim, key_v, arg_v, prim->ari_d, code,
                                   true);
}

static bool er_bc_compile_plain_call(er_bc_compiler* c, size_t depth_s, uint32_t ari_d,
                                     er_val f_v, const er_val* arg_v, size_t arg_s,
                                     er_bc_code* code)
{
    ENKI_PROFILE_ZONE("er_bc_compile_plain_call");
    if (f_v == 0) {
        return er_bc_emit(code, OP_PUSH_SELF) &&
               er_bc_compile_args(c, depth_s, ari_d, arg_v, arg_s, code) &&
               er_bc_compile_call_tail(code, ari_d, arg_s);
    }

    if (er_bc_is_var(depth_s, f_v)) {
        return er_bc_emit_var(code, f_v) &&
               er_bc_compile_args(c, depth_s, ari_d, arg_v, arg_s, code) &&
               er_bc_emit_u32(code, OP_CALLU, (uint32_t)arg_s);
    }

    er_val lit_v = er_bc_pull_const(f_v);
    if (er_bc_is_call(lit_v, &(er_val){0}, &(er_val){0})) {
        c->ok_f = false;
        return false;
    }
    if (er_bc_is_prim_pin(lit_v) && arg_s > 0) {
        if (arg_s > UINT32_MAX) {
            code->ok_f = false;
            return false;
        }
        return er_bc_emit_lit(code, lit_v) &&
               er_bc_compile_app_value(c, depth_s, ari_d, arg_v[0], code) &&
               er_bc_compile_args(c, depth_s, ari_d, arg_v + 1, arg_s - 1, code) &&
               er_bc_compile_call_tail(code, 1, arg_s);
    }
    uint32_t lit_ari_d = er_bc_arity(lit_v);
    if (lit_ari_d == 0) {
        if (arg_s > UINT32_MAX - 1u) {
            code->ok_f = false;
            return false;
        }
        return er_bc_emit_lit(code, lit_v) &&
               er_bc_compile_args(c, depth_s, ari_d, arg_v, arg_s, code) &&
               er_bc_emit_u32(code, OP_MK_APP, (uint32_t)arg_s + 1u);
    }

    return er_bc_emit_lit(code, lit_v) &&
           er_bc_compile_args(c, depth_s, ari_d, arg_v, arg_s, code) &&
           er_bc_compile_call_tail(code, lit_ari_d, arg_s);
}

static bool er_bc_compile_call(er_bc_compiler* c, size_t depth_s, uint32_t ari_d, er_val f_v,
                               er_val x_v, er_bc_code* code, bool tail_f)
{
    ENKI_PROFILE_ZONE("er_bc_compile_call");
    er_bc_vals lifted;
    if (!er_bc_lift_call(c, f_v, x_v, &lifted)) {
        er_bc_vals_free(&lifted);
        c->ok_f = false;
        return false;
    }
    if (lifted.val_s == 0) {
        er_bc_vals_free(&lifted);
        c->ok_f = false;
        return false;
    }

    er_val head_v = lifted.val_v[0];
    const er_val* arg_v = lifted.val_v + 1;
    size_t arg_s = lifted.val_s - 1;
    const er_bc_prim* prim = er_bc_prim_get(er_bc_pull_const(head_v));
    if (prim != NULL && prim->if_f && (!tail_f || arg_s != prim->ari_d)) {
        prim = NULL;
    }
    bool ok_f;
    if (prim != NULL) {
        ok_f = er_bc_compile_prim_call(c, depth_s, ari_d, prim, head_v, arg_v, arg_s, code,
                                       tail_f);
    } else {
        ok_f = er_bc_compile_plain_call(c, depth_s, ari_d, head_v, arg_v, arg_s, code);
    }
    er_bc_vals_free(&lifted);
    return ok_f;
}

static bool er_bc_compile_value(er_bc_compiler* c, size_t depth_s, uint32_t ari_d, er_val body_v,
                                er_bc_code* code)
{
    if (er_bc_is_var(depth_s, body_v)) {
        return er_bc_emit_var(code, body_v);
    }

    er_val f_v = 0;
    er_val x_v = 0;
    if (!er_bc_is_call(body_v, &f_v, &x_v)) {
        er_app* app = er_outt(er_tag_app, body_v);
        if (app != NULL && !(app->fn_v == 0 && app->arg_s == 1)) {
            if (app->arg_s > UINT32_MAX - 1u) {
                code->ok_f = false;
                return false;
            }
            if (!er_bc_compile_value(c, depth_s, ari_d, app->fn_v, code)) {
                return false;
            }
            for (size_t k = 0; k < app->arg_s; k++) {
                if (!er_bc_compile_value(c, depth_s, ari_d, app->arg_v[k], code)) {
                    return false;
                }
            }
            return er_bc_emit_u32(code, OP_MK_APP, (uint32_t)app->arg_s + 1u);
        }
        return er_bc_emit_lit(code, er_bc_pull_const(body_v));
    }

    er_bc_vals lifted;
    if (!er_bc_lift_call(c, f_v, x_v, &lifted)) {
        er_bc_vals_free(&lifted);
        c->ok_f = false;
        return false;
    }
    if (lifted.val_s == 0 || lifted.val_s > UINT32_MAX) {
        er_bc_vals_free(&lifted);
        c->ok_f = false;
        return false;
    }

    bool ok_f = true;
    er_val head_v = lifted.val_v[0];
    if (head_v == 0) {
        ok_f = er_bc_emit(code, OP_PUSH_SELF);
    } else {
        ok_f = er_bc_compile_value(c, depth_s, ari_d, head_v, code);
    }
    for (size_t k = 1; ok_f && k < lifted.val_s; k++) {
        ok_f = er_bc_compile_value(c, depth_s, ari_d, lifted.val_v[k], code);
    }
    if (ok_f) {
        ok_f = er_bc_emit_u32(code, OP_MK_CALL, (uint32_t)lifted.val_s);
    }
    er_bc_vals_free(&lifted);
    return ok_f;
}

static bool er_bc_compile_app_value(er_bc_compiler* c, size_t depth_s, uint32_t ari_d,
                                    er_val body_v, er_bc_code* code)
{
    er_val f_v = 0;
    er_val x_v = 0;
    if (!er_bc_is_call(body_v, &f_v, &x_v)) {
        return er_bc_compile_value(c, depth_s, ari_d, body_v, code);
    }

    er_bc_vals lifted;
    if (!er_bc_lift_call(c, f_v, x_v, &lifted)) {
        er_bc_vals_free(&lifted);
        c->ok_f = false;
        return false;
    }
    if (lifted.val_s == 0 || lifted.val_s > UINT32_MAX) {
        er_bc_vals_free(&lifted);
        c->ok_f = false;
        return false;
    }

    bool ok_f = true;
    for (size_t k = 0; ok_f && k < lifted.val_s; k++) {
        ok_f = er_bc_compile_value(c, depth_s, ari_d, lifted.val_v[k], code);
    }
    if (ok_f) {
        ok_f = er_bc_emit_u32(code, OP_MK_APP, (uint32_t)lifted.val_s);
    }
    er_bc_vals_free(&lifted);
    return ok_f;
}

static bool er_bc_compile_expr(er_bc_compiler* c, size_t depth_s, uint32_t ari_d, er_val body_v,
                               er_bc_code* code, bool tail_f)
{
    ENKI_PROFILE_ZONE("er_bc_compile_expr");
    bool done_f = false;
    if (!er_bc_compile_direct_prim_body(c, depth_s, ari_d, body_v, code, &done_f)) {
        return false;
    }
    if (done_f) {
        return true;
    }

    if (er_bc_is_var(depth_s, body_v)) {
        return er_bc_emit_var(code, body_v);
    }

    er_val f_v = 0;
    er_val x_v = 0;
    if (er_bc_is_call(body_v, &f_v, &x_v)) {
        return er_bc_compile_call(c, depth_s, ari_d, f_v, x_v, code, tail_f);
    }

    return er_bc_compile_value(c, depth_s, ari_d, body_v, code);
}

er_val er_law_compile(const enki_allocator* loc_a, er_val nam_v, er_val bod_v, uint32_t ari_d)
{
    ENKI_PROFILE_ZONE("er_law_compile");
    if (loc_a == NULL || loc_a->alloc == NULL || loc_a->free == NULL) {
        return 0;
    }
    enki_gc* gc = enki_gc_from_allocator(loc_a);
    const enki_allocator* work_a = gc == NULL ? loc_a : enki_gc_parent_allocator(gc);
    if (work_a == NULL || work_a->alloc == NULL || work_a->free == NULL) {
        return 0;
    }

    er_val scan_v = bod_v;
    er_val let_v = 0;
    er_val next_v = 0;
    size_t let_s = 0;
    while (er_bc_is_let(scan_v, &let_v, &next_v)) {
        if (let_s == UINT32_MAX || ari_d > UINT32_MAX - (uint32_t)let_s - 1) {
            return 0;
        }
        let_s++;
        scan_v = next_v;
    }

    er_val law_v = 0;
    er_val* lets_v = NULL;
    er_op** code_v = NULL;
    size_t* code_len_v = NULL;
    er_bc_compiler c = {
        .loc_a = work_a,
        .next_label_d = (uint32_t)let_s + 1,
        .ok_f = true,
    };

    if (let_s > 0) {
        lets_v = work_a->alloc(work_a->ctx, let_s * sizeof(er_val));
        if (lets_v == NULL) {
            goto cleanup;
        }
    }

    scan_v = bod_v;
    for (size_t k = 0; k < let_s; k++) {
        if (!er_bc_is_let(scan_v, &lets_v[k], &scan_v)) {
            goto cleanup;
        }
    }

    size_t depth_s = (size_t)ari_d + let_s;
    for (size_t k = 0; k < let_s; k++) {
        if (!er_bc_compile_label(&c, depth_s, ari_d, (uint32_t)k + 1, lets_v[k])) {
            goto cleanup;
        }
    }
    if (!er_bc_compile_label(&c, depth_s, ari_d, 0, scan_v)) {
        goto cleanup;
    }

    size_t bc_s = c.label_s;
    if (!c.ok_f || bc_s == 0) {
        goto cleanup;
    }
    code_v = work_a->alloc(work_a->ctx, bc_s * sizeof(er_op*));
    if (code_v == NULL) {
        goto cleanup;
    }
    code_len_v = work_a->alloc(work_a->ctx, bc_s * sizeof(size_t));
    if (code_len_v == NULL) {
        goto cleanup;
    }
    for (size_t k = 0; k < bc_s; k++) {
        if (!c.label_v[k].set_f || c.label_v[k].code_v == NULL) {
            goto cleanup;
        }
        code_v[k] = c.label_v[k].code_v;
        code_len_v[k] = c.label_v[k].code_s;
    }

    law_v = er_law_make_code(loc_a, nam_v, bod_v, ari_d, (uint32_t)let_s, bc_s, code_v,
                             code_len_v);

cleanup:
    if (code_len_v != NULL) {
        work_a->free(work_a->ctx, code_len_v);
    }
    if (code_v != NULL) {
        work_a->free(work_a->ctx, code_v);
    }
    if (lets_v != NULL) {
        work_a->free(work_a->ctx, lets_v);
    }
    er_bc_compiler_free(&c, true);
    return law_v;
}

#undef ER_BC_ROUTE6
#undef ER_BC_ROUTE4
#undef ER_BC_ROUTE3
#undef ER_BC_ROUTE2
#undef ER_BC_ROUTE1
#undef ER_BC_ROUTE0
#undef ER_BC_ROUTE_ARGS
#undef ER_BC_N
#undef ER_BC_W
#undef ER_BC_L
