#include "enki/plan.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "enki/app.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/nat.h"
#include "enki/pin.h"
#include "enki/value.h"

#define PLAN_CH(c) ((uint64_t)(uint8_t)(c))
#define PLAN_S1(a) ((enki_value)PLAN_CH(a))
#define PLAN_S2(a, b) ((enki_value)(PLAN_CH(a) | (PLAN_CH(b) << 8u)))
#define PLAN_S3(a, b, c) ((enki_value)(PLAN_S2(a, b) | (PLAN_CH(c) << 16u)))
#define PLAN_S4(a, b, c, d) ((enki_value)(PLAN_S3(a, b, c) | (PLAN_CH(d) << 24u)))
#define PLAN_S5(a, b, c, d, e) ((enki_value)(PLAN_S4(a, b, c, d) | (PLAN_CH(e) << 32u)))
#define PLAN_S6(a, b, c, d, e, f) ((enki_value)(PLAN_S5(a, b, c, d, e) | (PLAN_CH(f) << 40u)))
#define PLAN_S7(a, b, c, d, e, f, g) ((enki_value)(PLAN_S6(a, b, c, d, e, f) | (PLAN_CH(g) << 48u)))

typedef struct {
    enki_value head_v;
    size_t n_args_s;
    enki_value* args_v;
} enki_plan_spine;

typedef struct {
    enki_plan_spine spine;
    enki_value tag_v;
    size_t n_args_s;
    enki_value* args_v;
} enki_plan_row;

static enki_error plan_eval_whnf_inner(enki_plan* plan, enki_value val_v, enki_value* out_v);
static enki_error plan_eval_whnf_tail(enki_plan* plan, enki_value val_v, enki_value self_v,
                                      enki_law* law, enki_value* env_v, bool* tail_call_f,
                                      enki_value* out_v);
static enki_error plan_eval_nf_inner(enki_plan* plan, enki_value val_v, enki_value* out_v);
static enki_error plan_apply_inner(enki_plan* plan, enki_value fn_v, size_t n_args_s,
                                   const enki_value* args_v, enki_value* out_v);
static enki_error plan_arity_value(enki_plan* plan, enki_value val_v, size_t* out_s);

static enki_error plan_fail(enki_plan* plan, enki_error error, enki_value val_v)
{
    if (plan != NULL) {
        plan->error = error;
        plan->error_v = val_v;
    }
    return error;
}

static enki_value_header* plan_header(enki_value val_v)
{
    if (!IS_PTR(val_v))
        return NULL;
    return ENKI_AS(enki_value_header, val_v);
}

static bool plan_is_kind(enki_value val_v, uint8_t kind_b)
{
    enki_value_header* h = plan_header(val_v);
    return h != NULL && h->kind_b == kind_b;
}

static bool plan_is_nat(enki_value val_v)
{
    return !IS_PTR(val_v) || plan_is_kind(val_v, ENKI_NAT);
}

static bool plan_small_nat(enki_value val_v, size_t* out_s)
{
    if (!IS_PTR(val_v)) {
        *out_s = (size_t)val_v;
        return true;
    }
    if (!plan_is_kind(val_v, ENKI_NAT))
        return false;
    enki_nat* nat = ENKI_AS(enki_nat, val_v);
    if (nat->n_limbs_s == 0) {
        *out_s = 0;
        return true;
    }
    if (nat->n_limbs_s == 1 && nat->limbs[0] <= (mp_limb_t)SIZE_MAX) {
        *out_s = (size_t)nat->limbs[0];
        return true;
    }
    return false;
}

static enki_error plan_nat_inner(enki_plan* plan, enki_value val_v, enki_value* out_v)
{
    enki_error err = plan_eval_whnf_inner(plan, val_v, &val_v);
    if (err != ENKI_ERROR_OK)
        return err;
    *out_v = plan_is_nat(val_v) ? val_v : (enki_value)0;
    return ENKI_ERROR_OK;
}

static enki_error plan_alloc_app(enki_plan* plan, enki_value fn_v, size_t n_args_s,
                                 const enki_value* args_v, enki_value* out_v)
{
    if (n_args_s == 0) {
        *out_v = fn_v;
        return ENKI_ERROR_OK;
    }
    enki_value app_v = enki_app_alloc(plan->gc, fn_v, n_args_s);
    enki_app* app = ENKI_AS(enki_app, app_v);
    if (args_v != NULL) {
        memcpy(app->args_v, args_v, n_args_s * sizeof(enki_value));
    }
    *out_v = app_v;
    return ENKI_ERROR_OK;
}

static enki_error plan_make_app(enki_plan* plan, enki_value fn_v, size_t n_args_s,
                                const enki_value* args_v, enki_value* out_v)
{
    if (n_args_s == 0) {
        *out_v = fn_v;
        return ENKI_ERROR_OK;
    }
    if (plan_is_kind(fn_v, ENKI_APP)) {
        enki_app* old = ENKI_AS(enki_app, fn_v);
        size_t total_s = old->n_args_s + n_args_s;
        enki_value app_v = enki_app_alloc(plan->gc, old->fn_v, total_s);
        enki_app* app = ENKI_AS(enki_app, app_v);
        memcpy(app->args_v, old->args_v, old->n_args_s * sizeof(enki_value));
        memcpy(app->args_v + old->n_args_s, args_v, n_args_s * sizeof(enki_value));
        *out_v = app_v;
        return ENKI_ERROR_OK;
    }
    return plan_alloc_app(plan, fn_v, n_args_s, args_v, out_v);
}

static size_t plan_spine_count(enki_value val_v)
{
    if (!plan_is_kind(val_v, ENKI_APP))
        return 0;
    enki_app* app = ENKI_AS(enki_app, val_v);
    return plan_spine_count(app->fn_v) + app->n_args_s;
}

static enki_value plan_spine_head(enki_value val_v)
{
    while (plan_is_kind(val_v, ENKI_APP)) {
        enki_app* app = ENKI_AS(enki_app, val_v);
        val_v = app->fn_v;
    }
    return val_v;
}

static void plan_spine_fill(enki_value val_v, enki_value* args_v, size_t* cursor_s)
{
    if (!plan_is_kind(val_v, ENKI_APP))
        return;
    enki_app* app = ENKI_AS(enki_app, val_v);
    plan_spine_fill(app->fn_v, args_v, cursor_s);
    memcpy(args_v + *cursor_s, app->args_v, app->n_args_s * sizeof(enki_value));
    *cursor_s += app->n_args_s;
}

static bool plan_rebind_tail_call(enki_value val_v, enki_value self_v, enki_law* law,
                                  enki_value* env_v)
{
    if (!plan_is_kind(val_v, ENKI_APP))
        return false;
    if (plan_spine_head(val_v) != self_v)
        return false;
    if (plan_spine_count(val_v) != law->arity_s)
        return false;
    size_t cursor_s = 0;
    plan_spine_fill(val_v, env_v + 1, &cursor_s);
    return cursor_s == law->arity_s;
}

static enki_error plan_spine_open(enki_plan* plan, enki_value val_v, enki_plan_spine* out)
{
    out->head_v = plan_spine_head(val_v);
    out->n_args_s = plan_spine_count(val_v);
    out->args_v = NULL;
    if (out->n_args_s == 0)
        return ENKI_ERROR_OK;
    out->args_v = malloc(out->n_args_s * sizeof(enki_value));
    if (out->args_v == NULL)
        return plan_fail(plan, ENKI_ERROR_OOM, val_v);
    size_t cursor_s = 0;
    plan_spine_fill(val_v, out->args_v, &cursor_s);
    return ENKI_ERROR_OK;
}

static void plan_spine_close(enki_plan_spine* spine)
{
    free(spine->args_v);
    spine->args_v = NULL;
    spine->n_args_s = 0;
    spine->head_v = 0;
}

