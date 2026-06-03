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

enum {
    ER_PRIM_ROUTE_MAX_CODE = 64,
    ER_PRIM_ROUTE_MAX_DEPTH = 64,
};

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

static er_val er_thk_make_partial_call_frame(const enki_allocator* loc_a, er_app* app,
                                             size_t extra_s, const er_val extra_v[])
{
    if (app == NULL || app->arg_s > SIZE_MAX - extra_s) {
        return 0;
    }
    er_val flat_fn_v = app->fn_v;
    size_t total_arg_s = 0;
    if (!er_app_spine_count(app->fn_v, app->arg_s + extra_s, &flat_fn_v, &total_arg_s) ||
        total_arg_s > UINT32_MAX) {
        return 0;
    }
    uint32_t call_arity_d = (uint32_t)total_arg_s;
    size_t frame_s = er_call_frame_size(flat_fn_v, call_arity_d);
    if (frame_s == 0 || frame_s < (size_t)call_arity_d + 1u) {
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
    thk->arg_v[0] = flat_fn_v;
    size_t copied_s = 0;
    er_app_spine_copy(app->fn_v, thk->arg_v + 1, &copied_s);
    if (app->arg_s > 0) {
        memcpy(thk->arg_v + 1 + copied_s, app->arg_v, app->arg_s * sizeof(er_val));
        copied_s += app->arg_s;
    }
    if (extra_s > 0) {
        memcpy(thk->arg_v + 1 + copied_s, extra_v, extra_s * sizeof(er_val));
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
        return (size_t)arity_d + 1;
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
(define (OP_CALLF ari) ("OP_CALLF" ari)) ;; [<count>] -- a
(define (OP_CALLU ari) ("OP_CALLU" ari)) ;; [<count>] -- a
(define (OP_EVAL ari) ("OP_EVAL" ari)) ;; a -- whnf
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
plan_eval_whnf(er_vm *vm, er_val val_v)
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

    er_op *op;
    er_val f, app, target;
    uint32_t split;

    static void *const dispatch[OP_COUNT] = {
        [OP_PUSH_VAR]  = &&I_PUSH_VAR,
        [OP_PUSH_LIT]  = &&I_PUSH_LIT,
        [OP_MK_APP]    = &&I_MK_APP,
        [OP_MK_CALL]   = &&I_MK_CALL,
        [OP_CALLF]     = &&I_CALLF,
        [OP_CALLU]     = &&I_CALLU,
        [OP_PUSH_SELF] = &&I_PUSH_SELF,
        [OP_EVAL]      = &&I_EVAL,
        [OP_FORCE]     = &&I_FORCE,
        [OP_DROP]      = &&I_DROP,
        [OP_ROTATE]    = &&I_ROTATE,
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

#define KPUSH_RETURN(_pc, _env, _law_v, _label_d)  \
    do {                                           \
        vm->k_count++;                             \
        (ksp++)->ref = (_env);                     \
        (ksp++)->pc  = (_pc);                      \
        (ksp++)->u = (uintptr_t)(_label_d);        \
        (ksp++)->val_v = (_law_v);                 \
        (ksp++)->code = code;                      \
        (ksp++)->ref = dbase;                      \
        (ksp++)->lab = &&K_RETURN;                 \
    } while (0)

#define KPUSH_UPDATE(_target)                      \
    do {                                           \
        (ksp++)->val_v = (_target);                  \
        (ksp++)->ref = dbase;                      \
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
        void *dst = (--ksp)->lab;                  \
        goto *dst;                                 \
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

    /*
     * Entry: eval root by forcing it.
     */
    r = val_v;
    vm->gc_rp = &r;
    vm->gc_tmp_s = 0;
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
    r = er_app_make(vm->loc_a, hd_v, app_s - 1, &app_base[1]);
    CHECK_ALLOC(r);
    dsp = app_base;
    DPUSH(r);
    DISPATCH();
}

I_MK_CALL: {
    size_t app_s = op->as.u32;
    if (app_s == 0 || dsp < dbase || (size_t)(dsp - dbase) < app_s) {
      FAIL_ALLOC();
    }
    er_val* app_base = dsp - app_s;
    GC_SYNC();
    r = er_thk_make_unk_app(vm->loc_a, app_s, app_base);
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
    size_t frame_s = er_call_frame_size(call_base[0], (uint32_t)arg_s);
    if (frame_s == 0) {
      FAIL_TANK("bad call frame", call_base[0]);
    }
    GC_SYNC();
    r = er_thk_make_call_frame(vm->loc_a, frame_s, call_s, call_base);
    CHECK_ALLOC(r);
    dsp = call_base;
    KPUSH_RETURN(pc, env, code_law_v, code_label_d);
    goto FORCE_ENTRY;
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
    KPUSH_RETURN(pc, env, code_law_v, code_label_d);
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
    prim_word_v = op->as.lit_v;
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_loadn(vm->loc_a, prim_word_v, prim_a_v, prim_b_v));

I_OP_LOAD:
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_load(vm->loc_a, prim_a_v, prim_b_v, prim_c_v));

I_OP_STOREN:
    prim_word_v = op->as.lit_v;
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_storen(vm->loc_a, prim_word_v, prim_a_v, prim_b_v, prim_c_v));

I_OP_STORE:
    prim_d_v = DPOP();
    prim_c_v = DPOP();
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    /* Stack order is idx, width, value, n; eo_store wants idx, value, width, n. */
    PRIM_DONE_VALUE(eo_store(vm->loc_a, prim_a_v, prim_c_v, prim_b_v, prim_d_v));

I_OP_TRUNCN:
    prim_word_v = op->as.lit_v;
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_truncn(vm->loc_a, prim_word_v, prim_a_v));

I_OP_TRUNC:
    prim_b_v = DPOP();
    prim_a_v = DPOP();
    PRIM_DONE_VALUE(eo_trunc(vm->loc_a, prim_a_v, prim_b_v));

I_OP_MET:
    prim_word_v = op->as.lit_v;
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

    /*
     * Optional fast path: if r is already WHNF, avoid pushing K_RETURN.
     * Only push a continuation if evaluation actually enters something.
     */

    if (er_is_whnf(r)) {
        DPUSH(r);
        DISPATCH();
    }

    GC_SYNC();
    KPUSH_RETURN(pc, env, code_law_v, code_label_d);
    goto FORCE_ENTRY;

I_FORCE:
    r = DPOP();
    GC_SYNC();
    r = plan_eval_nf_inner(vm, r);
    dsp = vm->dsp;
    ksp = vm->ksp;
    CHECK_PRIM(r);
    DPUSH(r);
    DISPATCH();

    // ---------------------------------------------------------------------
    // Force mode
    // ------------------------------------------------------------------

FORCE_UNK_APP: {
    f = thk->arg_v[0];
    if ( !er_is_whnf(f) ) {
      thk->fun = ER_HOLE;
      KPUSH_APPHEAD(r);
      r = f;
      goto FORCE_ENTRY;
    }
    hav_d = (uint32_t)(thk->arg_s - 1);
	    if (!er_callable_arity(f, &wan_d)) {
	      if (hav_d == 0) {
	        RETURN(f);
	      }
	      GC_SYNC();
	      r = er_app_make_flat(vm->loc_a, f, thk->arg_s - 1, &thk->arg_v[1]);
	      CHECK_ALLOC(r);
	      goto FORCE_ENTRY;
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
      GC_SYNC();
      r = er_app_make_flat(vm->loc_a, f, thk->arg_s - 1, &thk->arg_v[1]);
      CHECK_ALLOC(r);
      goto FORCE_ENTRY;
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
      case ER_SUSP:
        goto FORCE_SUSP;
      case ER_HOLE:
        FAIL_TANK("thunk hole", er_into(er_tag_thk, thk));
      default:
        assert("bad thk tag" && 0);
    }
}

PRIMOP: {
    if (prim_set == 66) {
        goto PRIM66_DECODE;
    }
    if (prim_set == 0) {
        goto PRIM0_DECODE;
    }
    FAIL_TANK("bad primitive set", prim_arg);
}

PRIM0_DECODE: {
    prim_row = er_outt(er_tag_app, prim_arg);
    if (prim_row == NULL) {
        FAIL_TANK("expected primitive row", prim_arg);
    }
    prim_tag_v = prim_row->fn_v;
    prim_arg_v = prim_row->arg_v;
    prim_arg_s = prim_row->arg_s;
    if (prim_tag_v == 0 && prim_arg_s > 0 && er_is_cat(prim_arg_v[0]) &&
        prim_arg_v[0] <= (er_val)OP0_ELIM) {
        prim_tag_v = prim_arg_v[0];
        prim_arg_v = prim_row->arg_v + 1;
        prim_arg_s = prim_row->arg_s - 1;
    }
    if (!er_is_cat(prim_tag_v) || prim_tag_v > (er_val)OP0_ELIM) {
        FAIL_TANK("bad primitive tag", prim_tag_v);
    }
    prim_op_s = (size_t)prim_tag_v;
    if (!er_bc_prim0_route(prim_op_s, &prim_route)) {
        FAIL_TANK("bad primitive route", prim_tag_v);
    }
    PRIM_SELECT_ROUTE(prim_route);
}

PRIM66_DECODE: {
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
            FAIL_TANK("bad primitive descriptor", prim_tag_v);
        }
        prim_name_v = prim_desc->fn_v;
        prim_word_v = prim_desc->arg_v[0];
        if (!er_prim66_op_from_descriptor(prim_name_v, &prim_op_i) || prim_op_i < 0 ||
            (size_t)prim_op_i >= ER_OP66_COUNT) {
            FAIL_TANK("bad primitive descriptor", prim_tag_v);
        }
        if (!er_bc_prim66_route(prim_op_i, &prim_route)) {
            FAIL_TANK("bad primitive route", prim_tag_v);
        }
        switch (prim_op_i) {
        case OP66_LOAD:
            PRIM_SELECT(prim_word_v == 0 ? OP_LOAD : OP_LOADN, prim_word_v == 0 ? 3 : 2);
        case ER_OP66_STORE:
            PRIM_SELECT(prim_word_v == 0 ? OP_STORE : OP_STOREN, prim_word_v == 0 ? 4 : 3);
        case OP66_TRUNC:
            PRIM_SELECT(prim_word_v == 0 ? OP_TRUNC : OP_TRUNCN, prim_word_v == 0 ? 2 : 1);
        case ER_OP66_MET:
            PRIM_SELECT(OP_MET, 1);
        default:
            PRIM_SELECT_ROUTE(prim_route);
        }
    }
    if (!eo_op66_from_tag(prim_tag_v, &prim_op_i) || prim_op_i < 0 ||
        (size_t)prim_op_i >= ER_OP66_COUNT) {
        FAIL_TANK("bad primitive tag", prim_tag_v);
    }
    switch (prim_op_i) {
    case OP66_TYPE:
        if (prim_arg_s != 1) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        switch (er_get_tag(prim_a_v)) {
        case er_tag_bat:
            PRIM_DONE_VALUE(0);
        case er_tag_pin:
            PRIM_DONE_VALUE(1);
        case er_tag_law:
            PRIM_DONE_VALUE(2);
        case er_tag_app:
            PRIM_DONE_VALUE(3);
        default:
            if (er_is_cat(prim_a_v)) {
                PRIM_DONE_VALUE(0);
            }
            FAIL_TANK("bad value tag", prim_a_v);
        }
    case OP66_IS_PIN:
    case OP66_IS_LAW:
    case OP66_IS_APP:
    case OP66_IS_NAT:
        if (prim_arg_s != 1) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        if (prim_op_i == OP66_IS_PIN) {
            PRIM_DONE_VALUE(er_is_tag(er_tag_pin, prim_a_v) ? 1 : 0);
        }
        if (prim_op_i == OP66_IS_LAW) {
            PRIM_DONE_VALUE(er_is_tag(er_tag_law, prim_a_v) ? 1 : 0);
        }
        if (prim_op_i == OP66_IS_APP) {
            PRIM_DONE_VALUE(er_is_tag(er_tag_app, prim_a_v) ? 1 : 0);
        }
        PRIM_DONE_VALUE((er_is_cat(prim_a_v) || er_is_tag(er_tag_bat, prim_a_v)) ? 1 : 0);
    case OP66_NE:
    case OP66_LT:
    case OP66_GT:
    case OP66_GE:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        if (prim_op_i == OP66_NE) {
            PRIM_DONE_VALUE(eo_eq(prim_a_v, prim_b_v) == 0 ? 1 : 0);
        }
        prim_c_v = eo_cmp(prim_a_v, prim_b_v);
        if (prim_op_i == OP66_LT) {
            PRIM_DONE_VALUE(prim_c_v == 0 ? 1 : 0);
        }
        if (prim_op_i == OP66_GT) {
            PRIM_DONE_VALUE(prim_c_v == 2 ? 1 : 0);
        }
        PRIM_DONE_VALUE(prim_c_v == 0 ? 0 : 1);
    case OP66_SET:
    case OP66_CLEAR:
    case OP66_NIB:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        if (prim_op_i == OP66_NIB) {
            PRIM_DONE_VALUE(eo_load(vm->loc_a, prim_a_v, 4, prim_b_v));
        }
        PRIM_DONE_VALUE(eo_store(vm->loc_a, prim_a_v, prim_op_i == OP66_SET ? 1 : 0, 1,
                                 prim_b_v));
    case OP66_CASE: {
        if (prim_arg_s != 3) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        size_t case_idx_s = 0;
        er_app* case_row = er_outt(er_tag_app, prim_b_v);
        if (!eo_nat_to_size(prim_a_v, &case_idx_s) || case_row == NULL ||
            case_idx_s >= case_row->arg_s) {
            r = prim_arg_v[2];
        } else {
            r = case_row->arg_v[case_idx_s];
        }
        goto FORCE_ENTRY;
    }
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
    case OP66_CASE16: {
        size_t case_s = (size_t)(prim_op_i - OP66_CASE2 + 2);
        if (prim_arg_s != case_s + 1) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        size_t case_idx_s = 0;
        if (!eo_nat_to_size(prim_a_v, &case_idx_s) || case_idx_s >= case_s - 1) {
            r = prim_arg_v[case_s];
        } else {
            r = prim_arg_v[case_idx_s + 1];
        }
        goto FORCE_ENTRY;
    }
    case OP66_IX0:
    case OP66_IX1:
    case OP66_IX2:
    case OP66_IX3:
    case OP66_IX4:
    case OP66_IX5:
    case OP66_IX6:
    case OP66_IX7:
        if (prim_arg_s != 1) {
            PRIM_BAD_ARITY();
        }
        prim_a_v = (er_val)(prim_op_i - OP66_IX0);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[0]);
        PRIM_DONE_VALUE(eo_ix(prim_a_v, prim_b_v));
    case OP66_IF:
    case OP66_IFZ:
        if (prim_arg_s != 3) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        r = (prim_op_i == OP66_IF ? prim_a_v != 0 : prim_a_v == 0) ? prim_arg_v[1]
                                                                    : prim_arg_v[2];
        goto FORCE_ENTRY;
    case OP66_NOR:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        if (prim_a_v != 0) {
            PRIM_DONE_VALUE(0);
        }
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        PRIM_DONE_VALUE(prim_b_v == 0 ? 1 : 0);
    case OP66_ROW:
        if (prim_arg_s != 3) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        prim_a_v = eo_nat(prim_a_v);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        prim_c_v = prim_arg_v[2];
        {
            size_t row_count_s = 0;
            (void)eo_nat_to_size(prim_b_v, &row_count_s);
            if (row_count_s == 0) {
                PRIM_DONE_VALUE(prim_a_v);
            }

            GC_SYNC();
            GC_ROOT_PRIMS();
            er_app* row_app = er_app_alloc(vm->loc_a, row_count_s);
            prim_a_v = vm->gc_tmp_v[0];
            prim_b_v = vm->gc_tmp_v[1];
            prim_c_v = vm->gc_tmp_v[2];
            GC_CLEAR_ROOTS();
            if (row_app == NULL) {
                FAIL_ALLOC();
            }

            er_val row_v = er_app_init(row_app, prim_a_v, row_count_s, NULL);
            CHECK_ALLOC(row_v);
            er_val* row_root_p = dsp;
            DPUSH(row_v);

            for (size_t row_k_s = 0; row_k_s < row_count_s; row_k_s++) {
                PRIM_FORCE_VALUE_WHNF(prim_c_v, prim_c_v);
                row_app = er_outt(er_tag_app, *row_root_p);
                if (row_app == NULL) {
                    FAIL_ALLOC();
                }
                row_app->arg_v[row_k_s] = eo_ix(0, prim_c_v);
                prim_c_v = eo_ix(1, prim_c_v);
            }

            r = DPOP();
            goto FORCE_ENTRY;
        }
    case OP66_TRACE:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        fprintf(stderr, "trace: %s\n", enki_pvalue(vm->loc_a, prim_arg_v[0]));
        r = prim_arg_v[1];
        goto FORCE_ENTRY;
    case OP66_SEQ:
        if (prim_arg_s != 2) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        r = prim_arg_v[1];
        goto FORCE_ENTRY;
    case OP66_SEQ2:
        if (prim_arg_s != 3) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        r = prim_arg_v[2];
        goto FORCE_ENTRY;
    case OP66_SEQ3:
        if (prim_arg_s != 4) {
            PRIM_BAD_ARITY();
        }
        PRIM_FORCE_VALUE_WHNF(prim_a_v, prim_arg_v[0]);
        PRIM_FORCE_VALUE_WHNF(prim_b_v, prim_arg_v[1]);
        PRIM_FORCE_VALUE_WHNF(prim_c_v, prim_arg_v[2]);
        r = prim_arg_v[3];
        goto FORCE_ENTRY;
    case OP66_THROW:
        if (prim_arg_s != 1) {
            PRIM_BAD_ARITY();
        }
        FAIL_TANK("throw", prim_arg_v[0]);
    default:
        break;
    }
    if (!er_bc_prim66_route(prim_op_i, &prim_route)) {
        FAIL_TANK("bad primitive route", prim_tag_v);
    }
    PRIM_SELECT_ROUTE(prim_route);
}

