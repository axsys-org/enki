#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "enki/bytecode.h"
#include "enki/interp.h"
#include "enki/run.h"
#include "enki/run_ops.h"
#include "enki/util.h"

#define ER_S8(a, b, c, d, e, f, g, h) \
    ((er_val)(PLAN_S7(a, b, c, d, e, f, g) | (PLAN_CH(h) << 56u)))

enum {
    ER_OP66_STORE = OP66_PRINT_REX + 1,
    ER_OP66_MET,
    ER_OP66_COUNT,
};

typedef struct er_prim_route {
    er_optag tag;
    size_t arg_s;
    bool valid_f;
} er_prim_route;

#define ER_PRIM_ROUTE(_tag, _arg_s) \
    { .tag = (_tag), .arg_s = (_arg_s), .valid_f = true }

static bool er_alloc_size(size_t base_s, size_t count_s, size_t elem_s, size_t* out_s)
{
    if (elem_s != 0 && count_s > (SIZE_MAX - base_s) / elem_s) {
        return false;
    }
    *out_s = base_s + (count_s * elem_s);
    return true;
}

static void* er_alloc_bytes(const enki_allocator* allocator, size_t size_s)
{
    if (allocator == NULL || allocator->alloc == NULL || size_s == 0) {
        return NULL;
    }
    return allocator->alloc(allocator->ctx, size_s);
}

static void er_head_alloc_init(er_head* h, size_t size_s)
{
    h->siz_s = size_s;
    h->raw.fwd_f = 0;
    h->raw.nf_f = 0;
}

static size_t er_head_size(const er_head* h)
{
    return h->siz_s & ~(size_t)0x3;
}

static bool er_flex_fits(const er_head* h, size_t base_s, size_t count_s, size_t elem_s)
{
    size_t need_s = 0;
    return er_alloc_size(base_s, count_s, elem_s, &need_s) && need_s <= er_head_size(h);
}

er_bat* er_bat_alloc(const enki_allocator* allocator, size_t lim_s)
{
    size_t size_s = 0;
    if (!er_alloc_size(sizeof(er_bat), lim_s, sizeof(uint64_t), &size_s)) {
        return NULL;
    }
    er_bat* bat = (er_bat*)er_alloc_bytes(allocator, size_s);
    if (bat == NULL) {
        return NULL;
    }
    er_head_alloc_init(&bat->hed, size_s);
    bat->lim_s = lim_s;
    return bat;
}

er_val er_bat_init(er_bat* bat, size_t lim_s, const uint64_t lim_q[])
{
    if (bat == NULL) {
        return 0;
    }
    if (!er_flex_fits(&bat->hed, sizeof(er_bat), lim_s, sizeof(uint64_t))) {
        return 0;
    }
    bat->lim_s = lim_s;
    if (lim_s > 0) {
        if (lim_q != NULL) {
            memcpy(bat->lim_q, lim_q, lim_s * sizeof(uint64_t));
        } else {
            memset(bat->lim_q, 0, lim_s * sizeof(uint64_t));
        }
    }
    bat->hed.raw.nf_f = 1;
    return er_into(er_tag_bat, bat);
}


static er_val er_bat_qwords(const enki_allocator* loc_a, size_t lim_s, const uint64_t lim_q[])
{
  er_bat* bat = er_bat_alloc(loc_a, lim_s);
  return er_bat_init(bat, lim_s, lim_q);
}

static er_val er_bat_qword(const enki_allocator* loc_a, const uint64_t lim_q)
{
  uint64_t lim_qq = lim_q;
  return er_bat_qwords(loc_a, 1, &lim_qq);
}

er_pin* er_pin_alloc(const enki_allocator* allocator, size_t sub_s)
{
    size_t size_s = 0;
    if (!er_alloc_size(sizeof(er_pin), sub_s, sizeof(er_val), &size_s)) {
        return NULL;
    }
    er_pin* pin = (er_pin*)er_alloc_bytes(allocator, size_s);
    if (pin == NULL) {
        return NULL;
    }
    er_head_alloc_init(&pin->hed, size_s);
    memset(pin->hash_b, 0, sizeof(pin->hash_b));
    pin->val_v = 0;
    pin->sub_s = sub_s;
    return pin;
}

er_val er_pin_init(er_pin* pin, const uint8_t hash_b[32], er_val val_v, size_t sub_s,
    const er_val sub_v[])
{
    if (pin == NULL) {
        return 0;
    }
    if (!er_flex_fits(&pin->hed, sizeof(er_pin), sub_s, sizeof(er_val))) {
        return 0;
    }
    pin->val_v = val_v;
    pin->sub_s = sub_s;
    if (hash_b != NULL) {
        memcpy(pin->hash_b, hash_b, sizeof(pin->hash_b));
    } else {
        memset(pin->hash_b, 0, sizeof(pin->hash_b));
    }
    if (sub_s > 0) {
        if (sub_v != NULL) {
            memcpy(pin->sub_v, sub_v, sub_s * sizeof(er_val));
        } else {
            memset(pin->sub_v, 0, sub_s * sizeof(er_val));
        }
    }
    return er_into(er_tag_pin, pin);
}


er_val er_pin_make(const enki_allocator* loc_a, er_val val_v)
{
  er_pin* pin = er_pin_alloc(loc_a, 0);
  return er_pin_init(pin, NULL, val_v, 0, NULL);

}
er_law* er_law_alloc(const enki_allocator* allocator, size_t bc_s)
{
    size_t size_s = 0;
    if (!er_alloc_size(sizeof(er_law), bc_s, sizeof(er_op*), &size_s)) {
        return NULL;
    }
    er_law* law = (er_law*)er_alloc_bytes(allocator, size_s);
    if (law == NULL) {
        return NULL;
    }
    er_head_alloc_init(&law->h, size_s);
    law->name_v = 0;
    law->body_v = 0;
    law->ari_d = 0;
    law->let_d = 0;
    law->bc_s = bc_s;
    return law;
}

er_val er_law_init(er_law* law, er_val name_v, er_val body_v, uint32_t ari_d,
    uint32_t let_d, size_t bc_s, er_op* const bc_v[])
{
    if (law == NULL) {
        return 0;
    }
    if (bc_s == 0 || ari_d == UINT32_MAX || let_d > UINT32_MAX - ari_d - 1u ||
        bc_s < (size_t)let_d + 1u) {
        return 0;
    }
    if (!er_flex_fits(&law->h, sizeof(er_law), bc_s, sizeof(er_op*))) {
        return 0;
    }
    law->h.raw.nf_f = 1;
    law->name_v = name_v;
    law->body_v = body_v;
    law->ari_d = ari_d;
    law->let_d = let_d;
    law->bc_s = bc_s;
    if (bc_v == NULL) {
        memset(law->bc_v, 0, bc_s * sizeof(er_op*));
    } else {
        memcpy(law->bc_v, bc_v, bc_s * sizeof(er_op*));
    }
    return er_into(er_tag_law, law);
}

er_val er_law_make_code(const enki_allocator* loc_a, er_val nam_v, er_val bod_v,
                        uint32_t ari_d, uint32_t let_d, size_t bc_s, er_op* const bc_v[])
{
  er_law* law = er_law_alloc(loc_a, bc_s);
  if (law == NULL) {
    return 0;
  }
  return er_law_init(law, nam_v, bod_v, ari_d, let_d, bc_s, bc_v);
}

