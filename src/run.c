#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "enki/run.h"
#include "enki/util.h"

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

er_law* er_law_alloc(const enki_allocator* allocator, size_t n_lets)
{
    size_t size_s = 0;
    if (!er_alloc_size(sizeof(er_law), n_lets, sizeof(uint32_t), &size_s)) {
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
    law->start_d = 0;
    law->frame_d = 0;
    return law;
}

er_val er_law_init(er_law* law, er_val name_v, er_val body_v, uint32_t ari_d,
    uint32_t frame_d, size_t n_lets, const uint32_t let_off_d[])
{
    if (law == NULL) {
        return 0;
    }
    if ((uint64_t)frame_d != (uint64_t)ari_d + 1 + n_lets) {
        return 0;
    }
    if (!er_flex_fits(&law->h, sizeof(er_law), n_lets, sizeof(uint32_t))) {
        return 0;
    }
    law->h.raw.nf_f = 1;
    law->name_v = name_v;
    law->body_v = body_v;
    law->ari_d = ari_d;
    law->start_d = 0;
    law->frame_d = frame_d;
    if (n_lets > 0) {
        if (let_off_d != NULL) {
            memcpy(law->let_off_d, let_off_d, n_lets * sizeof(uint32_t));
        } else {
            memset(law->let_off_d, 0, n_lets * sizeof(uint32_t));
        }
    }
    return er_into(er_tag_law, law);
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
    if (law == NULL || law->frame_d < law->ari_d + 1) {
        return 0;
    }
    return (size_t)(law->frame_d - law->ari_d - 1);
}

static size_t er_call_frame_size(er_val fun_v, uint32_t arity_d)
{
    er_law* law = er_resolve_law(fun_v);
    if (law == NULL) {
        return (size_t)arity_d + 1;
    }
    if (law->ari_d != arity_d || law->frame_d < law->ari_d + 1) {
        return 0;
    }
    return law->frame_d;
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


static er_val er_exec_primop_66(er_val arg_v)
{
  er_app* app = er_outt(er_tag_app, arg_v);
  switch ( app->fn_v ) {
    case PLAN_S3('A', 'd', 'd'):
      assert(app->arg_s == 2);
      // ignoring bignat arithmetic for now
      return app->arg_v[0] + app->arg_v[1];
    case PLAN_S3('S', 'u', 'b'):
      if (app->arg_v[0] < app->arg_v[1]) {
        return 0;
      }
      return app->arg_v[0] - app->arg_v[1];
    case PLAN_S3('M', 'u', 'l'):
      return app->arg_v[0] * app->arg_v[1];
    default:
      assert("bad opcode" && false);

  }
  return er_bad;
}

static er_val er_exec_primop_0(er_val arg_v)
{
  er_app* app = er_outt(er_tag_app, arg_v);
  switch ( app->fn_v ) {
    case 0:
      assert(app->arg_s == 1);
      // TODO: make pin
    case 1:
      assert(app->arg_s == 3);
      // TODO make law
    case 2:
      assert(app->arg_s == 6);

    default:
      assert("bad opcode" && false);
      return er_bad;
  }
  return er_bad;
}
static er_val er_exec_primop(er_val set_v, er_val arg_v)
{
  if ( set_v == 66 ) {
    return er_exec_primop_66(arg_v);
  }
  if ( set_v == 0 ) {
    return er_exec_primop_0(arg_v);
  }
  return 0;
}




__attribute__((noinline))
er_val
plan_eval(er_vm *vm, er_val val_v)
{
    er_op* code  = vm->code;

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
        [OP_FORCE]     = &&I_FORCE,
        [OP_DROP]      = &&I_DROP,
        [OP_JUMP_IF_ZERO] = &&I_JUMP_IF_ZERO,
        [OP_ADD_NAT]   = &&I_ADD_NAT,
        [OP_RET]       = &&I_RET,
    };

#define DPUSH(_r)      do { *dsp++ = (_r); } while (0)
#define DPOP()         (*--dsp)

#define DISPATCH()                                 \
    do {                                           \
        op = &code[pc++];                          \
        goto *dispatch[op->tag];                   \
    } while (0)

#define KPUSH_RETURN(_pc, _env)                    \
    do {                                           \
        (ksp++)->ref = (_env);                     \
        (ksp++)->pc  = (_pc);                      \
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

I_ADD_NAT: {
    er_val b = DPOP();
    er_val a = DPOP();
    assert(er_is_cat(a) && er_is_cat(b));
    r = a + b;
    DPUSH(r);
    DISPATCH();

}

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

    KPUSH_RETURN(pc, env);
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
    // prim_set;
    // prim_arg;

    r = er_exec_primop(prim_set, prim_arg);
    goto FORCE_ENTRY;

    // ---------------------------------------------------------------------
    // Saturated application entry
    // ---------------------------------------------------------------------

FORCE_SUSP: {
    uint32_t susp_pc = (uint32_t)thk->arg_v[0];
    er_thk* fr = er_outt(er_tag_thk, thk->arg_v[1]);
    if (fr == NULL) {
      FAIL_ALLOC();
    }
    KPUSH_UPDATE(er_into(er_tag_thk, thk));
    pc = susp_pc;
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
	    size_t min_frame_s = (size_t)law->ari_d + 1;
	    if (law->frame_d < min_frame_s || thk->arg_s != law->frame_d) {
	      FAIL_ALLOC();
	    }
	    size_t n_lets = er_law_n_lets(law);
    for (size_t i = 0; i < n_lets; i++) {
      size_t slot_s = (size_t)law->ari_d + 1 + i;
      er_val susp_v = er_thk_make_susp(vm->loc_a, law->let_off_d[i], self_v);
      CHECK_ALLOC(susp_v);
      thk->arg_v[slot_s] = susp_v;
    }
    KPUSH_UPDATE(self_v);
    thk->fun = ER_HOLE;
    pc  = law->start_d;
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
     *   [ env ][ pc ][ &&K_RETURN ]
     *
     * The label has already been popped by RETURN.
     */
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
#undef FAIL_ALLOC
#undef RETURN
#undef KPUSH_OVERAPP
#undef KPUSH_APPHEAD
#undef KPUSH_UPDATE
#undef KPUSH_RETURN
#undef DISPATCH
#undef DPOP
#undef DPUSH
}
