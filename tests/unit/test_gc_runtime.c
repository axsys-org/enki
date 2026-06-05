#include "enki/gc.h"

#include <criterion/criterion.h>
#include <stdint.h>

typedef struct er_gc_roots {
  er_val val_v[8];
  size_t val_s;
} er_gc_roots;

static void trace_er_roots(enki_gc* gc, void* root_p) {
  er_gc_roots* roots = root_p;
  for (size_t k = 0; k < roots->val_s; k++) {
    roots->val_v[k] = enki_gc_copy(gc, roots->val_v[k]);
  }
}

static er_val gc_make_bat(const enki_allocator* loc_a, size_t lim_s,
                          const uint64_t lim_q[]) {
  er_bat* bat = er_bat_alloc(loc_a, lim_s);
  cr_assert_not_null(bat);
  er_val bat_v = er_bat_init(bat, lim_s, lim_q);
  cr_assert_eq(er_get_tag(bat_v), er_tag_bat);
  return bat_v;
}

static er_val gc_make_app(const enki_allocator* loc_a, er_val fn_v,
                          size_t arg_s, const er_val arg_v[]) {
  er_app* app = er_app_alloc(loc_a, arg_s);
  cr_assert_not_null(app);
  er_val app_v = er_app_init(app, fn_v, arg_s, arg_v);
  cr_assert_eq(er_get_tag(app_v), er_tag_app);
  return app_v;
}

static er_val gc_make_thunk(const enki_allocator* loc_a, er_execf fun,
                            size_t arg_s, const er_val arg_v[]) {
  er_thk* thk = er_thk_alloc(loc_a, arg_s);
  cr_assert_not_null(thk);
  er_val thk_v = er_thk_init(thk, fun, arg_s, arg_v);
  cr_assert_eq(er_get_tag(thk_v), er_tag_thk);
  return thk_v;
}

Test(gc_runtime, collect_moves_and_traces_er_runtime_tags) {
  enki_gc* gc = enki_gc_create(enki_allocator_system(), 8192, NULL);
  cr_assert_not_null(gc);
  const enki_allocator* loc_a = enki_gc_as_allocator(gc);

  uint64_t limbs_q[] = {UINT64_C(0x0123456789abcdef),
                        UINT64_C(0xfedcba9876543210)};
  er_val bat_v = gc_make_bat(loc_a, 2, limbs_q);
  er_bat* old_bat = er_outt(er_tag_bat, bat_v);

  er_op ret_op[] = {{.tag = OP_RET}};
  er_op* label_v[] = {ret_op};
  size_t label_len_v[] = {1};
  er_val law_v =
      er_law_make_code(loc_a, 77, bat_v, 0, 0, 1, label_v, label_len_v);
  cr_assert_eq(er_get_tag(law_v), er_tag_law);
  er_law* old_law = er_outt(er_tag_law, law_v);

  er_val sub_v[] = {bat_v};
  er_pin* pin = er_pin_alloc(loc_a, 1);
  cr_assert_not_null(pin);
  er_val pin_v = er_pin_init(pin, NULL, law_v, 1, sub_v);
  cr_assert_eq(er_get_tag(pin_v), er_tag_pin);
  er_pin* old_pin = er_outt(er_tag_pin, pin_v);

  er_val app_arg_v[] = {law_v, bat_v};
  er_val app_v = gc_make_app(loc_a, pin_v, 2, app_arg_v);
  er_app* old_app = er_outt(er_tag_app, app_v);

  er_val thunk_v = gc_make_thunk(loc_a, ER_XDONE, 1, &app_v);
  er_thk* old_thk = er_outt(er_tag_thk, thunk_v);

  er_gc_roots roots = {.val_v = {thunk_v}, .val_s = 1};
  enki_gc_set_trace_root(gc, &roots, trace_er_roots);
  enki_gc_collect(gc);

  er_thk* moved_thk = er_outt(er_tag_thk, roots.val_v[0]);
  cr_assert_not_null(moved_thk);
  cr_assert_neq(moved_thk, old_thk);
  cr_assert_eq(moved_thk->fun, ER_XDONE);

  er_app* moved_app = er_outt(er_tag_app, moved_thk->arg_v[0]);
  cr_assert_not_null(moved_app);
  cr_assert_neq(moved_app, old_app);
  cr_assert_eq(moved_app->arg_s, 2);

  er_pin* moved_pin = er_outt(er_tag_pin, moved_app->fn_v);
  cr_assert_not_null(moved_pin);
  cr_assert_neq(moved_pin, old_pin);
  cr_assert_eq(moved_pin->sub_s, 1);

  er_law* moved_law = er_outt(er_tag_law, moved_pin->val_v);
  cr_assert_not_null(moved_law);
  cr_assert_neq(moved_law, old_law);
  cr_assert_eq(moved_law->name_v, 77);
  cr_assert_eq(moved_law->bc_s, 1);
  er_op* moved_code = er_law_label_code(moved_law, 0);
  cr_assert_not_null(moved_code);
  cr_assert_neq(moved_code, ret_op);
  cr_assert_eq(moved_code[0].tag, OP_RET);

  er_bat* moved_bat = er_outt(er_tag_bat, moved_law->body_v);
  cr_assert_not_null(moved_bat);
  cr_assert_neq(moved_bat, old_bat);
  cr_assert_eq(moved_bat->lim_s, 2);
  cr_assert_eq(moved_bat->lim_q[0], limbs_q[0]);
  cr_assert_eq(moved_bat->lim_q[1], limbs_q[1]);
  cr_assert_eq(moved_pin->sub_v[0], moved_law->body_v);
  cr_assert_eq(moved_app->arg_v[0], moved_pin->val_v);
  cr_assert_eq(moved_app->arg_v[1], moved_law->body_v);

  enki_gc_destroy(gc);
}