static enki_error plan_row_open(enki_plan* plan, enki_value val_v, enki_plan_row* out)
{
    enki_error err = plan_spine_open(plan, val_v, &out->spine);
    if (err != ENKI_ERROR_OK)
        return err;
    if (out->spine.head_v == 0 && out->spine.n_args_s > 0) {
        out->tag_v = out->spine.args_v[0];
        out->args_v = out->spine.args_v + 1;
        out->n_args_s = out->spine.n_args_s - 1;
        return ENKI_ERROR_OK;
    }
    out->tag_v = out->spine.head_v;
    out->args_v = out->spine.args_v;
    out->n_args_s = out->spine.n_args_s;
    return ENKI_ERROR_OK;
}

static void plan_row_close(enki_plan_row* row)
{
    plan_spine_close(&row->spine);
    row->tag_v = 0;
    row->n_args_s = 0;
    row->args_v = NULL;
}

static enki_error plan_require_argc(enki_plan* plan, size_t have_s, size_t want_s)
{
    if (have_s != want_s)
        return plan_fail(plan, ENKI_ERROR_BOUNDS, (enki_value)have_s);
    return ENKI_ERROR_OK;
}

static enki_error plan_arg_nat(enki_plan* plan, const enki_value* args_v, size_t idx_s,
                               enki_value* out_v)
{
    return plan_nat_inner(plan, args_v[idx_s], out_v);
}

static enki_error plan_arg_small(enki_plan* plan, const enki_value* args_v, size_t idx_s,
                                 size_t* out_s)
{
    enki_value nat_v = 0;
    enki_error err = plan_arg_nat(plan, args_v, idx_s, &nat_v);
    if (err != ENKI_ERROR_OK)
        return err;
    if (!plan_small_nat(nat_v, out_s))
        return plan_fail(plan, ENKI_ERROR_BOUNDS, nat_v);
    return ENKI_ERROR_OK;
}

static enki_value plan_ix_at(enki_value idx_v, enki_value val_v)
{
    size_t idx_s = 0;
    if (!plan_small_nat(idx_v, &idx_s))
        return 0;
    if (!plan_is_kind(val_v, ENKI_APP))
        return 0;
    enki_app* app = ENKI_AS(enki_app, val_v);
    if (idx_s >= app->n_args_s)
        return 0;
    return app->args_v[idx_s];
}

static enki_error plan_structural_equal(enki_plan* plan, enki_value a_v, enki_value b_v,
                                        enki_value* out_v)
{
    enki_error err = plan_eval_nf_inner(plan, a_v, &a_v);
    if (err != ENKI_ERROR_OK)
        return err;
    err = plan_eval_nf_inner(plan, b_v, &b_v);
    if (err != ENKI_ERROR_OK)
        return err;
    if (plan_is_nat(a_v) && plan_is_nat(b_v)) {
        *out_v = enki_nat_eq(a_v, b_v);
        return ENKI_ERROR_OK;
    }
    if (!IS_PTR(a_v) || !IS_PTR(b_v)) {
        *out_v = (a_v == b_v) ? (enki_value)1 : (enki_value)0;
        return ENKI_ERROR_OK;
    }
    enki_value_header* ah = ENKI_AS(enki_value_header, a_v);
    enki_value_header* bh = ENKI_AS(enki_value_header, b_v);
    if (ah->kind_b != bh->kind_b) {
        *out_v = 0;
        return ENKI_ERROR_OK;
    }
    switch (ah->kind_b) {
    case ENKI_PIN: {
        enki_pin* a = ENKI_AS(enki_pin, a_v);
        enki_pin* b = ENKI_AS(enki_pin, b_v);
        if (a->n_subpins_s != b->n_subpins_s) {
            *out_v = 0;
            return ENKI_ERROR_OK;
        }
        err = plan_structural_equal(plan, a->inner_v, b->inner_v, out_v);
        if (err != ENKI_ERROR_OK || *out_v == 0)
            return err;
        for (size_t k = 0; k < a->n_subpins_s; k++) {
            err = plan_structural_equal(plan, a->subpins_v[k], b->subpins_v[k], out_v);
            if (err != ENKI_ERROR_OK || *out_v == 0)
                return err;
        }
        *out_v = 1;
        return ENKI_ERROR_OK;
    }
    case ENKI_LAW: {
        enki_law* a = ENKI_AS(enki_law, a_v);
        enki_law* b = ENKI_AS(enki_law, b_v);
        if (a->arity_s != b->arity_s) {
            *out_v = 0;
            return ENKI_ERROR_OK;
        }
        err = plan_structural_equal(plan, a->name_v, b->name_v, out_v);
        if (err != ENKI_ERROR_OK || *out_v == 0)
            return err;
        return plan_structural_equal(plan, a->body_v, b->body_v, out_v);
    }
    case ENKI_APP: {
        enki_app* a = ENKI_AS(enki_app, a_v);
        enki_app* b = ENKI_AS(enki_app, b_v);
        if (a->n_args_s != b->n_args_s) {
            *out_v = 0;
            return ENKI_ERROR_OK;
        }
        err = plan_structural_equal(plan, a->fn_v, b->fn_v, out_v);
        if (err != ENKI_ERROR_OK || *out_v == 0)
            return err;
        for (size_t k = 0; k < a->n_args_s; k++) {
            err = plan_structural_equal(plan, a->args_v[k], b->args_v[k], out_v);
            if (err != ENKI_ERROR_OK || *out_v == 0)
                return err;
        }
        *out_v = 1;
        return ENKI_ERROR_OK;
    }
    default:
        *out_v = 0;
        return ENKI_ERROR_OK;
    }
}