er_val er_law_make(const enki_allocator* loc_a, er_val nam_v, er_val bod_v, uint32_t ari_d)
{
  return er_law_compile(loc_a, nam_v, bod_v, ari_d);
}

er_app* er_app_alloc(const enki_allocator* allocator, size_t arg_s)
{
    size_t size_s = 0;
    if (!er_alloc_size(sizeof(er_app), arg_s, sizeof(er_val), &size_s)) {
        return NULL;
    }
    er_app* app = (er_app*)er_alloc_bytes(allocator, size_s);
    if (app == NULL) {
        return NULL;
    }
    er_head_alloc_init(&app->h, size_s);
    app->fn_v = 0;
    app->arg_s = arg_s;
    return app;
}

er_val er_app_init(er_app* app, er_val fn_v, size_t arg_s, const er_val arg_v[])
{
    if (app == NULL) {
        return 0;
    }
    if (!er_flex_fits(&app->h, sizeof(er_app), arg_s, sizeof(er_val))) {
        return 0;
    }
    app->fn_v = fn_v;
    app->arg_s = arg_s;
    if (arg_s > 0) {
        if (arg_v != NULL) {
            memcpy(app->arg_v, arg_v, arg_s * sizeof(er_val));
        } else {
            memset(app->arg_v, 0, arg_s * sizeof(er_val));
        }
    }
    return er_into(er_tag_app, app);
}

er_thk* er_thk_alloc(const enki_allocator* allocator, size_t arg_s)
{
    size_t siz_s = 0;
    if (!er_alloc_size(sizeof(er_thk), arg_s, sizeof(er_val), &siz_s)) {
        return NULL;
    }
    er_thk* thk = (er_thk*)er_alloc_bytes(allocator, siz_s);
    if (thk == NULL) {
        return NULL;
    }
    er_head_alloc_init(&thk->hed, siz_s);
    thk->fun = ER_XDONE;
    thk->arg_s = arg_s;
    return thk;
}

er_val er_thk_init(er_thk* thk, er_execf fun, size_t arg_s, const er_val arg_v[])
{
    if (thk == NULL) {
        return 0;
    }
    if (!er_flex_fits(&thk->hed, sizeof(er_thk), arg_s, sizeof(er_val))) {
        return 0;
    }
    thk->fun = fun;
    thk->arg_s = arg_s;
    if (arg_s > 0) {
        if (arg_v != NULL) {
            memcpy(thk->arg_v, arg_v, arg_s * sizeof(er_val));
        } else {
            memset(thk->arg_v, 0, arg_s * sizeof(er_val));
        }
    }
    return er_into(er_tag_thk, thk);
}

static er_val er_thk_make_call(const enki_allocator* loc_a, size_t arg_s, const er_val arg_v[])
{
    er_thk* thk = er_thk_alloc(loc_a, arg_s);
    if (thk == NULL) {
        return 0;
    }
    return er_thk_init(thk, ER_CALL, arg_s, arg_v);
}

static er_val er_thk_make_call_frame(const enki_allocator* loc_a, size_t frame_s,
    size_t copy_s, const er_val arg_v[])
{
    if (copy_s > frame_s) {
        return 0;
    }
    er_thk* thk = er_thk_alloc(loc_a, frame_s);
    if (thk == NULL) {
        return 0;
    }
    er_val out_v = er_thk_init(thk, ER_CALL, frame_s, NULL);
    if (out_v == 0) {
        return 0;
    }
    if (copy_s > 0 && arg_v != NULL) {
        memcpy(thk->arg_v, arg_v, copy_s * sizeof(er_val));
    }
    return out_v;
}

static er_val er_thk_make_susp(const enki_allocator* loc_a, uint32_t pc, er_val frame_v)
{
    er_val arg_v[] = {
        (er_val)pc,
        frame_v,
    };
    er_thk* thk = er_thk_alloc(loc_a, 2);
    if (thk == NULL) {
        return 0;
    }
    return er_thk_init(thk, ER_SUSP, 2, arg_v);
}

static er_val er_thk_make_unk_app(const enki_allocator* loc_a, size_t arg_s,
    const er_val arg_v[])
{
    er_thk* thk = er_thk_alloc(loc_a, arg_s);
    if (thk == NULL) {
        return 0;
    }
    return er_thk_init(thk, ER_XUNK_APP, arg_s, arg_v);
}

static er_val er_app_make(const enki_allocator* loc_a, er_val fn_v, size_t arg_s,
                          const er_val arg_v[])
{
    er_app* app = er_app_alloc(loc_a, arg_s);
    if (app == NULL) {
        return 0;
    }
    return er_app_init(app, fn_v, arg_s, arg_v);
}

static er_val er_app_take(const enki_allocator* loc_a, er_app* old, size_t arg_s)
{
    if (old == NULL) {
        return 0;
    }
    er_app* app = er_app_alloc(loc_a, arg_s);
    if (app == NULL) {
        return 0;
    }
    return er_app_init(app, old->fn_v, arg_s, old->arg_v);
}

static er_val er_thk_take_call(const enki_allocator* loc_a, er_thk* old, size_t arg_s)
{
    if (old == NULL) {
        return 0;
    }
    size_t copy_s = old->arg_s < arg_s ? old->arg_s : arg_s;
    return er_thk_make_call_frame(loc_a, arg_s, copy_s, old->arg_v);
}

static er_val er_app_drop(const enki_allocator* loc_a, er_app* old, size_t dop_s)
{
    if (old == NULL) {
        return 0;
    }
    if (old->arg_s < dop_s) {
        return 0;
    }
    size_t siz_s = old->arg_s - dop_s;
    er_app* app = er_app_alloc(loc_a, siz_s);
    if (app == NULL) {
        return 0;
    }
    return er_app_init(app, old->fn_v, siz_s, &old->arg_v[dop_s]);
}

static er_val er_app_drop_coup(const enki_allocator* loc_a, er_thk* old, er_val fn_v, size_t dop_s)
{
    if (old == NULL) {
        return 0;
    }
    if (old->arg_s <= dop_s) {
        return 0;
    }
    size_t siz_s = old->arg_s - dop_s - 1;
    er_app* app = er_app_alloc(loc_a, siz_s);
    if (app == NULL) {
        return 0;
    }
    return er_app_init(app, fn_v, siz_s, &old->arg_v[dop_s + 1]);
}

static er_law* er_resolve_law(er_val val_v)
{
  er_pin* pin;
  switch ( er_get_tag(val_v) ) {
    case er_tag_pin:
      pin = er_outa(val_v);
      if (er_is_cat(pin->val_v)) {
        return NULL;
      }
      return er_outt(er_tag_law, pin->val_v);
    case er_tag_law:
      return er_outa(val_v);
    default:
      return NULL;
  }
}

static size_t er_law_n_lets(const er_law* law)
{
    if (law == NULL) {
        return 0;
    }
    return law->let_d;
}

static size_t er_call_frame_size(er_val fun_v, uint32_t arity_d)
{
    er_law* law = er_resolve_law(fun_v);
    if (law == NULL) {
        return (size_t)arity_d + 1;
    }
    if (law->ari_d != arity_d || law->ari_d == UINT32_MAX ||
        law->let_d > UINT32_MAX - law->ari_d - 1u) {
        return 0;
    }
    return (size_t)law->ari_d + 1 + law->let_d;
}