PRIM_ROUTE_DISPATCH: {
    if (prim_byte_op >= OP_COUNT || dispatch[prim_byte_op] == NULL) {
        FAIL_TANK("bad primitive route", prim_tag_v);
    }
    if (prim_arg_s != prim_need_s || prim_need_s > ER_BC_MAX_PRIM_ARITY ||
        prim_route_depth_s >= ER_PRIM_ROUTE_MAX_DEPTH) {
        PRIM_BAD_ARITY();
    }
    for (size_t prim_k_s = 0; prim_k_s < prim_need_s; prim_k_s++) {
        DPUSH(prim_arg_v[prim_k_s]);
    }

    size_t prim_route_slot_s = prim_route_depth_s++;
    er_op* prim_code_v = prim_route_code_v[prim_route_slot_s];
    size_t prim_code_s = 0;
    er_val prim_lit_v = 0;
    switch (prim_byte_op) {
    case OP_LOADN:
    case OP_STOREN:
    case OP_TRUNCN:
    case OP_MET:
        prim_lit_v = prim_word_v;
        break;
    default:
        break;
    }
    if (!er_bc_emit_prim_route_fragment(prim_code_v, ER_PRIM_ROUTE_MAX_CODE, prim_byte_op,
                                        prim_need_s, prim_route.arg_eval_v, prim_lit_v,
                                        &prim_code_s)) {
        FAIL_TANK("bad primitive route", prim_tag_v);
    }
    prim_route_final_pc_v[prim_route_slot_s] = prim_code_s;

    code = prim_code_v;
    vm->code = code;
    code_law_v = 0;
    code_label_d = 0;
    vm->code_law_v = 0;
    vm->code_label_d = 0;
    pc = 0;
    DISPATCH();
}