Test(gc_runtime, allocator_collection_preserves_registered_root) {
  enki_gc* gc = enki_gc_create(enki_allocator_system(), 1024, NULL);
  cr_assert_not_null(gc);
  const enki_allocator* loc_a = enki_gc_as_allocator(gc);

  er_val root_arg_v[] = {11, 22, 33};
  er_gc_roots roots = {
      .val_v = {gc_make_app(loc_a, 99, 3, root_arg_v)},
      .val_s = 1,
  };
  enki_gc_set_trace_root(gc, &roots, trace_er_roots);

  er_app* old_root = er_outt(er_tag_app, roots.val_v[0]);
  for (size_t k = 0; k < 64; k++) {
    er_val arg_v[] = {(er_val)k, (er_val)(k + 1), roots.val_v[0]};
    (void)gc_make_app(loc_a, 0, 3, arg_v);
  }

  er_app* root = er_outt(er_tag_app, roots.val_v[0]);
  cr_assert_not_null(root);
  cr_assert_neq(root, old_root);
  cr_assert_eq(root->fn_v, 99);
  cr_assert_eq(root->arg_s, 3);
  cr_assert_eq(root->arg_v[0], 11);
  cr_assert_eq(root->arg_v[1], 22);
  cr_assert_eq(root->arg_v[2], 33);

  enki_gc_destroy(gc);
}

Test(gc_runtime, law_bytecode_literals_are_traced) {
  enki_gc* gc = enki_gc_create(enki_allocator_system(), 8192, NULL);
  cr_assert_not_null(gc);
  const enki_allocator* loc_a = enki_gc_as_allocator(gc);

  uint64_t limbs_q[] = {UINT64_C(0x1234)};
  er_val bat_v = gc_make_bat(loc_a, 1, limbs_q);
  er_bat* old_bat = er_outt(er_tag_bat, bat_v);
  cr_assert_not_null(old_bat);

  er_op code[] = {
      [0] = {.tag = OP_PUSH_LIT, .as.lit_v = bat_v},
      [1] = {.tag = OP_RET},
  };
  er_op* label_v[] = {code};
  size_t label_len_v[] = {2};
  er_val law_v = er_law_make_code(loc_a, 0, 0, 0, 0, 1, label_v, label_len_v);
  cr_assert_eq(er_get_tag(law_v), er_tag_law);

  er_gc_roots roots = {.val_v = {law_v}, .val_s = 1};
  enki_gc_set_trace_root(gc, &roots, trace_er_roots);
  enki_gc_collect(gc);

  er_law* moved_law = er_outt(er_tag_law, roots.val_v[0]);
  cr_assert_not_null(moved_law);
  er_op* moved_code = er_law_label_code(moved_law, 0);
  cr_assert_not_null(moved_code);
  er_bat* moved_bat = er_outt(er_tag_bat, moved_code[0].as.lit_v);
  cr_assert_not_null(moved_bat);
  cr_assert_neq(moved_bat, old_bat);
  cr_assert_eq(moved_bat->lim_s, 1);
  cr_assert_eq(moved_bat->lim_q[0], limbs_q[0]);

  enki_gc_destroy(gc);
}