static uint32_t er_arity(er_val val_v)
{
  er_pin* pin;
  er_app* app;
  er_law* law;
  switch ( er_get_tag(val_v) ) {
    case er_tag_bat:
      return 0;
    case er_tag_pin:
      pin = er_outa(val_v);
      if (er_is_cat(pin->val_v)) {
        return 1;
      }
      law = er_outt(er_tag_law, pin->val_v);
      assert("bad pin arity" && law);
      return law->ari_d;
    case er_tag_app:
      app = er_outa(val_v);
      return er_arity(app->fn_v) - (uint32_t)app->arg_s;
    case er_tag_law:
      law = er_outa(val_v);
      return law->ari_d;
    case er_tag_thk:
    case er_tag_fwd:
    case er_tag_bad:
      assert("bad arity" && 0);
    default:
      return 0;
  }
}

/*
 * (define (OP_PUSH_VAR val) ("OP_PUSH_VAR" val)) ;; [] -- a
(define OP_PUSH_SELF ("OP_PUSH_SELF" 0)) ;; [] -- a
(define (OP_PUSH_LIT val) ("OP_PUSH_LIT" val)) ;; [] -- a
(define (OP_MK_APP count) ("OP_MK_APP" count)) ;; [<count>] -- a
(define (OP_CALLF ari) ("OP_CALLF" ari)) ;; [<count>] -- a
(define (OP_CALLU ari) ("OP_CALLU" ari)) ;; [<count>] -- a
(define (OP_FORCE ari) ("OP_FORCE" ari)) ;; a -- a
(define (OP_PRIM_UNK set) ("OP_PRIM_UNK" set)) ;; a -- a
(define OP_RET ("OP_RET" 0))
;;(define (OP_DROP idx) ("OP_DROP" ari)) ;; [a b] -- [a]

;; -- primops
(define OP_PIN ("OP_PIN" 0)) ;; a -- a
(define OP_LAW ("OP_LAW" 0)) ;; [a m b] -- l
(define OP_ELIM ("OP_ELIM" 0)) ;; [p l a z m o] -- b
(define OP_TYPE ("OP_TYPE" 0)) ;; n -- n
(define OP_NAT ("OP_NAT" 0)) ;; n -- n
(define OP_ARI ("OP_ARI" 0)) ;; l -- n
(define OP_NAM ("OP_NAM" 0)) ;; l -- n
(define OP_BODY ("OP_BODY" 0)) ;; l -- n
(define OP_UNPIN ("OP_UNPIN" 0)) ;; l -- n
(define OP_SZ ("OP_SZ" 0)) ;; r -- n
(define OP_LAST ("OP_LAST" 0)) ;; r -- r
(define OP_INIT ("OP_INIT" 0)) ;; r -- r


;; -- math
(define OP_ADD ("OP_ADD" 0)) ;; [a b] -- a
(define OP_SUB ("OP_SUB" 0)) ;; [a b] -- a
(define OP_RSH ("OP_RSH" 0)) ;; [a b] -- a
(define OP_LSH ("OP_LSH" 0)) ;; [a b] -- a
(define OP_DIV ("OP_DIV" 0)) ;; [a b] -- a
(define OP_MUL ("OP_MUL" 0)) ;; [a b] -- a
(define OP_MOD ("OP_MOD" 0)) ;; [a b] -- a
(define OP_TEST ("OP_TEST" 0)) ;; [i n] -- a
(define OP_NIB ("OP_NIB" 0)) ;; [ni n] -- a
(define (OP_LOADN w) ("OP_LOAD" w)) ;; [i n] -- a
(define OP_LOAD ("OP_LOAD" 0)) ;; [i n] -- a
(define (OP_STOREN w) ("OP_STOREN" w))
(define OP_STORE ("OP_STORE" 0))
(define (OP_SET v) ("OP_SET" v))
(define (OP_TRUNCN w) ("OP_TRUNC" w))
(define OP_TRUNCN ("OP_TRUNC" 0))
(define (OP_MET w) ("OP_MET" w))

;; -- row builders
(define OP_REP ("OP_REP" 0)) ;; [hd item count] -- r
(define OP_SLICE ("OP_REP" 0)) ;; [off count row] -- r
(define OP_WELD ("OP_WELD" 0)) ;; [x y] -- r
(define OP_NF ("OP_NF" 0)) ;; x -- nf
(define OP_UP ("OP_UP" 0)) ;; [i v r] -- r
(define OP_COUP ("OP_COUP" 0)) ;; [v r] -- r
(define OP_TRY ("OP_TRY" 0)) ;; [v r] -- r
(define OP_THROW ("OP_THROW" 0)) ;; r -- r
(define OP_HD ("OP_THROW" 0))
(define OP_IX ("OP_IX" 0)) ;; [i r] -- r
(define OP_NOT ("OP_NOT" 0)) ;; r -- r
(define OP_TRU ("OP_TRU" 0))
(define OP_OR ("OP_OR" 0)) ;; [x y] -- z
(define OP_AND ("OP_AND" 0)) ;; [x y] -- z
(define (OP_JUMP_IF label) ("OP_JUMP_IF" label)) ;; c -- []
(define OP_EQ ("OP_EQ" 0)) ;; [x y] -- r
(define OP_CMP ("OP_CMP" 0)) ;; [x y] -- r
*/