PRIM_DONE: {
    bool prim_route_done_f =
        prim_route_depth_s > 0 &&
        code == prim_route_code_v[prim_route_depth_s - 1u] &&
        pc == prim_route_final_pc_v[prim_route_depth_s - 1u];
    if (prim_route_done_f) {
      prim_route_depth_s--;
      r = DPOP();
      goto FORCE_ENTRY;
    }
    DISPATCH();
}

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
    er_pin* pin;
    er_val self_v = er_into(er_tag_thk, thk);
    f = thk->arg_v[0];
    if ( er_is_tag(er_tag_pin, f) ) {
      pin = er_outa(f);
      if ( er_is_cat(pin->val_v) ) {
        prim_set = pin->val_v;
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
      f = pin->val_v;
    }
    er_law* law = er_outt(er_tag_law, f);
    if (law == NULL) {
      er_app* part = er_outt(er_tag_app, f);
      if (part != NULL) {
        GC_SYNC();
        r = er_thk_make_partial_call_frame(vm->loc_a, part, thk->arg_s - 1,
                                           &thk->arg_v[1]);
        CHECK_ALLOC(r);
        goto FORCE_ENTRY;
      }
      if (thk->arg_s <= 1) {
        RETURN(thk->arg_v[0]);
      }
      GC_SYNC();
      r = er_app_make_flat(vm->loc_a, thk->arg_v[0], thk->arg_s - 1, &thk->arg_v[1]);
      CHECK_ALLOC(r);
      goto FORCE_ENTRY;
    }
    size_t frame_s = (size_t)law->ari_d + 1 + law->let_d;
    if (thk->arg_s != frame_s || er_law_label_code(law, 0) == NULL) {
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
    /*
     * Payload layout:
     *
     *   [ env ][ pc ][ label ][ law ][ code ][ dbase ][ &&K_RETURN ]
     *
     * The label has already been popped by RETURN.
     */
    dbase = (--ksp)->ref;
    code = (--ksp)->code;
    vm->code = code;
    code_law_v = (--ksp)->val_v;
    code_label_d = (uint32_t)(--ksp)->u;
    vm->code_law_v = code_law_v;
    vm->code_label_d = code_label_d;
    pc  = (--ksp)->pc;
    env = (--ksp)->ref;

    DPUSH(r);
    DISPATCH();

K_UPDATE:
    /*
     * Payload layout:
     *
     *   [ target ][ dbase ][ &&K_UPDATE ]
     */
    dbase = (--ksp)->ref;
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
      FAIL_TANK("bad update target", target);
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
#undef PRIM_BAD_ARITY
#undef PRIM_DONE_VALUE
#undef PRIM_FORCE_VALUE_WHNF
#undef CHECK_PRIM
#undef FAIL_ALLOC
#undef RETURN
#undef KPUSH_OVERAPP
#undef KPUSH_APPHEAD
#undef KPUSH_UPDATE
#undef KPUSH_RETURN
#undef DISPATCH
#undef CODE_REFRESH
#undef CODE_SET
#undef DPOP
#undef DPUSH
}

static er_val plan_eval_whnf_preserve(er_vm* vm, er_val val_v)
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
        (vm->ksp++)->val_v = saved_code_law_v;
    }

    er_val out_v = plan_eval_whnf(vm, val_v);

    if (root_code_law_f) {
        saved_code_law_v = base_ksp->val_v;
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

static er_val plan_eval_nf_inner(er_vm* vm, er_val val_v)
{
    er_val* base_dsp = vm->dsp;
    er_val root_v = plan_eval_whnf_preserve(vm, val_v);
    if (!er_is_good(root_v) || er_is_cat(root_v)) {
        return root_v;
    }

    switch (er_get_tag(root_v)) {
    case er_tag_bat:
    case er_tag_pin:
    case er_tag_law:
    case er_tag_app:
        break;
    default:
        return er_tank_make(vm->loc_a, root_v, "bad value tag");
    }

    er_head* h = er_outa(root_v);
    if (h->raw.nf_f) {
        return root_v;
    }

    er_val* root_p = vm->dsp++;
    *root_p = root_v;

    switch (er_get_tag(root_v)) {
    case er_tag_bat:
        h = er_outa(*root_p);
        h->raw.nf_f = 1;
        break;

    case er_tag_pin: {
        er_pin* pin = er_outt(er_tag_pin, *root_p);
        if (pin == NULL) {
            root_v = er_tank_make(vm->loc_a, *root_p, "bad pin");
            break;
        }

        er_val child_v = plan_eval_nf_inner(vm, pin->val_v);
        if (!er_is_good(child_v)) {
            root_v = child_v;
            break;
        }

        pin = er_outt(er_tag_pin, *root_p);
        if (pin == NULL) {
            root_v = er_tank_make(vm->loc_a, *root_p, "bad pin");
            break;
        }
        pin->val_v = child_v;

        for (size_t k = 0; k < pin->sub_s; k++) {
            child_v = plan_eval_nf_inner(vm, pin->sub_v[k]);
            if (!er_is_good(child_v)) {
                root_v = child_v;
                break;
            }
            pin = er_outt(er_tag_pin, *root_p);
            if (pin == NULL) {
                root_v = er_tank_make(vm->loc_a, *root_p, "bad pin");
                break;
            }
            pin->sub_v[k] = child_v;
        }
        if (!er_is_good(root_v)) {
            break;
        }

        h = er_outa(*root_p);
        h->raw.nf_f = 1;
        break;
    }

    case er_tag_law: {
        er_law* law = er_outt(er_tag_law, *root_p);
        if (law == NULL) {
            root_v = er_tank_make(vm->loc_a, *root_p, "bad law");
            break;
        }

        er_val child_v = plan_eval_nf_inner(vm, law->name_v);
        if (!er_is_good(child_v)) {
            root_v = child_v;
            break;
        }

        law = er_outt(er_tag_law, *root_p);
        if (law == NULL) {
            root_v = er_tank_make(vm->loc_a, *root_p, "bad law");
            break;
        }
        law->name_v = child_v;

        child_v = plan_eval_nf_inner(vm, law->body_v);
        if (!er_is_good(child_v)) {
            root_v = child_v;
            break;
        }

        law = er_outt(er_tag_law, *root_p);
        if (law == NULL) {
            root_v = er_tank_make(vm->loc_a, *root_p, "bad law");
            break;
        }
        law->body_v = child_v;

        h = er_outa(*root_p);
        h->raw.nf_f = 1;
        break;
    }

    case er_tag_app: {
        er_app* app = er_outt(er_tag_app, *root_p);
        if (app == NULL) {
            root_v = er_tank_make(vm->loc_a, *root_p, "bad app");
            break;
        }

        er_val child_v = plan_eval_nf_inner(vm, app->fn_v);
        if (!er_is_good(child_v)) {
            root_v = child_v;
            break;
        }

        app = er_outt(er_tag_app, *root_p);
        if (app == NULL) {
            root_v = er_tank_make(vm->loc_a, *root_p, "bad app");
            break;
        }
        app->fn_v = child_v;

        for (size_t k = 0; k < app->arg_s; k++) {
            child_v = plan_eval_nf_inner(vm, app->arg_v[k]);
            if (!er_is_good(child_v)) {
                root_v = child_v;
                break;
            }
            app = er_outt(er_tag_app, *root_p);
            if (app == NULL) {
                root_v = er_tank_make(vm->loc_a, *root_p, "bad app");
                break;
            }
            app->arg_v[k] = child_v;
        }
        if (!er_is_good(root_v)) {
            break;
        }

        h = er_outa(*root_p);
        h->raw.nf_f = 1;
        break;
    }

    default:
        root_v = er_tank_make(vm->loc_a, root_v, "bad value tag");
        break;
    }

    if (er_is_good(root_v)) {
        root_v = *root_p;
    }
    vm->dsp = base_dsp;
    return root_v;
}

er_val plan_eval(er_vm* vm, er_val val_v, er_eval_mode mode)
{
    ENKI_PROFILE_ZONE("plan_eval");
    if (mode == ER_EVAL_NF) {
        return plan_eval_nf_inner(vm, val_v);
    }
    return plan_eval_whnf_preserve(vm, val_v);
}