static bool plan_op66_from_tag(enki_value tag_v, int* out_op)
{
    if (IS_PTR(tag_v))
        return false;
    if (tag_v <= (enki_value)OP66_PRINT_REX) {
        *out_op = (int)tag_v;
        return true;
    }
    switch (tag_v) {
    case PLAN_S3('I', 'n', 'c'):
        *out_op = OP66_INC;
        return true;
    case PLAN_S3('D', 'e', 'c'):
        *out_op = OP66_DEC;
        return true;
    case PLAN_S3('A', 'd', 'd'):
        *out_op = OP66_ADD;
        return true;
    case PLAN_S3('S', 'u', 'b'):
        *out_op = OP66_SUB;
        return true;
    case PLAN_S3('M', 'u', 'l'):
        *out_op = OP66_MUL;
        return true;
    case PLAN_S3('D', 'i', 'v'):
        *out_op = OP66_DIV;
        return true;
    case PLAN_S3('M', 'o', 'd'):
        *out_op = OP66_MOD;
        return true;
    case PLAN_S2('E', 'q'):
        *out_op = OP66_EQ;
        return true;
    case PLAN_S2('N', 'e'):
        *out_op = OP66_NE;
        return true;
    case PLAN_S2('L', 't'):
        *out_op = OP66_LT;
        return true;
    case PLAN_S2('L', 'e'):
        *out_op = OP66_LE;
        return true;
    case PLAN_S2('G', 't'):
        *out_op = OP66_GT;
        return true;
    case PLAN_S2('G', 'e'):
        *out_op = OP66_GE;
        return true;
    case PLAN_S3('C', 'm', 'p'):
        *out_op = OP66_CMP;
        return true;
    case PLAN_S3('R', 's', 'h'):
        *out_op = OP66_RSH;
        return true;
    case PLAN_S3('L', 's', 'h'):
        *out_op = OP66_LSH;
        return true;
    case PLAN_S4('T', 'e', 's', 't'):
        *out_op = OP66_TEST;
        return true;
    case PLAN_S3('S', 'e', 't'):
        *out_op = OP66_SET;
        return true;
    case PLAN_S5('C', 'l', 'e', 'a', 'r'):
        *out_op = OP66_CLEAR;
        return true;
    case PLAN_S3('B', 'e', 'x'):
        *out_op = OP66_BEX;
        return true;
    case PLAN_S4('B', 'i', 't', 's'):
        *out_op = OP66_BITS;
        return true;
    case PLAN_S5('B', 'y', 't', 'e', 's'):
        *out_op = OP66_BYTES;
        return true;
    case PLAN_S3('N', 'i', 'b'):
        *out_op = OP66_NIB;
        return true;
    case PLAN_S5('L', 'o', 'a', 'd', '8'):
        *out_op = OP66_LOAD8;
        return true;
    case PLAN_S6('S', 't', 'o', 'r', 'e', '8'):
        *out_op = OP66_STORE8;
        return true;
    case PLAN_S5('T', 'r', 'u', 'n', 'c'):
        *out_op = OP66_TRUNC;
        return true;
    case PLAN_S6('T', 'r', 'u', 'n', 'c', '8'):
        *out_op = OP66_TRUNC8;
        return true;
    case PLAN_S7('T', 'r', 'u', 'n', 'c', '1', '6'):
        *out_op = OP66_TRUNC16;
        return true;
    case PLAN_S7('T', 'r', 'u', 'n', 'c', '3', '2'):
        *out_op = OP66_TRUNC32;
        return true;
    case PLAN_S7('T', 'r', 'u', 'n', 'c', '6', '4'):
        *out_op = OP66_TRUNC64;
        return true;
    case PLAN_S4('T', 'y', 'p', 'e'):
        *out_op = OP66_TYPE;
        return true;
    case PLAN_S5('I', 's', 'P', 'i', 'n'):
        *out_op = OP66_IS_PIN;
        return true;
    case PLAN_S5('I', 's', 'L', 'a', 'w'):
        *out_op = OP66_IS_LAW;
        return true;
    case PLAN_S5('I', 's', 'A', 'p', 'p'):
        *out_op = OP66_IS_APP;
        return true;
    case PLAN_S5('I', 's', 'N', 'a', 't'):
        *out_op = OP66_IS_NAT;
        return true;
    case PLAN_S3('N', 'a', 't'):
        *out_op = OP66_NAT;
        return true;
    case PLAN_S5('U', 'n', 'p', 'i', 'n'):
        *out_op = OP66_UNPIN;
        return true;
    case PLAN_S5('A', 'r', 'i', 't', 'y'):
        *out_op = OP66_ARITY;
        return true;
    case PLAN_S4('N', 'a', 'm', 'e'):
        *out_op = OP66_NAME;
        return true;
    case PLAN_S4('B', 'o', 'd', 'y'):
        *out_op = OP66_BODY;
        return true;
    case PLAN_S2('H', 'd'):
        *out_op = OP66_HD;
        return true;
    case PLAN_S4('L', 'a', 's', 't'):
        *out_op = OP66_LAST;
        return true;
    case PLAN_S4('I', 'n', 'i', 't'):
        *out_op = OP66_INIT;
        return true;
    case PLAN_S3('R', 'o', 'w'):
        *out_op = OP66_ROW;
        return true;
    case PLAN_S3('R', 'e', 'p'):
        *out_op = OP66_REP;
        return true;
    case PLAN_S5('S', 'l', 'i', 'c', 'e'):
        *out_op = OP66_SLICE;
        return true;
    case PLAN_S4('W', 'e', 'l', 'd'):
        *out_op = OP66_WELD;
        return true;
    case PLAN_S2('U', 'p'):
        *out_op = OP66_UP;
        return true;
    case PLAN_S6('U', 'p', 'U', 'n', 'i', 'q'):
        *out_op = OP66_UP_UNIQ;
        return true;
    case PLAN_S4('C', 'o', 'u', 'p'):
        *out_op = OP66_COUP;
        return true;
    case PLAN_S2('S', 'z'):
        *out_op = OP66_SZ;
        return true;
    case PLAN_S2('I', 'x'):
        *out_op = OP66_IX;
        return true;
    case PLAN_S3('I', 'x', '0'):
        *out_op = OP66_IX0;
        return true;
    case PLAN_S3('I', 'x', '1'):
        *out_op = OP66_IX1;
        return true;
    case PLAN_S3('I', 'x', '2'):
        *out_op = OP66_IX2;
        return true;
    case PLAN_S3('I', 'x', '3'):
        *out_op = OP66_IX3;
        return true;
    case PLAN_S3('I', 'x', '4'):
        *out_op = OP66_IX4;
        return true;
    case PLAN_S3('I', 'x', '5'):
        *out_op = OP66_IX5;
        return true;
    case PLAN_S3('I', 'x', '6'):
        *out_op = OP66_IX6;
        return true;
    case PLAN_S3('I', 'x', '7'):
        *out_op = OP66_IX7;
        return true;
    case PLAN_S4('C', 'a', 's', 'e'):
        *out_op = OP66_CASE;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '2'):
        *out_op = OP66_CASE2;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '3'):
        *out_op = OP66_CASE3;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '4'):
        *out_op = OP66_CASE4;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '5'):
        *out_op = OP66_CASE5;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '6'):
        *out_op = OP66_CASE6;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '7'):
        *out_op = OP66_CASE7;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '8'):
        *out_op = OP66_CASE8;
        return true;
    case PLAN_S5('C', 'a', 's', 'e', '9'):
        *out_op = OP66_CASE9;
        return true;
    case PLAN_S3('N', 'i', 'l'):
        *out_op = OP66_NIL;
        return true;
    case PLAN_S5('T', 'r', 'u', 't', 'h'):
        *out_op = OP66_TRUTH;
        return true;
    case PLAN_S2('O', 'r'):
        *out_op = OP66_OR;
        return true;
    case PLAN_S3('N', 'o', 'r'):
        *out_op = OP66_NOR;
        return true;
    case PLAN_S3('A', 'n', 'd'):
        *out_op = OP66_AND;
        return true;
    case PLAN_S2('I', 'f'):
        *out_op = OP66_IF;
        return true;
    case PLAN_S3('I', 'f', 'z'):
        *out_op = OP66_IFZ;
        return true;
    case PLAN_S3('S', 'e', 'q'):
        *out_op = OP66_SEQ;
        return true;
    case PLAN_S4('S', 'e', 'q', '2'):
        *out_op = OP66_SEQ2;
        return true;
    case PLAN_S4('S', 'e', 'q', '3'):
        *out_op = OP66_SEQ3;
        return true;
    case PLAN_S3('S', 'a', 'p'):
        *out_op = OP66_SAP;
        return true;
    case PLAN_S4('S', 'a', 'p', '2'):
        *out_op = OP66_SAP2;
        return true;
    case PLAN_S5('F', 'o', 'r', 'c', 'e'):
        *out_op = OP66_FORCE;
        return true;
    case PLAN_S7('D', 'e', 'e', 'p', 's', 'e', 'q'):
        *out_op = OP66_DEEPSEQ;
        return true;
    case PLAN_S3('T', 'r', 'y'):
        *out_op = OP66_TRY;
        return true;
    case PLAN_S5('T', 'h', 'r', 'o', 'w'):
        *out_op = OP66_THROW;
        return true;
    case PLAN_S4('S', 'a', 'v', 'e'):
        *out_op = OP66_SAVE;
        return true;
    case PLAN_S4('L', 'o', 'a', 'd'):
        *out_op = OP66_LOAD;
        return true;
    case PLAN_S5('T', 'r', 'a', 'c', 'e'):
        *out_op = OP66_TRACE;
        return true;
    case PLAN_S5('E', 'q', 'u', 'a', 'l'):
        *out_op = OP66_EQUAL;
        return true;
    default:
        return false;
    }
}