__attribute__((noinline))
er_val
plan_eval(er_vm *vm, er_val val_v)
{
    er_op* code  = vm->code;
    er_law* code_law = NULL;

    er_val* dsp   = vm->dsp;
    er_kon* kbase = vm->kbase;
    er_kon* ksp   = vm->ksp;

    uint32_t  pc  = 0;
    // uint32_t ari_d = 0;
    er_val hd_v = er_bad;
    er_val* env = NULL;
    // size_t env_s = 0;
    er_val r   = val_v;
    er_thk* thk;
    uint32_t wan_d, hav_d;
    uint64_t prim_set;
    er_val prim_arg;
    er_app* prim_row;
    er_app* prim_desc;
    const er_val* prim_arg_v;
    size_t prim_arg_s;
    er_val prim_tag_v;
    er_val prim_word_v;
    er_val prim_name_v;
    int prim_op_i;
    size_t prim_op_s;
    er_val prim_a_v;
    er_val prim_b_v;
    er_val prim_c_v;
    er_val prim_d_v;
    er_val prim_e_v;
    er_val prim_f_v;
    er_optag prim_byte_op;
    size_t prim_need_s;
    er_prim_route prim_route;
    bool prim_force_f = false;

    er_op *op;
    er_val f, app, target;
    // er_app* app;
    // er_thk* sat;
    uint32_t split;

    static void *const dispatch[OP_COUNT] = {
        [OP_PUSH_VAR]  = &&I_PUSH_VAR,
        [OP_PUSH_LIT]  = &&I_PUSH_LIT,
        [OP_MK_APP]    = &&I_MK_APP,
        [OP_MK_CALL]   = &&I_MK_CALL,
        [OP_CALLF]     = &&I_CALLF,
        [OP_CALLU]     = &&I_CALLU,
        [OP_PUSH_SELF] = &&I_PUSH_SELF,
        [OP_FORCE]     = &&I_FORCE,
        [OP_DROP]      = &&I_DROP,
        [OP_JUMP_IF_ZERO] = &&I_JUMP_IF_ZERO,
        [OP_JUMP_IF]   = &&I_JUMP_IF,
        [OP_ADD_NAT]   = &&I_ADD_NAT,
        [OP_PIN]       = &&I_OP_PIN,
        [OP_LAW]       = &&I_OP_LAW,
        [OP_ELIM]      = &&I_OP_ELIM,
        [OP_INC]       = &&I_OP_INC,
        [OP_DEC]       = &&I_OP_DEC,
        [OP_NAM]       = &&I_OP_NAM,
        [OP_BODY]      = &&I_OP_BODY,
        [OP_NAT]       = &&I_OP_NAT,
        [OP_ARI]       = &&I_OP_ARI,
        [OP_UNPIN]     = &&I_OP_UNPIN,
        [OP_SZ]        = &&I_OP_SZ,
        [OP_LAST]      = &&I_OP_LAST,
        [OP_INIT]      = &&I_OP_INIT,
        [OP_ADD]       = &&I_OP_ADD,
        [OP_SUB]       = &&I_OP_SUB,
        [OP_RSH]       = &&I_OP_RSH,
        [OP_LSH]       = &&I_OP_LSH,
        [OP_DIV]       = &&I_OP_DIV,
        [OP_MUL]       = &&I_OP_MUL,
        [OP_MOD]       = &&I_OP_MOD,
        [OP_TEST]      = &&I_OP_TEST,
        [OP_LOADN]     = &&I_OP_LOADN,
        [OP_LOAD]      = &&I_OP_LOAD,
        [OP_STOREN]    = &&I_OP_STOREN,
        [OP_STORE]     = &&I_OP_STORE,
        [OP_TRUNCN]    = &&I_OP_TRUNCN,
        [OP_TRUNC]     = &&I_OP_TRUNC,
        [OP_MET]       = &&I_OP_MET,
        [OP_MET_DYN]   = &&I_OP_MET_DYN,
        [OP_BEX]       = &&I_OP_BEX,
        [OP_BITS]      = &&I_OP_BITS,
        [OP_BYTES]     = &&I_OP_BYTES,
        [OP_LOAD8]     = &&I_OP_LOAD8,
        [OP_STORE8]    = &&I_OP_STORE8,
        [OP_TRUNC8]    = &&I_OP_TRUNC8,
        [OP_TRUNC16]   = &&I_OP_TRUNC16,
        [OP_TRUNC32]   = &&I_OP_TRUNC32,
        [OP_TRUNC64]   = &&I_OP_TRUNC64,
        [OP_REP]       = &&I_OP_REP,
        [OP_SLICE]     = &&I_OP_SLICE,
        [OP_WELD]      = &&I_OP_WELD,
        [OP_UP]        = &&I_OP_UP,
        [OP_COUP]      = &&I_OP_COUP,
        [OP_HD]        = &&I_OP_HD,
        [OP_IX]        = &&I_OP_IX,
        [OP_NOT]       = &&I_OP_NOT,
        [OP_TRU]       = &&I_OP_TRU,
        [OP_OR]        = &&I_OP_OR,
        [OP_AND]       = &&I_OP_AND,
        [OP_EQ]        = &&I_OP_EQ,
        [OP_LE]        = &&I_OP_LE,
        [OP_CMP]       = &&I_OP_CMP,
        [OP_RET]       = &&I_RET,
    };

    static const er_prim_route prim0_route[] = {
        [OP0_PIN]  = ER_PRIM_ROUTE(OP_PIN, 1),
        [OP0_LAW]  = ER_PRIM_ROUTE(OP_LAW, 3),
        [OP0_ELIM] = ER_PRIM_ROUTE(OP_ELIM, 6),
    };

    static const er_prim_route prim66_route[ER_OP66_COUNT] = {
        [OP66_INC]      = ER_PRIM_ROUTE(OP_INC, 1),
        [OP66_DEC]      = ER_PRIM_ROUTE(OP_DEC, 1),
        [OP66_ADD]      = ER_PRIM_ROUTE(OP_ADD, 2),
        [OP66_SUB]      = ER_PRIM_ROUTE(OP_SUB, 2),
        [OP66_MUL]      = ER_PRIM_ROUTE(OP_MUL, 2),
        [OP66_DIV]      = ER_PRIM_ROUTE(OP_DIV, 2),
        [OP66_MOD]      = ER_PRIM_ROUTE(OP_MOD, 2),
        [OP66_EQ]       = ER_PRIM_ROUTE(OP_EQ, 2),
        [OP66_LE]       = ER_PRIM_ROUTE(OP_LE, 2),
        [OP66_CMP]      = ER_PRIM_ROUTE(OP_CMP, 2),
        [OP66_RSH]      = ER_PRIM_ROUTE(OP_RSH, 2),
        [OP66_LSH]      = ER_PRIM_ROUTE(OP_LSH, 2),
        [OP66_TEST]     = ER_PRIM_ROUTE(OP_TEST, 2),
        [OP66_BEX]      = ER_PRIM_ROUTE(OP_BEX, 1),
        [OP66_BITS]     = ER_PRIM_ROUTE(OP_BITS, 1),
        [OP66_BYTES]    = ER_PRIM_ROUTE(OP_BYTES, 1),
        [OP66_LOAD8]    = ER_PRIM_ROUTE(OP_LOAD8, 2),
        [OP66_STORE8]   = ER_PRIM_ROUTE(OP_STORE8, 3),
        [OP66_TRUNC]    = ER_PRIM_ROUTE(OP_TRUNC, 2),
        [OP66_TRUNC8]   = ER_PRIM_ROUTE(OP_TRUNC8, 1),
        [OP66_TRUNC16]  = ER_PRIM_ROUTE(OP_TRUNC16, 1),
        [OP66_TRUNC32]  = ER_PRIM_ROUTE(OP_TRUNC32, 1),
        [OP66_TRUNC64]  = ER_PRIM_ROUTE(OP_TRUNC64, 1),
        [OP66_NAT]      = ER_PRIM_ROUTE(OP_NAT, 1),
        [OP66_UNPIN]    = ER_PRIM_ROUTE(OP_UNPIN, 1),
        [OP66_ARITY]    = ER_PRIM_ROUTE(OP_ARI, 1),
        [OP66_NAME]     = ER_PRIM_ROUTE(OP_NAM, 1),
        [OP66_BODY]     = ER_PRIM_ROUTE(OP_BODY, 1),
        [OP66_HD]       = ER_PRIM_ROUTE(OP_HD, 1),
        [OP66_LAST]     = ER_PRIM_ROUTE(OP_LAST, 1),
        [OP66_INIT]     = ER_PRIM_ROUTE(OP_INIT, 1),
        [OP66_REP]      = ER_PRIM_ROUTE(OP_REP, 3),
        [OP66_SLICE]    = ER_PRIM_ROUTE(OP_SLICE, 3),
        [OP66_WELD]     = ER_PRIM_ROUTE(OP_WELD, 2),
        [OP66_UP]       = ER_PRIM_ROUTE(OP_UP, 3),
        [OP66_UP_UNIQ]  = ER_PRIM_ROUTE(OP_UP, 3),
        [OP66_COUP]     = ER_PRIM_ROUTE(OP_COUP, 2),
        [OP66_SZ]       = ER_PRIM_ROUTE(OP_SZ, 1),
        [OP66_IX]       = ER_PRIM_ROUTE(OP_IX, 2),
        [OP66_NIL]      = ER_PRIM_ROUTE(OP_NOT, 1),
        [OP66_TRUTH]    = ER_PRIM_ROUTE(OP_TRU, 1),
        [OP66_OR]       = ER_PRIM_ROUTE(OP_OR, 2),
        [OP66_AND]      = ER_PRIM_ROUTE(OP_AND, 2),
        [OP66_LOAD]     = ER_PRIM_ROUTE(OP_LOAD, 3),
        [ER_OP66_STORE] = ER_PRIM_ROUTE(OP_STORE, 4),
        [ER_OP66_MET]   = ER_PRIM_ROUTE(OP_MET_DYN, 2),
    };

#define DPUSH(_r)      do { *dsp++ = (_r); } while (0)
#define DPOP()         (*--dsp)

#define DISPATCH()                                 \
    do {                                           \
        vm->b_count++;                              \
        prim_force_f = false;                       \
        op = &code[pc++];                          \
        goto *dispatch[op->tag];                   \
    } while (0)

#define KPUSH_RETURN(_pc, _env, _code, _law)       \
    do {                                           \
        vm->k_count++;                              \
        (ksp++)->ref = (_env);                     \
        (ksp++)->pc  = (_pc);                      \
        (ksp++)->code = (_code);                   \
        (ksp++)->law = (_law);                     \
        (ksp++)->lab = &&K_RETURN;                 \
    } while (0)

#define KPUSH_UPDATE(_target)                      \
    do {                                           \
        (ksp++)->val_v = (_target);                  \
        (ksp++)->lab = &&K_UPDATE;                 \
    } while (0)

#define KPUSH_APPHEAD(_app)                        \
    do {                                           \
        (ksp++)->val_v = (_app);                     \
        (ksp++)->lab = &&K_APPHEAD;                \
    } while (0)

#define KPUSH_OVERAPP(_app, _split)                \
    do {                                           \
        (ksp++)->val_v = (_app);                     \
        (ksp++)->u   = (uintptr_t)(_split);        \
        (ksp++)->lab = &&K_OVERAPP;                \
    } while (0)

#define RETURN(_r)                                 \
    do {                                           \
        r = (_r);                                  \
        if (ksp == kbase) return r;                \
        void *dst = (--ksp)->lab;                  \
        goto *dst;                                 \
    } while (0)

#define FAIL_ALLOC()                               \
    do {                                           \
        vm->dsp = dsp;                             \
        vm->ksp = ksp;                             \
        return er_bad;                             \
    } while (0)

#define CHECK_ALLOC(_v)                            \
    do {                                           \
        if ((_v) == 0) {                           \
            FAIL_ALLOC();                          \
        }                                          \
    } while (0)

#define CHECK_PRIM(_v)                             \
    do {                                           \
        if ((_v) == er_bad) {                      \
            FAIL_ALLOC();                          \
        }                                          \
    } while (0)

#define PRIM_DONE_VALUE(_v)                        \
    do {                                           \
        er_val prim_res_v = (_v);                  \
        CHECK_PRIM(prim_res_v);                    \
        DPUSH(prim_res_v);                         \
        goto PRIM_DONE;                            \
    } while (0)

#define PRIM_BAD_ARITY()                           \
    do {                                           \
        DPUSH(0);                                  \
        goto PRIM_DONE;                            \
    } while (0)

#define PRIM_PUSH_ARGS(_n)                         \
    do {                                           \
        if (prim_arg_s != (size_t)(_n)) {          \
            PRIM_BAD_ARITY();                      \
        }                                          \
        for (size_t prim_k_s = 0;                  \
             prim_k_s < (size_t)(_n);              \
             prim_k_s++) {                         \
            DPUSH(prim_arg_v[prim_k_s]);           \
        }                                          \
    } while (0)

#define PRIM_SELECT(_tag, _arg_s)                  \
    do {                                           \
        prim_byte_op = (_tag);                     \
        prim_need_s = (size_t)(_arg_s);            \
        goto PRIM_ROUTE_DISPATCH;                  \
    } while (0)

    /*
     * Entry: eval root by forcing it.
     */
    r = val_v;
    goto FORCE_ENTRY;

    // ---------------------------------------------------------------------
    // Bytecode dispatch
    // ---------------------------------------------------------------------

I_PUSH_VAR:
    r = env[op->as.slot];
    DPUSH(r);
    DISPATCH();

I_PUSH_LIT:
    r = op->as.lit_v;
    DPUSH(r);
    DISPATCH();

I_MK_APP: {
    size_t app_s = op->as.u32;
    if (app_s == 0 || (size_t)(dsp - vm->dstack) < app_s) {
      FAIL_ALLOC();
    }
    er_val* app_base = dsp - app_s;
    hd_v = app_base[0];
    r = er_app_make(vm->loc_a, hd_v, app_s - 1, &app_base[1]);
    CHECK_ALLOC(r);
    dsp = app_base;
    DPUSH(r);
    DISPATCH();
}

I_MK_CALL: {
    size_t app_s = op->as.u32;
    if (app_s == 0 || (size_t)(dsp - vm->dstack) < app_s) {
      FAIL_ALLOC();
    }
    er_val* app_base = dsp - app_s;
    r = er_thk_make_unk_app(vm->loc_a, app_s, app_base);
    CHECK_ALLOC(r);
    dsp = app_base;
    DPUSH(r);
    DISPATCH();
}

I_CALLF: {
    size_t arg_s = op->as.u32;
    size_t call_s = arg_s + 1;
    if (arg_s == SIZE_MAX || (size_t)(dsp - vm->dstack) < call_s) {
      FAIL_ALLOC();
    }
    er_val* call_base = dsp - call_s;
    size_t frame_s = er_call_frame_size(call_base[0], (uint32_t)arg_s);
    CHECK_ALLOC(frame_s);
    r = er_thk_make_call_frame(vm->loc_a, frame_s, call_s, call_base);
    CHECK_ALLOC(r);
    dsp = call_base;
    KPUSH_RETURN(pc, env, code, code_law);
    goto FORCE_ENTRY;
}

I_CALLU: {
    size_t arg_s = op->as.u32;
    size_t call_s = arg_s + 1;
    if (arg_s == SIZE_MAX || (size_t)(dsp - vm->dstack) < call_s) {
      FAIL_ALLOC();
    }
    er_val* call_base = dsp - call_s;
    r = er_thk_make_unk_app(vm->loc_a, call_s, call_base);
    CHECK_ALLOC(r);
    dsp = call_base;
    KPUSH_RETURN(pc, env, code, code_law);
    goto FORCE_ENTRY;
}

I_PUSH_SELF:
    DPUSH(env[0]);
    DISPATCH();

I_ADD_NAT: {
    er_val b = DPOP();
    er_val a = DPOP();
    assert(er_is_cat(a) && er_is_cat(b));
    r = a + b;
    DPUSH(r);
    DISPATCH();

}

I_OP_PIN:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_pin(vm->loc_a, prim_a_v));

