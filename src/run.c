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
#include "enki/perf.h"

#include "run_opcode.def"

enum {
  ER_PRIM_ROUTE_MAX_CODE = 64,
  ER_PRIM_ROUTE_MAX_DEPTH = 64,
};

typedef struct er_bytecode_arena {
  er_op* op_v;
  size_t op_s;
  size_t cap_s;
} er_bytecode_arena;

static er_bytecode_arena er_bc_a;

#define ER_ASSERT_OPCODE_VALUE(_tag, _label, _value)                           \
  static_assert((_tag) == (_value), #_tag " opcode value changed");
ER_ALL_OPS(ER_ASSERT_OPCODE_VALUE)
#undef ER_ASSERT_OPCODE_VALUE

static_assert(OP_COUNT == 73, "OP_COUNT opcode value changed");

static bool er_prim66_op_from_descriptor(er_val tag_v, int* out_op) {
  if (eo_op66_from_tag(tag_v, out_op)) {
    return true;
  }
  if (tag_v == PLAN_S5('O', 'P', '_', 'L', 'E')) {
    *out_op = OP66_LE;
    return true;
  }
  return false;
}

static bool er_alloc_size(size_t base_s, size_t count_s, size_t elem_s,
                          size_t* out_s) {
  if (elem_s != 0 && count_s > (SIZE_MAX - base_s) / elem_s) {
    return false;
  }
  *out_s = base_s + (count_s * elem_s);
  return true;
}

static bool er_bytecode_reserve(size_t need_s) {
  if (need_s <= er_bc_a.cap_s) {
    return true;
  }
  if (need_s > (size_t)ER_BCPC_NONE) {
    return false;
  }
  size_t next_s = er_bc_a.cap_s == 0 ? 1024u : er_bc_a.cap_s;
  while (next_s < need_s) {
    if (next_s > (size_t)ER_BCPC_NONE / 2u) {
      next_s = (size_t)ER_BCPC_NONE;
      break;
    }
    next_s *= 2u;
  }
  if (next_s < need_s) {
    return false;
  }
  const enki_allocator* sys = enki_allocator_system();
  void* next_p = NULL;
  if (sys->realloc != NULL) {
    next_p = sys->realloc(sys->ctx, er_bc_a.op_v, next_s * sizeof(er_op));
  } else {
    next_p = sys->alloc(sys->ctx, next_s * sizeof(er_op));
    if (next_p != NULL && er_bc_a.op_v != NULL && er_bc_a.op_s > 0) {
      memcpy(next_p, er_bc_a.op_v, er_bc_a.op_s * sizeof(er_op));
      sys->free(sys->ctx, er_bc_a.op_v);
    }
  }
  if (next_p == NULL) {
    return false;
  }
  er_bc_a.op_v = next_p;
  er_bc_a.cap_s = next_s;
  return true;
}

static bool er_bytecode_append(er_op* const op_v[], const size_t op_len_v[],
                               size_t label_s, size_t total_s,
                               er_bcpc* out_pc) {
  if (op_v == NULL || op_len_v == NULL || out_pc == NULL || label_s == 0 ||
      total_s == 0 || total_s > (size_t)ER_BCPC_NONE ||
      er_bc_a.op_s > (size_t)ER_BCPC_NONE - total_s) {
    return false;
  }
  size_t start_s = er_bc_a.op_s;
  if (!er_bytecode_reserve(start_s + total_s)) {
    return false;
  }
  size_t off_s = start_s;
  for (size_t k = 0; k < label_s; k++) {
    if (op_v[k] == NULL || op_len_v[k] == 0) {
      return false;
    }
    memcpy(er_bc_a.op_v + off_s, op_v[k], op_len_v[k] * sizeof(er_op));
    off_s += op_len_v[k];
  }
  er_bc_a.op_s = off_s;
  *out_pc = (er_bcpc)start_s;
  return true;
}

er_op* er_bytecode_at(er_bcpc pc) {
  if (pc == ER_BCPC_NONE || (size_t)pc >= er_bc_a.op_s) {
    return NULL;
  }
  return er_bc_a.op_v + (size_t)pc;
}

const er_op* er_bytecode_at_const(er_bcpc pc) {
  if (pc == ER_BCPC_NONE || (size_t)pc >= er_bc_a.op_s) {
    return NULL;
  }
  return er_bc_a.op_v + (size_t)pc;
}

bool er_bytecode_span_valid(er_bcpc pc, size_t op_s) {
  return pc != ER_BCPC_NONE && op_s != 0 && (size_t)pc <= er_bc_a.op_s &&
         op_s <= er_bc_a.op_s - (size_t)pc;
}

size_t er_bytecode_arena_op_count(void) {
  return er_bc_a.op_s;
}

static void* er_alloc_bytes(enki_gc* gc, size_t size_s, size_t align_s) {
  if (gc == NULL || size_s == 0) {
    return NULL;
  }
  return enki_gc_alloc(gc, size_s, align_s);
}

static void er_head_alloc_init(er_head* h, size_t size_s) {
  h->siz_s = size_s;
  h->raw.fwd_f = 0;
  h->raw.nf_f = 0;
}

static size_t er_head_size(const er_head* h) {
  return h->siz_s & ~(size_t)0x3;
}

static bool er_flex_fits(const er_head* h, size_t base_s, size_t count_s,
                         size_t elem_s) {
  size_t need_s = 0;
  return er_alloc_size(base_s, count_s, elem_s, &need_s) &&
         need_s <= er_head_size(h);
}

er_tank* er_tank_alloc(enki_gc* gc) {
  ENKI_PROFILE_ZONE("er_tank_alloc");
  return (er_tank*)er_alloc_bytes(gc, sizeof(er_tank), _Alignof(er_tank));
}

er_val er_tank_init(er_tank* tank, er_val val_v, char* msg_c) {
  if (tank == NULL) {
    return 0;
  }
  tank->val_v = val_v;
  tank->msg_c = msg_c == NULL ? "" : msg_c;
  return er_into(er_tag_tank, tank);
}

er_val er_tank_make(enki_gc* gc, er_val val_v, char* msg_c) {
  er_tank* tank = er_tank_alloc(gc);
  if (tank == NULL) {
    return er_bad;
  }
  er_val out_v = er_tank_init(tank, val_v, msg_c);
  return out_v == 0 ? er_bad : out_v;
}

er_bat* er_bat_alloc(enki_gc* gc, size_t lim_s) {
  ENKI_PROFILE_ZONE("er_bat_alloc");
  size_t size_s = 0;
  if (!er_alloc_size(sizeof(er_bat), lim_s, sizeof(uint64_t), &size_s)) {
    return NULL;
  }
  er_bat* bat = (er_bat*)er_alloc_bytes(gc, size_s, _Alignof(er_bat));
  if (bat == NULL) {
    return NULL;
  }
  er_head_alloc_init(&bat->hed, size_s);
  bat->lim_s = lim_s;
  return bat;
}

er_val er_bat_init(er_bat* bat, size_t lim_s, const uint64_t lim_q[]) {
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

static er_val er_bat_qwords(enki_gc* gc, size_t lim_s, const uint64_t lim_q[]) {
  er_bat* bat = er_bat_alloc(gc, lim_s);
  return er_bat_init(bat, lim_s, lim_q);
}

static er_val er_bat_qword(enki_gc* gc, const uint64_t lim_q) {
  uint64_t lim_qq = lim_q;
  return er_bat_qwords(gc, 1, &lim_qq);
}

er_pin* er_pin_alloc(enki_gc* gc, size_t sub_s) {
  ENKI_PROFILE_ZONE("er_pin_alloc");
  size_t size_s = 0;
  if (!er_alloc_size(sizeof(er_pin), sub_s, sizeof(er_val), &size_s)) {
    return NULL;
  }
  er_pin* pin = (er_pin*)er_alloc_bytes(gc, size_s, _Alignof(er_pin));
  if (pin == NULL) {
    return NULL;
  }
  er_head_alloc_init(&pin->hed, size_s);
  memset(pin->hash_b, 0, sizeof(pin->hash_b));
  pin->val_v = 0;
  pin->sub_s = sub_s;
  return pin;
}

er_val er_pin_init(er_pin* pin, const uint8_t hash_b[32], er_val val_v,
                   size_t sub_s, const er_val sub_v[]) {
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

er_val er_pin_make(enki_gc* gc, er_val val_v) {
  er_pin* pin = er_pin_alloc(gc, 0);
  return er_pin_init(pin, NULL, val_v, 0, NULL);
}

static bool er_law_layout(size_t bc_s, size_t* size_s) {
  return er_alloc_size(sizeof(er_law), bc_s, sizeof(er_law_label), size_s);
}

static bool er_law_total_ops(size_t bc_s, const er_op* const bc_v[],
                             const size_t bc_len_v[], size_t* out_s) {
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

er_law* er_law_alloc(enki_gc* gc, size_t bc_s, size_t op_s) {
  ENKI_PROFILE_ZONE("er_law_alloc");
  size_t size_s = 0;
  if (op_s > (size_t)ER_BCPC_NONE || !er_law_layout(bc_s, &size_s)) {
    return NULL;
  }
  er_law* law = (er_law*)er_alloc_bytes(gc, size_s, _Alignof(er_law));
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
  if (bc_s > 0) {
    memset(law->bc_v, 0, bc_s * sizeof(er_law_label));
  }
  return law;
}

er_val er_law_init(er_law* law, er_val name_v, er_val body_v, uint32_t ari_d,
                   uint32_t let_d, size_t bc_s, er_op* const bc_v[],
                   const size_t bc_len_v[]) {
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
  size_t size_s = 0;
  if (!er_law_layout(bc_s, &size_s) || size_s > er_head_size(&law->h) ||
      law->op_s < op_s || op_s > (size_t)ER_BCPC_NONE) {
    return 0;
  }
  law->name_v = name_v;
  law->body_v = body_v;
  law->ari_d = ari_d;
  law->let_d = let_d;
  law->bc_s = bc_s;
  law->op_s = op_s;
  law->h.raw.nf_f = 1;

  er_bcpc start_pc = ER_BCPC_NONE;
  if (!er_bytecode_append(bc_v, bc_len_v, bc_s, op_s, &start_pc)) {
    return 0;
  }
  er_bcpc pc = start_pc;
  for (size_t k = 0; k < bc_s; k++) {
    law->bc_v[k] = (er_law_label){.pc = pc, .op_s = bc_len_v[k]};
    pc += (er_bcpc)bc_len_v[k];
  }
  for (size_t label_s = 0; label_s < bc_s; label_s++) {
    er_law_label label = law->bc_v[label_s];
    er_op* code_v = er_law_label_code(law, label_s);
    if (code_v == NULL) {
      return 0;
    }
    for (size_t op_s_i = 0; op_s_i < label.op_s; op_s_i++) {
      er_op* op = code_v + op_s_i;
      if (op->tag == OP_JUMP_IF || op->tag == OP_JMP ||
          op->tag == OP_MAKE_SUSP) {
        size_t target_s = (size_t)op->as.u32;
        if (target_s >= bc_s || law->bc_v[target_s].op_s == 0) {
          return 0;
        }
      } else if (op->tag == OP_JUMP_IF_ZERO) {
        size_t target_s = (size_t)op->as.u32;
        if (target_s < bc_s && law->bc_v[target_s].op_s != 0) {
          op->as.u32 = law->bc_v[target_s].pc;
        } else {
          if (target_s >= label.op_s ||
              target_s > (size_t)ER_BCPC_NONE - label.pc) {
            return 0;
          }
          op->as.u32 = label.pc + (er_bcpc)target_s;
        }
      }
    }
  }
  return er_into(er_tag_law, law);
}

er_op* er_law_label_code(er_law* law, size_t label_s) {
  if (law == NULL || label_s >= law->bc_s) {
    return NULL;
  }
  er_law_label label = law->bc_v[label_s];
  if (label.op_s == 0 || !er_bytecode_span_valid(label.pc, label.op_s)) {
    return NULL;
  }
  return er_bytecode_at(label.pc);
}

const er_op* er_law_label_code_const(const er_law* law, size_t label_s) {
  if (law == NULL || label_s >= law->bc_s) {
    return NULL;
  }
  er_law_label label = law->bc_v[label_s];
  if (label.op_s == 0 || !er_bytecode_span_valid(label.pc, label.op_s)) {
    return NULL;
  }
  return er_bytecode_at_const(label.pc);
}

typedef struct er_app_route_cache {
  uint32_t app_s;
  er_bcpc pc;
  er_bcpc end_pc;
} er_app_route_cache;

typedef struct er_prim_route_cache {
  er_optag tag;
  uint32_t arg_s;
  er_val lit_v;
  er_bc_eval_req arg_eval_v[ER_BC_MAX_PRIM_ARITY];
  er_bcpc pc;
  er_bcpc end_pc;
} er_prim_route_cache;

static er_app_route_cache* er_app_route_cache_v;
static size_t er_app_route_cache_s;
static size_t er_app_route_cache_cap_s;
static er_prim_route_cache* er_prim_route_cache_v;
static size_t er_prim_route_cache_s;
static size_t er_prim_route_cache_cap_s;

static bool er_cache_reserve(void** ptr, size_t* cap_s, size_t need_s,
                             size_t elem_s) {
  if (ptr == NULL || cap_s == NULL || elem_s == 0) {
    return false;
  }
  if (need_s <= *cap_s) {
    return true;
  }
  size_t next_s = *cap_s == 0 ? 16u : *cap_s;
  while (next_s < need_s) {
    if (next_s > SIZE_MAX / 2u) {
      return false;
    }
    next_s *= 2u;
  }
  if (next_s > SIZE_MAX / elem_s) {
    return false;
  }
  const enki_allocator* sys = enki_allocator_system();
  void* next_p = sys->realloc != NULL
                     ? sys->realloc(sys->ctx, *ptr, next_s * elem_s)
                     : NULL;
  if (next_p == NULL) {
    next_p = sys->alloc(sys->ctx, next_s * elem_s);
    if (next_p == NULL) {
      return false;
    }
    if (*ptr != NULL && *cap_s != 0) {
      memcpy(next_p, *ptr, *cap_s * elem_s);
      sys->free(sys->ctx, *ptr);
    }
  }
  *ptr = next_p;
  *cap_s = next_s;
  return true;
}

static bool er_bytecode_append_ops(er_op op_v[], size_t op_s, er_bcpc* out_pc,
                                   er_bcpc* out_end_pc) {
  size_t len_v[] = {op_s};
  er_op* code_v[] = {op_v};
  er_bcpc pc = ER_BCPC_NONE;
  if (!er_bytecode_append(code_v, len_v, 1, op_s, &pc) ||
      op_s > (size_t)ER_BCPC_NONE - (size_t)pc) {
    return false;
  }
  *out_pc = pc;
  *out_end_pc = pc + (er_bcpc)op_s;
  return true;
}

static bool er_app_route_intern(uint32_t app_s, er_bcpc* out_pc,
                                er_bcpc* out_end_pc) {
  if (out_pc == NULL || out_end_pc == NULL || app_s == 0) {
    return false;
  }
  for (size_t k = 0; k < er_app_route_cache_s; k++) {
    if (er_app_route_cache_v[k].app_s == app_s) {
      *out_pc = er_app_route_cache_v[k].pc;
      *out_end_pc = er_app_route_cache_v[k].end_pc;
      return true;
    }
  }
  if (!er_cache_reserve((void**)&er_app_route_cache_v,
                        &er_app_route_cache_cap_s, er_app_route_cache_s + 1,
                        sizeof(er_app_route_cache))) {
    return false;
  }
  er_op code_v[] = {
      {.tag = OP_MK_APP, .as.u32 = app_s},
      {.tag = OP_RET},
  };
  er_bcpc pc = ER_BCPC_NONE;
  er_bcpc end_pc = ER_BCPC_NONE;
  if (!er_bytecode_append_ops(code_v, sizeof(code_v) / sizeof(code_v[0]), &pc,
                              &end_pc)) {
    return false;
  }
  er_app_route_cache_v[er_app_route_cache_s++] = (er_app_route_cache){
      .app_s = app_s,
      .pc = pc,
      .end_pc = end_pc,
  };
  *out_pc = pc;
  *out_end_pc = end_pc;
  return true;
}

static bool er_prim_route_matches(const er_prim_route_cache* cached,
                                  er_optag tag, uint32_t arg_s,
                                  const er_bc_eval_req arg_eval_v[],
                                  er_val lit_v) {
  if (cached == NULL || cached->tag != tag || cached->arg_s != arg_s ||
      cached->lit_v != lit_v) {
    return false;
  }
  for (size_t k = 0; k < ER_BC_MAX_PRIM_ARITY; k++) {
    if (cached->arg_eval_v[k] != arg_eval_v[k]) {
      return false;
    }
  }
  return true;
}

static bool er_prim_route_intern(er_optag tag, uint32_t arg_s,
                                 const er_bc_eval_req arg_eval_v[],
                                 er_val lit_v, er_bcpc* out_pc,
                                 er_bcpc* out_end_pc) {
  if (arg_eval_v == NULL || out_pc == NULL || out_end_pc == NULL ||
      arg_s > ER_BC_MAX_PRIM_ARITY) {
    return false;
  }
  for (size_t k = 0; k < er_prim_route_cache_s; k++) {
    if (er_prim_route_matches(&er_prim_route_cache_v[k], tag, arg_s, arg_eval_v,
                              lit_v)) {
      *out_pc = er_prim_route_cache_v[k].pc;
      *out_end_pc = er_prim_route_cache_v[k].end_pc;
      return true;
    }
  }
  if (!er_cache_reserve((void**)&er_prim_route_cache_v,
                        &er_prim_route_cache_cap_s, er_prim_route_cache_s + 1,
                        sizeof(er_prim_route_cache))) {
    return false;
  }
  er_op code_v[ER_PRIM_ROUTE_MAX_CODE] = {0};
  size_t code_s = 0;
  if (!er_bc_emit_prim_route_fragment(code_v, ER_PRIM_ROUTE_MAX_CODE, tag,
                                      arg_s, arg_eval_v, lit_v, &code_s)) {
    return false;
  }
  er_bcpc pc = ER_BCPC_NONE;
  er_bcpc end_pc = ER_BCPC_NONE;
  if (!er_bytecode_append_ops(code_v, code_s, &pc, &end_pc)) {
    return false;
  }
  er_prim_route_cache* cached = &er_prim_route_cache_v[er_prim_route_cache_s++];
  *cached = (er_prim_route_cache){
      .tag = tag,
      .arg_s = arg_s,
      .lit_v = lit_v,
      .pc = pc,
      .end_pc = end_pc,
  };
  for (size_t k = 0; k < ER_BC_MAX_PRIM_ARITY; k++) {
    cached->arg_eval_v[k] = arg_eval_v[k];
  }
  *out_pc = pc;
  *out_end_pc = end_pc;
  return true;
}

er_val er_law_make_code(enki_gc* gc, er_val nam_v, er_val bod_v, uint32_t ari_d,
                        uint32_t let_d, size_t bc_s, er_op* const bc_v[],
                        const size_t bc_len_v[]) {
  size_t op_s = 0;
  if (!er_law_total_ops(bc_s, (const er_op* const*)bc_v, bc_len_v, &op_s)) {
    return 0;
  }
  er_law* law = er_law_alloc(gc, bc_s, op_s);
  if (law == NULL) {
    return 0;
  }
  er_val law_v =
      er_law_init(law, nam_v, bod_v, ari_d, let_d, bc_s, bc_v, bc_len_v);
  return law_v;
}

er_val er_law_make(enki_gc* gc, er_val nam_v, er_val bod_v, uint32_t ari_d) {
  return er_law_compile(gc, nam_v, bod_v, ari_d);
}

er_app* er_app_alloc(enki_gc* gc, size_t arg_s) {
  ENKI_PROFILE_ZONE("er_app_alloc");
  size_t size_s = 0;
  if (!er_alloc_size(sizeof(er_app), arg_s, sizeof(er_val), &size_s)) {
    return NULL;
  }
  er_app* app = (er_app*)er_alloc_bytes(gc, size_s, _Alignof(er_app));
  if (app == NULL) {
    return NULL;
  }
  er_head_alloc_init(&app->h, size_s);
  app->fn_v = 0;
  app->arg_s = arg_s;
  return app;
}

er_val er_app_init(er_app* app, er_val fn_v, size_t arg_s,
                   const er_val arg_v[]) {
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

er_thk* er_thk_alloc(enki_gc* gc, size_t arg_s) {
  ENKI_PROFILE_ZONE("er_thk_alloc");
  size_t siz_s = 0;
  if (!er_alloc_size(sizeof(er_thk), arg_s, sizeof(er_val), &siz_s)) {
    return NULL;
  }
  er_thk* thk = (er_thk*)er_alloc_bytes(gc, siz_s, _Alignof(er_app));
  if (thk == NULL) {
    return NULL;
  }
  er_head_alloc_init(&thk->hed, siz_s);
  thk->fun = ER_XDONE;
  thk->arg_s = arg_s;
  return thk;
}

er_val er_thk_init(er_thk* thk, er_execf fun, size_t arg_s,
                   const er_val arg_v[]) {
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

static er_val er_thk_make_call(enki_gc* gc, size_t arg_s,
                               const er_val arg_v[]) {
  er_thk* thk = er_thk_alloc(gc, arg_s);
  if (thk == NULL) {
    return 0;
  }
  return er_thk_init(thk, ER_CALL, arg_s, arg_v);
}

static size_t er_call_frame_size(er_val fun_v, uint32_t arity_d);

static er_val er_thk_make_prim(enki_gc* gc, er_val prim_set_v,
                               er_val prim_arg_v) {
  er_val arg_v[] = {prim_set_v, prim_arg_v};
  er_thk* thk = er_thk_alloc(gc, 2);
  if (thk == NULL) {
    return 0;
  }
  return er_thk_init(thk, ER_XPRIM, 2, arg_v);
}

static er_val er_thk_make_call_frame(enki_gc* gc, size_t frame_s, size_t copy_s,
                                     const er_val arg_v[]) {
  if (copy_s > frame_s) {
    return 0;
  }
  er_thk* thk = er_thk_alloc(gc, frame_s);
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

static er_val er_thk_make_unk_app_flat(enki_gc* gc, er_val fn_v, size_t arg_s,
                                       const er_val arg_v[]) {
  er_val flat_fn_v = fn_v;
  size_t total_arg_s = 0;
  if (!er_app_spine_count(fn_v, arg_s, &flat_fn_v, &total_arg_s) ||
      total_arg_s > UINT32_MAX) {
    return 0;
  }
  er_thk* thk = er_thk_alloc(gc, total_arg_s + 1u);
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

static er_val er_thk_make_susp(enki_gc* gc, uint32_t pc, er_val frame_v,
                               er_val law_v) {
  er_val arg_v[] = {
      (er_val)pc,
      frame_v,
      law_v,
  };
  er_thk* thk = er_thk_alloc(gc, 3);
  if (thk == NULL) {
    return 0;
  }
  return er_thk_init(thk, ER_SUSP, 3, arg_v);
}

static er_val er_thk_make_env_frame(enki_gc* gc, size_t frame_s,
                                    const er_val frame_v[]) {
  er_thk* thk = er_thk_alloc(gc, frame_s);
  if (thk == NULL) {
    return 0;
  }
  return er_thk_init(thk, ER_XDONE, frame_s, frame_v);
}

static er_val er_thk_make_unk_app(enki_gc* gc, size_t arg_s,
                                  const er_val arg_v[]) {
  er_thk* thk = er_thk_alloc(gc, arg_s);
  if (thk == NULL) {
    return 0;
  }
  return er_thk_init(thk, ER_XUNK_APP, arg_s, arg_v);
}

static er_val er_app_make(enki_gc* gc, er_val fn_v, size_t arg_s,
                          const er_val arg_v[]) {
  er_app* app = er_app_alloc(gc, arg_s);
  if (app == NULL) {
    return 0;
  }
  return er_app_init(app, fn_v, arg_s, arg_v);
}

static bool er_app_spine_count(er_val fn_v, size_t extra_s, er_val* out_fn_v,
                               size_t* out_arg_s) {
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

static void er_app_spine_copy(er_val fn_v, er_val* out_v, size_t* out_s) {
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

static er_val er_app_make_flat(enki_gc* gc, er_val fn_v, size_t arg_s,
                               const er_val arg_v[]) {
  er_val flat_fn_v = fn_v;
  size_t total_s = arg_s;
  if (!er_app_spine_count(fn_v, arg_s, &flat_fn_v, &total_s)) {
    return 0;
  }
  if (flat_fn_v == fn_v && total_s == arg_s) {
    return er_app_make(gc, fn_v, arg_s, arg_v);
  }
  er_app* app = er_app_alloc(gc, total_s);
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

static er_val er_eval_with_heap(enki_gc* gc, er_val val_v, er_eval_mode mode) {
  ENKI_PROFILE_ZONE("er_eval");
  const enki_allocator* work_a = enki_gc_parent_allocator(gc);
  if (gc == NULL || work_a == NULL || work_a->alloc == NULL ||
      work_a->free == NULL) {
    return er_bad;
  }

  enum {
    ER_EVAL_DSTACK_S = 65536,
    ER_EVAL_KSTACK_S = 262144,
  };

  er_val* dstack_v =
      work_a->alloc(work_a->ctx, ER_EVAL_DSTACK_S * sizeof(er_val));
  er_kon* kstack_v =
      work_a->alloc(work_a->ctx, ER_EVAL_KSTACK_S * sizeof(er_kon));
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
      .pc = ER_BCPC_NONE,
      .gc = gc,
      .dstack = dstack_v,
      .dsp = dstack_v,
      .kbase = kstack_v,
      .ksp = kstack_v,
      .b_count = 0,
      .k_count = 0,
      .gc_rp = NULL,
      .gc_tmp_s = 0,
      .outer_trace_root = NULL,
      .outer_trace_fn = NULL,
  };
  void* old_root = NULL;
  enki_gc_trace_fn old_trace = NULL;
  if (gc != NULL) {
    old_root = gc->trace_root;
    old_trace = gc->trace_fn;
    vm.outer_trace_root = old_root;
    vm.outer_trace_fn = old_trace;
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

er_val er_eval(enki_gc* gc, er_val val_v) {
  return er_eval_with_heap(gc, val_v, ER_EVAL_WHNF);
}

er_val er_eval_to(enki_gc* gc, er_val val_v, er_eval_mode mode) {
  return er_eval_with_heap(gc, val_v, mode);
}

er_val er_eval_gc(enki_gc* gc, er_val val_v) {
  if (gc == NULL) {
    return er_bad;
  }
  return er_eval_with_heap(gc, val_v, ER_EVAL_WHNF);
}

static er_val er_app_take(enki_gc* gc, er_app* old, size_t arg_s) {
  if (old == NULL) {
    return 0;
  }
  er_app* app = er_app_alloc(gc, arg_s);
  if (app == NULL) {
    return 0;
  }
  return er_app_init(app, old->fn_v, arg_s, old->arg_v);
}

static er_val er_thk_take_call(enki_gc* gc, er_thk* old, size_t arg_s) {
  if (old == NULL) {
    return 0;
  }
  size_t copy_s = old->arg_s < arg_s ? old->arg_s : arg_s;
  return er_thk_make_call_frame(gc, arg_s, copy_s, old->arg_v);
}

static er_val er_app_drop(enki_gc* gc, er_app* old, size_t dop_s) {
  if (old == NULL) {
    return 0;
  }
  if (old->arg_s < dop_s) {
    return 0;
  }
  size_t siz_s = old->arg_s - dop_s;
  er_app* app = er_app_alloc(gc, siz_s);
  if (app == NULL) {
    return 0;
  }
  return er_app_init(app, old->fn_v, siz_s, &old->arg_v[dop_s]);
}

static er_val er_app_drop_coup(enki_gc* gc, er_thk* old, er_val fn_v,
                               size_t dop_s) {
  if (old == NULL) {
    return 0;
  }
  if (old->arg_s <= dop_s) {
    return 0;
  }
  size_t siz_s = old->arg_s - dop_s - 1;
  er_thk* thk = er_thk_alloc(gc, siz_s + 1);
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

static er_law* er_resolve_law(er_val val_v) {
  er_pin* pin;
  switch (er_get_tag(val_v)) {
  case er_tag_pin:
    pin = er_outa(val_v);
    return er_outt(er_tag_law, pin->val_v);
  case er_tag_law:
    return er_outa(val_v);
  default:
    return NULL;
  }
}

static size_t er_law_n_lets(const er_law* law) {
  if (law == NULL) {
    return 0;
  }
  return law->let_d;
}

static size_t er_call_frame_size(er_val fun_v, uint32_t arity_d) {
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

static bool er_callable_arity(er_val val_v, uint32_t* out_d) {
  er_pin* pin;
  er_app* app;
  er_law* law;
  switch (er_get_tag(val_v)) {
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

static uint32_t er_arity(er_val val_v) {
  er_law* law = er_outt(er_tag_law, val_v);
  return law == NULL ? 0 : law->ari_d;
}

static er_val plan_eval_nf_inner(er_vm* vm, er_val val_v);
static er_val plan_eval_whnf_preserve(er_vm* vm, er_val val_v);

static er_val op_eval_arg(er_vm* vm, er_app* app, size_t arg_s,
                          er_bc_eval_req eval) {
  if (app == NULL || arg_s >= app->arg_s) {
    return er_tank_make(vm->gc, 0, "bad primitive argument");
  }
  if (eval == ER_BC_EVAL_NONE) {
    return er_into(er_tag_app, app);
  }

  er_val* root_v = vm->dsp;
  root_v[0] = er_into(er_tag_app, app);
  root_v[1] = app->arg_v[arg_s];
  vm->dsp = root_v + 2;

  er_val out_v = eval == ER_BC_EVAL_NF ? plan_eval_nf_inner(vm, root_v[1])
                                       : plan_eval_whnf_preserve(vm, root_v[1]);
  if (!er_is_good(out_v)) {
    vm->dsp = root_v;
    return out_v;
  }
  app = er_outt(er_tag_app, root_v[0]);
  if (app == NULL || arg_s >= app->arg_s) {
    vm->dsp = root_v;
    return er_tank_make(vm->gc, 0, "bad primitive argument");
  }
  app->arg_v[arg_s] = out_v;
  er_val app_v = er_into(er_tag_app, app);
  vm->dsp = root_v;
  return app_v;
}

static er_val op_eval_arg_route_app(er_vm* vm, er_app* app, size_t arg_off_s,
                                    size_t arg_s,
                                    const er_bc_prim_route* route) {
  if (app == NULL) {
    return er_tank_make(vm->gc, 0, "expected primitive row");
  }
  if (route == NULL || arg_s != route->arg_s || arg_s > ER_BC_MAX_PRIM_ARITY) {
    return er_into(er_tag_app, app);
  }
  for (size_t k = 0; k < arg_s; k++) {
    er_val row_v = op_eval_arg(vm, app, arg_off_s + k, route->arg_eval_v[k]);
    if (!er_is_good(row_v)) {
      return row_v;
    }
    app = er_outt(er_tag_app, row_v);
  }
  return er_into(er_tag_app, app);
}

static er_bc_prim_route op_arg_route(er_optag tag, size_t arg_s,
                                     const er_bc_eval_req arg_eval_v[]) {
  er_bc_prim_route route = {
      .tag = tag,
      .arg_s = arg_s,
      .valid_f = true,
  };
  for (size_t k = 0; k < arg_s && k < ER_BC_MAX_PRIM_ARITY; k++) {
    route.arg_eval_v[k] = arg_eval_v[k];
  }
  return route;
}

static er_val op66_eval_arg_app(er_vm* vm, er_app* app) {
  if (app == NULL) {
    return er_tank_make(vm->gc, 0, "expected primitive row");
  }

  er_val tag_v = app->fn_v;
  size_t arg_off_s = 0;
  size_t arg_s = app->arg_s;
  if (tag_v == 0 && app->arg_s > 0) {
    tag_v = app->arg_v[0];
    arg_off_s = 1;
    arg_s = app->arg_s - 1u;
  }

  int op_i = 0;
  er_app* desc = er_outt(er_tag_app, tag_v);
  er_bc_prim_route route = {0};
  if (desc != NULL) {
    if (desc->arg_s != 1 || !er_prim66_op_from_descriptor(desc->fn_v, &op_i)) {
      return er_into(er_tag_app, app);
    }
    er_val width_v = desc->arg_v[0];
    switch (op_i) {
    case OP66_LOAD:
      (void)er_bc_prim_route_strict(width_v == 0 ? OP_LOAD : OP_LOADN,
                                    width_v == 0 ? 3 : 2, &route);
      return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
    case ER_OP66_STORE:
      (void)er_bc_prim_route_strict(width_v == 0 ? OP_STORE : OP_STOREN,
                                    width_v == 0 ? 4 : 3, &route);
      return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
    case OP66_TRUNC:
      (void)er_bc_prim_route_strict(width_v == 0 ? OP_TRUNC : OP_TRUNCN,
                                    width_v == 0 ? 2 : 1, &route);
      return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
    case ER_OP66_MET:
      (void)er_bc_prim_route_strict(OP_MET, 1, &route);
      return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
    default:
      if (er_bc_prim66_route(op_i, &route)) {
        return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
      }
      return er_into(er_tag_app, app);
    }
  }

  if (!eo_op66_from_tag(tag_v, &op_i)) {
    return er_into(er_tag_app, app);
  }

  switch (op_i) {
  case OP66_TYPE:
  case OP66_IS_PIN:
  case OP66_IS_LAW:
  case OP66_IS_APP:
  case OP66_IS_NAT:
  case OP66_IX0:
  case OP66_IX1:
  case OP66_IX2:
  case OP66_IX3:
  case OP66_IX4:
  case OP66_IX5:
  case OP66_IX6:
  case OP66_IX7: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_WHNF};
    route = op_arg_route(OP_COUNT, 1, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_NE:
  case OP66_LT:
  case OP66_GT:
  case OP66_GE:
  case OP66_SET:
  case OP66_CLEAR:
  case OP66_NIB: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_WHNF, ER_BC_EVAL_WHNF};
    route = op_arg_route(OP_COUNT, 2, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_CASE: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_WHNF, ER_BC_EVAL_WHNF,
                               ER_BC_EVAL_NONE};
    route = op_arg_route(OP_COUNT, 3, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
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
    if (arg_s != (size_t)(op_i - OP66_CASE2 + 3)) {
      return er_into(er_tag_app, app);
    }
    return op_eval_arg(vm, app, arg_off_s, ER_BC_EVAL_WHNF);
  }
  case OP66_IF:
  case OP66_IFZ: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_WHNF, ER_BC_EVAL_NONE,
                               ER_BC_EVAL_NONE};
    route = op_arg_route(OP_COUNT, 3, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_NOR: {
    if (arg_s != 2) {
      return er_into(er_tag_app, app);
    }
    er_val row_v = op_eval_arg(vm, app, arg_off_s, ER_BC_EVAL_WHNF);
    if (!er_is_good(row_v)) {
      return row_v;
    }
    app = er_outt(er_tag_app, row_v);
    if (app->arg_v[arg_off_s] != 0) {
      return row_v;
    }
    return op_eval_arg(vm, app, arg_off_s + 1u, ER_BC_EVAL_WHNF);
  }
  case OP66_ROW: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_WHNF, ER_BC_EVAL_WHNF,
                               ER_BC_EVAL_NONE};
    route = op_arg_route(OP_COUNT, 3, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_SEQ: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_WHNF, ER_BC_EVAL_NONE};
    route = op_arg_route(OP_COUNT, 2, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_SEQ2: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_WHNF, ER_BC_EVAL_WHNF,
                               ER_BC_EVAL_NONE};
    route = op_arg_route(OP_COUNT, 3, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_SEQ3: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_WHNF, ER_BC_EVAL_WHNF,
                               ER_BC_EVAL_WHNF, ER_BC_EVAL_NONE};
    route = op_arg_route(OP_COUNT, 4, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_SAP: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_NONE, ER_BC_EVAL_WHNF};
    route = op_arg_route(OP_COUNT, 2, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_SAP2: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_NONE, ER_BC_EVAL_WHNF,
                               ER_BC_EVAL_WHNF};
    route = op_arg_route(OP_COUNT, 3, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_FORCE:
  case OP66_THROW: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_NF};
    route = op_arg_route(OP_COUNT, 1, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_DEEPSEQ: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_NF, ER_BC_EVAL_NONE};
    route = op_arg_route(OP_COUNT, 2, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_TRACE: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_NONE, ER_BC_EVAL_NONE};
    route = op_arg_route(OP_COUNT, 2, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  case OP66_EQUAL: {
    er_bc_eval_req eval_v[] = {ER_BC_EVAL_WHNF, ER_BC_EVAL_WHNF};
    route = op_arg_route(OP_COUNT, 2, eval_v);
    return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  }
  default:
    if (er_bc_prim66_route(op_i, &route)) {
      return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
    }
    return er_into(er_tag_app, app);
  }
}

static er_val op0_eval_arg_app(er_vm* vm, er_app* app) {
  if (app == NULL) {
    return er_tank_make(vm->gc, 0, "expected primitive row");
  }

  er_val tag_v = app->fn_v;
  size_t arg_off_s = 0;
  size_t arg_s = app->arg_s;
  if (tag_v == 0 && app->arg_s > 1) {
    tag_v = app->arg_v[0];
    arg_off_s = 1;
    arg_s = app->arg_s - 1u;
  }

  size_t op_s = 0;
  er_bc_prim_route route = {0};
  if (!eo_nat_to_size(tag_v, &op_s) || !er_bc_prim0_route(op_s, &route)) {
    return er_into(er_tag_app, app);
  }
  return op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
}

static bool op0_op_from_name(er_val tag_v, size_t* out_op_s) {
  if (out_op_s == NULL) {
    return false;
  }
  switch (tag_v) {
  case PLAN_S3('P', 'i', 'n'):
    *out_op_s = OP0_PIN;
    return true;
  case PLAN_S3('L', 'a', 'w'):
    *out_op_s = OP0_LAW;
    return true;
  case PLAN_S4('E', 'l', 'i', 'm'):
    *out_op_s = OP0_ELIM;
    return true;
  default:
    return false;
  }
}

static bool op0_named_app_view(er_app* app, size_t* out_op_s,
                               size_t* out_arg_off_s, size_t* out_arg_s) {
  if (app == NULL || out_op_s == NULL || out_arg_off_s == NULL ||
      out_arg_s == NULL) {
    return false;
  }
  er_val tag_v = app->fn_v;
  size_t arg_off_s = 0;
  size_t arg_s = app->arg_s;
  if (tag_v == 0 && app->arg_s > 0) {
    tag_v = app->arg_v[0];
    arg_off_s = 1;
    arg_s = app->arg_s - 1u;
  }
  if (!op0_op_from_name(tag_v, out_op_s)) {
    return false;
  }
  *out_arg_off_s = arg_off_s;
  *out_arg_s = arg_s;
  return true;
}

static bool op66_exec_named_op0_app(er_vm* vm, er_app* app, er_val* out_v) {
  size_t op_s = 0;
  size_t arg_off_s = 0;
  size_t arg_s = 0;
  er_bc_prim_route route = {0};
  if (!op0_named_app_view(app, &op_s, &arg_off_s, &arg_s)) {
    return false;
  }
  if (!er_bc_prim0_route(op_s, &route)) {
    *out_v = er_tank_make(vm->gc, (er_val)op_s, "bad primitive route");
    return true;
  }

  er_val app_v = op_eval_arg_route_app(vm, app, arg_off_s, arg_s, &route);
  if (!er_is_good(app_v)) {
    *out_v = app_v;
    return true;
  }
  app = er_outt(er_tag_app, app_v);
  if (app == NULL) {
    *out_v = er_tank_make(vm->gc, 0, "expected primitive row");
    return true;
  }
  if (!op0_named_app_view(app, &op_s, &arg_off_s, &arg_s)) {
    *out_v = er_tank_make(vm->gc, 0, "bad primitive tag");
    return true;
  }
  *out_v = eo_exec_op0(vm->gc, (int)op_s, arg_s, app->arg_v + arg_off_s);
  return true;
}

static bool op66_app_view(er_app* app, int* out_op_i, const er_val** out_arg_v,
                          size_t* out_arg_s) {
  if (app == NULL || out_op_i == NULL || out_arg_v == NULL ||
      out_arg_s == NULL) {
    return false;
  }
  er_val tag_v = app->fn_v;
  const er_val* arg_v = app->arg_v;
  size_t arg_s = app->arg_s;
  if (tag_v == 0 && arg_s > 0) {
    tag_v = app->arg_v[0];
    arg_v = app->arg_v + 1;
    arg_s = app->arg_s - 1u;
  }
  if (!eo_op66_from_tag(tag_v, out_op_i)) {
    return false;
  }
  *out_arg_v = arg_v;
  *out_arg_s = arg_s;
  return true;
}

static er_val op66_exec_row_app(er_vm* vm, const er_val arg_v[], size_t arg_s) {
  if (arg_s != 3) {
    return er_tank_make(vm->gc, (er_val)arg_s, "bad primitive arity");
  }

  er_val* root_v = vm->dsp;
  root_v[0] = arg_v[0];
  root_v[1] = arg_v[1];
  root_v[2] = arg_v[2];
  vm->dsp = root_v + 3;

  root_v[0] = plan_eval_whnf_preserve(vm, root_v[0]);
  if (!er_is_good(root_v[0])) {
    er_val err_v = root_v[0];
    vm->dsp = root_v;
    return err_v;
  }
  root_v[0] = eo_nat(root_v[0]);
  root_v[1] = plan_eval_whnf_preserve(vm, root_v[1]);
  if (!er_is_good(root_v[1])) {
    er_val err_v = root_v[1];
    vm->dsp = root_v;
    return err_v;
  }

  size_t row_count_s = 0;
  (void)eo_nat_to_size(root_v[1], &row_count_s);
  if (row_count_s == 0) {
    er_val out_v = root_v[0];
    vm->dsp = root_v;
    return out_v;
  }

  er_app* row_app = er_app_alloc(vm->gc, row_count_s);
  if (row_app == NULL) {
    vm->dsp = root_v;
    return er_bad;
  }
  er_val row_v = er_app_init(row_app, root_v[0], row_count_s, NULL);
  if (row_v == 0) {
    vm->dsp = root_v;
    return er_bad;
  }

  root_v[0] = row_v;
  root_v[1] = root_v[2];
  vm->dsp = root_v + 2;
  for (size_t k = 0; k < row_count_s; k++) {
    root_v[1] = plan_eval_whnf_preserve(vm, root_v[1]);
    if (!er_is_good(root_v[1])) {
      er_val err_v = root_v[1];
      vm->dsp = root_v;
      return err_v;
    }
    row_app = er_outt(er_tag_app, root_v[0]);
    if (row_app == NULL) {
      vm->dsp = root_v;
      return er_bad;
    }
    row_app->arg_v[k] = eo_ix(0, root_v[1]);
    root_v[1] = eo_ix(1, root_v[1]);
  }

  er_val out_v = root_v[0];
  vm->dsp = root_v;
  return out_v;
}

static er_val op66_exec_try_app(er_vm* vm, const er_val arg_v[], size_t arg_s) {
  if (arg_s != 2) {
    return er_tank_make(vm->gc, (er_val)arg_s, "bad primitive arity");
  }

  er_val* root_v = vm->dsp;
  root_v[0] = arg_v[0];
  root_v[1] = arg_v[1];
  vm->dsp = root_v + 2;

  er_val call_v = er_thk_make_unk_app(vm->gc, 2, root_v);
  if (call_v == 0) {
    vm->dsp = root_v;
    return er_bad;
  }
  root_v[0] = call_v;
  vm->dsp = root_v + 1;

  er_val res_v = plan_eval_nf_inner(vm, root_v[0]);
  er_val tag_v = 0;
  er_val try_val_v = res_v;
  er_tank* tank = er_outt(er_tag_tank, res_v);
  if (tank != NULL && tank->msg_c != NULL &&
      strcmp(tank->msg_c, "throw") == 0) {
    tag_v = 1;
    try_val_v = tank->val_v;
  } else {
    er_app* thrown_app = er_outt(er_tag_app, res_v);
    if (thrown_app != NULL && thrown_app->arg_s == 1 &&
        (thrown_app->fn_v == PLAN_S5('T', 'h', 'r', 'o', 'w') ||
         thrown_app->fn_v == PLAN_S5('E', 'r', 'r', 'o', 'r'))) {
      tag_v = 1;
      try_val_v = thrown_app->arg_v[0];
    }
  }

  if (tag_v == 1) {
    root_v[0] = try_val_v;
    vm->dsp = root_v + 1;
    try_val_v = plan_eval_nf_inner(vm, root_v[0]);
    if (!er_is_good(try_val_v)) {
      vm->dsp = root_v;
      return try_val_v;
    }
  } else if (!er_is_good(res_v)) {
    vm->dsp = root_v;
    return res_v;
  }

  root_v[0] = try_val_v;
  vm->dsp = root_v + 1;
  er_val out_v = er_app_make(vm->gc, tag_v, 1, root_v);
  vm->dsp = root_v;
  return out_v == 0 ? er_bad : out_v;
}

static bool op66_exec_special_app(er_vm* vm, er_app* app, er_val* out_v) {
  if (op66_exec_named_op0_app(vm, app, out_v)) {
    return true;
  }

  int op_i = 0;
  const er_val* arg_v = NULL;
  size_t arg_s = 0;
  if (!op66_app_view(app, &op_i, &arg_v, &arg_s)) {
    return false;
  }
  switch (op_i) {
  case OP66_ROW:
    *out_v = op66_exec_row_app(vm, arg_v, arg_s);
    return true;
  case OP66_TRY:
    *out_v = op66_exec_try_app(vm, arg_v, arg_s);
    return true;
  default:
    return false;
  }
}

/*
 * (define (OP_PUSH_VAR val) ("OP_PUSH_VAR" val)) ;; [] -- a
(define OP_PUSH_SELF ("OP_PUSH_SELF" 0)) ;; [] -- a
(define (OP_PUSH_LIT val) ("OP_PUSH_LIT" val)) ;; [] -- a
(define (OP_MK_APP count) ("OP_MK_APP" count)) ;; [<count>] -- a
(define (OP_CALLF argc) ("OP_CALLF" argc)) ;; [fn, args...] -- whnf
(define (OP_CALLS argc) ("OP_CALLS" argc)) ;; [fn, args...] -- whnf
(define (OP_CALLU argc) ("OP_CALLU" argc)) ;; [fn, args...] -- thunk
(define (OP_APPLY_UNK count) ("OP_APPLY_UNK" count)) ;; [fn, args...] -- thunk
(define (OP_APPLY_FAST count) ("OP_APPLY_FAST" count)) ;; [fn, args...] -- thunk
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
(define (OP_JMP label) ("OP_JMP" label)) ;; [] -- []
(define (OP_JUMP_IF label) ("OP_JUMP_IF" label)) ;; c -- []
(define (OP_MAKE_SUSP label) ("OP_MAKE_SUSP" label)) ;; [] -- thunk
(define OP_EQ ("OP_EQ" 0)) ;; [x y] -- r
(define OP_CMP ("OP_CMP" 0)) ;; [x y] -- r
*/

__attribute__((noinline)) static er_val plan_eval_whnf(er_vm* vm, er_val val_v,
                                                       er_eval_mode mode) {
  ENKI_PROFILE_ZONE("plan_eval_whnf");
  er_val* dbase = vm->dsp;
  er_val* dsp = vm->dsp;
  er_kon* kbase = vm->ksp;
  er_kon* ksp = vm->ksp;

  er_bcpc pc = vm->pc;
  er_val hd_v = er_bad;
  er_val* env = NULL;
  er_val env_v = 0;
  er_val r = val_v;
  er_thk* thk;
  uint32_t wan_d, hav_d;
  uint64_t prim_set;
  er_val prim_arg;
  er_app* prim_row;
  er_val prim_word_v;
  er_val prim_a_v = 0;
  er_val prim_b_v = 0;
  er_val prim_c_v = 0;
  er_val prim_d_v = 0;
  er_val prim_e_v = 0;
  er_val prim_f_v = 0;

  er_val f, app, target;
  er_head* head;
  er_kon kon;
  er_op op;
  uint32_t split;
  size_t idx_s;

#define ER_DISPATCH_ENTRY(_tag, _label, _value) [_tag] = &&_label,
  static void* const dispatch[OP_COUNT] = {
      /* Core VM/control opcodes, including private fast paths. */
      ER_CORE_OPS(ER_DISPATCH_ENTRY)

      /* Primitive leaves keep one dispatch label per bytecode tag. */
      ER_PRIM_LEAF_OPS(ER_DISPATCH_ENTRY)

      /* Return/control opcodes. */
      ER_RETURN_CONTROL_OPS(ER_DISPATCH_ENTRY)};
#undef ER_DISPATCH_ENTRY

#define DPUSH(_r)                                                              \
  do {                                                                         \
    *dsp++ = (_r);                                                             \
  } while (0)
#define DPOP() (*--dsp)

#define GC_SYNC()                                                              \
  do {                                                                         \
    vm->dsp = dsp;                                                             \
    vm->ksp = ksp;                                                             \
    vm->pc = pc;                                                               \
  } while (0)

#define GC_ROOT_PRIMS()                                                        \
  do {                                                                         \
    vm->gc_tmp_v[0] = prim_a_v;                                                \
    vm->gc_tmp_v[1] = prim_b_v;                                                \
    vm->gc_tmp_v[2] = prim_c_v;                                                \
    vm->gc_tmp_v[3] = prim_d_v;                                                \
    vm->gc_tmp_v[4] = prim_e_v;                                                \
    vm->gc_tmp_v[5] = prim_f_v;                                                \
    vm->gc_tmp_s = 6;                                                          \
  } while (0)

#define GC_CLEAR_ROOTS()                                                       \
  do {                                                                         \
    vm->gc_tmp_s = 0;                                                          \
  } while (0)

#define CODE_SET_PC(_pc)                                                       \
  do {                                                                         \
    pc = (_pc);                                                                \
    vm->pc = pc;                                                               \
  } while (0)

#define CODE_SET_LABEL(_law_v, _label_d)                                       \
  do {                                                                         \
    er_law* code_set_law = er_resolve_law((_law_v));                           \
    size_t code_set_label_s = (size_t)(_label_d);                              \
    if (code_set_law == NULL || code_set_label_s >= code_set_law->bc_s ||      \
        er_law_label_code(code_set_law, code_set_label_s) == NULL) {           \
      FAIL_TANK("missing bytecode", (_law_v));                                 \
    }                                                                          \
    CODE_SET_PC(code_set_law->bc_v[code_set_label_s].pc);                      \
  } while (0)

#define CODE_SET_CURRENT_LABEL(_label_d)                                       \
  do {                                                                         \
    if (env == NULL) {                                                         \
      FAIL_TANK("missing bytecode env", (er_val)(_label_d));                   \
    }                                                                          \
    CODE_SET_LABEL(env[0], (_label_d));                                        \
  } while (0)

#define DISPATCH()                                                             \
  do {                                                                         \
    vm->b_count++;                                                             \
    er_op* dispatch_op = er_bytecode_at(pc);                                   \
    ep_debug_if((dispatch_op == NULL),                                         \
                { FAIL_TANK("missing bytecode", (er_val)pc); });               \
    op = *dispatch_op;                                                         \
    pc++;                                                                      \
    vm->pc = pc;                                                               \
    ep_debug_if(((size_t)op.tag >= OP_COUNT || dispatch[op.tag] == NULL),      \
                { FAIL_TANK("bad bytecode op", (er_val)op.tag); });            \
    goto* dispatch[op.tag];                                                    \
  } while (0)

#define KPUSH_BYTECODE_RETURN(_pc, _env, _env_v)                               \
  do {                                                                         \
    vm->k_count++;                                                             \
    *ksp++ = (er_kon){                                                         \
        .tag = ER_K_BYTECODE_RETURN,                                           \
        .as.bytecode_return =                                                  \
            {                                                                  \
                .env = (_env),                                                 \
                .env_v = (_env_v),                                             \
                .pc = (_pc),                                                   \
                .dbase = dbase,                                                \
            },                                                                 \
    };                                                                         \
    vm->ksp = ksp;                                                             \
  } while (0)

#define KPUSH_UPDATE(_target)                                                  \
  do {                                                                         \
    *ksp++ = (er_kon){                                                         \
        .tag = ER_K_UPDATE,                                                    \
        .as.update =                                                           \
            {                                                                  \
                .target_v = (_target),                                         \
                .dbase = dbase,                                                \
            },                                                                 \
    };                                                                         \
    vm->ksp = ksp;                                                             \
  } while (0)

#define KPUSH_APPHEAD(_app)                                                    \
  do {                                                                         \
    *ksp++ = (er_kon){                                                         \
        .tag = ER_K_APPHEAD,                                                   \
        .as.apphead =                                                          \
            {                                                                  \
                .app_v = (_app),                                               \
            },                                                                 \
    };                                                                         \
    vm->ksp = ksp;                                                             \
  } while (0)

#define KPUSH_APP_IDX(_app, _idx)                                              \
  do {                                                                         \
    *ksp++ = (er_kon){                                                         \
        .tag = ER_K_APP_IDX,                                                   \
        .as.appidx =                                                           \
            {                                                                  \
                .app_v = (_app),                                               \
                .idx_s = (_idx),                                               \
            },                                                                 \
    };                                                                         \
    vm->ksp = ksp;                                                             \
  } while (0)

#define KPUSH_OVERAPP(_app, _split)                                            \
  do {                                                                         \
    *ksp++ = (er_kon){                                                         \
        .tag = ER_K_OVERAPP,                                                   \
        .as.overapp =                                                          \
            {                                                                  \
                .app_v = (_app),                                               \
                .split_d = (uint32_t)(_split),                                 \
            },                                                                 \
    };                                                                         \
    vm->ksp = ksp;                                                             \
  } while (0)

#define KPUSH_NORMAL()                                                         \
  do {                                                                         \
    *ksp++ = (er_kon){.tag = ER_K_NORMAL};                                     \
    vm->ksp = ksp;                                                             \
  } while (0)

#define RETURN(_r)                                                             \
  do {                                                                         \
    r = (_r);                                                                  \
    if (!er_is_good(r)) {                                                      \
      vm->dsp = dsp;                                                           \
      vm->ksp = ksp;                                                           \
      return r;                                                                \
    }                                                                          \
    if (ksp == kbase) {                                                        \
      ENKI_PROFILE_PLOT_I("plan_eval.bytecode_steps", (int64_t)vm->b_count);   \
      ENKI_PROFILE_PLOT_I("plan_eval.reductions", (int64_t)vm->k_count);       \
      return r;                                                                \
    }                                                                          \
    kon = *--ksp;                                                              \
    vm->ksp = ksp;                                                             \
    switch (kon.tag) {                                                         \
    case ER_K_BYTECODE_RETURN:                                                 \
      goto K_RETURN;                                                           \
    case ER_K_UPDATE:                                                          \
      goto K_UPDATE;                                                           \
    case ER_K_APPHEAD:                                                         \
      goto K_APPHEAD;                                                          \
    case ER_K_APP_IDX:                                                         \
      goto K_APP_IDX;                                                          \
    case ER_K_OVERAPP:                                                         \
      goto K_OVERAPP;                                                          \
    case ER_K_NORMAL:                                                          \
      goto K_NORMAL;                                                           \
    default:                                                                   \
      assert("bad continuation" && 0);                                         \
      vm->dsp = dsp;                                                           \
      vm->ksp = ksp;                                                           \
      return er_bad;                                                           \
    }                                                                          \
  } while (0)

#define FAIL_ALLOC()                                                           \
  do {                                                                         \
    vm->dsp = dsp;                                                             \
    vm->ksp = ksp;                                                             \
    return er_bad;                                                             \
  } while (0)

#define FAIL_TANK(_msg, _val)                                                  \
  do {                                                                         \
    GC_SYNC();                                                                 \
    vm->gc_tmp_v[0] = (_val);                                                  \
    vm->gc_tmp_s = 1;                                                          \
    er_val tank_v = er_tank_make(vm->gc, vm->gc_tmp_v[0], (_msg));             \
    vm->gc_tmp_s = 0;                                                          \
    return tank_v;                                                             \
  } while (0)

#define CHECK_ALLOC(_v)                                                        \
  do {                                                                         \
    if ((_v) == 0) {                                                           \
      FAIL_ALLOC();                                                            \
    }                                                                          \
    pc = vm->pc;                                                               \
  } while (0)

#define CHECK_PRIM(_v)                                                         \
  do {                                                                         \
    if (!er_is_good(_v)) {                                                     \
      vm->dsp = dsp;                                                           \
      vm->ksp = ksp;                                                           \
      return (_v);                                                             \
    }                                                                          \
    pc = vm->pc;                                                               \
  } while (0)

#define PRIM_FORCE_VALUE_WHNF(_dst, _src)                                      \
  do {                                                                         \
    GC_SYNC();                                                                 \
    (_dst) = plan_eval_whnf_preserve(vm, (_src));                              \
    CHECK_PRIM(_dst);                                                          \
  } while (0)

#define PRIM_FORCE_VALUE_NF(_dst, _src)                                        \
  do {                                                                         \
    GC_SYNC();                                                                 \
    (_dst) = plan_eval_nf_inner(vm, (_src));                                   \
    CHECK_PRIM(_dst);                                                          \
  } while (0)

#define PRIM_DONE_VALUE(_v)                                                    \
  do {                                                                         \
    ENKI_PROFILE_ZONE_BEGIN(enki_primop_zone, "plan_eval.primop_exec");        \
    GC_SYNC();                                                                 \
    GC_ROOT_PRIMS();                                                           \
    er_val prim_res_v = (_v);                                                  \
    GC_CLEAR_ROOTS();                                                          \
    ENKI_PROFILE_ZONE_END(enki_primop_zone);                                   \
    CHECK_PRIM(prim_res_v);                                                    \
    DPUSH(prim_res_v);                                                         \
    goto PRIM_DONE;                                                            \
  } while (0)

#define PRIM_BAD_ARITY()                                                       \
  do {                                                                         \
    FAIL_TANK("bad primitive arity", prim_arg);                                \
  } while (0)

#define PRIM_SELECT_ROUTE(_route)                                              \
  do {                                                                         \
    prim_route = (_route);                                                     \
    prim_byte_op = prim_route.tag;                                             \
    prim_need_s = prim_route.arg_s;                                            \
    goto PRIM_ROUTE_DISPATCH;                                                  \
  } while (0)

#define PRIM_SELECT(_tag, _arg_s)                                              \
  do {                                                                         \
    if (!er_bc_prim_route_strict((_tag), (size_t)(_arg_s), &prim_route)) {     \
      FAIL_TANK("bad primitive route", prim_arg);                              \
    }                                                                          \
    PRIM_SELECT_ROUTE(prim_route);                                             \
  } while (0)

#define CALLU_DISPATCH_MK_APP()                                                \
  do {                                                                         \
    if (thk->arg_s == 0 || thk->arg_s > UINT32_MAX) {                          \
      FAIL_ALLOC();                                                            \
    }                                                                          \
    er_val callu_source_v = er_into(er_tag_thk, thk);                          \
    for (size_t app_k_s = 0; app_k_s < thk->arg_s; app_k_s++) {                \
      DPUSH(thk->arg_v[app_k_s]);                                              \
    }                                                                          \
    er_bcpc app_route_pc = ER_BCPC_NONE;                                       \
    er_bcpc app_route_end_pc = ER_BCPC_NONE;                                   \
    if (!er_app_route_intern((uint32_t)thk->arg_s, &app_route_pc,              \
                             &app_route_end_pc)) {                             \
      FAIL_ALLOC();                                                            \
    }                                                                          \
    (void)app_route_end_pc;                                                    \
    KPUSH_UPDATE(callu_source_v);                                              \
    thk->fun = ER_HOLE;                                                        \
    CODE_SET_PC(app_route_pc);                                                 \
    DISPATCH();                                                                \
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
  r = env[op.as.slot];
  DPUSH(r);
  DISPATCH();

I_PUSH_LIT:
  r = op.as.lit_v;
  DPUSH(r);
  DISPATCH();

I_MK_APP: {
  size_t app_s = op.as.u32;
  if (app_s == 0 || dsp < dbase || (size_t)(dsp - dbase) < app_s) {
    FAIL_ALLOC();
  }
  er_val* app_base = dsp - app_s;
  hd_v = app_base[0];
  GC_SYNC();
  r = er_app_make_flat(vm->gc, hd_v, app_s - 1, &app_base[1]);
  CHECK_ALLOC(r);
  dsp = app_base;
  DPUSH(r);
  DISPATCH();
}

I_CALLF: {
  size_t arg_s = op.as.u32;
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
  r = er_thk_make_call_frame(vm->gc, frame_s, call_s, call_base);
  CHECK_ALLOC(r);
  dsp = call_base;
  KPUSH_BYTECODE_RETURN(pc, env, env_v);
  goto FORCE_ENTRY;
}

I_CALLS: {
  size_t arg_s = op.as.u32;
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
    FAIL_TANK("bad standard call", hd_v);
  }
  GC_SYNC();
  r = er_thk_make_call_frame(vm->gc, frame_s, call_s, call_base);
  CHECK_ALLOC(r);
  dsp = call_base;
  KPUSH_BYTECODE_RETURN(pc, env, env_v);
  goto FORCE_ENTRY;
}

I_APPLY_FAST: {
  size_t call_s = op.as.u32;
  if (call_s == 0 || dsp < dbase || (size_t)(dsp - dbase) < call_s) {
    FAIL_ALLOC();
  }
  size_t arg_s = call_s - 1u;
  er_val* call_base = dsp - call_s;
  hd_v = call_base[0];
  size_t frame_s = er_call_frame_size(hd_v, (uint32_t)arg_s);
#ifndef NDEBUG
  assert(er_is_whnf(hd_v));
  assert(frame_s != 0);
#endif
  if (!er_is_whnf(hd_v) || frame_s == 0) {
    FAIL_TANK("bad fast application", hd_v);
  }
  GC_SYNC();
  r = er_thk_make_call_frame(vm->gc, frame_s, call_s, call_base);
  CHECK_ALLOC(r);
  dsp = call_base;
  DPUSH(r);
  DISPATCH();
}

I_CALLU: {
  size_t arg_s = op.as.u32;
  size_t call_s = arg_s + 1;
  if (arg_s == SIZE_MAX || dsp < dbase || (size_t)(dsp - dbase) < call_s) {
    FAIL_ALLOC();
  }
  er_val* call_base = dsp - call_s;
  GC_SYNC();
  r = er_thk_make_unk_app(vm->gc, call_s, call_base);
  CHECK_ALLOC(r);
  dsp = call_base;
  DPUSH(r);
  DISPATCH();
}

I_APPLY_UNK: {
  size_t call_s = op.as.u32;
  if (call_s == 0 || dsp < dbase || (size_t)(dsp - dbase) < call_s) {
    FAIL_ALLOC();
  }
  er_val* call_base = dsp - call_s;
  hd_v = call_base[0];
  er_pin* pin = call_s == 2 ? er_outt(er_tag_pin, hd_v) : NULL;
  GC_SYNC();
  if (pin != NULL && er_is_cat(pin->val_v)) {
    r = er_thk_make_prim(vm->gc, pin->val_v, call_base[1]);
  } else {
    r = er_thk_make_unk_app(vm->gc, call_s, call_base);
  }
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
  r = er_thk_make_prim(vm->gc, pin->val_v, call_base[1]);
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
  size_t rotate_s = op.as.u32;
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
    CODE_SET_PC((er_bcpc)op.as.u32);
  }
  DISPATCH();

I_JMP:
  CODE_SET_CURRENT_LABEL(op.as.u32);
  DISPATCH();

I_JUMP_IF:
  r = DPOP();
  if (r != 0) {
    CODE_SET_CURRENT_LABEL(op.as.u32);
  }
  DISPATCH();

I_MAKE_SUSP: {
  if (env == NULL) {
    FAIL_TANK("missing bytecode env", (er_val)op.as.u32);
  }
  uint32_t susp_label_d = op.as.u32;
  er_law* susp_law = er_resolve_law(env[0]);
  if (susp_law == NULL || (size_t)susp_label_d >= susp_law->bc_s ||
      er_law_label_code(susp_law, susp_label_d) == NULL) {
    FAIL_TANK("bad suspension label", env[0]);
  }
  size_t frame_s = er_call_frame_size(env[0], susp_law->ari_d);
  if (frame_s == 0) {
    FAIL_TANK("bad suspension frame", env[0]);
  }
  GC_SYNC();
  er_val susp_env_v = er_thk_make_env_frame(vm->gc, frame_s, env);
  CHECK_ALLOC(susp_env_v);
  vm->gc_tmp_v[0] = susp_env_v;
  vm->gc_tmp_s = 1;
  r = er_thk_make_susp(vm->gc, susp_label_d, susp_env_v, env[0]);
  vm->gc_tmp_s = 0;
  CHECK_ALLOC(r);
  DPUSH(r);
  DISPATCH();
}

I_EVAL:
  r = DPOP();
  GC_SYNC();
  KPUSH_BYTECODE_RETURN(pc, env, env_v);
  goto FORCE_ENTRY;

I_TAIL_EVAL:
  r = DPOP();
  GC_SYNC();
  goto FORCE_ENTRY;

I_FORCE:
  r = DPOP();
  GC_SYNC();
  KPUSH_BYTECODE_RETURN(pc, env, env_v);
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
  if (!er_is_whnf(f)) {
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
    target = er_into(er_tag_thk, thk);
    GC_SYNC();
    r = er_thk_make_prim(vm->gc, pin->val_v, thk->arg_v[1]);
    CHECK_ALLOC(r);
    KPUSH_UPDATE(target);
    thk = er_outt(er_tag_thk, target);
    if (thk == NULL) {
      FAIL_TANK("bad primitive app", target);
    }
    thk->fun = ER_HOLE;
    if (hav_s > 1) {
      KPUSH_OVERAPP(target, 1);
    }
    goto FORCE_ENTRY;
  }

  if (er_is_tag(er_tag_app, f)) {
    target = er_into(er_tag_thk, thk);
    GC_SYNC();
    r = er_thk_make_unk_app_flat(vm->gc, f, hav_s, &thk->arg_v[1]);
    CHECK_ALLOC(r);
    KPUSH_UPDATE(target);
    thk = er_outt(er_tag_thk, target);
    if (thk == NULL) {
      FAIL_TANK("bad flat app", target);
    }
    thk->fun = ER_HOLE;
    goto FORCE_ENTRY;
  }

  if (!er_callable_arity(f, &wan_d)) {
    if (hav_s == 0) {
      RETURN(f);
    }
    CALLU_DISPATCH_MK_APP();
  }
  if (hav_d == wan_d) {
    size_t frame_s = er_call_frame_size(f, wan_d);
    if (frame_s == 0) {
      FAIL_TANK("bad call frame", f);
    }
    if (frame_s == thk->arg_s) {
      thk->fun = ER_CALL;
      goto FORCE_ENTRY;
    }
    er_val source_thk_v = er_into(er_tag_thk, thk);
    GC_SYNC();
    r = er_thk_take_call(vm->gc, thk, frame_s);
    CHECK_ALLOC(r);
    thk = er_outt(er_tag_thk, source_thk_v);
    if (thk == NULL) {
      FAIL_TANK("bad call frame", source_thk_v);
    }
    KPUSH_UPDATE(source_thk_v);
    thk->fun = ER_HOLE;
    goto FORCE_ENTRY;
  } else if (hav_d < wan_d) {
    CALLU_DISPATCH_MK_APP();
  } else {
    split = wan_d;
    size_t frame_s = er_call_frame_size(f, wan_d);
    if (frame_s == 0) {
      FAIL_TANK("bad call frame", f);
    }
    target = er_into(er_tag_thk, thk);
    GC_SYNC();
    r = er_thk_make_call_frame(vm->gc, frame_s, (size_t)wan_d + 1, thk->arg_v);
    CHECK_ALLOC(r);
    KPUSH_UPDATE(target);
    thk = er_outt(er_tag_thk, target);
    if (thk == NULL) {
      FAIL_TANK("bad call frame", target);
    }
    thk->fun = ER_HOLE;
    KPUSH_OVERAPP(target, split);
    goto FORCE_ENTRY;
  }
}

FORCE_XPRIM: {
  if (thk->arg_s != 2 || !er_is_cat(thk->arg_v[0])) {
    FAIL_TANK("bad primitive thunk", er_into(er_tag_thk, thk));
  }
  prim_set = thk->arg_v[0];
  prim_arg = thk->arg_v[1];
  target = er_into(er_tag_thk, thk);
  KPUSH_UPDATE(target);
  thk->fun = ER_HOLE;
  if (!er_is_whnf(prim_arg)) {
    GC_SYNC();
    prim_arg = plan_eval_whnf_preserve(vm, prim_arg);
    dsp = vm->dsp;
    ksp = vm->ksp;
    CHECK_PRIM(prim_arg);
  }
  prim_row = er_outt(er_tag_app, prim_arg);
  if (prim_set == 66) {
    GC_SYNC();
    if (op66_exec_special_app(vm, prim_row, &r)) {
      dsp = vm->dsp;
      ksp = vm->ksp;
      CHECK_PRIM(r);
      goto FORCE_ENTRY;
    }
    GC_SYNC();
    prim_arg = op66_eval_arg_app(vm, prim_row);
    dsp = vm->dsp;
    ksp = vm->ksp;
    CHECK_PRIM(prim_arg);
    prim_row = er_outt(er_tag_app, prim_arg);
    GC_SYNC();
    vm->gc_tmp_v[0] = prim_arg;
    vm->gc_tmp_s = 1;
    r = eo_exec_op66_er_app(vm->gc, prim_row);
    vm->gc_tmp_s = 0;
  } else if (prim_set == 0) {
    GC_SYNC();
    prim_arg = op0_eval_arg_app(vm, prim_row);
    dsp = vm->dsp;
    ksp = vm->ksp;
    CHECK_PRIM(prim_arg);
    prim_row = er_outt(er_tag_app, prim_arg);
    GC_SYNC();
    vm->gc_tmp_v[0] = prim_arg;
    vm->gc_tmp_s = 1;
    r = eo_exec_op0_er_app(vm->gc, prim_row);
    vm->gc_tmp_s = 0;
  } else {
    FAIL_TANK("bad primitive set", prim_arg);
  }
  CHECK_PRIM(r);
  goto FORCE_ENTRY;
}

FORCE_ENTRY: {
  if (!er_is_good(r)) {
    vm->dsp = dsp;
    vm->ksp = ksp;
    return r;
  }
  if (er_is_whnf(r)) {
    RETURN(r);
  }
  thk = er_outt(er_tag_thk, r);
  if (thk == NULL) {
    FAIL_TANK("expected thunk", r);
  }
  switch (thk->fun) {
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
  CODE_SET_LABEL(susp_law_v, susp_label);
  env_v = thk->arg_v[1];
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
  if (frame_s == 0 || thk->arg_s != frame_s ||
      er_law_label_code(law, 0) == NULL) {
    FAIL_TANK("bad call frame", self_v);
  }
  size_t n_lets = er_law_n_lets(law);

  for (size_t i = 0; i < n_lets; i++) {
    size_t slot_s = (size_t)law->ari_d + 1 + i;
    GC_SYNC();
    er_val susp_v = er_thk_make_susp(vm->gc, (uint32_t)i + 1, self_v, f);
    CHECK_ALLOC(susp_v);
    thk->arg_v[slot_s] = susp_v;
  }
  if (n_lets > 0) {
    GC_SYNC();
    er_val env_frame_v = er_thk_make_env_frame(vm->gc, thk->arg_s, thk->arg_v);
    CHECK_ALLOC(env_frame_v);
    for (size_t i = 0; i < n_lets; i++) {
      size_t slot_s = (size_t)law->ari_d + 1 + i;
      er_thk* susp = er_outt(er_tag_thk, thk->arg_v[slot_s]);
      if (susp == NULL || susp->fun != ER_SUSP || susp->arg_s < 2) {
        FAIL_TANK("bad suspension", thk->arg_v[slot_s]);
      }
      susp->arg_v[1] = env_frame_v;
    }
  }
  KPUSH_UPDATE(self_v);
  thk->fun = ER_HOLE;
  CODE_SET_LABEL(f, 0);
  env_v = self_v;
  env = thk->arg_v;
  dbase = dsp;
  DISPATCH();
}

  // ---------------------------------------------------------------------
  // Continuation handlers
  // ---------------------------------------------------------------------

K_RETURN:
  dbase = kon.as.bytecode_return.dbase;
  pc = kon.as.bytecode_return.pc;
  vm->pc = pc;
  env_v = kon.as.bytecode_return.env_v;
  er_thk* return_env = er_outt(er_tag_thk, env_v);
  env = return_env == NULL ? kon.as.bytecode_return.env : return_env->arg_v;

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
  if (!thk) {
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
    r = er_app_drop_coup(vm->gc, thk, r, split);
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
#undef PRIM_FORCE_VALUE_NF
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
#undef CODE_SET_CURRENT_LABEL
#undef CODE_SET_LABEL
#undef CODE_SET_PC
#undef DPOP
#undef DPUSH
}

static er_val plan_eval_preserve(er_vm* vm, er_val val_v, er_eval_mode mode) {
  er_val* base_dsp = vm->dsp;
  er_kon* base_ksp = vm->ksp;
  er_val* saved_gc_rp = vm->gc_rp;
  size_t saved_gc_tmp_s = vm->gc_tmp_s;
  er_bcpc saved_pc = vm->pc;

  er_val out_v = plan_eval_whnf(vm, val_v, mode);

  vm->dsp = base_dsp;
  vm->ksp = base_ksp;
  vm->gc_rp = saved_gc_rp;
  vm->gc_tmp_s = saved_gc_tmp_s;
  vm->pc = saved_pc;
  return out_v;
}

static er_val plan_eval_whnf_preserve(er_vm* vm, er_val val_v) {
  return plan_eval_preserve(vm, val_v, ER_EVAL_WHNF);
}

static er_val plan_eval_nf_inner(er_vm* vm, er_val val_v) {
  return plan_eval_preserve(vm, val_v, ER_EVAL_NF);
}

er_val plan_eval(er_vm* vm, er_val val_v, er_eval_mode mode) {
  ENKI_PROFILE_ZONE("plan_eval");
  if (mode == ER_EVAL_NF) {
    return plan_eval_nf_inner(vm, val_v);
  }
  return plan_eval_whnf_preserve(vm, val_v);
}