Test(gc_runtime, eval_gc_forces_collected_thunk) {
  enki_gc* gc = enki_gc_create(enki_allocator_system(), 4096, NULL);
  cr_assert_not_null(gc);
  const enki_allocator* loc_a = enki_gc_as_allocator(gc);

  er_val answer_v = 1234;
  er_val thunk_v = gc_make_thunk(loc_a, ER_XDONE, 1, &answer_v);
  er_gc_roots roots = {.val_v = {thunk_v}, .val_s = 1};
  enki_gc_set_trace_root(gc, &roots, trace_er_roots);
  enki_gc_collect(gc);

  cr_assert_eq(er_eval_gc(gc, roots.val_v[0]), answer_v);
  enki_gc_destroy(gc);
}

Test(gc_runtime, plan_eval_nf_keeps_parent_rooted_when_child_allocates) {
  enki_gc* gc = enki_gc_create(enki_allocator_system(), 512, NULL);
  cr_assert_not_null(gc);
  const enki_allocator* loc_a = enki_gc_as_allocator(gc);

  er_val max_small_v = UINT64_C(0x7fffffffffffffff);
  er_val row_v = gc_make_app(loc_a, PLAN_S3('I', 'n', 'c'), 1, &max_small_v);
  er_val call_arg_v[] = {66, row_v};
  er_val child_v = gc_make_thunk(loc_a, ER_XPRIM, 2, call_arg_v);
  er_val root_v = gc_make_app(loc_a, 0, 1, &child_v);
  er_app* old_root = er_outt(er_tag_app, root_v);
  cr_assert_not_null(old_root);

  er_gc_roots roots = {.val_v = {root_v}, .val_s = 1};
  enki_gc_set_trace_root(gc, &roots, trace_er_roots);

  size_t align_s = _Alignof(er_head);
  size_t off_s = (gc->active_a->off_o + align_s - 1u) & ~(align_s - 1u);
  size_t bat_s = sizeof(er_bat) + sizeof(uint64_t);
  cr_assert_gt(gc->active_a->cap_s, off_s);
  size_t remaining_s = gc->active_a->cap_s - off_s;
  if (remaining_s >= bat_s) {
    void* fill_p = enki_gc_alloc(gc, remaining_s - bat_s + 1u, align_s);
    cr_assert_not_null(fill_p);
  }

  er_val dstack_v[64] = {0};
  er_kon kstack_v[128] = {0};
  er_vm vm = {
      .pc = ER_BCPC_NONE,
      .loc_a = loc_a,
      .dstack = dstack_v,
      .dsp = dstack_v,
      .kbase = kstack_v,
      .ksp = kstack_v,
  };
  enki_gc_set_trace_root(gc, &vm, enki_gc_trace_vm);

  er_val result_v = plan_eval(&vm, roots.val_v[0], ER_EVAL_NF);

  er_app* root = er_outt(er_tag_app, result_v);
  cr_assert_not_null(root);
  cr_assert_neq(root, old_root);
  cr_assert_eq(root->h.raw.nf_f, 1);
  er_bat* child = er_outt(er_tag_bat, root->arg_v[0]);
  cr_assert_not_null(child);
  cr_assert_eq(child->lim_s, 1);
  cr_assert_eq(child->lim_q[0], UINT64_C(0x8000000000000000));

  enki_gc_destroy(gc);
}