I_OP_LAW:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_law(vm->loc_a, prim_b_v, prim_c_v, prim_a_v));

I_OP_ELIM:
    prim_f_v = DPOP();
    prim_e_v = DPOP();
    prim_d_v = DPOP();
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_elim(vm->loc_a, prim_a_v, prim_b_v, prim_c_v, prim_d_v, prim_e_v,
                            prim_f_v));

I_OP_NAM:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_nam(prim_a_v));

I_OP_BODY:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_body(prim_a_v));

I_OP_NAT:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_nat(prim_a_v));

I_OP_ARI:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE((er_val)er_arity(prim_a_v));

I_OP_UNPIN:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_unpin(prim_a_v));

I_OP_SZ:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_sz(prim_a_v));

I_OP_LAST:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_last(prim_a_v));

I_OP_INIT:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_init(vm->loc_a, prim_a_v));

I_OP_ADD:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_add(vm->loc_a, prim_a_v, prim_b_v));

I_OP_SUB:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_sub(vm->loc_a, prim_a_v, prim_b_v));

I_OP_RSH:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_rsh(vm->loc_a, prim_a_v, prim_b_v));

I_OP_LSH:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_lsh(vm->loc_a, prim_a_v, prim_b_v));

I_OP_DIV:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_div(vm->loc_a, prim_a_v, prim_b_v));