static enki_error plan_case_n(enki_plan* plan, const enki_value* args_v, size_t n_args_s,
                              size_t n_cases_s, enki_value* out_v)
{
    if (n_args_s != n_cases_s + 2)
        return plan_fail(plan, ENKI_ERROR_BOUNDS, (enki_value)n_args_s);
    size_t ix_s = 0;
    if (!plan_small_nat(args_v[0], &ix_s) || ix_s >= n_cases_s) {
        *out_v = args_v[n_cases_s + 1];
        return ENKI_ERROR_OK;
    }
    *out_v = args_v[ix_s + 1];
    return ENKI_ERROR_OK;
}

static enki_error plan_dispatch_op66(enki_plan* plan, enki_value row_v, enki_value* out_v)
{
    enki_plan_row row;
    enki_error err = plan_row_open(plan, row_v, &row);
    if (err != ENKI_ERROR_OK)
        return err;
    int op = 0;
    if (!plan_op66_from_tag(row.tag_v, &op)) {
        err = plan_fail(plan, ENKI_ERROR_BAD_TAG, row.tag_v);
        goto done;
    }
    enki_value a_v = 0;
    enki_value b_v = 0;
    enki_value c_v = 0;
    size_t n_s = 0;
    switch (op) {
    case OP66_INC:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_nat(plan, row.args_v, 0, &a_v)) != ENKI_ERROR_OK)
            break;
        *out_v = enki_nat_inc(plan->gc, a_v);
        break;
    case OP66_DEC:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_nat(plan, row.args_v, 0, &a_v)) != ENKI_ERROR_OK)
            break;
        *out_v = enki_nat_dec(plan->gc, a_v);
        break;
    case OP66_ADD:
    case OP66_SUB:
    case OP66_MUL:
    case OP66_DIV:
    case OP66_MOD:
    case OP66_EQ:
    case OP66_NE:
    case OP66_LT:
    case OP66_LE:
    case OP66_GT:
    case OP66_GE:
    case OP66_CMP:
    case OP66_RSH:
    case OP66_LSH:
    case OP66_TEST:
    case OP66_SET:
    case OP66_CLEAR:
    case OP66_NIB:
    case OP66_LOAD8:
    case OP66_TRUNC:
        if ((err = plan_require_argc(plan, row.n_args_s, 2)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_nat(plan, row.args_v, 0, &a_v)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_nat(plan, row.args_v, 1, &b_v)) != ENKI_ERROR_OK)
            break;
        switch (op) {
        case OP66_ADD:
            *out_v = enki_nat_add(plan->gc, a_v, b_v);
            break;
        case OP66_SUB:
            *out_v = enki_nat_sub(plan->gc, a_v, b_v);
            break;
        case OP66_MUL:
            *out_v = enki_nat_mul(plan->gc, a_v, b_v);
            break;
        case OP66_DIV:
            if (enki_nat_is_zero(b_v)) {
                err = plan_fail(plan, ENKI_ERROR_DIV_ZERO, b_v);
            } else {
                *out_v = enki_nat_div(plan->gc, a_v, b_v);
            }
            break;
        case OP66_MOD:
            if (enki_nat_is_zero(b_v)) {
                err = plan_fail(plan, ENKI_ERROR_DIV_ZERO, b_v);
            } else {
                *out_v = enki_nat_mod(plan->gc, a_v, b_v);
            }
            break;
        case OP66_EQ:
            *out_v = enki_nat_eq(a_v, b_v);
            break;
        case OP66_NE:
            *out_v = enki_nat_ne(a_v, b_v);
            break;
        case OP66_LT:
            *out_v = enki_nat_lt(a_v, b_v);
            break;
        case OP66_LE:
            *out_v = enki_nat_le(a_v, b_v);
            break;
        case OP66_GT:
            *out_v = enki_nat_gt(a_v, b_v);
            break;
        case OP66_GE:
            *out_v = enki_nat_ge(a_v, b_v);
            break;
        case OP66_CMP:
            *out_v = (enki_value)enki_nat_cmp(a_v, b_v);
            break;
        case OP66_RSH:
            *out_v = enki_nat_rsh(plan->gc, a_v, b_v);
            break;
        case OP66_LSH:
            *out_v = enki_nat_lsh(plan->gc, a_v, b_v);
            break;
        case OP66_TEST:
            *out_v = enki_nat_test(plan->gc, a_v, b_v);
            break;
        case OP66_SET:
            *out_v = enki_nat_set(plan->gc, a_v, b_v);
            break;
        case OP66_CLEAR:
            *out_v = enki_nat_clear(plan->gc, a_v, b_v);
            break;
        case OP66_NIB:
            *out_v = enki_nat_nib(plan->gc, a_v, b_v);
            break;
        case OP66_LOAD8:
            *out_v = enki_nat_load8(plan->gc, a_v, b_v);
            break;
        case OP66_TRUNC:
            *out_v = enki_nat_trunc(plan->gc, a_v, b_v);
            break;
        default:
            break;
        }
        break;
    case OP66_BEX:
    case OP66_BITS:
    case OP66_BYTES:
    case OP66_TRUNC8:
    case OP66_TRUNC16:
    case OP66_TRUNC32:
    case OP66_TRUNC64:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_nat(plan, row.args_v, 0, &a_v)) != ENKI_ERROR_OK)
            break;
        if (op == OP66_BEX)
            *out_v = enki_nat_bex(plan->gc, a_v);
        else if (op == OP66_BITS)
            *out_v = enki_nat_bits(plan->gc, a_v);
        else if (op == OP66_BYTES)
            *out_v = enki_nat_bytes(plan->gc, a_v);
        else if (op == OP66_TRUNC8)
            *out_v = enki_nat_trunc8(plan->gc, a_v);
        else if (op == OP66_TRUNC16)
            *out_v = enki_nat_trunc16(plan->gc, a_v);
        else if (op == OP66_TRUNC32)
            *out_v = enki_nat_trunc32(plan->gc, a_v);
        else
            *out_v = enki_nat_trunc64(plan->gc, a_v);
        break;
    case OP66_STORE8:
        if ((err = plan_require_argc(plan, row.n_args_s, 3)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_nat(plan, row.args_v, 0, &a_v)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_nat(plan, row.args_v, 1, &b_v)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_nat(plan, row.args_v, 2, &c_v)) != ENKI_ERROR_OK)
            break;
        *out_v = enki_nat_store8(plan->gc, a_v, b_v, c_v);
        break;
    case OP66_TYPE:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        a_v = row.args_v[0];
        if (!IS_PTR(a_v) || plan_is_kind(a_v, ENKI_NAT))
            *out_v = 0;
        else if (plan_is_kind(a_v, ENKI_PIN))
            *out_v = 1;
        else if (plan_is_kind(a_v, ENKI_LAW))
            *out_v = 2;
        else if (plan_is_kind(a_v, ENKI_APP))
            *out_v = 3;
        else
            err = plan_fail(plan, ENKI_ERROR_BAD_TAG, a_v);
        break;
    case OP66_IS_PIN:
    case OP66_IS_LAW:
    case OP66_IS_APP:
    case OP66_IS_NAT:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        a_v = row.args_v[0];
        if (op == OP66_IS_PIN)
            *out_v = plan_is_kind(a_v, ENKI_PIN) ? 1 : 0;
        else if (op == OP66_IS_LAW)
            *out_v = plan_is_kind(a_v, ENKI_LAW) ? 1 : 0;
        else if (op == OP66_IS_APP)
            *out_v = plan_is_kind(a_v, ENKI_APP) ? 1 : 0;
        else
            *out_v = plan_is_nat(a_v) ? 1 : 0;
        break;
    case OP66_NAT:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        err = plan_nat_inner(plan, row.args_v[0], out_v);
        break;
    case OP66_UNPIN:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        *out_v =
            plan_is_kind(row.args_v[0], ENKI_PIN) ? ENKI_AS(enki_pin, row.args_v[0])->inner_v : 0;
        break;
    case OP66_ARITY:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arity_value(plan, row.args_v[0], &n_s)) != ENKI_ERROR_OK)
            break;
        *out_v = (enki_value)n_s;
        break;
    case OP66_NAME:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        *out_v =
            plan_is_kind(row.args_v[0], ENKI_LAW) ? ENKI_AS(enki_law, row.args_v[0])->name_v : 0;
        break;
    case OP66_BODY:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        *out_v =
            plan_is_kind(row.args_v[0], ENKI_LAW) ? ENKI_AS(enki_law, row.args_v[0])->body_v : 0;
        break;
    case OP66_HD:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        *out_v = plan_is_kind(row.args_v[0], ENKI_APP) ? ENKI_AS(enki_app, row.args_v[0])->fn_v
                                                       : row.args_v[0];
        break;
    case OP66_LAST:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        if (plan_is_kind(row.args_v[0], ENKI_APP)) {
            enki_app* app = ENKI_AS(enki_app, row.args_v[0]);
            *out_v = app->n_args_s == 0 ? 0 : app->args_v[app->n_args_s - 1];
        } else {
            *out_v = 0;
        }
        break;
    case OP66_INIT:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        if (!plan_is_kind(row.args_v[0], ENKI_APP)) {
            *out_v = 0;
        } else {
            enki_app* app = ENKI_AS(enki_app, row.args_v[0]);
            if (app->n_args_s <= 1) {
                *out_v = app->fn_v;
            } else {
                err = plan_alloc_app(plan, app->fn_v, app->n_args_s - 1, app->args_v, out_v);
            }
        }
        break;
    case OP66_ROW:
        if ((err = plan_require_argc(plan, row.n_args_s, 3)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_small(plan, row.args_v, 1, &n_s)) != ENKI_ERROR_OK)
            break;
        {
            enki_value app_v = enki_app_alloc(plan->gc, row.args_v[0], n_s);
            enki_app* app = ENKI_AS(enki_app, app_v);
            enki_value cur_v = row.args_v[2];
            for (size_t k = 0; k < n_s; k++) {
                app->args_v[k] = plan_ix_at(0, cur_v);
                cur_v = plan_ix_at(1, cur_v);
            }
            *out_v = app_v;
        }
        break;
    case OP66_REP:
        if ((err = plan_require_argc(plan, row.n_args_s, 3)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_small(plan, row.args_v, 2, &n_s)) != ENKI_ERROR_OK)
            break;
        {
            enki_value app_v = enki_app_alloc(plan->gc, row.args_v[0], n_s);
            enki_app* app = ENKI_AS(enki_app, app_v);
            for (size_t k = 0; k < n_s; k++)
                app->args_v[k] = row.args_v[1];
            *out_v = app_v;
        }
        break;
    case OP66_SLICE:
        if ((err = plan_require_argc(plan, row.n_args_s, 3)) != ENKI_ERROR_OK)
            break;
        {
            size_t off_s = 0;
            size_t len_s = 0;
            if ((err = plan_arg_small(plan, row.args_v, 0, &off_s)) != ENKI_ERROR_OK)
                break;
            if ((err = plan_arg_small(plan, row.args_v, 1, &len_s)) != ENKI_ERROR_OK)
                break;
            if (!plan_is_kind(row.args_v[2], ENKI_APP)) {
                *out_v = 0;
                break;
            }
            enki_app* app = ENKI_AS(enki_app, row.args_v[2]);
            if (off_s >= app->n_args_s) {
                *out_v = 0;
                break;
            }
            size_t keep_s = app->n_args_s - off_s;
            if (keep_s > len_s)
                keep_s = len_s;
            err = plan_alloc_app(plan, 0, keep_s, app->args_v + off_s, out_v);
        }
        break;
    case OP66_WELD:
        if ((err = plan_require_argc(plan, row.n_args_s, 2)) != ENKI_ERROR_OK)
            break;
        {
            enki_app* x =
                plan_is_kind(row.args_v[0], ENKI_APP) ? ENKI_AS(enki_app, row.args_v[0]) : NULL;
            enki_app* y =
                plan_is_kind(row.args_v[1], ENKI_APP) ? ENKI_AS(enki_app, row.args_v[1]) : NULL;
            size_t x_s = x == NULL ? 0 : x->n_args_s;
            size_t y_s = y == NULL ? 0 : y->n_args_s;
            enki_value app_v = enki_app_alloc(plan->gc, 0, x_s + y_s);
            enki_app* app = ENKI_AS(enki_app, app_v);
            if (x_s > 0)
                memcpy(app->args_v, x->args_v, x_s * sizeof(enki_value));
            if (y_s > 0)
                memcpy(app->args_v + x_s, y->args_v, y_s * sizeof(enki_value));
            *out_v = app_v;
        }
        break;
    case OP66_UP:
    case OP66_UP_UNIQ:
        if ((err = plan_require_argc(plan, row.n_args_s, 3)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_arg_small(plan, row.args_v, 0, &n_s)) != ENKI_ERROR_OK)
            break;
        if (!plan_is_kind(row.args_v[2], ENKI_APP)) {
            *out_v = row.args_v[2];
            break;
        }
        {
            enki_app* old = ENKI_AS(enki_app, row.args_v[2]);
            err = plan_alloc_app(plan, old->fn_v, old->n_args_s, old->args_v, out_v);
            if (err != ENKI_ERROR_OK)
                break;
            if (n_s < old->n_args_s)
                ENKI_AS(enki_app, *out_v)->args_v[n_s] = row.args_v[1];
        }
        break;
    case OP66_COUP:
        if ((err = plan_require_argc(plan, row.n_args_s, 2)) != ENKI_ERROR_OK)
            break;
        if (!plan_is_kind(row.args_v[1], ENKI_APP)) {
            *out_v = row.args_v[0];
        } else {
            enki_app* app = ENKI_AS(enki_app, row.args_v[1]);
            err = plan_apply_inner(plan, row.args_v[0], app->n_args_s, app->args_v, out_v);
        }
        break;
    case OP66_SZ:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        *out_v = plan_is_kind(row.args_v[0], ENKI_APP)
                     ? (enki_value)ENKI_AS(enki_app, row.args_v[0])->n_args_s
                     : 0;
        break;
    case OP66_IX:
        if ((err = plan_require_argc(plan, row.n_args_s, 2)) != ENKI_ERROR_OK)
            break;
        *out_v = plan_ix_at(row.args_v[0], row.args_v[1]);
        break;
    case OP66_IX0:
    case OP66_IX1:
    case OP66_IX2:
    case OP66_IX3:
    case OP66_IX4:
    case OP66_IX5:
    case OP66_IX6:
    case OP66_IX7:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        *out_v = plan_ix_at((enki_value)(op - OP66_IX0), row.args_v[0]);
        break;
    case OP66_CASE:
        if ((err = plan_require_argc(plan, row.n_args_s, 3)) != ENKI_ERROR_OK)
            break;
        if (!plan_small_nat(row.args_v[0], &n_s) || !plan_is_kind(row.args_v[1], ENKI_APP)) {
            *out_v = row.args_v[2];
        } else {
            enki_app* app = ENKI_AS(enki_app, row.args_v[1]);
            *out_v = n_s < app->n_args_s ? app->args_v[n_s] : row.args_v[2];
        }
        break;
    case OP66_CASE2:
    case OP66_CASE3:
    case OP66_CASE4:
    case OP66_CASE5:
    case OP66_CASE6:
    case OP66_CASE7:
    case OP66_CASE8:
    case OP66_CASE9:
    case OP66_CASE10:
    case OP66_CASE11:
    case OP66_CASE12:
    case OP66_CASE13:
    case OP66_CASE14:
    case OP66_CASE15:
    case OP66_CASE16:
        err = plan_case_n(plan, row.args_v, row.n_args_s, (size_t)(op - OP66_CASE2 + 2), out_v);
        break;
    case OP66_NIL:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        *out_v = row.args_v[0] == 0 ? 1 : 0;
        break;
    case OP66_TRUTH:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        *out_v = row.args_v[0] == 0 ? 0 : 1;
        break;
    case OP66_OR:
    case OP66_NOR:
    case OP66_AND:
        if ((err = plan_require_argc(plan, row.n_args_s, 2)) != ENKI_ERROR_OK)
            break;
        if (op == OP66_OR)
            *out_v = row.args_v[0] == 0 ? row.args_v[1] : row.args_v[0];
        else if (op == OP66_NOR)
            *out_v = row.args_v[0] != 0 ? 0 : (row.args_v[1] == 0 ? 1 : 0);
        else
            *out_v = row.args_v[0] == 0 ? 0 : row.args_v[1];
        break;
    case OP66_IF:
    case OP66_IFZ:
        if ((err = plan_require_argc(plan, row.n_args_s, 3)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[0], &a_v)) != ENKI_ERROR_OK)
            break;
        *out_v = (op == OP66_IF ? a_v != 0 : a_v == 0) ? row.args_v[1] : row.args_v[2];
        break;
    case OP66_SEQ:
        if ((err = plan_require_argc(plan, row.n_args_s, 2)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[0], &a_v)) != ENKI_ERROR_OK)
            break;
        *out_v = row.args_v[1];
        break;
    case OP66_SEQ2:
        if ((err = plan_require_argc(plan, row.n_args_s, 3)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[0], &a_v)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[1], &a_v)) != ENKI_ERROR_OK)
            break;
        *out_v = row.args_v[2];
        break;
    case OP66_SEQ3:
        if ((err = plan_require_argc(plan, row.n_args_s, 4)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[0], &a_v)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[1], &a_v)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[2], &a_v)) != ENKI_ERROR_OK)
            break;
        *out_v = row.args_v[3];
        break;
    case OP66_SAP:
        if ((err = plan_require_argc(plan, row.n_args_s, 2)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[1], &a_v)) != ENKI_ERROR_OK)
            break;
        err = plan_apply_inner(plan, row.args_v[0], 1, &a_v, out_v);
        break;
    case OP66_SAP2:
        if ((err = plan_require_argc(plan, row.n_args_s, 3)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[1], &a_v)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[2], &b_v)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_apply_inner(plan, row.args_v[0], 1, &a_v, &c_v)) != ENKI_ERROR_OK)
            break;
        err = plan_apply_inner(plan, c_v, 1, &b_v, out_v);
        break;
    case OP66_FORCE:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        err = plan_eval_nf_inner(plan, row.args_v[0], out_v);
        break;
    case OP66_DEEPSEQ:
        if ((err = plan_require_argc(plan, row.n_args_s, 2)) != ENKI_ERROR_OK)
            break;
        if ((err = plan_eval_nf_inner(plan, row.args_v[0], &a_v)) != ENKI_ERROR_OK)
            break;
        *out_v = row.args_v[1];
        break;
    case OP66_TRACE:
        if ((err = plan_require_argc(plan, row.n_args_s, 2)) != ENKI_ERROR_OK)
            break;
        *out_v = row.args_v[1];
        break;
    case OP66_EQUAL:
        if ((err = plan_require_argc(plan, row.n_args_s, 2)) != ENKI_ERROR_OK)
            break;
        err = plan_structural_equal(plan, row.args_v[0], row.args_v[1], out_v);
        break;
    case OP66_THROW:
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        err = plan_eval_nf_inner(plan, row.args_v[0], out_v);
        if (err == ENKI_ERROR_OK)
            err = plan_fail(plan, ENKI_ERROR_THROW, *out_v);
        break;
    case OP66_SAVE:
    case OP66_LOAD:
    case OP66_TRY:
    case OP66_PARSE_REX:
    case OP66_PRINT_REX:
    default:
        err = plan_fail(plan, ENKI_ERROR_BAD_TAG, row.tag_v);
        break;
    }