Test(gc_runtime, bytecode_return_pc_survives_collection_during_eval) {
  enki_gc* gc = enki_gc_create(enki_allocator_system(), 2048, NULL);
  cr_assert_not_null(gc);
  const enki_allocator* loc_a = enki_gc_as_allocator(gc);

  er_val max_small_v = UINT64_C(0x7fffffffffffffff);
  er_val row_v = gc_make_app(loc_a, PLAN_S3('I', 'n', 'c'), 1, &max_small_v);
  er_val child_arg_v[] = {66, row_v};
  er_val child_v = gc_make_thunk(loc_a, ER_XPRIM, 2, child_arg_v);
  er_op code[] = {
      [0] = {.tag = OP_PUSH_LIT, .as.lit_v = child_v},
      [1] = {.tag = OP_EVAL},
      [2] = {.tag = OP_DROP},
      [3] = {.tag = OP_PUSH_LIT, .as.lit_v = 42},
      [4] = {.tag = OP_RET},
  };
  er_op* label_v[] = {code};
  size_t label_len_v[] = {5};
  er_val law_v = er_law_make_code(loc_a, 0, 0, 0, 0, 1, label_v, label_len_v);
  cr_assert_eq(er_get_tag(law_v), er_tag_law);
  er_val call_v = gc_make_thunk(loc_a, ER_CALL, 1, &law_v);
  er_gc_roots roots = {.val_v = {call_v}, .val_s = 1};
  enki_gc_set_trace_root(gc, &roots, trace_er_roots);

  size_t align_s = _Alignof(er_head);
  size_t off_s = (gc->active_a->off_o + align_s - 1u) & ~(align_s - 1u);
  size_t bat_s = sizeof(er_bat) + sizeof(uint64_t);
  cr_assert_gt(gc->active_a->cap_s, off_s);
  size_t remaining_s = gc->active_a->cap_s - off_s;
  if (remaining_s >= bat_s) {
    void* fill_p = enki_gc_alloc(gc, remaining_s - bat_s + 1u, align_s);
    cr_assert_not_null(fill_p);
  }

  er_val dstack_v[64] = {0};
  er_kon kstack_v[128] = {0};
  er_vm vm = {
      .pc = ER_BCPC_NONE,
      .loc_a = loc_a,
      .dstack = dstack_v,
      .dsp = dstack_v,
      .kbase = kstack_v,
      .ksp = kstack_v,
  };
  enki_gc_set_trace_root(gc, &vm, enki_gc_trace_vm);

  cr_assert_eq(plan_eval(&vm, roots.val_v[0], ER_EVAL_WHNF), 42);
  enki_gc_destroy(gc);
}

Test(gc_runtime, compiled_law_keeps_inline_bytecode_valid_after_collection) {
  enki_gc* gc = enki_gc_create(enki_allocator_system(), 16384, NULL);
  cr_assert_not_null(gc);
  const enki_allocator* loc_a = enki_gc_as_allocator(gc);

  er_val prim66_v = er_pin_make(loc_a, 66);
  cr_assert_eq(er_get_tag(prim66_v), er_tag_pin);
  er_val add_row_arg_v[] = {10, 32};
  er_val add_row_v =
      gc_make_app(loc_a, PLAN_S3('A', 'd', 'd'), 2, add_row_arg_v);
  er_val body_arg_v[] = {prim66_v, add_row_v};
  er_val body_v = gc_make_app(loc_a, 0, 2, body_arg_v);
  er_val law_v = er_law_make(loc_a, 0, body_v, 0);
  cr_assert_eq(er_get_tag(law_v), er_tag_law);

  er_gc_roots roots = {.val_v = {law_v}, .val_s = 1};
  enki_gc_set_trace_root(gc, &roots, trace_er_roots);
  enki_gc_collect(gc);

  er_val call_v = gc_make_thunk(loc_a, ER_CALL, 1, &roots.val_v[0]);
  cr_assert_eq(er_eval_gc(gc, call_v), 42);
  enki_gc_destroy(gc);
}