I_OP_MUL:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_mul(vm->loc_a, prim_a_v, prim_b_v));

I_OP_MOD:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_mod(vm->loc_a, prim_a_v, prim_b_v));

I_OP_TEST:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_test(prim_a_v, prim_b_v));

I_OP_LOADN:
    if (!prim_force_f) {
        prim_word_v = op->as.lit_v;
    }
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_loadn(vm->loc_a, prim_word_v, prim_a_v, prim_b_v));

I_OP_LOAD:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_load(vm->loc_a, prim_a_v, prim_b_v, prim_c_v));

I_OP_STOREN:
    if (!prim_force_f) {
        prim_word_v = op->as.lit_v;
    }
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_storen(vm->loc_a, prim_word_v, prim_a_v, prim_b_v, prim_c_v));

I_OP_STORE:
    prim_d_v = DPOP();
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_store(vm->loc_a, prim_a_v, prim_c_v, prim_b_v, prim_d_v));

I_OP_TRUNCN:
    if (!prim_force_f) {
        prim_word_v = op->as.lit_v;
    }
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_truncn(vm->loc_a, prim_word_v, prim_a_v));

I_OP_TRUNC:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc(vm->loc_a, prim_a_v, prim_b_v));

I_OP_MET:
    if (!prim_force_f) {
        prim_word_v = op->as.lit_v;
    }
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_met(prim_word_v, prim_a_v));

I_OP_MET_DYN:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_met(prim_a_v, prim_b_v));

I_OP_BEX:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_bex(vm->loc_a, prim_a_v));

I_OP_BITS:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_bits(prim_a_v));

I_OP_BYTES:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_bytes(prim_a_v));

I_OP_LOAD8:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_load8(vm->loc_a, prim_a_v, prim_b_v));

I_OP_STORE8:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_store8(vm->loc_a, prim_a_v, prim_b_v, prim_c_v));

I_OP_TRUNC8:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc8(vm->loc_a, prim_a_v));

I_OP_TRUNC16:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc16(vm->loc_a, prim_a_v));

I_OP_TRUNC32:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc32(vm->loc_a, prim_a_v));

I_OP_TRUNC64:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc64(vm->loc_a, prim_a_v));

I_OP_REP:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_rep(vm->loc_a, prim_a_v, prim_b_v, prim_c_v));

I_OP_SLICE:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_slice(vm->loc_a, prim_a_v, prim_b_v, prim_c_v));

I_OP_WELD:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_weld(vm->loc_a, prim_a_v, prim_b_v));

I_OP_UP:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_up(vm->loc_a, prim_a_v, prim_b_v, prim_c_v));

I_OP_COUP:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_coup(vm->loc_a, prim_a_v, prim_b_v));

I_OP_HD:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_hd(prim_a_v));

I_OP_IX:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_ix(prim_a_v, prim_b_v));

I_OP_NOT:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_not(prim_a_v));

I_OP_TRU:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_tru(prim_a_v));

I_OP_OR:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_or(prim_a_v, prim_b_v));

I_OP_AND:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_and(prim_a_v, prim_b_v));

I_OP_EQ:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_eq(prim_a_v, prim_b_v));

I_OP_LE:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_le(prim_a_v, prim_b_v));

I_OP_CMP:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_cmp(prim_a_v, prim_b_v));

I_OP_INC:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_inc(vm->loc_a, prim_a_v));

I_OP_DEC:
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_dec(vm->loc_a, prim_a_v));

I_RET:
    r = DPOP();
    RETURN(r);

I_DROP:
    (void)DPOP();
    DISPATCH();

I_JUMP_IF_ZERO:
    r = DPOP();
    if (r == 0) {
      pc = op->as.u32;
    }
    DISPATCH();

I_JUMP_IF:
    r = DPOP();
    if (r != 0) {
      if (code_law == NULL || op->as.u32 >= code_law->bc_s ||
          code_law->bc_v[op->as.u32] == NULL) {
        FAIL_ALLOC();
      }
      code = code_law->bc_v[op->as.u32];
      pc = 0;
    }
    DISPATCH();

I_FORCE:
    r = DPOP();

    /*
     * Optional fast path: if r is already WHNF, avoid pushing K_RETURN.
     * Only push a continuation if forcing actually enters something.
     */

    if (er_is_whnf(r)) {
        DPUSH(r);
        DISPATCH();
    }

    KPUSH_RETURN(pc, env, code, code_law);
    goto FORCE_ENTRY;

    // ---------------------------------------------------------------------
    // Force mode
    // ------------------------------------------------------------------

FORCE_UNK_APP:
    f = thk->arg_v[0];
    if ( !er_is_whnf(f) ) {
      thk->fun = ER_HOLE;
      KPUSH_APPHEAD(r);
      r = f;
      goto FORCE_ENTRY;
    }
    wan_d = er_arity(f);
    hav_d = (uint32_t)(thk->arg_s - 1);
    if ( hav_d ==  wan_d ) {
      size_t frame_s = er_call_frame_size(f, wan_d);
      CHECK_ALLOC(frame_s);
      if (frame_s == thk->arg_s) {
        thk->fun = ER_CALL;
        goto FORCE_ENTRY;
      }
      r = er_thk_take_call(vm->loc_a, thk, frame_s);
      CHECK_ALLOC(r);
      goto FORCE_ENTRY;
    } else if ( hav_d < wan_d ) {
      r = er_app_make(vm->loc_a, f, thk->arg_s - 1, &thk->arg_v[1]);
      CHECK_ALLOC(r);
      goto FORCE_ENTRY;
    } else {
      split = wan_d;
      size_t frame_s = er_call_frame_size(f, wan_d);
      CHECK_ALLOC(frame_s);
      r = er_thk_make_call_frame(vm->loc_a, frame_s, (size_t)wan_d + 1, thk->arg_v);
      CHECK_ALLOC(r);
      KPUSH_OVERAPP(er_into(er_tag_thk, thk), split);
      goto FORCE_ENTRY;
    }


FORCE_ENTRY:
    if( er_is_whnf(r) ) {
      RETURN(r);
    }
    thk = er_outt(er_tag_thk, r);
    if (thk == NULL) {
      FAIL_ALLOC();
    }
    switch(thk->fun) {
      case ER_XDONE:
        RETURN(thk->arg_v[0]);
      case ER_XUNK_APP:
        goto FORCE_UNK_APP;
      case ER_CALL:
        goto ENTER_CALL;
      case ER_SUSP:
        goto FORCE_SUSP;
      case ER_HOLE:
        FAIL_ALLOC();
      default:
        assert("bad thk tag" && 0);
    }