done:
    plan_row_close(&row);
    return err;
}

static enki_error plan_elim(enki_plan* plan, enki_value p_v, enki_value l_v, enki_value a_v,
                            enki_value z_v, enki_value m_v, enki_value o_v, enki_value* out_v)
{
    enki_error err = plan_eval_whnf_inner(plan, o_v, &o_v);
    if (err != ENKI_ERROR_OK)
        return err;
    if (plan_is_kind(o_v, ENKI_PIN)) {
        enki_value inner_v = ENKI_AS(enki_pin, o_v)->inner_v;
        return plan_apply_inner(plan, p_v, 1, &inner_v, out_v);
    }
    if (plan_is_kind(o_v, ENKI_LAW)) {
        enki_law* law = ENKI_AS(enki_law, o_v);
        enki_value args_v[] = {(enki_value)law->arity_s, law->name_v, law->body_v};
        return plan_apply_inner(plan, l_v, 3, args_v, out_v);
    }
    if (plan_is_kind(o_v, ENKI_APP)) {
        enki_plan_spine spine;
        err = plan_spine_open(plan, o_v, &spine);
        if (err != ENKI_ERROR_OK)
            return err;
        enki_value* args_v = malloc((spine.n_args_s + 1) * sizeof(enki_value));
        if (args_v == NULL) {
            plan_spine_close(&spine);
            return plan_fail(plan, ENKI_ERROR_OOM, o_v);
        }
        args_v[0] = spine.head_v;
        memcpy(args_v + 1, spine.args_v, spine.n_args_s * sizeof(enki_value));
        err = plan_apply_inner(plan, a_v, spine.n_args_s + 1, args_v, out_v);
        free(args_v);
        plan_spine_close(&spine);
        return err;
    }
    if (plan_is_nat(o_v)) {
        if (enki_nat_is_zero(o_v)) {
            *out_v = z_v;
            return ENKI_ERROR_OK;
        }
        enki_value dec_v = enki_nat_dec(plan->gc, o_v);
        return plan_apply_inner(plan, m_v, 1, &dec_v, out_v);
    }
    return plan_fail(plan, ENKI_ERROR_BAD_TAG, o_v);
}

