#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "bytecode_internal.h"

#include "enki/bytecode.h"
#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/profile.h"
#include "enki/run.h"
#include "enki/run_ops.h"
#include "enki/util.h"
#include "enki/print.h"

#include "run_opcode.def"

enum {
    ER_PRIM_ROUTE_MAX_CODE = 64,
    ER_PRIM_ROUTE_MAX_DEPTH = 64,
};

#define ER_ASSERT_OPCODE_VALUE(_tag, _label, _value) \
    static_assert((_tag) == (_value), #_tag " opcode value changed");
ER_ALL_OPS(ER_ASSERT_OPCODE_VALUE)
#undef ER_ASSERT_OPCODE_VALUE

static_assert(OP_COUNT == 68, "OP_COUNT opcode value changed");

static bool er_prim66_op_from_descriptor(er_val tag_v, int* out_op)
{
    if (eo_op66_from_tag(tag_v, out_op)) {
        return true;
    }
    if (tag_v == PLAN_S5('O', 'P', '_', 'L', 'E')) {
        *out_op = OP66_LE;
        return true;
    }
    return false;
}

static bool er_alloc_size(size_t base_s, size_t count_s, size_t elem_s, size_t* out_s)
{
    if (elem_s != 0 && count_s > (SIZE_MAX - base_s) / elem_s) {
        return false;
    }
    *out_s = base_s + (count_s * elem_s);
    return true;
}

static bool er_align_size(size_t size_s, size_t align_s, size_t* out_s)
{
    if (align_s == 0) {
        return false;
    }
    size_t rem_s = size_s % align_s;
    if (rem_s == 0) {
        *out_s = size_s;
        return true;
    }
    size_t add_s = align_s - rem_s;
    if (size_s > SIZE_MAX - add_s) {
        return false;
    }
    *out_s = size_s + add_s;
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

er_tank* er_tank_alloc(const enki_allocator* allocator)
{
    ENKI_PROFILE_ZONE("er_tank_alloc");
    return (er_tank*)er_alloc_bytes(allocator, sizeof(er_tank));
}

er_val er_tank_init(er_tank* tank, er_val val_v, char* msg_c)
{
    if (tank == NULL) {
        return 0;
    }
    tank->val_v = val_v;
    tank->msg_c = msg_c == NULL ? "" : msg_c;
    return er_into(er_tag_tank, tank);
}

er_val er_tank_make(const enki_allocator* loc_a, er_val val_v, char* msg_c)
{
    er_tank* tank = er_tank_alloc(loc_a);
    if (tank == NULL) {
        return er_bad;
    }
    er_val out_v = er_tank_init(tank, val_v, msg_c);
    return out_v == 0 ? er_bad : out_v;
}

er_bat* er_bat_alloc(const enki_allocator* allocator, size_t lim_s)
{
    ENKI_PROFILE_ZONE("er_bat_alloc");
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
    ENKI_PROFILE_ZONE("er_pin_alloc");
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
    pin->hed.raw.nf_f = 1;
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

static bool er_law_layout(size_t bc_s, size_t op_s, size_t* code_o, size_t* size_s)
{
    size_t label_end_s = 0;
    if (!er_alloc_size(sizeof(er_law), bc_s, sizeof(er_law_label), &label_end_s)) {
        return false;
    }
    if (!er_align_size(label_end_s, _Alignof(er_op), code_o)) {
        return false;
    }
    return er_alloc_size(*code_o, op_s, sizeof(er_op), size_s);
}

static bool er_law_total_ops(size_t bc_s, const er_op* const bc_v[],
                             const size_t bc_len_v[], size_t* out_s)
{
    if (bc_s == 0 || bc_v == NULL || bc_len_v == NULL) {
        return false;
    }
    size_t op_s = 0;
    for (size_t k = 0; k < bc_s; k++) {
        if (bc_v[k] == NULL || bc_len_v[k] == 0 || op_s > SIZE_MAX - bc_len_v[k]) {
            return false;
        }
        op_s += bc_len_v[k];
    }
    *out_s = op_s;
    return true;
}

er_law* er_law_alloc(const enki_allocator* allocator, size_t bc_s, size_t op_s)
{
    ENKI_PROFILE_ZONE("er_law_alloc");
    size_t size_s = 0;
    size_t code_o = 0;
    if (!er_law_layout(bc_s, op_s, &code_o, &size_s)) {
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
    law->op_s = op_s;
    law->code_o = code_o;
    if (bc_s > 0) {
        memset(law->bc_v, 0, bc_s * sizeof(er_law_label));
    }
    return law;
}

er_val er_law_init(er_law* law, er_val name_v, er_val body_v, uint32_t ari_d,
    uint32_t let_d, size_t bc_s, er_op* const bc_v[], const size_t bc_len_v[])
{
    if (law == NULL) {
        return 0;
    }
    if (bc_s == 0 || ari_d == UINT32_MAX || let_d > UINT32_MAX - ari_d - 1u ||
        bc_s < (size_t)let_d + 1u) {
        return 0;
    }
    size_t op_s = 0;
    if (!er_law_total_ops(bc_s, (const er_op* const*)bc_v, bc_len_v, &op_s)) {
        return 0;
    }
    size_t code_o = 0;
    size_t size_s = 0;
    if (!er_law_layout(bc_s, op_s, &code_o, &size_s) || size_s > er_head_size(&law->h)) {
        return 0;
    }
    if (law->code_o != code_o || law->op_s < op_s) {
        return 0;
    }
    law->name_v = name_v;
    law->body_v = body_v;
    law->ari_d = ari_d;
    law->let_d = let_d;
    law->bc_s = bc_s;
    law->op_s = op_s;
    law->h.raw.nf_f = 1;

    er_op* dst_v = er_law_code_base(law);
    size_t off_s = 0;
    for (size_t k = 0; k < bc_s; k++) {
        law->bc_v[k] = (er_law_label){.off_s = off_s, .op_s = bc_len_v[k]};
        memcpy(dst_v + off_s, bc_v[k], bc_len_v[k] * sizeof(er_op));
        off_s += bc_len_v[k];
    }
    return er_into(er_tag_law, law);
}

er_val er_law_make_code(const enki_allocator* loc_a, er_val nam_v, er_val bod_v,
                        uint32_t ari_d, uint32_t let_d, size_t bc_s, er_op* const bc_v[],
                        const size_t bc_len_v[])
{
  size_t op_s = 0;
  if (!er_law_total_ops(bc_s, (const er_op* const*)bc_v, bc_len_v, &op_s)) {
    return 0;
  }
  er_law* law = er_law_alloc(loc_a, bc_s, op_s);
  if (law == NULL) {
    return 0;
  }
  er_val law_v = er_law_init(law, nam_v, bod_v, ari_d, let_d, bc_s, bc_v, bc_len_v);
  if (law_v == 0 && loc_a != NULL && loc_a->free != NULL) {
    loc_a->free(loc_a->ctx, law);
  }
  return law_v;
}

er_val er_law_make(const enki_allocator* loc_a, er_val nam_v, er_val bod_v, uint32_t ari_d)
{
  return er_law_compile(loc_a, nam_v, bod_v, ari_d);
}

er_app* er_app_alloc(const enki_allocator* allocator, size_t arg_s)
{
    ENKI_PROFILE_ZONE("er_app_alloc");
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
    ENKI_PROFILE_ZONE("er_thk_alloc");
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

static size_t er_call_frame_size(er_val fun_v, uint32_t arity_d);

static er_val er_thk_make_prim(const enki_allocator* loc_a, er_val prim_set_v,
                               er_val prim_arg_v)
{
    er_val arg_v[] = {prim_set_v, prim_arg_v};
    er_thk* thk = er_thk_alloc(loc_a, 2);
    if (thk == NULL) {
        return 0;
    }
    return er_thk_init(thk, ER_XPRIM, 2, arg_v);
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

static bool er_app_spine_count(er_val fn_v, size_t extra_s, er_val* out_fn_v,
                               size_t* out_arg_s);
static void er_app_spine_copy(er_val fn_v, er_val* out_v, size_t* out_s);

static er_val er_thk_make_unk_app_flat(const enki_allocator* loc_a, er_val fn_v,
                                       size_t arg_s, const er_val arg_v[])
{
    er_val flat_fn_v = fn_v;
    size_t total_arg_s = 0;
    if (!er_app_spine_count(fn_v, arg_s, &flat_fn_v, &total_arg_s) ||
        total_arg_s > UINT32_MAX) {
        return 0;
    }
    er_thk* thk = er_thk_alloc(loc_a, total_arg_s + 1u);
    if (thk == NULL) {
        return 0;
    }
    er_val out_v = er_thk_init(thk, ER_XUNK_APP, total_arg_s + 1u, NULL);
    if (out_v == 0) {
        return 0;
    }
    thk->arg_v[0] = flat_fn_v;
    size_t copied_s = 0;
    er_app_spine_copy(fn_v, thk->arg_v + 1, &copied_s);
    if (arg_s > 0) {
        memcpy(thk->arg_v + 1 + copied_s, arg_v, arg_s * sizeof(er_val));
    }
    return out_v;
}

static er_val er_thk_make_susp(const enki_allocator* loc_a, uint32_t pc, er_val frame_v,
                               er_val law_v)
{
    er_val arg_v[] = {
        (er_val)pc,
        frame_v,
        law_v,
    };
    er_thk* thk = er_thk_alloc(loc_a, 3);
    if (thk == NULL) {
        return 0;
    }
    return er_thk_init(thk, ER_SUSP, 3, arg_v);
}

static er_val er_thk_make_env_frame(const enki_allocator* loc_a, size_t frame_s,
                                    const er_val frame_v[])
{
    er_thk* thk = er_thk_alloc(loc_a, frame_s);
    if (thk == NULL) {
        return 0;
    }
    return er_thk_init(thk, ER_XDONE, frame_s, frame_v);
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

static bool er_app_spine_count(er_val fn_v, size_t extra_s, er_val* out_fn_v,
                               size_t* out_arg_s)
{
    size_t total_s = extra_s;
    er_val cur_v = fn_v;
    er_app* app = er_outt(er_tag_app, cur_v);
    while (app != NULL) {
        if (app->arg_s > SIZE_MAX - total_s) {
            return false;
        }
        total_s += app->arg_s;
        cur_v = app->fn_v;
        app = er_outt(er_tag_app, cur_v);
    }
    *out_fn_v = cur_v;
    *out_arg_s = total_s;
    return true;
}

static void er_app_spine_copy(er_val fn_v, er_val* out_v, size_t* out_s)
{
    er_app* app = er_outt(er_tag_app, fn_v);
    if (app == NULL) {
        return;
    }
    er_app_spine_copy(app->fn_v, out_v, out_s);
    if (app->arg_s > 0) {
        memcpy(out_v + *out_s, app->arg_v, app->arg_s * sizeof(er_val));
        *out_s += app->arg_s;
    }
}

static er_val er_app_make_flat(const enki_allocator* loc_a, er_val fn_v, size_t arg_s,
                               const er_val arg_v[])
{
    er_val flat_fn_v = fn_v;
    size_t total_s = arg_s;
    if (!er_app_spine_count(fn_v, arg_s, &flat_fn_v, &total_s)) {
        return 0;
    }
    if (flat_fn_v == fn_v && total_s == arg_s) {
        return er_app_make(loc_a, fn_v, arg_s, arg_v);
    }
    er_app* app = er_app_alloc(loc_a, total_s);
    if (app == NULL) {
        return 0;
    }
    er_val out_v = er_app_init(app, flat_fn_v, total_s, NULL);
    if (out_v == 0) {
        return 0;
    }
    size_t copied_s = 0;
    er_app_spine_copy(fn_v, app->arg_v, &copied_s);
    if (arg_s > 0) {
        memcpy(app->arg_v + copied_s, arg_v, arg_s * sizeof(er_val));
    }
    return out_v;
}

static er_val er_eval_with_heap(const enki_allocator* heap_a, const enki_allocator* work_a,
                                enki_gc* gc, er_val val_v, er_eval_mode mode)
{
    ENKI_PROFILE_ZONE("er_eval");
    if (heap_a == NULL || heap_a->alloc == NULL || heap_a->free == NULL ||
        work_a == NULL || work_a->alloc == NULL || work_a->free == NULL) {
        return er_bad;
    }

    enum {
        ER_EVAL_DSTACK_S = 65536,
        ER_EVAL_KSTACK_S = 262144,
    };

    er_val* dstack_v = work_a->alloc(work_a->ctx, ER_EVAL_DSTACK_S * sizeof(er_val));
    er_kon* kstack_v = work_a->alloc(work_a->ctx, ER_EVAL_KSTACK_S * sizeof(er_kon));
    if (dstack_v == NULL || kstack_v == NULL) {
        if (dstack_v != NULL) {
            work_a->free(work_a->ctx, dstack_v);
        }
        if (kstack_v != NULL) {
            work_a->free(work_a->ctx, kstack_v);
        }
        return er_bad;
    }

    er_vm vm = {
        .code = NULL,
        .loc_a = heap_a,
        .dstack = dstack_v,
        .dsp = dstack_v,
        .kbase = kstack_v,
        .ksp = kstack_v,
        .b_count = 0,
        .k_count = 0,
        .gc_rp = NULL,
        .gc_tmp_s = 0,
    };
    void* old_root = NULL;
    enki_gc_trace_fn old_trace = NULL;
    if (gc != NULL) {
        old_root = gc->trace_root;
        old_trace = gc->trace_fn;
        enki_gc_set_trace_root(gc, &vm, enki_gc_trace_vm);
    }
    er_val out_v = plan_eval(&vm, val_v, mode);
    ENKI_PROFILE_PLOT_I("er_eval.bytecode_steps", (int64_t)vm.b_count);
    ENKI_PROFILE_PLOT_I("er_eval.reductions", (int64_t)vm.k_count);
    if (gc != NULL) {
        enki_gc_set_trace_root(gc, old_root, old_trace);
    }
    work_a->free(work_a->ctx, kstack_v);
    work_a->free(work_a->ctx, dstack_v);
    return out_v;
}

er_val er_eval(const enki_allocator* loc_a, er_val val_v)
{
    return er_eval_with_heap(loc_a, loc_a, NULL, val_v, ER_EVAL_WHNF);
}

er_val er_eval_to(const enki_allocator* loc_a, er_val val_v, er_eval_mode mode)
{
    return er_eval_with_heap(loc_a, loc_a, NULL, val_v, mode);
}

er_val er_eval_gc(enki_gc* gc, er_val val_v)
{
    if (gc == NULL) {
        return er_bad;
    }
    return er_eval_with_heap(enki_gc_as_allocator(gc), enki_gc_parent_allocator(gc), gc, val_v,
                             ER_EVAL_WHNF);
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
    er_thk* thk = er_thk_alloc(loc_a, siz_s + 1);
    if (thk == NULL) {
        return 0;
    }
    er_val out_v = er_thk_init(thk, ER_XUNK_APP, siz_s + 1, NULL);
    if (out_v == 0) {
        return 0;
    }
    thk->arg_v[0] = fn_v;
    if (siz_s > 0) {
        memcpy(thk->arg_v + 1, &old->arg_v[dop_s + 1], siz_s * sizeof(er_val));
    }
    return out_v;
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
        return 0;
    }
    if (law->ari_d != arity_d || law->ari_d == UINT32_MAX ||
        law->let_d > UINT32_MAX - law->ari_d - 1u) {
        return 0;
    }
    return (size_t)law->ari_d + 1 + law->let_d;
}

static bool er_callable_arity(er_val val_v, uint32_t* out_d)
{
  er_pin* pin;
  er_app* app;
  er_law* law;
  switch ( er_get_tag(val_v) ) {
    case er_tag_pin:
      pin = er_outa(val_v);
      if (er_is_cat(pin->val_v)) {
        *out_d = 1;
        return true;
      }
      law = er_outt(er_tag_law, pin->val_v);
      if (law == NULL) {
        return false;
      }
      *out_d = law->ari_d;
      return true;
    case er_tag_app: {
      app = er_outa(val_v);
      uint32_t fun_ari_d = 0;
      if (!er_callable_arity(app->fn_v, &fun_ari_d) || fun_ari_d <= app->arg_s) {
        return false;
      }
      *out_d = fun_ari_d - (uint32_t)app->arg_s;
      return true;
    }
    case er_tag_law:
      law = er_outa(val_v);
      *out_d = law->ari_d;
      return true;
    default:
      return false;
  }
}

static uint32_t er_arity(er_val val_v)
{
  er_law* law = er_outt(er_tag_law, val_v);
  return law == NULL ? 0 : law->ari_d;
}

static er_val plan_eval_nf_inner(er_vm* vm, er_val val_v);
static er_val plan_eval_whnf_preserve(er_vm* vm, er_val val_v);

/*
 * (define (OP_PUSH_VAR val) ("OP_PUSH_VAR" val)) ;; [] -- a
(define OP_PUSH_SELF ("OP_PUSH_SELF" 0)) ;; [] -- a
(define (OP_PUSH_LIT val) ("OP_PUSH_LIT" val)) ;; [] -- a
(define (OP_MK_APP count) ("OP_MK_APP" count)) ;; [<count>] -- a
(define (OP_CALLF argc) ("OP_CALLF" argc)) ;; [fn, args...] -- thunk
(define (OP_CALLU argc) ("OP_CALLU" argc)) ;; [fn, args...] -- thunk
(define (OP_EVAL ari) ("OP_EVAL" ari)) ;; a -- whnf
(define OP_TAIL_EVAL ("OP_TAIL_EVAL" 0)) ;; a -- whnf
(define OP_FORCE ("OP_FORCE" 0)) ;; a -- nf
(define (OP_PRIM_UNK set) ("OP_PRIM_UNK" set)) ;; a -- a
(define OP_RET ("OP_RET" 0))
(define (OP_DROP idx) ("OP_DROP" ari)) ;; [a b] -- [a]
(define (OP_ROTATE n) ("OP_ROTATE" n)) ;; [a b c] -- [b c a]

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
static er_val
plan_eval_whnf(er_vm *vm, er_val val_v, er_eval_mode mode)
{
    ENKI_PROFILE_ZONE("plan_eval_whnf");
    er_op* code  = vm->code;
    er_val code_law_v = vm->code_law_v;
    uint32_t code_label_d = vm->code_label_d;

    er_val* dbase = vm->dsp;
    er_val* dsp   = vm->dsp;
    er_kon* kbase = vm->ksp;
    er_kon* ksp   = vm->ksp;

    uint32_t  pc  = 0;
    er_val hd_v = er_bad;
    er_val* env = NULL;
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
    er_val prim_a_v = 0;
    er_val prim_b_v = 0;
    er_val prim_c_v = 0;
    er_val prim_d_v = 0;
    er_val prim_e_v = 0;
    er_val prim_f_v = 0;
    er_optag prim_byte_op;
    size_t prim_need_s;
    er_bc_prim_route prim_route;
    er_op prim_route_code_v[ER_PRIM_ROUTE_MAX_DEPTH][ER_PRIM_ROUTE_MAX_CODE];
    size_t prim_route_final_pc_v[ER_PRIM_ROUTE_MAX_DEPTH] = {0};
    size_t prim_route_depth_s = 0;
    er_op app_route_code_v[2];

    er_op *op;
    er_val f, app, target;
    er_head* head;
    er_kon kon;
    uint32_t split;
    size_t idx_s;

#define ER_DISPATCH_ENTRY(_tag, _label, _value) [_tag] = &&_label,
    static void *const dispatch[OP_COUNT] = {
        /* Core VM/control opcodes, including private fast paths. */
        ER_CORE_OPS(ER_DISPATCH_ENTRY)

        /* Primitive leaves keep one dispatch label per bytecode tag. */
        ER_PRIM_LEAF_OPS(ER_DISPATCH_ENTRY)

        /* Return/control opcodes. */
        ER_RETURN_CONTROL_OPS(ER_DISPATCH_ENTRY)
    };
#undef ER_DISPATCH_ENTRY

#define DPUSH(_r)      do { *dsp++ = (_r); } while (0)
#define DPOP()         (*--dsp)

#define GC_SYNC()                                     \
    do {                                              \
        vm->dsp = dsp;                                \
        vm->ksp = ksp;                                \
        vm->code_law_v = code_law_v;                  \
        vm->code_label_d = code_label_d;              \
    } while (0)

#define GC_ROOT_PRIMS()                               \
    do {                                              \
        vm->gc_tmp_v[0] = prim_a_v;                   \
        vm->gc_tmp_v[1] = prim_b_v;                   \
        vm->gc_tmp_v[2] = prim_c_v;                   \
        vm->gc_tmp_v[3] = prim_d_v;                   \
        vm->gc_tmp_v[4] = prim_e_v;                   \
        vm->gc_tmp_v[5] = prim_f_v;                   \
        vm->gc_tmp_s = 6;                             \
    } while (0)

#define GC_CLEAR_ROOTS()                              \
    do {                                              \
        vm->gc_tmp_s = 0;                             \
    } while (0)

#define CODE_SET(_law_v, _label_d)                 \
    do {                                           \
        code_law_v = (_law_v);                     \
        code_label_d = (uint32_t)(_label_d);       \
        vm->code_law_v = code_law_v;               \
        vm->code_label_d = code_label_d;           \
    } while (0)

#define CODE_REFRESH()                             \
    do {                                           \
        code_law_v = vm->code_law_v;               \
        code_label_d = vm->code_label_d;           \
        if (code_law_v != 0) {                     \
            er_law* dispatch_law = er_resolve_law(code_law_v); \
            code = er_law_label_code(dispatch_law, code_label_d); \
        } else {                                   \
            code = vm->code;                       \
        }                                          \
        if (code == NULL) {                        \
            FAIL_TANK("missing bytecode", code_law_v); \
        }                                          \
    } while (0)

#define DISPATCH()                                 \
    do {                                           \
        vm->b_count++;                             \
        CODE_REFRESH();                            \
        op = &code[pc++];                          \
        if ((size_t)op->tag >= OP_COUNT || dispatch[op->tag] == NULL) { \
            FAIL_TANK("bad bytecode op", (er_val)op->tag); \
        }                                          \
        goto *dispatch[op->tag];                   \
    } while (0)

#define KPUSH_BYTECODE_RETURN(_pc, _env, _law_v, _label_d) \
    do {                                                   \
        vm->k_count++;                                     \
        *ksp++ = (er_kon){                                 \
            .tag = ER_K_BYTECODE_RETURN,                   \
            .as.bytecode_return = {                        \
                .env = (_env),                             \
                .pc = (_pc),                               \
                .code_law_v = (_law_v),                    \
                .code_label_d = (uint32_t)(_label_d),      \
                .code = code,                              \
                .dbase = dbase,                            \
            },                                             \
        };                                                 \
    } while (0)

#define KPUSH_UPDATE(_target)                              \
    do {                                                   \
        *ksp++ = (er_kon){                                 \
            .tag = ER_K_UPDATE,                            \
            .as.update = {                                 \
                .target_v = (_target),                     \
                .dbase = dbase,                            \
            },                                             \
        };                                                 \
    } while (0)

#define KPUSH_APPHEAD(_app)                                \
    do {                                                   \
        *ksp++ = (er_kon){                                 \
            .tag = ER_K_APPHEAD,                           \
            .as.apphead = {                                \
                .app_v = (_app),                           \
            },                                             \
        };                                                 \
    } while (0)

#define KPUSH_APP_IDX(_app, _idx)                          \
    do {                                                   \
        *ksp++ = (er_kon){                                 \
            .tag = ER_K_APP_IDX,                           \
            .as.appidx = {                                 \
                .app_v = (_app),                           \
                .idx_s = (_idx),                           \
            },                                             \
        };                                                 \
    } while (0)

#define KPUSH_OVERAPP(_app, _split)                        \
    do {                                                   \
        *ksp++ = (er_kon){                                 \
            .tag = ER_K_OVERAPP,                           \
            .as.overapp = {                                \
                .app_v = (_app),                           \
                .split_d = (uint32_t)(_split),             \
            },                                             \
        };                                                 \
    } while (0)

#define KPUSH_NORMAL()                                     \
    do {                                                   \
        *ksp++ = (er_kon){.tag = ER_K_NORMAL};             \
    } while (0)

#define RETURN(_r)                                 \
    do {                                           \
        r = (_r);                                  \
        if (!er_is_good(r)) {                      \
            vm->dsp = dsp;                         \
            vm->ksp = ksp;                         \
            return r;                              \
        }                                          \
        if (ksp == kbase) {                        \
            ENKI_PROFILE_PLOT_I("plan_eval.bytecode_steps", (int64_t)vm->b_count); \
            ENKI_PROFILE_PLOT_I("plan_eval.reductions", (int64_t)vm->k_count); \
            return r;                              \
        }                                          \
        kon = *--ksp;                              \
        switch (kon.tag) {                         \
        case ER_K_BYTECODE_RETURN:                 \
            goto K_RETURN;                         \
        case ER_K_UPDATE:                          \
            goto K_UPDATE;                         \
        case ER_K_APPHEAD:                         \
            goto K_APPHEAD;                        \
        case ER_K_APP_IDX:                         \
            goto K_APP_IDX;                        \
        case ER_K_OVERAPP:                         \
            goto K_OVERAPP;                        \
        case ER_K_NORMAL:                          \
            goto K_NORMAL;                         \
        default:                                   \
            assert("bad continuation" && 0);       \
            vm->dsp = dsp;                         \
            vm->ksp = ksp;                         \
            return er_bad;                         \
        }                                          \
    } while (0)

#define FAIL_ALLOC()                               \
    do {                                           \
        vm->dsp = dsp;                             \
        vm->ksp = ksp;                             \
        return er_bad;                             \
    } while (0)

#define FAIL_TANK(_msg, _val)                      \
    do {                                           \
        GC_SYNC();                                 \
        vm->gc_tmp_v[0] = (_val);                  \
        vm->gc_tmp_s = 1;                          \
        er_val tank_v = er_tank_make(vm->loc_a, vm->gc_tmp_v[0], (_msg)); \
        vm->gc_tmp_s = 0;                          \
        return tank_v;                             \
    } while (0)

#define CHECK_ALLOC(_v)                            \
    do {                                           \
        if ((_v) == 0) {                           \
            FAIL_ALLOC();                          \
        }                                          \
        code_law_v = vm->code_law_v;               \
        code_label_d = vm->code_label_d;           \
    } while (0)

#define CHECK_PRIM(_v)                             \
    do {                                           \
        if (!er_is_good(_v)) {                     \
            vm->dsp = dsp;                         \
            vm->ksp = ksp;                         \
            return (_v);                           \
        }                                          \
        code_law_v = vm->code_law_v;               \
        code_label_d = vm->code_label_d;           \
    } while (0)

#define PRIM_FORCE_VALUE_WHNF(_dst, _src)          \
    do {                                           \
        GC_SYNC();                                 \
        (_dst) = plan_eval_whnf_preserve(vm, (_src)); \
        CHECK_PRIM(_dst);                          \
    } while (0)

#define PRIM_DONE_VALUE(_v)                        \
    do {                                           \
        ENKI_PROFILE_ZONE_BEGIN(enki_primop_zone, "plan_eval.primop_exec"); \
        GC_SYNC();                                 \
        GC_ROOT_PRIMS();                           \
        er_val prim_res_v = (_v);                  \
        GC_CLEAR_ROOTS();                          \
        ENKI_PROFILE_ZONE_END(enki_primop_zone);   \
        CHECK_PRIM(prim_res_v);                    \
        DPUSH(prim_res_v);                         \
        goto PRIM_DONE;                            \
    } while (0)

#define PRIM_BAD_ARITY()                           \
    do {                                           \
        FAIL_TANK("bad primitive arity", prim_arg); \
    } while (0)

#define PRIM_SELECT_ROUTE(_route)                  \
    do {                                           \
        prim_route = (_route);                     \
        prim_byte_op = prim_route.tag;             \
        prim_need_s = prim_route.arg_s;            \
        goto PRIM_ROUTE_DISPATCH;                  \
    } while (0)

#define PRIM_SELECT(_tag, _arg_s)                  \
    do {                                           \
        if (!er_bc_prim_route_strict((_tag), (size_t)(_arg_s), &prim_route)) { \
            FAIL_TANK("bad primitive route", prim_arg); \
        }                                          \
        PRIM_SELECT_ROUTE(prim_route);             \
    } while (0)

#define CALLU_DISPATCH_MK_APP()                    \
    do {                                           \
        if (thk->arg_s == 0 || thk->arg_s > UINT32_MAX) { \
            FAIL_ALLOC();                          \
        }                                          \
        for (size_t app_k_s = 0; app_k_s < thk->arg_s; app_k_s++) { \
            DPUSH(thk->arg_v[app_k_s]);            \
        }                                          \
        app_route_code_v[0] = (er_op){             \
            .tag = OP_MK_APP,                      \
            .as.u32 = (uint32_t)thk->arg_s,        \
        };                                         \
        app_route_code_v[1] = (er_op){.tag = OP_RET}; \
        code = app_route_code_v;                   \
        vm->code = code;                           \
        code_law_v = 0;                            \
        code_label_d = 0;                          \
        vm->code_law_v = 0;                        \
        vm->code_label_d = 0;                      \
        pc = 0;                                    \
        DISPATCH();                                \
    } while (0)

    /*
     * Entry: eval root by forcing it.
     */
    r = val_v;
    vm->gc_rp = &r;
    vm->gc_tmp_s = 0;
    if (mode == ER_EVAL_NF) {
        KPUSH_NORMAL();
    }
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
    if (app_s == 0 || dsp < dbase || (size_t)(dsp - dbase) < app_s) {
      FAIL_ALLOC();
    }
    er_val* app_base = dsp - app_s;
    hd_v = app_base[0];
    GC_SYNC();
    r = er_app_make_flat(vm->loc_a, hd_v, app_s - 1, &app_base[1]);
    CHECK_ALLOC(r);
    dsp = app_base;
    DPUSH(r);
    DISPATCH();
}

I_CALLF: {
    size_t arg_s = op->as.u32;
    size_t call_s = arg_s + 1;
    if (arg_s == SIZE_MAX || dsp < dbase || (size_t)(dsp - dbase) < call_s) {
      FAIL_ALLOC();
    }
    er_val* call_base = dsp - call_s;
    hd_v = call_base[0];
    size_t frame_s = er_call_frame_size(hd_v, (uint32_t)arg_s);
#ifndef NDEBUG
    assert(er_is_whnf(hd_v));
    assert(frame_s != 0);
#endif
    if (!er_is_whnf(hd_v) || frame_s == 0) {
      FAIL_TANK("bad fast call", hd_v);
    }
    GC_SYNC();
    r = er_thk_make_call_frame(vm->loc_a, frame_s, call_s, call_base);
    CHECK_ALLOC(r);
    dsp = call_base;
    DPUSH(r);
    DISPATCH();
}

I_CALLU: {
    size_t arg_s = op->as.u32;
    size_t call_s = arg_s + 1;
    if (arg_s == SIZE_MAX || dsp < dbase || (size_t)(dsp - dbase) < call_s) {
      FAIL_ALLOC();
    }
    er_val* call_base = dsp - call_s;
    GC_SYNC();
    r = er_thk_make_unk_app(vm->loc_a, call_s, call_base);
    CHECK_ALLOC(r);
    dsp = call_base;
    DPUSH(r);
    DISPATCH();
}

I_CALLP: {
    size_t call_s = 2;
    if (dsp < dbase || (size_t)(dsp - dbase) < call_s) {
      FAIL_ALLOC();
    }
    er_val* call_base = dsp - call_s;
    hd_v = call_base[0];
    er_pin* pin = er_outt(er_tag_pin, hd_v);
    if (pin == NULL || !er_is_cat(pin->val_v)) {
      FAIL_TANK("bad primitive call", hd_v);
    }
    GC_SYNC();
    r = er_thk_make_prim(vm->loc_a, pin->val_v, call_base[1]);
    CHECK_ALLOC(r);
    dsp = call_base;
    DPUSH(r);
    DISPATCH();
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

#include "run_prim_leaf.inc"

I_RET:
    r = DPOP();
    RETURN(r);

I_DROP:
    (void)DPOP();
    DISPATCH();

I_ROTATE: {
    size_t rotate_s = op->as.u32;
    if (dsp < dbase || rotate_s > (size_t)(dsp - dbase)) {
      FAIL_ALLOC();
    }
    if (rotate_s > 1) {
      er_val* base_v = dsp - rotate_s;
      er_val first_v = base_v[0];
      memmove(base_v, base_v + 1, (rotate_s - 1u) * sizeof(er_val));
      base_v[rotate_s - 1u] = first_v;
    }
    DISPATCH();
}

I_JUMP_IF_ZERO:
    r = DPOP();
    if (r == 0) {
      er_law* jump_law = er_resolve_law(code_law_v);
      if (jump_law != NULL && er_law_label_code(jump_law, op->as.u32) != NULL) {
        CODE_SET(code_law_v, op->as.u32);
        pc = 0;
      } else {
        pc = op->as.u32;
      }
    }
    DISPATCH();

I_JUMP_IF:
    r = DPOP();
    if (r != 0) {
      er_law* jump_law = er_resolve_law(code_law_v);
      if (jump_law == NULL || er_law_label_code(jump_law, op->as.u32) == NULL) {
        FAIL_ALLOC();
      }
      CODE_SET(code_law_v, op->as.u32);
      pc = 0;
    }
    DISPATCH();

I_EVAL:
    r = DPOP();
    GC_SYNC();
    KPUSH_BYTECODE_RETURN(pc, env, code_law_v, code_label_d);
    goto FORCE_ENTRY;

I_TAIL_EVAL:
    r = DPOP();
    GC_SYNC();
    goto FORCE_ENTRY;

I_FORCE:
    r = DPOP();
    GC_SYNC();
    KPUSH_BYTECODE_RETURN(pc, env, code_law_v, code_label_d);
    KPUSH_NORMAL();
    goto FORCE_ENTRY;

    // ---------------------------------------------------------------------
    // Force mode
    // ------------------------------------------------------------------

FORCE_UNK_APP: {
    if (thk->arg_s == 0) {
      FAIL_TANK("bad unknown application", er_into(er_tag_thk, thk));
    }
    f = thk->arg_v[0];
    if ( !er_is_whnf(f) ) {
      thk->fun = ER_HOLE;
      KPUSH_APPHEAD(r);
      r = f;
      goto FORCE_ENTRY;
    }
    size_t hav_s = thk->arg_s - 1u;
    if (hav_s > UINT32_MAX) {
      FAIL_ALLOC();
    }
    hav_d = (uint32_t)hav_s;

    er_pin* pin = er_outt(er_tag_pin, f);
    if (pin != NULL && er_is_cat(pin->val_v)) {
      if (hav_s == 0) {
        CALLU_DISPATCH_MK_APP();
      }
      if (hav_s > 1) {
        KPUSH_OVERAPP(er_into(er_tag_thk, thk), 1);
      }
      GC_SYNC();
      r = er_thk_make_prim(vm->loc_a, pin->val_v, thk->arg_v[1]);
      CHECK_ALLOC(r);
      goto FORCE_ENTRY;
    }

    if (er_is_tag(er_tag_app, f)) {
      GC_SYNC();
      r = er_thk_make_unk_app_flat(vm->loc_a, f, hav_s, &thk->arg_v[1]);
      CHECK_ALLOC(r);
      goto FORCE_ENTRY;
    }

    if (!er_callable_arity(f, &wan_d)) {
      if (hav_s == 0) {
        RETURN(f);
      }
      CALLU_DISPATCH_MK_APP();
    }
    if ( hav_d ==  wan_d ) {
      size_t frame_s = er_call_frame_size(f, wan_d);
      if (frame_s == 0) {
        FAIL_TANK("bad call frame", f);
      }
      if (frame_s == thk->arg_s) {
        thk->fun = ER_CALL;
        goto FORCE_ENTRY;
      }
      GC_SYNC();
      r = er_thk_take_call(vm->loc_a, thk, frame_s);
      CHECK_ALLOC(r);
      goto FORCE_ENTRY;
    } else if ( hav_d < wan_d ) {
      CALLU_DISPATCH_MK_APP();
    } else {
      split = wan_d;
      size_t frame_s = er_call_frame_size(f, wan_d);
      if (frame_s == 0) {
        FAIL_TANK("bad call frame", f);
      }
      GC_SYNC();
      r = er_thk_make_call_frame(vm->loc_a, frame_s, (size_t)wan_d + 1, thk->arg_v);
      CHECK_ALLOC(r);
      KPUSH_OVERAPP(er_into(er_tag_thk, thk), split);
      goto FORCE_ENTRY;
    }
}

FORCE_XPRIM: {
    if (thk->arg_s != 2 || !er_is_cat(thk->arg_v[0])) {
      FAIL_TANK("bad primitive thunk", er_into(er_tag_thk, thk));
    }
    prim_set = thk->arg_v[0];
    prim_arg = thk->arg_v[1];
    if (!er_is_whnf(prim_arg)) {
      GC_SYNC();
      prim_arg = plan_eval_whnf_preserve(vm, prim_arg);
      dsp = vm->dsp;
      ksp = vm->ksp;
      CHECK_PRIM(prim_arg);
    }
    goto PRIMOP;
}


FORCE_ENTRY: {
    if (!er_is_good(r)) {
      vm->dsp = dsp;
      vm->ksp = ksp;
      return r;
    }
    if( er_is_whnf(r) ) {
      RETURN(r);
    }
    thk = er_outt(er_tag_thk, r);
    if (thk == NULL) {
      FAIL_TANK("expected thunk", r);
    }
    switch(thk->fun) {
      case ER_XDONE:
        RETURN(thk->arg_v[0]);
      case ER_XUNK_APP:
        goto FORCE_UNK_APP;
      case ER_CALL:
        goto ENTER_CALL;
      case ER_XPRIM:
        goto FORCE_XPRIM;
      case ER_SUSP:
        goto FORCE_SUSP;
      case ER_HOLE:
        FAIL_TANK("thunk hole", er_into(er_tag_thk, thk));
      default:
        assert("bad thk tag" && 0);
    }
}

#include "run_primop.inc"

    // ---------------------------------------------------------------------
    // Saturated application entry
    // ---------------------------------------------------------------------

FORCE_SUSP: {
    uint32_t susp_label = (uint32_t)thk->arg_v[0];
    er_thk* fr = er_outt(er_tag_thk, thk->arg_v[1]);
    if (fr == NULL) {
      FAIL_TANK("bad suspension frame", thk->arg_v[1]);
    }
    er_val susp_law_v = thk->arg_s >= 3 ? thk->arg_v[2] : fr->arg_v[0];
    er_law* law = er_resolve_law(susp_law_v);
    if (er_law_label_code(law, susp_label) == NULL) {
      FAIL_TANK("bad suspension label", susp_law_v);
    }
    KPUSH_UPDATE(er_into(er_tag_thk, thk));
    CODE_SET(susp_law_v, susp_label);
    pc = 0;
    env = fr->arg_v;
    dbase = dsp;
    thk->fun = ER_HOLE;
    DISPATCH();
}

ENTER_CALL: {
    er_val self_v = er_into(er_tag_thk, thk);
    f = thk->arg_v[0];
    er_law* law = er_resolve_law(f);
    if (law == NULL) {
      FAIL_TANK("bad call frame", self_v);
    }
    size_t frame_s = er_call_frame_size(f, law->ari_d);
    if (frame_s == 0 || thk->arg_s != frame_s || er_law_label_code(law, 0) == NULL) {
      FAIL_TANK("bad call frame", self_v);
    }
    size_t n_lets = er_law_n_lets(law);

    for (size_t i = 0; i < n_lets; i++) {
      size_t slot_s = (size_t)law->ari_d + 1 + i;
      GC_SYNC();
      er_val susp_v = er_thk_make_susp(vm->loc_a, (uint32_t)i + 1, self_v, f);
      CHECK_ALLOC(susp_v);
      thk->arg_v[slot_s] = susp_v;
    }
    if (n_lets > 0) {
      GC_SYNC();
      er_val env_v = er_thk_make_env_frame(vm->loc_a, thk->arg_s, thk->arg_v);
      CHECK_ALLOC(env_v);
      for (size_t i = 0; i < n_lets; i++) {
        size_t slot_s = (size_t)law->ari_d + 1 + i;
        er_thk* susp = er_outt(er_tag_thk, thk->arg_v[slot_s]);
        if (susp == NULL || susp->fun != ER_SUSP || susp->arg_s < 2) {
          FAIL_TANK("bad suspension", thk->arg_v[slot_s]);
        }
        susp->arg_v[1] = env_v;
      }
    }
    KPUSH_UPDATE(self_v);
    thk->fun = ER_HOLE;
    CODE_SET(f, 0);
    pc  = 0;
    env = thk->arg_v;
    dbase = dsp;
    DISPATCH();
}

    // ---------------------------------------------------------------------
    // Continuation handlers
    // ---------------------------------------------------------------------

K_RETURN:
    dbase = kon.as.bytecode_return.dbase;
    code = kon.as.bytecode_return.code;
    vm->code = code;
    code_law_v = kon.as.bytecode_return.code_law_v;
    code_label_d = kon.as.bytecode_return.code_label_d;
    vm->code_law_v = code_law_v;
    vm->code_label_d = code_label_d;
    pc = kon.as.bytecode_return.pc;
    env = kon.as.bytecode_return.env;

    DPUSH(r);
    DISPATCH();

K_UPDATE:
    dbase = kon.as.update.dbase;
    target = kon.as.update.target_v;
    if (!er_is_whnf(r)) {
      KPUSH_UPDATE(target);
      goto FORCE_ENTRY;
    }

    /*
     * Update by indirection, not shallow copy.
     */
    thk = er_outt(er_tag_thk, target);
    if (thk == NULL) {
      FAIL_TANK("bad update target", target);
    }
    thk->fun = ER_XDONE;
    thk->arg_v[0] = r;
    RETURN(r);

K_APP_IDX:
    app = kon.as.appidx.app_v;
    idx_s = kon.as.appidx.idx_s;
    prim_row = er_outt(er_tag_app, app);
    if (prim_row == NULL) {
      FAIL_TANK("bad app", app);
    }
    if (idx_s == 0) {
      prim_row->fn_v = r;
    } else {
      idx_s--;
      if (idx_s >= prim_row->arg_s) {
        FAIL_TANK("bad app index", app);
      }
      prim_row->arg_v[idx_s] = r;
    }
    RETURN(app);

K_NORMAL:
    if (er_is_nf(r)) {
      RETURN(r);
    }
    if (!er_is_tag(er_tag_app, r)) {
      head = er_outa(r);
      head->raw.nf_f = 1;
      RETURN(r);
    }

    prim_row = er_outt(er_tag_app, r);
    if (prim_row == NULL) {
      FAIL_TANK("bad app", r);
    }
    if (!er_is_nf(prim_row->fn_v)) {
      KPUSH_NORMAL();
      KPUSH_APP_IDX(r, 0);
      KPUSH_NORMAL();
      r = prim_row->fn_v;
      goto FORCE_ENTRY;
    }
    for (idx_s = 0; idx_s < prim_row->arg_s; idx_s++) {
      if (!er_is_nf(prim_row->arg_v[idx_s])) {
        KPUSH_NORMAL();
        KPUSH_APP_IDX(r, idx_s + 1);
        KPUSH_NORMAL();
        r = prim_row->arg_v[idx_s];
        goto FORCE_ENTRY;
      }
    }
    prim_row->h.raw.nf_f = 1;
    RETURN(r);

K_APPHEAD:
    app = kon.as.apphead.app_v;
    thk = er_outt(er_tag_thk, app);
    if ( !thk ) {
      FAIL_TANK("bad app head", app);
    }
    assert(thk->fun == ER_HOLE);
    if (thk->fun != ER_HOLE) {
      FAIL_TANK("bad app head", app);
    }
    thk->fun = ER_XUNK_APP;
    thk->arg_v[0] = r;
    r = er_into(er_tag_thk, thk);
    goto FORCE_ENTRY;

K_OVERAPP:
    split = kon.as.overapp.split_d;
    app = kon.as.overapp.app_v;

    {
      thk = er_outt(er_tag_thk, app);
      assert("bad overapp" && thk);
      GC_SYNC();
      r = er_app_drop_coup(vm->loc_a, thk, r, split);
      CHECK_ALLOC(r);
        goto FORCE_ENTRY;
    }

#undef GC_CLEAR_ROOTS
#undef GC_ROOT_PRIMS
#undef GC_SYNC
#undef CHECK_ALLOC
#undef FAIL_TANK
#undef PRIM_SELECT_ROUTE
#undef PRIM_SELECT
#undef CALLU_DISPATCH_MK_APP
#undef PRIM_BAD_ARITY
#undef PRIM_DONE_VALUE
#undef PRIM_FORCE_VALUE_WHNF
#undef CHECK_PRIM
#undef FAIL_ALLOC
#undef RETURN
#undef KPUSH_NORMAL
#undef KPUSH_OVERAPP
#undef KPUSH_APP_IDX
#undef KPUSH_APPHEAD
#undef KPUSH_UPDATE
#undef KPUSH_BYTECODE_RETURN
#undef DISPATCH
#undef CODE_REFRESH
#undef CODE_SET
#undef DPOP
#undef DPUSH
}

static er_val plan_eval_preserve(er_vm* vm, er_val val_v, er_eval_mode mode)
{
    er_val* base_dsp = vm->dsp;
    er_kon* base_ksp = vm->ksp;
    er_val* saved_gc_rp = vm->gc_rp;
    size_t saved_gc_tmp_s = vm->gc_tmp_s;
    er_op* saved_code = vm->code;
    er_val saved_code_law_v = vm->code_law_v;
    uint32_t saved_code_label_d = vm->code_label_d;
    bool root_code_law_f = saved_code_law_v != 0;
    if (root_code_law_f) {
        *vm->ksp++ = (er_kon){
            .tag = ER_K_VALUE_ROOT,
            .as.value_root = {
                .val_v = saved_code_law_v,
            },
        };
    }

    er_val out_v = plan_eval_whnf(vm, val_v, mode);

    if (root_code_law_f) {
        saved_code_law_v = base_ksp->as.value_root.val_v;
    }
    vm->dsp = base_dsp;
    vm->ksp = base_ksp;
    vm->gc_rp = saved_gc_rp;
    vm->gc_tmp_s = saved_gc_tmp_s;
    vm->code = saved_code;
    vm->code_law_v = saved_code_law_v;
    vm->code_label_d = saved_code_label_d;
    return out_v;
}

static er_val plan_eval_whnf_preserve(er_vm* vm, er_val val_v)
{
    return plan_eval_preserve(vm, val_v, ER_EVAL_WHNF);
}

static er_val plan_eval_nf_inner(er_vm* vm, er_val val_v)
{
    return plan_eval_preserve(vm, val_v, ER_EVAL_NF);
}

er_val plan_eval(er_vm* vm, er_val val_v, er_eval_mode mode)
{
    ENKI_PROFILE_ZONE("plan_eval");
    if (mode == ER_EVAL_NF) {
        return plan_eval_nf_inner(vm, val_v);
    }
    return plan_eval_whnf_preserve(vm, val_v);
}