PRIMOP:
    prim_force_f = true;
    if (prim_set == 66) {
        goto PRIM66_DECODE;
    }
    if (prim_set == 0) {
        goto PRIM0_DECODE;
    }
    FAIL_ALLOC();

PRIM0_DECODE:
    prim_row = er_outt(er_tag_app, prim_arg);
    if (prim_row == NULL) {
        FAIL_ALLOC();
    }
    prim_tag_v = prim_row->fn_v;
    prim_arg_v = prim_row->arg_v;
    prim_arg_s = prim_row->arg_s;
    if (prim_tag_v == 0 && prim_row->arg_s > 0 && er_is_cat(prim_row->arg_v[0]) &&
        prim_row->arg_v[0] <= (er_val)OP0_ELIM) {
        prim_op_s = (size_t)prim_row->arg_v[0];
        prim_arg_v = prim_row->arg_v + 1;
        prim_arg_s = prim_row->arg_s - 1;
        prim_route = prim0_route[prim_op_s];
        if (!prim_route.valid_f) {
            FAIL_ALLOC();
        }
        PRIM_SELECT(prim_route.tag, prim_route.arg_s);
    }
    if (!er_is_cat(prim_tag_v) || prim_tag_v > (er_val)OP0_ELIM) {
        FAIL_ALLOC();
    }
    prim_op_s = (size_t)prim_tag_v;
    prim_route = prim0_route[prim_op_s];
    if (!prim_route.valid_f) {
        FAIL_ALLOC();
    }
    PRIM_SELECT(prim_route.tag, prim_route.arg_s);

PRIM66_DECODE:
    prim_row = er_outt(er_tag_app, prim_arg);
    prim_tag_v = prim_arg;
    prim_arg_v = NULL;
    prim_arg_s = 0;
    if (prim_row != NULL) {
        prim_tag_v = prim_row->fn_v;
        prim_arg_v = prim_row->arg_v;
        prim_arg_s = prim_row->arg_s;
        if (prim_tag_v == 0 && prim_row->arg_s > 0) {
            prim_tag_v = prim_row->arg_v[0];
            prim_arg_v = prim_row->arg_v + 1;
            prim_arg_s = prim_row->arg_s - 1;
        }
    }

    prim_desc = er_outt(er_tag_app, prim_tag_v);
    if (prim_desc != NULL) {
        if (prim_desc->arg_s != 1) {
            FAIL_ALLOC();
        }
        prim_name_v = prim_desc->fn_v;
        prim_word_v = prim_desc->arg_v[0];
        switch (prim_name_v) {
        case PLAN_S6('O', 'P', '_', 'N', 'A', 'M'):
            PRIM_SELECT(OP_NAM, 1);
        case PLAN_S7('O', 'P', '_', 'B', 'O', 'D', 'Y'):
            PRIM_SELECT(OP_BODY, 1);
        case PLAN_S6('O', 'P', '_', 'N', 'A', 'T'):
            PRIM_SELECT(OP_NAT, 1);
        case PLAN_S6('O', 'P', '_', 'A', 'R', 'I'):
            PRIM_SELECT(OP_ARI, 1);
        case ER_S8('O', 'P', '_', 'U', 'N', 'P', 'I', 'N'):
            PRIM_SELECT(OP_UNPIN, 1);
        case PLAN_S5('O', 'P', '_', 'S', 'Z'):
            PRIM_SELECT(OP_SZ, 1);
        case PLAN_S7('O', 'P', '_', 'L', 'A', 'S', 'T'):
            PRIM_SELECT(OP_LAST, 1);
        case PLAN_S7('O', 'P', '_', 'I', 'N', 'I', 'T'):
            PRIM_SELECT(OP_INIT, 1);
        case PLAN_S6('O', 'P', '_', 'A', 'D', 'D'):
            PRIM_SELECT(OP_ADD, 2);
        case PLAN_S6('O', 'P', '_', 'S', 'U', 'B'):
            PRIM_SELECT(OP_SUB, 2);
        case PLAN_S6('O', 'P', '_', 'R', 'S', 'H'):
            PRIM_SELECT(OP_RSH, 2);
        case PLAN_S6('O', 'P', '_', 'L', 'S', 'H'):
            PRIM_SELECT(OP_LSH, 2);
        case PLAN_S6('O', 'P', '_', 'D', 'I', 'V'):
            PRIM_SELECT(OP_DIV, 2);
        case PLAN_S6('O', 'P', '_', 'M', 'U', 'L'):
            PRIM_SELECT(OP_MUL, 2);
        case PLAN_S6('O', 'P', '_', 'M', 'O', 'D'):
            PRIM_SELECT(OP_MOD, 2);
        case PLAN_S7('O', 'P', '_', 'T', 'E', 'S', 'T'):
            PRIM_SELECT(OP_TEST, 2);
        case PLAN_S7('O', 'P', '_', 'L', 'O', 'A', 'D'):
            if (prim_word_v == 0) {
                PRIM_SELECT(OP_LOAD, 3);
            }
            PRIM_SELECT(OP_LOADN, 2);
        case ER_S8('O', 'P', '_', 'S', 'T', 'O', 'R', 'E'):
            if (prim_word_v == 0) {
                PRIM_SELECT(OP_STORE, 4);
            }
            PRIM_SELECT(OP_STOREN, 3);
        case ER_S8('O', 'P', '_', 'T', 'R', 'U', 'N', 'C'):
            if (prim_word_v == 0) {
                PRIM_SELECT(OP_TRUNC, 2);
            }
            PRIM_SELECT(OP_TRUNCN, 1);
        case PLAN_S6('O', 'P', '_', 'M', 'E', 'T'):
            PRIM_SELECT(OP_MET, 1);
        case PLAN_S6('O', 'P', '_', 'R', 'E', 'P'):
            PRIM_SELECT(OP_REP, 3);
        case ER_S8('O', 'P', '_', 'S', 'L', 'I', 'C', 'E'):
            PRIM_SELECT(OP_SLICE, 3);
        case PLAN_S7('O', 'P', '_', 'W', 'E', 'L', 'D'):
            PRIM_SELECT(OP_WELD, 2);
        case PLAN_S5('O', 'P', '_', 'U', 'P'):
            PRIM_SELECT(OP_UP, 3);
        case PLAN_S7('O', 'P', '_', 'C', 'O', 'U', 'P'):
            PRIM_SELECT(OP_COUP, 2);
        case PLAN_S5('O', 'P', '_', 'H', 'D'):
            PRIM_SELECT(OP_HD, 1);
        case PLAN_S5('O', 'P', '_', 'I', 'X'):
            PRIM_SELECT(OP_IX, 2);
        case PLAN_S6('O', 'P', '_', 'N', 'O', 'T'):
            PRIM_SELECT(OP_NOT, 1);
        case PLAN_S6('O', 'P', '_', 'T', 'R', 'U'):
            PRIM_SELECT(OP_TRU, 1);
        case PLAN_S5('O', 'P', '_', 'O', 'R'):
            PRIM_SELECT(OP_OR, 2);
        case PLAN_S6('O', 'P', '_', 'A', 'N', 'D'):
            PRIM_SELECT(OP_AND, 2);
        case PLAN_S5('O', 'P', '_', 'E', 'Q'):
            PRIM_SELECT(OP_EQ, 2);
        case PLAN_S5('O', 'P', '_', 'L', 'E'):
            PRIM_SELECT(OP_LE, 2);
        case PLAN_S6('O', 'P', '_', 'C', 'M', 'P'):
            PRIM_SELECT(OP_CMP, 2);
        default:
            FAIL_ALLOC();
        }
    }
    if (!eo_op66_from_tag(prim_tag_v, &prim_op_i) || prim_op_i < 0 ||
        (size_t)prim_op_i >= ER_OP66_COUNT) {
        FAIL_ALLOC();
    }
    prim_route = prim66_route[prim_op_i];
    if (!prim_route.valid_f) {
        FAIL_ALLOC();
    }
    PRIM_SELECT(prim_route.tag, prim_route.arg_s);