static enki_error plan_dispatch_op0(enki_plan* plan, enki_value row_v, enki_value* out_v)
{
    enki_plan_row row;
    enki_error err = plan_row_open(plan, row_v, &row);
    if (err != ENKI_ERROR_OK)
        return err;
    size_t op_s = 0;
    if (!plan_small_nat(row.tag_v, &op_s)) {
        err = plan_fail(plan, ENKI_ERROR_BAD_TAG, row.tag_v);
        goto done;
    }
    switch (op_s) {
    case OP0_PIN: {
        if ((err = plan_require_argc(plan, row.n_args_s, 1)) != ENKI_ERROR_OK)
            break;
        enki_value inner_v = 0;
        if ((err = plan_eval_whnf_inner(plan, row.args_v[0], &inner_v)) != ENKI_ERROR_OK)
            break;
        uint8_t hash_b[32] = {0};
        *out_v = enki_pin_alloc(plan->gc, hash_b, inner_v, 0, NULL);
        break;
    }
    case OP0_LAW: {
        if ((err = plan_require_argc(plan, row.n_args_s, 3)) != ENKI_ERROR_OK)
            break;
        size_t arity_s = 0;
        if ((err = plan_arg_small(plan, row.args_v, 0, &arity_s)) != ENKI_ERROR_OK)
            break;
        *out_v =
            enki_law_alloc(plan->gc, arity_s + 1, row.args_v[1], row.args_v[2], 0, 0, NULL, NULL);
        break;
    }
    case OP0_ELIM:
        if ((err = plan_require_argc(plan, row.n_args_s, 6)) != ENKI_ERROR_OK)
            break;
        err = plan_elim(plan, row.args_v[0], row.args_v[1], row.args_v[2], row.args_v[3],
                        row.args_v[4], row.args_v[5], out_v);
        break;
    default:
        err = plan_fail(plan, ENKI_ERROR_BAD_TAG, row.tag_v);
        break;
    }

done:
    plan_row_close(&row);
    return err;
}

static enki_error plan_dispatch_group(enki_plan* plan, enki_value group_v, enki_value row_v,
                                      enki_value* out_v)
{
    size_t group_s = 0;
    if (!plan_small_nat(group_v, &group_s))
        return plan_fail(plan, ENKI_ERROR_BAD_TAG, group_v);
    if (group_s == 0)
        return plan_dispatch_op0(plan, row_v, out_v);
    if (group_s == 66)
        return plan_dispatch_op66(plan, row_v, out_v);
    return plan_fail(plan, ENKI_ERROR_BAD_TAG, group_v);
}

static enki_error plan_run_body(enki_plan* plan, size_t depth_s, const enki_value* env_v,
                                enki_value body_v, enki_value* out_v)
{
    if (!IS_PTR(body_v) && body_v <= (enki_value)depth_s) {
        *out_v = env_v[(size_t)body_v];
        return ENKI_ERROR_OK;
    }
    if (!plan_is_kind(body_v, ENKI_APP)) {
        *out_v = body_v;
        return ENKI_ERROR_OK;
    }
    enki_app* app = ENKI_AS(enki_app, body_v);
    if (app->fn_v == 0) {
        if (app->n_args_s == 1) {
            *out_v = app->args_v[0];
            return ENKI_ERROR_OK;
        }
        if (app->n_args_s == 2) {
            enki_value fn_v = 0;
            enki_value arg_v = 0;
            enki_error err = plan_run_body(plan, depth_s, env_v, app->args_v[0], &fn_v);
            if (err != ENKI_ERROR_OK)
                return err;
            err = plan_run_body(plan, depth_s, env_v, app->args_v[1], &arg_v);
            if (err != ENKI_ERROR_OK)
                return err;
            return plan_make_app(plan, fn_v, 1, &arg_v, out_v);
        }
    }
    if (app->fn_v == 1 && app->n_args_s == 2) {
        enki_value bound_v = 0;
        enki_error err = plan_run_body(plan, depth_s, env_v, app->args_v[0], &bound_v);
        if (err != ENKI_ERROR_OK)
            return err;
        err = plan_eval_whnf_inner(plan, bound_v, &bound_v);
        if (err != ENKI_ERROR_OK)
            return err;
        enki_value* next_env_v = malloc((depth_s + 2) * sizeof(enki_value));
        if (next_env_v == NULL)
            return plan_fail(plan, ENKI_ERROR_OOM, body_v);
        memcpy(next_env_v, env_v, (depth_s + 1) * sizeof(enki_value));
        next_env_v[depth_s + 1] = bound_v;
        err = plan_run_body(plan, depth_s + 1, next_env_v, app->args_v[1], out_v);
        free(next_env_v);
        return err;
    }
    *out_v = body_v;
    return ENKI_ERROR_OK;
}

static enki_error plan_apply_law(enki_plan* plan, enki_value self_v, enki_law* law, size_t n_args_s,
                                 const enki_value* args_v, enki_value* out_v)
{
    if (n_args_s != law->arity_s)
        return plan_fail(plan, ENKI_ERROR_BOUNDS, (enki_value)n_args_s);
    size_t env_s = n_args_s + 1;
    enki_value* env_v = malloc(env_s * sizeof(enki_value));
    if (env_v == NULL)
        return plan_fail(plan, ENKI_ERROR_OOM, self_v);
    env_v[0] = self_v;
    if (n_args_s > 0)
        memcpy(env_v + 1, args_v, n_args_s * sizeof(enki_value));
    enki_error err = ENKI_ERROR_OK;
    for (;;) {
        enki_value body_v = 0;
        err = plan_run_body(plan, n_args_s, env_v, law->body_v, &body_v);
        if (err != ENKI_ERROR_OK)
            break;
        bool tail_call_f = false;
        err = plan_eval_whnf_tail(plan, body_v, self_v, law, env_v, &tail_call_f, out_v);
        if (err != ENKI_ERROR_OK || !tail_call_f)
            break;
    }
    free(env_v);
    return err;
}

static enki_error plan_reduce_exact(enki_plan* plan, enki_value fn_v, size_t n_args_s,
                                    const enki_value* args_v, enki_value* out_v)
{
    if (plan_is_kind(fn_v, ENKI_LAW)) {
        return plan_apply_law(plan, fn_v, ENKI_AS(enki_law, fn_v), n_args_s, args_v, out_v);
    }
    if (plan_is_kind(fn_v, ENKI_PIN)) {
        enki_pin* pin = ENKI_AS(enki_pin, fn_v);
        if (plan_is_kind(pin->inner_v, ENKI_LAW)) {
            return plan_apply_law(plan, fn_v, ENKI_AS(enki_law, pin->inner_v), n_args_s, args_v,
                                  out_v);
        }
        if (plan_is_nat(pin->inner_v)) {
            if (n_args_s != 1)
                return plan_fail(plan, ENKI_ERROR_BOUNDS, (enki_value)n_args_s);
            enki_value row_v = 0;
            enki_error err = plan_eval_whnf_inner(plan, args_v[0], &row_v);
            if (err != ENKI_ERROR_OK)
                return err;
            return plan_dispatch_group(plan, pin->inner_v, row_v, out_v);
        }
    }
    return plan_fail(plan, ENKI_ERROR_BAD_TAG, fn_v);
}

static enki_error plan_arity_value(enki_plan* plan, enki_value val_v, size_t* out_s)
{
    (void)plan;
    if (!IS_PTR(val_v)) {
        *out_s = 0;
        return ENKI_ERROR_OK;
    }
    enki_value_header* h = ENKI_AS(enki_value_header, val_v);
    switch (h->kind_b) {
    case ENKI_NAT:
        *out_s = 0;
        return ENKI_ERROR_OK;
    case ENKI_LAW:
        *out_s = ENKI_AS(enki_law, val_v)->arity_s;
        return ENKI_ERROR_OK;
    case ENKI_PIN: {
        enki_value inner_v = ENKI_AS(enki_pin, val_v)->inner_v;
        if (plan_is_kind(inner_v, ENKI_LAW)) {
            *out_s = ENKI_AS(enki_law, inner_v)->arity_s;
        } else if (plan_is_nat(inner_v)) {
            *out_s = 1;
        } else {
            *out_s = 0;
        }
        return ENKI_ERROR_OK;
    }
    case ENKI_APP: {
        enki_app* app = ENKI_AS(enki_app, val_v);
        size_t fn_arity_s = 0;
        enki_error err = plan_arity_value(plan, app->fn_v, &fn_arity_s);
        if (err != ENKI_ERROR_OK)
            return err;
        *out_s = fn_arity_s > app->n_args_s ? fn_arity_s - app->n_args_s : 0;
        return ENKI_ERROR_OK;
    }
    default:
        return plan_fail(plan, ENKI_ERROR_BAD_TAG, val_v);
    }
}