PRIM_ROUTE_DISPATCH:
    prim_force_f = true;
    if (prim_byte_op >= OP_COUNT || dispatch[prim_byte_op] == NULL) {
        FAIL_ALLOC();
    }
    PRIM_PUSH_ARGS(prim_need_s);
    goto *dispatch[prim_byte_op];

PRIM_DONE:
    if (prim_force_f) {
      r = DPOP();
      goto FORCE_ENTRY;
    }
    DISPATCH();

    // ---------------------------------------------------------------------
    // Saturated application entry
    // ---------------------------------------------------------------------

FORCE_SUSP: {
    uint32_t susp_label = (uint32_t)thk->arg_v[0];
    er_thk* fr = er_outt(er_tag_thk, thk->arg_v[1]);
    if (fr == NULL) {
      FAIL_ALLOC();
    }
    er_law* law = er_resolve_law(fr->arg_v[0]);
    if (law == NULL || susp_label >= law->bc_s || law->bc_v[susp_label] == NULL) {
      FAIL_ALLOC();
    }
    KPUSH_UPDATE(er_into(er_tag_thk, thk));
    code_law = law;
    code = law->bc_v[susp_label];
    pc = 0;
    env = fr->arg_v;
    thk->fun = ER_HOLE;
    DISPATCH();
}

ENTER_CALL: {
    er_pin* pin;
    er_val self_v = er_into(er_tag_thk, thk);
    f = thk->arg_v[0];
    if ( er_is_tag(er_tag_pin, f) ) {
      pin = er_outa(f);
      if ( er_is_cat(pin->val_v) ) {
        prim_set = pin->val_v;
        prim_arg = thk->arg_v[1];
        goto PRIMOP;
      }
      f = pin->val_v;
    }
    er_law* law = er_outt(er_tag_law, f);
    if (law == NULL) {
      FAIL_ALLOC();
    }
    size_t frame_s = (size_t)law->ari_d + 1 + law->let_d;
    if (thk->arg_s != frame_s || law->bc_s == 0 || law->bc_v[0] == NULL) {
      FAIL_ALLOC();
    }
    size_t n_lets = er_law_n_lets(law);

    for (size_t i = 0; i < n_lets; i++) {
      size_t slot_s = (size_t)law->ari_d + 1 + i;
      er_val susp_v = er_thk_make_susp(vm->loc_a, (uint32_t)i + 1, self_v);
      CHECK_ALLOC(susp_v);
      thk->arg_v[slot_s] = susp_v;
    }
    KPUSH_UPDATE(self_v);
    thk->fun = ER_HOLE;
    code_law = law;
    code = law->bc_v[0];
    pc  = 0;
    env = thk->arg_v;
    // env_s = thk->arg_s;
    DISPATCH();
}

    // ---------------------------------------------------------------------
    // Continuation handlers
    // ---------------------------------------------------------------------

K_RETURN:
    /*
     * Payload layout:
     *
     *   [ env ][ pc ][ code ][ law ][ &&K_RETURN ]
     *
     * The label has already been popped by RETURN.
     */
    code_law = (--ksp)->law;
    code = (--ksp)->code;
    pc  = (--ksp)->pc;
    env = (--ksp)->ref;

    DPUSH(r);
    DISPATCH();

K_UPDATE:
    /*
     * Payload layout:
     *
     *   [ target ][ &&K_UPDATE ]
     */
    target = (--ksp)->val_v;
    if (!er_is_whnf(r)) {
      KPUSH_UPDATE(target);
      goto FORCE_ENTRY;
    }

    /*
     * Update by indirection, not shallow copy.
     */
    thk = er_outt(er_tag_thk, target);
    if (thk == NULL) {
      FAIL_ALLOC();
    }
    thk->fun = ER_XDONE;
    thk->arg_v[0] = r;
    RETURN(r);

K_APPHEAD:
    /*
     * Payload layout:
     *
     *   [ app ][ &&K_APPHEAD ]
     */
    app = (--ksp)->val_v;
    thk = er_outt(er_tag_thk, app);
    if ( !thk ) {
      FAIL_ALLOC();
    }
    assert(thk->fun == ER_HOLE);
    if (thk->fun != ER_HOLE) {
      FAIL_ALLOC();
    }
    thk->fun = ER_XUNK_APP;
    thk->arg_v[0] = r;
    r = er_into(er_tag_thk, thk);
    goto FORCE_ENTRY;

    //
    // if ( !er_is_tag(er_tag_app, app) ) {
    //     RETURN(app);
    // }
    //
    // {
    //     hd_v = app_fun(app);
    //     arg_v = app_args(app);
    //     wan_d   = er_arity(head);
    //     hav_d = args_len(args);
    //
    //     // if (af < 0) {
    //     //     panic_negative_arity();
    //     // }
    //
    //     if (hav_d < (uint32_t)wan_d) {
    //         /*
    //          * Partial application. The app is already WHNF.
    //          */
    //         RETURN(app);
    //     }
    //
    //     if (hav_d == wan_d) {
    //         sat = er_thk_make_call();
    //         goto ENTER_SAT;
    //     }
    //
    //     /*
    //      * Overapplication:
    //      *
    //      *   app = f a b c d
    //      *   law arity says saturated prefix is f a b
    //      *
    //      * Instead of storing the rest vector in the continuation, store only
    //      * the split index. K_OVERAPP recovers the rest from the original app.
    //      */
    //     split = (uint32_t)af;
    //
    //     sat = er_outa(er_app_take(vm->loc_a, app, wan_d));
    //
    //     KPUSH_OVERAPP(app, wan_d);
    //     goto ENTER_SAT;
    // }

K_OVERAPP:
    /*
     * Payload layout:
     *
     *   [ app ][ split ][ &&K_OVERAPP ]
     */
    split = (uint32_t)(--ksp)->u;
    app   = (--ksp)->val_v;

    {
      thk = er_outt(er_tag_thk, app);
      assert("bad overapp" && thk);
      r = er_app_drop_coup(vm->loc_a, thk, r, split);
      CHECK_ALLOC(r);
        goto FORCE_ENTRY;
    }

#undef CHECK_ALLOC
#undef PRIM_SELECT
#undef PRIM_PUSH_ARGS
#undef PRIM_BAD_ARITY
#undef PRIM_DONE_VALUE
#undef CHECK_PRIM
#undef FAIL_ALLOC
#undef RETURN
#undef KPUSH_OVERAPP
#undef KPUSH_APPHEAD
#undef KPUSH_UPDATE
#undef KPUSH_RETURN
#undef DISPATCH
#undef DPOP
#undef DPUSH
#undef ER_PRIM_ROUTE
}