static enki_error plan_eval_whnf_loop(enki_plan* plan, enki_value val_v, enki_value self_v,
                                      enki_law* tail_law, enki_value* tail_env_v,
                                      bool* tail_call_f, enki_value* out_v)
{
    if (tail_call_f != NULL)
        *tail_call_f = false;
    for (;;) {
        if (tail_law != NULL && plan_rebind_tail_call(val_v, self_v, tail_law, tail_env_v)) {
            if (tail_call_f != NULL)
                *tail_call_f = true;
            *out_v = val_v;
            return ENKI_ERROR_OK;
        }
        if (!IS_PTR(val_v)) {
            *out_v = val_v;
            return ENKI_ERROR_OK;
        }
        enki_value_header* h = ENKI_AS(enki_value_header, val_v);
        switch (h->kind_b) {
        case ENKI_NAT:
            h->state_b = NF;
            *out_v = val_v;
            return ENKI_ERROR_OK;
        case ENKI_PIN:
            h->state_b = WHNF;
            *out_v = val_v;
            return ENKI_ERROR_OK;
        case ENKI_LAW: {
            enki_law* law = ENKI_AS(enki_law, val_v);
            if (law->arity_s > 0) {
                h->state_b = WHNF;
                *out_v = val_v;
                return ENKI_ERROR_OK;
            }
            enki_error err = plan_apply_law(plan, val_v, law, 0, NULL, &val_v);
            if (err != ENKI_ERROR_OK)
                return err;
            break;
        }
        case ENKI_APP: {
            enki_app* app = ENKI_AS(enki_app, val_v);
            enki_value fn_v = 0;
            enki_error err = plan_eval_whnf_inner(plan, app->fn_v, &fn_v);
            if (err != ENKI_ERROR_OK)
                return err;
            size_t arity_s = 0;
            err = plan_arity_value(plan, fn_v, &arity_s);
            if (err != ENKI_ERROR_OK)
                return err;
            if (arity_s == 0 || app->n_args_s < arity_s) {
                if (fn_v != app->fn_v || (arity_s == 0 && plan_is_kind(fn_v, ENKI_APP))) {
                    return plan_make_app(plan, fn_v, app->n_args_s, app->args_v, out_v);
                }
                h->state_b = WHNF;
                *out_v = val_v;
                return ENKI_ERROR_OK;
            }
            size_t rest_s = app->n_args_s - arity_s;
            enki_value* rest_v = NULL;
            if (rest_s > 0) {
                rest_v = malloc(rest_s * sizeof(enki_value));
                if (rest_v == NULL)
                    return plan_fail(plan, ENKI_ERROR_OOM, val_v);
                memcpy(rest_v, app->args_v + arity_s, rest_s * sizeof(enki_value));
            }
            err = plan_reduce_exact(plan, fn_v, arity_s, app->args_v, &val_v);
            if (err != ENKI_ERROR_OK) {
                free(rest_v);
                return err;
            }
            if (rest_s > 0) {
                err = plan_make_app(plan, val_v, rest_s, rest_v, &val_v);
                free(rest_v);
                if (err != ENKI_ERROR_OK)
                    return err;
            }
            break;
        }
        default:
            return plan_fail(plan, ENKI_ERROR_BAD_TAG, val_v);
        }
    }
}

static enki_error plan_eval_whnf_inner(enki_plan* plan, enki_value val_v, enki_value* out_v)
{
    return plan_eval_whnf_loop(plan, val_v, 0, NULL, NULL, NULL, out_v);
}

static enki_error plan_eval_whnf_tail(enki_plan* plan, enki_value val_v, enki_value self_v,
                                      enki_law* law, enki_value* env_v, bool* tail_call_f,
                                      enki_value* out_v)
{
    return plan_eval_whnf_loop(plan, val_v, self_v, law, env_v, tail_call_f, out_v);
}

static enki_error plan_eval_nf_inner(enki_plan* plan, enki_value val_v, enki_value* out_v)
{
    enki_error err = plan_eval_whnf_inner(plan, val_v, &val_v);
    if (err != ENKI_ERROR_OK)
        return err;
    if (!IS_PTR(val_v)) {
        *out_v = val_v;
        return ENKI_ERROR_OK;
    }
    enki_value_header* h = ENKI_AS(enki_value_header, val_v);
    switch (h->kind_b) {
    case ENKI_NAT:
        h->state_b = NF;
        break;
    case ENKI_PIN:
        h->state_b = WHNF;
        break;
    case ENKI_LAW: {
        enki_law* law = ENKI_AS(enki_law, val_v);
        err = plan_eval_nf_inner(plan, law->name_v, &law->name_v);
        if (err != ENKI_ERROR_OK)
            return err;
        err = plan_eval_nf_inner(plan, law->body_v, &law->body_v);
        if (err != ENKI_ERROR_OK)
            return err;
        h->state_b = NF;
        break;
    }
    case ENKI_APP: {
        enki_app* app = ENKI_AS(enki_app, val_v);
        err = plan_eval_nf_inner(plan, app->fn_v, &app->fn_v);
        if (err != ENKI_ERROR_OK)
            return err;
        for (size_t k = 0; k < app->n_args_s; k++) {
            err = plan_eval_nf_inner(plan, app->args_v[k], &app->args_v[k]);
            if (err != ENKI_ERROR_OK)
                return err;
        }
        h->state_b = NF;
        break;
    }
    default:
        return plan_fail(plan, ENKI_ERROR_BAD_TAG, val_v);
    }
    *out_v = val_v;
    return ENKI_ERROR_OK;
}

static enki_error plan_apply_inner(enki_plan* plan, enki_value fn_v, size_t n_args_s,
                                   const enki_value* args_v, enki_value* out_v)
{
    if (n_args_s == 0)
        return plan_eval_whnf_inner(plan, fn_v, out_v);
    enki_value app_v = 0;
    enki_error err = plan_make_app(plan, fn_v, n_args_s, args_v, &app_v);
    if (err != ENKI_ERROR_OK)
        return err;
    return plan_eval_whnf_inner(plan, app_v, out_v);
}

void enki_plan_init(enki_plan* plan, enki_gc* gc)
{
    if (plan == NULL)
        return;
    plan->gc = gc;
    plan->error = ENKI_ERROR_OK;
    plan->error_v = 0;
}

void enki_plan_clear_error(enki_plan* plan)
{
    if (plan == NULL)
        return;
    plan->error = ENKI_ERROR_OK;
    plan->error_v = 0;
}

enki_error enki_plan_eval_whnf(enki_plan* plan, enki_value val_v, enki_value* out_v)
{
    if (plan == NULL || plan->gc == NULL || out_v == NULL)
        return ENKI_ERROR_TYPE;
    enki_plan_clear_error(plan);
    return plan_eval_whnf_inner(plan, val_v, out_v);
}

enki_error enki_plan_eval_nf(enki_plan* plan, enki_value val_v, enki_value* out_v)
{
    if (plan == NULL || plan->gc == NULL || out_v == NULL)
        return ENKI_ERROR_TYPE;
    enki_plan_clear_error(plan);
    return plan_eval_nf_inner(plan, val_v, out_v);
}

enki_error enki_plan_apply(enki_plan* plan, enki_value fn_v, size_t n_args_s,
                           const enki_value* args_v, enki_value* out_v)
{
    if (plan == NULL || plan->gc == NULL || out_v == NULL)
        return ENKI_ERROR_TYPE;
    if (n_args_s > 0 && args_v == NULL)
        return ENKI_ERROR_TYPE;
    enki_plan_clear_error(plan);
    return plan_apply_inner(plan, fn_v, n_args_s, args_v, out_v);
}

enki_error enki_plan_nat(enki_plan* plan, enki_value val_v, enki_value* out_v)
{
    if (plan == NULL || plan->gc == NULL || out_v == NULL)
        return ENKI_ERROR_TYPE;
    enki_plan_clear_error(plan);
    return plan_nat_inner(plan, val_v, out_v);
}

enki_error enki_plan_arity(enki_plan* plan, enki_value val_v, size_t* out_s)
{
    if (plan == NULL || out_s == NULL)
        return ENKI_ERROR_TYPE;
    enki_plan_clear_error(plan);
    return plan_arity_value(plan, val_v, out_s);
}
