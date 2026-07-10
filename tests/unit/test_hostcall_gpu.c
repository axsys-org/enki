#include <criterion/criterion.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_plan.h"

#include "enki/actor.h"
#include "plan/bytecode.h"
#include "plan/hostcall.h"

/*
 * The op-83 binding table and, on Darwin, the direct-Metal gpu.* slice:
 * a pinned materialization request (artifact bytes + root payload +
 * extents, no handles) drives session-open -> arena -> alloc ->
 * library -> pipeline -> target -> command-graph -> readback, each
 * step an admitted host-call returning a witness/refusal row.  Carrier
 * records (memory-arena, heap-pointer, command-step/graph, pair) are
 * under-applied laws per foil/shrine/no-api-carriers.plan.
 */

#ifdef PL_HOST_GPU_METAL
#include "host/gpu_metal.h"
#endif

/* ── Local helpers ─────────────────────────────────────────────────────── */

static bool nat_is(pl_val v, const char* s) {
  if (!pl_is_nat(v))
    return false;
  size_t n = strlen(s);
  if (pl_nat_byte_len(v) != n)
    return false;
  for (size_t i = 0; i < n; i++) {
    if (pl_nat_byte_at(v, i) != (uint8_t)s[i])
      return false;
  }
  return true;
}

static pl_val string_nat(pl_thread* t, const char* s) {
  return pl_nat_from_bytes(t, (const uint8_t*)s, strlen(s));
}

/* bytesBar: payload bytes then the 0x01 top byte (op-82 convention). */
static pl_val bar_nat(pl_thread* t, const uint8_t* b, size_t n) {
  uint8_t* bar = malloc(n + 1);
  cr_assert_not_null(bar);
  memcpy(bar, b, n);
  bar[n] = 0x01;
  pl_val out = pl_nat_from_bytes(t, bar, n + 1);
  free(bar);
  return out;
}

/* Carrier record: under-applied law named kind_c over the fields pushed
 * at [fields_base, vsp).  Body nat 1 (first-field ref) matches the
 * no-api-carriers.plan convention, so the shapes pin-hash identically. */
static pl_val record(pl_thread* t, const char* kind_c, size_t fields_base) {
  uint32_t n = (uint32_t)(t->vsp - fields_base);
  pl_vpush(t, string_nat(t, kind_c));
  pl_gc_reserve(t, PL_LAW_CELLS + PL_APP_CELLS(n));
  PL_GC_FORBID(t);
  pl_val law = pl_mk_law(t, n + 1, t->vstack[fields_base + n], 1);
  pl_val rec = pl_mk_app_from(t, law, n, &t->vstack[fields_base]);
  PL_GC_ALLOW(t);
  t->vsp = fields_base;
  return rec;
}

/* Invoke binding name_c on the args pushed at [args_base, vsp):
 * (pin 83) % (name arg…). */
static pl_val hostcall(pl_thread* t, const char* name_c, size_t args_base) {
  uint32_t n = (uint32_t)(t->vsp - args_base);
  pl_vpush(t, string_nat(t, name_c));
  pl_gc_reserve(t, PL_APP_CELLS(n));
  PL_GC_FORBID(t);
  pl_val row =
      pl_mk_app_from(t, t->vstack[args_base + n], n, &t->vstack[args_base]);
  PL_GC_ALLOW(t);
  t->vsp = args_base;
  pl_vpush(t, row);
  pl_vpush(t, 83);
  pl_val p83 = pl_pin(t, t->vstack[args_base + 1]);
  t->vsp = args_base + 1; /* keep the row rooted */
  pl_val out = pl_apply(t, p83, t->vstack[args_base]);
  t->vsp = args_base;
  return out;
}

/* Witness/refusal rows: (0 tag binding payload…). */
static pl_cell* result_row(pl_val v, const char* tag, const char* binding) {
  pl_cell* p = pl_as(PL_TAG_APP, v);
  if (p == NULL || pl_app_head(p) != 0 || pl_app_n(p) < 2)
    return NULL;
  if (!nat_is(pl_app_args(p)[0], tag) || !nat_is(pl_app_args(p)[1], binding))
    return NULL;
  return p;
}

static uint64_t witness_payload_u64(pl_thread* t, pl_val v, const char* binding,
                                    uint32_t i) {
  pl_cell* p = result_row(v, "witness", binding);
  if (p == NULL) {
    pl_cell* r = pl_as(PL_TAG_APP, v);
    cr_assert_fail("expected %s witness, got %s row", binding,
                   r != NULL && pl_app_n(r) >= 1 &&
                           nat_is(pl_app_args(r)[0], "refusal")
                       ? "refusal"
                       : "no");
  }
  AX_UNUSED(t);
  cr_assert_gt(pl_app_n(p), 2 + i);
  return pl_nat_u64_clamp(pl_app_args(p)[2 + i]);
}

static uint64_t refusal_kind(pl_val v, const char* binding) {
  pl_cell* p = result_row(v, "refusal", binding);
  if (p == NULL)
    return 0;
  return pl_nat_u64_clamp(pl_app_args(p)[2]);
}

/* ── The binding table itself ──────────────────────────────────────────── */

static pl_val nop_body(pl_thread* t, size_t ab) {
  AX_UNUSED(ab);
  return pl_hostcall_witness(t, "test.nop", t->vsp);
}

Test(hostcall, duplicate_binding_refused) {
  cr_assert(pl_hostcall_register("test.nop", 1, 0b1, nop_body));
  size_t n = pl_hostcall_count();
  cr_assert_not(pl_hostcall_register("test.nop", 1, 0b1, nop_body));
  cr_assert_eq(pl_hostcall_count(), n);
}

Test(hostcall, gate_refuses_without_mode) {
  cr_assert(pl_hostcall_register("test.nop", 1, 0b1, nop_body));
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_not_null(t->exn_msg);
    cr_assert_not_null(strstr(t->exn_msg, "HOSTCALL"));
    test_rt_free(&rt);
    return;
  }
  size_t base = t->vsp;
  pl_vpush(t, 0);
  (void)hostcall(t, "test.nop", base);
  cr_assert_fail("hostcall ran without HOSTCALL mode");
}

Test(hostcall, unknown_binding_raises) {
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_not_null(t->exn_msg);
    cr_assert_not_null(strstr(t->exn_msg, "no primop 83"));
    test_rt_free(&rt);
    return;
  }
  size_t base = t->vsp;
  pl_vpush(t, 0);
  (void)hostcall(t, "gpu.unbound", base);
  cr_assert_fail("unknown binding dispatched");
}

/*
 * Gap 17 (main-line port): bytecode decode resolves an op-83 binding
 * through pl_op_lookup_all and bakes its registry index into the
 * PRIM_KNOWN operand slot, exactly as it does for the static table —
 * dispatch then routes the baked index through pl_op_desc (the same
 * unified descriptor path every by-name gpu.* fixture already
 * exercises via F_OPARG frames).
 */
Test(hostcall, bytecode_bakes_hostcall_index) {
  if (getenv("PL_NO_BYTECODE") != NULL)
    cr_skip("PL_NO_BYTECODE: decode disabled by design");
  cr_assert(pl_hostcall_register("test.nop", 1, 0b1, nop_body));
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;

  /* the compiled-pin row: MK_THK 2 PRIM_KNOWN <pin 83> "test.nop" RET */
  size_t base = t->vsp;
  pl_vpush(t, 83);
  pl_val p83 = pl_pin(t, t->vstack[base]);
  t->vsp = base;
  pl_vpush(t, OP_MK_THK);
  pl_vpush(t, 1); /* the binding's argc: decode feeds it to the lookup */
  pl_vpush(t, PL_BAN_PRIM_KNOWN);
  pl_vpush(t, p83);
  pl_vpush(t, string_nat(t, "test.nop"));
  pl_vpush(t, OP_RET);
  uint32_t n = (uint32_t)(t->vsp - base);
  pl_gc_reserve(t, PL_APP_CELLS(n));
  PL_GC_FORBID(t);
  pl_val row = pl_mk_app_from(t, 0, n, &t->vstack[base]);
  PL_GC_ALLOW(t);
  t->vsp = base;
  pl_vpush(t, row);

  pl_code* code = pl_bytecode_from_val(t->vstack[base]);
  cr_assert_not_null(code, "decode refused the op-83 known-primop row");
  /* the pin operand was overwritten with the registry index (a small
   * nat, not the tagged pin word) and MK_THK+RET fused to TAILCALL */
  cr_assert_neq(code->ops[3], (pl_op_t)p83, "registry index not baked");
  cr_assert_eq(code->ops[4], 0u);
  cr_assert_eq(code->ops[0], (pl_op_t)OP_TAILCALL);
  test_rt_free(&rt);
}

/* ── Independent R(3,1,1) antiproduct reference ────────────────────── */

static int psa_pc(int n) {
  int c = 0;
  while (n) {
    c += n & 1;
    n >>= 1;
  }
  return c;
}

static int psa_inv(int a, int b) {
  int s = 0;
  for (int j = 0; j < 5; j++)
    if ((b >> j) & 1)
      s += psa_pc(a >> (j + 1));
  return s;
}

/* geometric product sign under metric (-1,1,1,1,0); 0 = annihilated */
static int psa_gp_sgn(int a, int b) {
  if (((a & b) >> 4) & 1)
    return 0;
  return ((psa_inv(a, b) + ((a & b) & 1)) & 1) ? -1 : 1;
}

static void psa_ap(int a, int b, int* sgn, int* idx) {
  *idx = 31 - (a ^ b);
  int gs = psa_gp_sgn(31 - a, 31 - b);
  if (gs == 0) {
    *sgn = 0;
    return;
  }
  int tot = (gs < 0 ? 1 : 0) + (psa_inv(a, 31 - a) & 1) +
            (psa_inv(b, 31 - b) & 1) + (psa_inv(31 - (a ^ b), a ^ b) & 1);
  *sgn = (tot & 1) ? -1 : 1;
}

static int psa_ar_sgn(int i) {
  int ag = 5 - psa_pc(i);
  return ((ag * (ag - 1) / 2) & 1) ? -1 : 1;
}

/*
 * Gap 19: the foil kernel-IR audit tier.  foil/shrine/kernel-ir.plan
 * generates the motor-sandwich kernel as predicated-atom IR and
 * interprets it by law over zigzag-encoded signed integers, tracing one
 * per-row output checksum (cs*256+2).  This test only VERIFIES: it
 * execs wisp and matches both checksums against an independent C
 * derivation of the same signed-integer sandwich — small-integer
 * vectors, exact by construction, no floats.  Needs no Metal device:
 * the interpreter is pure law.
 */
Test(hostcall, kernel_ir_interpreter_matches_c_reference) {
  const char* bin = getenv("ENKI_WISP_BIN");
  if (bin == NULL || bin[0] == '\0')
    cr_skip("ENKI_WISP_BIN not set; kernel-ir wisp fixture skipped");
  char cmd[512];
  (void)snprintf(cmd, sizeof(cmd), "%s foil/shrine kernel-ir main 2>&1", bin);
  FILE* p = popen(cmd, "r");
  cr_assert_not_null(p, "failed to exec %s", cmd);
  enum { OUT_CAP = 65536 };
  char* out = malloc(OUT_CAP);
  cr_assert_not_null(out);
  size_t got = fread(out, 1, OUT_CAP - 1, p);
  out[got] = '\0';
  int st = pclose(p);
  cr_assert_eq(st, 0, "kernel-ir run exited %d", st);

  /* the pure-digit lines, in trace order: cs_row0*256+2, cs_row1*256+2 */
  char* digit_lines[8] = {0};
  int nd = 0;
  for (char* line = strtok(out, "\n"); line != NULL;
       line = strtok(NULL, "\n")) {
    size_t len = strlen(line);
    if (len == 0)
      continue;
    bool digits = true;
    for (size_t i = 0; i < len; i++)
      if (line[i] < '0' || line[i] > '9')
        digits = false;
    if (digits && nd < 8)
      digit_lines[nd++] = line;
  }
  cr_assert_geq(nd, 2, "expected 2 numeric trace lines, got %d", nd);
  /* main returns 0 which may print as a final "0" line — take the two
   * substantive ones in order */
  if (nd > 2 && strcmp(digit_lines[nd - 1], "0") == 0)
    nd--;
  uint64_t wisp_cs[2];
  for (int r = 0; r < 2; r++) {
    uint64_t v = strtoull(digit_lines[nd - 2 + r], NULL, 10);
    cr_assert_eq(v % 256, 2u, "row %d trace lacks the *256+2 wrap", r);
    wisp_cs[r] = v / 256;
  }

  /* independent C reference: the signed-integer motor sandwich per
   * thread over the module's fixed small-integer vectors */
  int64_t m[32], x[2][32], mrev[32];
  for (int i = 0; i < 32; i++)
    m[i] = ((7 * i) % 11) - 5;
  for (int j = 0; j < 32; j++) {
    x[0][j] = ((3 * j) % 7) - 3;
    x[1][j] = ((5 * j) % 9) - 4;
    mrev[j] = psa_ar_sgn(j) * m[j];
  }
  for (int row = 0; row < 2; row++) {
    int64_t tmp[32] = {0}, ref[32] = {0};
    for (int i = 0; i < 32; i++)
      for (int j = 0; j < 32; j++) {
        int s, idx;
        psa_ap(i, j, &s, &idx);
        if (s != 0)
          tmp[idx] += (int64_t)s * m[i] * x[row][j];
      }
    for (int i = 0; i < 32; i++)
      for (int j = 0; j < 32; j++) {
        int s, idx;
        psa_ap(i, j, &s, &idx);
        if (s != 0)
          ref[idx] += (int64_t)s * tmp[i] * mrev[j];
      }
    uint64_t cs = 0;
    for (int k = 0; k < 32; k++) {
      int64_t v = ref[k];
      uint64_t zz = v >= 0 ? (uint64_t)(2 * v) : (uint64_t)(2 * (-v) - 1);
      cs += (uint64_t)(k + 1) * zz;
    }
    cr_assert_eq(wisp_cs[row], cs,
                 "row %d: interpreter checksum %llu != C reference %llu", row,
                 (unsigned long long)wisp_cs[row], (unsigned long long)cs);
  }
  free(out);
}

/*
 * Gap 12: a divergent replay aborts.  The recording holds one test.nop
 * call; the replay calls a different binding at the same site — the
 * (actor, binding, args-hash) verification trips ax_assume, never a
 * silent wrong answer.  Needs no Metal device: rows are rows.
 */
Test(hostcall, hostcall_replay_divergence_aborts, .signal = SIGABRT) {
  cr_assert(pl_hostcall_register("test.nop", 1, 0b1, nop_body));
  cr_assert(pl_hostcall_register("test.nop2", 1, 0b1, nop_body));
  test_rt rt = test_rt_new();
  rt.t->hostcall_f = true;
  er_log* log = er_log_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  (void)er_scheduler_adopt(sys, rt.t);
  er_scheduler_record(sys, log);
  size_t args = rt.t->vsp;
  pl_vpush(rt.t, 0);
  cr_assert_not_null(
      result_row(hostcall(rt.t, "test.nop", args), "witness", "test.nop"));
  cr_assert_eq(er_log_events(log), 1u);

  test_rt rt2 = test_rt_new();
  rt2.t->hostcall_f = true;
  er_scheduler* sys2 = er_scheduler_new(rt2.store, (er_config){0});
  (void)er_scheduler_adopt(sys2, rt2.t);
  er_scheduler_replay(sys2, log);
  args = rt2.t->vsp;
  pl_vpush(rt2.t, 0);
  (void)hostcall(rt2.t, "test.nop2", args); /* binding divergence: abort */
  cr_assert_fail("divergent replay did not abort");
}

/* ── The gfx vertical slice ────────────────────────────────────────────── */

#ifdef PL_HOST_GPU_METAL

/*
 * The backend source module: the artifact carrier the request pins.
 * One vertex and one fragment function (selected by type, never name);
 * the root payload is rows of float4 box (x y w h, 0..1) and float4
 * color — a layout the artifact declares and native never inspects.
 */
static const char gfx_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Row { float4 box; float4 color; };\n"
    "struct VOut { float4 pos [[position]]; float4 color; };\n"
    "vertex VOut shrine_vertex(uint vid [[vertex_id]],\n"
    "                          uint iid [[instance_id]],\n"
    "                          const device Row* rows [[buffer(0)]]) {\n"
    "  Row r = rows[iid];\n"
    "  float2 c[6] = {\n"
    "      float2(r.box.x, r.box.y),\n"
    "      float2(r.box.x + r.box.z, r.box.y),\n"
    "      float2(r.box.x, r.box.y + r.box.w),\n"
    "      float2(r.box.x + r.box.z, r.box.y),\n"
    "      float2(r.box.x + r.box.z, r.box.y + r.box.w),\n"
    "      float2(r.box.x, r.box.y + r.box.w),\n"
    "  };\n"
    "  float2 p = c[vid];\n"
    "  VOut o;\n"
    "  o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "  o.color = r.color;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 shrine_fragment(VOut in [[stage_in]]) {\n"
    "  return in.color;\n"
    "}\n";

#define GFX_W 64
#define GFX_H 64

/* The pinned materialization request: artifact + root payload + extents
 * + step count.  Handles never appear here — they are runtime
 * materialization complements, not request state. */
static pl_val gfx_request_pin(pl_thread* t) {
  /* root payload: one row, box {.25 .25 .5 .5}, color {1 .5 0 1} */
  float payload[8] = {0.25f, 0.25f, 0.5f, 0.5f, 1.0f, 0.5f, 0.0f, 1.0f};
  size_t base = t->vsp;
  pl_vpush(t, string_nat(t, "gfx-slice-request"));
  pl_vpush(t, bar_nat(t, (const uint8_t*)gfx_msl, sizeof(gfx_msl) - 1));
  pl_vpush(t, bar_nat(t, (const uint8_t*)payload, sizeof(payload)));
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* instance count */
  uint32_t n = (uint32_t)(t->vsp - base - 1);
  pl_gc_reserve(t, PL_APP_CELLS(1 + n));
  PL_GC_FORBID(t);
  pl_val args[1 + 8];
  args[0] = t->vstack[base];
  for (uint32_t i = 0; i < n; i++)
    args[1 + i] = t->vstack[base + 1 + i];
  pl_val row = pl_mk_app_from(t, 0, 1 + n, args);
  PL_GC_ALLOW(t);
  t->vsp = base;
  pl_vpush(t, row);
  pl_val pin = pl_pin(t, t->vstack[base]);
  t->vsp = base;
  return pin;
}

/* heap-pointer record (alloc offset length byte-tag generation). */
static pl_val pointer_record(pl_thread* t, uint64_t alloc, uint64_t offset,
                             uint64_t length, uint64_t generation) {
  size_t base = t->vsp;
  pl_vpush(t, alloc);
  pl_vpush(t, offset);
  pl_vpush(t, length);
  pl_vpush(t, 7); /* byte-tag: f32 rows */
  pl_vpush(t, generation);
  return record(t, "heap-pointer", base);
}

/* command-graph record: `draws` draw steps, with `hazards` declared
 * hazard-fact rows (fragment->fragment color-buffer triples) attached
 * to the graph and an optional timeline wait-value. */
static pl_val graph_record(pl_thread* t, uint64_t vertices, uint64_t instances,
                           uint64_t draws, uint64_t hazards, uint64_t wait) {
  size_t base = t->vsp;
  pl_vpush(t, 0); /* steps list, built back to front */
  for (uint64_t i = 0; i < draws; i++) {
    size_t sb = t->vsp;
    pl_vpush(t, 1); /* kind: draw */
    pl_vpush(t, vertices);
    pl_vpush(t, instances);
    pl_vpush(t, 0);
    pl_vpush(t, 0);
    pl_val draw = record(t, "command-step", sb);
    pl_vpush(t, draw);
    pl_vpush(t, t->vstack[base]);
    t->vstack[base] = record(t, "pair", sb);
  }
  pl_vpush(t, 0); /* hazard list at base+1 */
  for (uint64_t i = 0; i < hazards; i++) {
    size_t sb = t->vsp;
    pl_vpush(t, 2); /* before-stage: fragment */
    pl_vpush(t, 2); /* after-stage: fragment */
    pl_vpush(t, 2); /* access-class: color-buffer */
    pl_val fact = record(t, "hazard-fact", sb);
    pl_vpush(t, fact);
    pl_vpush(t, t->vstack[base + 1]);
    t->vstack[base + 1] = record(t, "pair", sb);
  }
  size_t gb = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, draws); /* step count */
  pl_vpush(t, t->vstack[base + 1]);
  pl_vpush(t, wait);
  pl_val graph = record(t, "command-graph", gb);
  t->vsp = base;
  return graph;
}

/* pass-desc (Gap 13): one color attachment with a clear load, store,
 * and no depth — the migrated shape of the old clear-step graphs. */
static pl_val pass_desc(pl_thread* t, uint64_t target, uint64_t clear_bgra8) {
  size_t base = t->vsp;
  {
    size_t f = t->vsp;
    pl_vpush(t, target);
    pl_vpush(t, 1); /* load: clear */
    pl_vpush(t, 1); /* store: store */
    pl_vpush(t, clear_bgra8);
    pl_val ca = record(t, "color-attachment", f);
    pl_vpush(t, ca);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "pair", f));
  }
  {
    size_t f = t->vsp - 1;
    pl_vpush(t, 0); /* no depth attachment */
    pl_vpush(t, 0); /* no ds-state */
    pl_val pd = record(t, "pass-desc", f);
    t->vsp = base;
    return pd;
  }
}

Test(hostcall, gfx_slice_renders_and_reads_back) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t base = t->vsp;

  /* The request is a pinned noun: pin it, then reload it through the
   * store backend by hash — the chain below runs from the loaded pin. */
  pl_vpush(t, gfx_request_pin(t));
  uint8_t hash[32];
  memcpy(hash, pl_pin_hash(t->vstack[base]), 32);
  pl_val loaded = pl_store_load(t, hash);
  cr_assert_eq(loaded, t->vstack[base]); /* interned: same pin */
  pl_cell* req = pl_as(PL_TAG_APP, pl_pin_body(pl_ptr(loaded)));
  cr_assert_not_null(req);
  cr_assert(nat_is(pl_app_args(req)[0], "gfx-slice-request"));
  pl_val msl = pl_app_args(req)[1];
  pl_val payload = pl_app_args(req)[2];
  uint64_t w = pl_nat_u64_clamp(pl_app_args(req)[3]);
  uint64_t h = pl_nat_u64_clamp(pl_app_args(req)[4]);
  uint64_t instances = pl_nat_u64_clamp(pl_app_args(req)[5]);
  size_t payload_len = pl_nat_byte_len(payload) - 1;

  /* session-open (skip cleanly when the host has no device) */
  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; gfx slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  /* arena <- the admitted memory-arena carrier */
  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);     /* scope: session */
    pl_vpush(t, 65536); /* capacity */
    pl_vpush(t, 0);     /* generation-policy: bump-on-free */
    pl_vpush(t, record(t, "memory-arena", f));
  }
  uint64_t capacity =
      witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);
  cr_assert_eq(capacity, 65536u);

  /* root payload allocation <- pointer complement witness */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, payload);
  pl_val alloc_w = hostcall(t, "gpu.alloc", args);
  uint64_t alloc_idx = witness_payload_u64(t, alloc_w, "gpu.alloc", 0);
  uint64_t alloc_len = witness_payload_u64(t, alloc_w, "gpu.alloc", 2);
  uint64_t alloc_gen = witness_payload_u64(t, alloc_w, "gpu.alloc", 3);
  cr_assert_eq(alloc_len, (uint64_t)payload_len);
  cr_assert_eq(alloc_gen, 1u);

  /* library <- the pinned artifact bytes */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, msl);
  uint64_t library = witness_payload_u64(t, hostcall(t, "gpu.library", args),
                                         "gpu.library", 0);
  cr_assert_neq(library, 0u);

  /* pipeline <- same artifact key; second request must hit the cache */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, msl);
  pl_val pipe_w = hostcall(t, "gpu.pipeline-render", args);
  uint64_t pipeline = witness_payload_u64(t, pipe_w, "gpu.pipeline-render", 0);
  cr_assert_eq(witness_payload_u64(t, pipe_w, "gpu.pipeline-render", 1), 0u);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, msl);
  pl_val pipe_w2 = hostcall(t, "gpu.pipeline-render", args);
  cr_assert_eq(witness_payload_u64(t, pipe_w2, "gpu.pipeline-render", 1), 1u);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, w);
  pl_vpush(t, h);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t target =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  /* the generic command graph: clear(opaque black) then draw */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, instances, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, alloc_idx, 0, alloc_len, alloc_gen));
  pl_vpush(t, 0); /* no descriptor heap */
  pl_val graph_w = hostcall(t, "gpu.command-graph", args);
  cr_assert_eq(witness_payload_u64(t, graph_w, "gpu.command-graph", 0), 1u);
  cr_assert_eq(witness_payload_u64(t, graph_w, "gpu.command-graph", 1), 0u);
  uint64_t event_value =
      witness_payload_u64(t, graph_w, "gpu.command-graph", 2);
  cr_assert_eq(event_value, 1u);

  /* readback witnesses: BGRA8 bytes as u32le (A<<24|R<<16|G<<8|B) */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, target);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  uint64_t center = witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                        "gpu.readback", 0);
  cr_assert_eq((center >> 24) & 0xff, 0xffu); /* a */
  cr_assert_eq((center >> 16) & 0xff, 0xffu); /* r */
  cr_assert_geq((center >> 8) & 0xff, 0x7fu); /* g ~ 0.5 */
  cr_assert_leq((center >> 8) & 0xff, 0x80u);
  cr_assert_eq(center & 0xff, 0x00u); /* b */

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, target);
  pl_vpush(t, 1);
  pl_vpush(t, 1);
  uint64_t corner = witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                        "gpu.readback", 0);
  cr_assert_eq(corner, 0xff000000u); /* clear color from the graph */

  /* a pointer window past the allocation refuses with bounds */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, instances, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, alloc_idx, 0, alloc_len * 2, alloc_gen));
  pl_vpush(t, 0);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.command-graph", args), "gpu.command-graph"),
      948u);

  /* readback outside the target refuses with bounds */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, target);
  pl_vpush(t, GFX_W);
  pl_vpush(t, 0);
  cr_assert_eq(refusal_kind(hostcall(t, "gpu.readback", args), "gpu.readback"),
               948u);

  /* gpu.free consumes the complement: the generation bumps and any
   * further use of the outdated complement is the forbidden ACL
   * contraction — refusal 950, never a dangling read */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pointer_record(t, alloc_idx, 0, alloc_len, alloc_gen));
  pl_val freed = hostcall(t, "gpu.free", args);
  cr_assert_eq(witness_payload_u64(t, freed, "gpu.free", 1), alloc_gen + 1);

  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, instances, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, alloc_idx, 0, alloc_len, alloc_gen));
  pl_vpush(t, 0);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.command-graph", args), "gpu.command-graph"),
      950u);

  /* a fresh allocation reuses the slot at the bumped generation */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, payload);
  pl_val realloc_w = hostcall(t, "gpu.alloc", args);
  cr_assert_eq(witness_payload_u64(t, realloc_w, "gpu.alloc", 0), alloc_idx);
  cr_assert_eq(witness_payload_u64(t, realloc_w, "gpu.alloc", 3),
               alloc_gen + 2);

  args = t->vsp;
  pl_vpush(t, session);
  pl_val closed = hostcall(t, "gpu.session-close", args);
  cr_assert_not_null(result_row(closed, "witness", "gpu.session-close"));

  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/*
 * Gap 4: the descriptor heap.  Descriptors are bytes at index x stride;
 * the shader reads its texture purely through a heap index — no
 * per-draw binding call names the texture.  Sampling state is constexpr
 * in the artifact (no API sampler objects).
 */
static const char gfx_textured_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Row { float4 box; float4 color; };\n"
    "struct DHeap { texture2d<float> tex; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut shrine_vertex(uint vid [[vertex_id]],\n"
    "                          uint iid [[instance_id]],\n"
    "                          const device Row* rows [[buffer(0)]]) {\n"
    "  Row r = rows[iid];\n"
    "  float2 c[6] = {\n"
    "      float2(r.box.x, r.box.y),\n"
    "      float2(r.box.x + r.box.z, r.box.y),\n"
    "      float2(r.box.x, r.box.y + r.box.w),\n"
    "      float2(r.box.x + r.box.z, r.box.y),\n"
    "      float2(r.box.x + r.box.z, r.box.y + r.box.w),\n"
    "      float2(r.box.x, r.box.y + r.box.w),\n"
    "  };\n"
    "  float2 p = c[vid];\n"
    "  VOut o;\n"
    "  o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "  o.uv = (p - r.box.xy) / r.box.zw;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 shrine_fragment(VOut in [[stage_in]],\n"
    "                                const device DHeap* heap [[buffer(1)]]) "
    "{\n"
    "  constexpr sampler s(filter::nearest);\n"
    "  return heap[0].tex.sample(s, in.uv);\n"
    "}\n";

/* descriptor-window record (heap index stride-class generation). */
static pl_val window_record(pl_thread* t, uint64_t heap, uint64_t index,
                            uint64_t generation) {
  size_t base = t->vsp;
  pl_vpush(t, heap);
  pl_vpush(t, index);
  pl_vpush(t, 1); /* stride-class: sampled */
  pl_vpush(t, generation);
  return record(t, "descriptor-window", base);
}

Test(hostcall, gfx_textured_draw_samples_descriptor_heap) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during textured slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; textured slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  /* 2x2 BGRA8 quadrants: TL red, TR green, BL blue, BR white */
  static const uint8_t texels[16] = {
      0,   0, 255, 255, 0,   255, 0,   255, /* row 0: red, green */
      255, 0, 0,   255, 255, 255, 255, 255, /* row 1: blue, white */
  };
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, 2);
  pl_vpush(t, 2);
  pl_vpush(t, bar_nat(t, texels, sizeof(texels)));
  pl_vpush(t, 1); /* bgra8 */
  uint64_t texture = witness_payload_u64(t, hostcall(t, "gpu.texture-2d", args),
                                         "gpu.texture-2d", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 4); /* count */
    pl_vpush(t, 1); /* stride-class: sampled */
    pl_vpush(t, record(t, "descriptor-heap", f));
  }
  pl_val dheap_w = hostcall(t, "gpu.descriptor-heap", args);
  uint64_t dheap = witness_payload_u64(t, dheap_w, "gpu.descriptor-heap", 0);
  cr_assert_eq(witness_payload_u64(t, dheap_w, "gpu.descriptor-heap", 1), 4u);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, dheap);
  pl_vpush(t, window_record(t, dheap, 0, 1));
  pl_vpush(t, texture);
  pl_val wrote = hostcall(t, "gpu.descriptor-write", args);
  cr_assert_eq(witness_payload_u64(t, wrote, "gpu.descriptor-write", 0), 0u);

  /* a stale window generation refuses (950); an index outside the heap
   * refuses (948) */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, dheap);
  pl_vpush(t, window_record(t, dheap, 0, 2));
  pl_vpush(t, texture);
  cr_assert_eq(refusal_kind(hostcall(t, "gpu.descriptor-write", args),
                            "gpu.descriptor-write"),
               950u);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, dheap);
  pl_vpush(t, window_record(t, dheap, 5, 1));
  pl_vpush(t, texture);
  cr_assert_eq(refusal_kind(hostcall(t, "gpu.descriptor-write", args),
                            "gpu.descriptor-write"),
               948u);

  pl_val msl = bar_nat(t, (const uint8_t*)gfx_textured_msl,
                       sizeof(gfx_textured_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl); /* root across calls */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t pipeline = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t target =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  /* full-target quad; color field unused by the textured artifact */
  float payload[8] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, bar_nat(t, (const uint8_t*)payload, sizeof(payload)));
  pl_val alloc_w = hostcall(t, "gpu.alloc", args);
  uint64_t alloc_idx = witness_payload_u64(t, alloc_w, "gpu.alloc", 0);
  uint64_t alloc_len = witness_payload_u64(t, alloc_w, "gpu.alloc", 2);
  uint64_t alloc_gen = witness_payload_u64(t, alloc_w, "gpu.alloc", 3);

  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, alloc_idx, 0, alloc_len, alloc_gen));
  pl_vpush(t, dheap);
  (void)witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                            "gpu.command-graph", 0);

  /* quadrant centers read the sampled texel colors */
  static const struct {
    uint64_t x, y;
    uint64_t bgra;
  } probes[4] = {
      {GFX_W / 4, GFX_H / 4, 0xffff0000u},         /* red */
      {3 * GFX_W / 4, GFX_H / 4, 0xff00ff00u},     /* green */
      {GFX_W / 4, 3 * GFX_H / 4, 0xff0000ffu},     /* blue */
      {3 * GFX_W / 4, 3 * GFX_H / 4, 0xffffffffu}, /* white */
  };
  for (int i = 0; i < 4; i++) {
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, target);
    pl_vpush(t, probes[i].x);
    pl_vpush(t, probes[i].y);
    uint64_t pixel = witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                         "gpu.readback", 0);
    cr_assert_eq(pixel, probes[i].bgra, "quadrant %d: got %08llx", i,
                 (unsigned long long)pixel);
  }

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/*
 * Gap 5: declared hazard facts lower to pass splits between draw steps;
 * absent facts mean no split.  The witness carries (executed, splits,
 * event-value) and the event value is the only thing readback waits on.
 */
Test(hostcall, gfx_hazard_facts_lower_to_pass_splits) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during hazard slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; hazard slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  pl_val msl = bar_nat(t, (const uint8_t*)gfx_msl, sizeof(gfx_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t pipeline = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t target =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  float payload[8] = {0.25f, 0.25f, 0.5f, 0.5f, 1.0f, 0.5f, 0.0f, 1.0f};
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, bar_nat(t, (const uint8_t*)payload, sizeof(payload)));
  pl_val alloc_w = hostcall(t, "gpu.alloc", args);
  uint64_t alloc_idx = witness_payload_u64(t, alloc_w, "gpu.alloc", 0);
  uint64_t alloc_len = witness_payload_u64(t, alloc_w, "gpu.alloc", 2);
  uint64_t alloc_gen = witness_payload_u64(t, alloc_w, "gpu.alloc", 3);

  /* two draws, no declared fact: one pass, zero splits */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 2, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, alloc_idx, 0, alloc_len, alloc_gen));
  pl_vpush(t, 0);
  pl_val no_fact = hostcall(t, "gpu.command-graph", args);
  cr_assert_eq(witness_payload_u64(t, no_fact, "gpu.command-graph", 0), 2u);
  cr_assert_eq(witness_payload_u64(t, no_fact, "gpu.command-graph", 1), 0u);

  /* two draws with a declared fact: the hazard lowers to one split */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 2, 1, 0));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, alloc_idx, 0, alloc_len, alloc_gen));
  pl_vpush(t, 0);
  pl_val with_fact = hostcall(t, "gpu.command-graph", args);
  cr_assert_eq(witness_payload_u64(t, with_fact, "gpu.command-graph", 0), 2u);
  cr_assert_eq(witness_payload_u64(t, with_fact, "gpu.command-graph", 1), 1u);
  cr_assert_eq(witness_payload_u64(t, with_fact, "gpu.command-graph", 2), 2u);

  /* the split-pass render still lands the right pixels */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, target);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  uint64_t center = witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                        "gpu.readback", 0);
  cr_assert_eq((center >> 16) & 0xff, 0xffu);
  cr_assert_eq(center & 0xff, 0x00u);

  /* a non-hazard-fact row in the hazard list refuses as graph shape */
  args = t->vsp;
  {
    size_t gb = t->vsp;
    size_t sb;
    pl_vpush(t, 0); /* steps: none — count mismatch is fine, hazards
                       are validated first */
    sb = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 2);
    pl_val bogus = record(t, "pair", sb); /* pair is not a hazard-fact */
    sb = t->vsp;
    pl_vpush(t, bogus);
    pl_vpush(t, 0);
    pl_val hazards = record(t, "pair", sb);
    pl_vpush(t, t->vstack[gb]);
    pl_vpush(t, 0);
    pl_vpush(t, hazards);
    pl_vpush(t, 0);
    pl_val graph = record(t, "command-graph", t->vsp - 4);
    t->vsp = gb;
    pl_vpush(t, graph);
  }
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, alloc_idx, 0, alloc_len, alloc_gen));
  pl_vpush(t, 0);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.command-graph", args), "gpu.command-graph"),
      952u);

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/*
 * The exit-criterion fixture: a non-graphics PSA kernel (the motor
 * sandwich — rotor + translator applied to point rows) dispatched as a
 * generic command-graph step against an arena-allocated root, readback
 * witness matching the CPU table-walker.  The root struct is the
 * NoGraphicsApi root-arguments discipline made literal: one pointer at
 * buffer(0), 64-bit device addresses inside it that only the shader
 * chases (the whole arena is resident via useHeap).
 */
static const char gfx_motor_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Root {\n"
    "  float4 q;        // rotor quaternion (x y z w)\n"
    "  float4 t;        // translator (xyz; w unused)\n"
    "  ulong in_addr;   // device address of input point rows\n"
    "  ulong out_addr;  // device address of output point rows\n"
    "  uint count;\n"
    "};\n"
    "kernel void shrine_motor_sandwich(uint tid "
    "[[thread_position_in_grid]],\n"
    "                                  const device Root* d [[buffer(0)]]) "
    "{\n"
    "  if (tid >= d->count) return;\n"
    "  const device float4* in = (const device float4*)(d->in_addr);\n"
    "  device float4* out = (device float4*)(d->out_addr);\n"
    "  float3 p = in[tid].xyz;\n"
    "  float3 u = d->q.xyz;\n"
    "  float w = d->q.w;\n"
    "  float3 r = p + 2.0f * cross(u, cross(u, p) + w * p);\n"
    "  out[tid] = float4(r + d->t.xyz, 1.0f);\n"
    "}\n";

/* The CPU table-walker the readback witness must match. */
static void motor_sandwich_cpu(const float q[4], const float tr[4],
                               const float* in, float* out, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    const float* p = &in[i * 4];
    float c1[3] = {q[1] * p[2] - q[2] * p[1], q[2] * p[0] - q[0] * p[2],
                   q[0] * p[1] - q[1] * p[0]};
    float v[3] = {c1[0] + q[3] * p[0], c1[1] + q[3] * p[1],
                  c1[2] + q[3] * p[2]};
    float c2[3] = {q[1] * v[2] - q[2] * v[1], q[2] * v[0] - q[0] * v[2],
                   q[0] * v[1] - q[1] * v[0]};
    out[i * 4 + 0] = p[0] + 2.0f * c2[0] + tr[0];
    out[i * 4 + 1] = p[1] + 2.0f * c2[1] + tr[1];
    out[i * 4 + 2] = p[2] + 2.0f * c2[2] + tr[2];
    out[i * 4 + 3] = 1.0f;
  }
}

/* compute command-graph record: `dispatches` x dispatch {gx gy gz tw}
 * with `hazards` declared compute-stage buffer facts. */
static pl_val graph_compute_record(pl_thread* t, uint64_t gx, uint64_t gy,
                                   uint64_t gz, uint64_t tw,
                                   uint64_t dispatches, uint64_t hazards,
                                   uint64_t wait) {
  size_t base = t->vsp;
  pl_vpush(t, 0); /* steps list */
  for (uint64_t i = 0; i < dispatches; i++) {
    size_t sb = t->vsp;
    pl_vpush(t, 3); /* kind: dispatch */
    pl_vpush(t, gx);
    pl_vpush(t, gy);
    pl_vpush(t, gz);
    pl_vpush(t, tw);
    pl_val step = record(t, "command-step", sb);
    pl_vpush(t, step);
    pl_vpush(t, t->vstack[base]);
    t->vstack[base] = record(t, "pair", sb);
  }
  pl_vpush(t, 0); /* hazard list at base+1 */
  for (uint64_t i = 0; i < hazards; i++) {
    size_t sb = t->vsp;
    pl_vpush(t, 4); /* before-stage: compute */
    pl_vpush(t, 4); /* after-stage: compute */
    pl_vpush(t, 0); /* access-class: buffers */
    pl_val fact = record(t, "hazard-fact", sb);
    pl_vpush(t, fact);
    pl_vpush(t, t->vstack[base + 1]);
    t->vstack[base + 1] = record(t, "pair", sb);
  }
  size_t gb = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, dispatches);
  pl_vpush(t, t->vstack[base + 1]);
  pl_vpush(t, wait);
  pl_val graph = record(t, "command-graph", gb);
  t->vsp = base;
  return graph;
}

Test(hostcall, gfx_motor_sandwich_dispatches_through_graph) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during motor slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; motor slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  /* motor: 90-degree rotation about z, then translate (1, 2, 3) */
  enum { POINTS = 8 };
  const float q[4] = {0.0f, 0.0f, 0.70710678f, 0.70710678f};
  const float tr[4] = {1.0f, 2.0f, 3.0f, 0.0f};
  float points[POINTS * 4];
  for (int i = 0; i < POINTS; i++) {
    points[i * 4 + 0] = (float)i * 0.5f;
    points[i * 4 + 1] = (float)(i % 3) - 1.0f;
    points[i * 4 + 2] = (float)(i % 5) * 0.25f;
    points[i * 4 + 3] = 1.0f;
  }

  /* input + output rows live in sibling arena allocations the root
   * references by device address */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, bar_nat(t, (const uint8_t*)points, sizeof(points)));
  pl_val in_w = hostcall(t, "gpu.alloc", args);
  uint64_t in_idx = witness_payload_u64(t, in_w, "gpu.alloc", 0);
  uint64_t in_gen = witness_payload_u64(t, in_w, "gpu.alloc", 3);

  uint8_t zeros[POINTS * 16] = {0};
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, bar_nat(t, zeros, sizeof(zeros)));
  pl_val out_w = hostcall(t, "gpu.alloc", args);
  uint64_t out_idx = witness_payload_u64(t, out_w, "gpu.alloc", 0);
  uint64_t out_gen = witness_payload_u64(t, out_w, "gpu.alloc", 3);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pointer_record(t, in_idx, 0, sizeof(points), in_gen));
  uint64_t in_addr = witness_payload_u64(
      t, hostcall(t, "gpu.device-address", args), "gpu.device-address", 0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pointer_record(t, out_idx, 0, sizeof(zeros), out_gen));
  uint64_t out_addr = witness_payload_u64(
      t, hostcall(t, "gpu.device-address", args), "gpu.device-address", 0);
  cr_assert_neq(in_addr, 0u);
  cr_assert_neq(out_addr, 0u);

  /* the root payload: q, t, in/out device addresses, count (matches
   * the MSL Root struct layout, 64 bytes) */
  uint8_t root[64] = {0};
  memcpy(root + 0, q, 16);
  memcpy(root + 16, tr, 16);
  memcpy(root + 32, &in_addr, 8);
  memcpy(root + 40, &out_addr, 8);
  uint32_t count = POINTS;
  memcpy(root + 48, &count, 4);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, bar_nat(t, root, sizeof(root)));
  pl_val root_w = hostcall(t, "gpu.alloc", args);
  uint64_t root_idx = witness_payload_u64(t, root_w, "gpu.alloc", 0);
  uint64_t root_gen = witness_payload_u64(t, root_w, "gpu.alloc", 3);

  pl_val msl =
      bar_nat(t, (const uint8_t*)gfx_motor_msl, sizeof(gfx_motor_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  pl_val pipe_w = hostcall(t, "gpu.pipeline-compute", args);
  uint64_t kernel = witness_payload_u64(t, pipe_w, "gpu.pipeline-compute", 0);
  cr_assert_eq(witness_payload_u64(t, pipe_w, "gpu.pipeline-compute", 1), 0u);

  /* two dispatches with a declared compute->compute buffer fact: the
   * second redundantly recomputes the same rows; the fact lowers to
   * one real memory barrier inside the concurrent encoder */
  args = t->vsp;
  pl_vpush(t, graph_compute_record(t, 1, 1, 1, 64, 2, 1, 0));
  pl_vpush(t, session);
  pl_vpush(t, kernel);
  pl_vpush(t, 0); /* compute graph takes no target */
  pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(root), root_gen));
  pl_vpush(t, 0);
  pl_val graph_w = hostcall(t, "gpu.command-graph", args);
  cr_assert_eq(witness_payload_u64(t, graph_w, "gpu.command-graph", 0), 2u);
  cr_assert_eq(witness_payload_u64(t, graph_w, "gpu.command-graph", 1), 1u);

  /* readback through the complement; the witness must match the CPU
   * table-walker */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pointer_record(t, out_idx, 0, sizeof(zeros), out_gen));
  pl_val read_w = hostcall(t, "gpu.read", args);
  pl_cell* row = result_row(read_w, "witness", "gpu.read");
  cr_assert_not_null(row);
  pl_val bytes = pl_app_args(row)[2];
  cr_assert_eq(pl_nat_byte_len(bytes), sizeof(zeros) + 1);
  float gpu_out[POINTS * 4];
  for (size_t i = 0; i < sizeof(zeros); i++)
    ((uint8_t*)gpu_out)[i] = pl_nat_byte_at(bytes, i);

  float cpu_out[POINTS * 4];
  motor_sandwich_cpu(q, tr, points, cpu_out, POINTS);
  for (int i = 0; i < POINTS * 4; i++) {
    float diff = gpu_out[i] - cpu_out[i];
    if (diff < 0.0f)
      diff = -diff;
    cr_assert(diff < 1e-5f, "row %d component %d: gpu %f cpu %f", i / 4, i % 4,
              (double)gpu_out[i], (double)cpu_out[i]);
  }

  /* a draw step in a compute graph refuses as graph shape */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, kernel);
  pl_vpush(t, 0);
  pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(root), root_gen));
  pl_vpush(t, 0);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.command-graph", args), "gpu.command-graph"),
      952u);

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/* ── Slice 2 helpers ───────────────────────────────────────────────────── */

/* gpu.alloc wrapper: returns the allocation index, writes generation. */
static uint64_t alloc_bytes(pl_thread* t, uint64_t session, const void* data,
                            size_t n, uint64_t* out_gen) {
  size_t args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, bar_nat(t, (const uint8_t*)data, n));
  pl_val w = hostcall(t, "gpu.alloc", args);
  *out_gen = witness_payload_u64(t, w, "gpu.alloc", 3);
  return witness_payload_u64(t, w, "gpu.alloc", 0);
}

/* gpu.device-address wrapper over a whole allocation window. */
static uint64_t dev_addr(pl_thread* t, uint64_t session, uint64_t idx,
                         size_t len, uint64_t gen) {
  size_t args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pointer_record(t, idx, 0, len, gen));
  return witness_payload_u64(t, hostcall(t, "gpu.device-address", args),
                             "gpu.device-address", 0);
}

/* gpu.read wrapper: copies the window bytes out of the witness bar. */
static void read_bytes(pl_thread* t, uint64_t session, uint64_t idx, size_t len,
                       uint64_t gen, void* out) {
  size_t args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pointer_record(t, idx, 0, len, gen));
  pl_val w = hostcall(t, "gpu.read", args);
  pl_cell* row = result_row(w, "witness", "gpu.read");
  cr_assert_not_null(row);
  pl_val bytes = pl_app_args(row)[2];
  cr_assert_eq(pl_nat_byte_len(bytes), len + 1);
  for (size_t i = 0; i < len; i++)
    ((uint8_t*)out)[i] = pl_nat_byte_at(bytes, i);
}

/*
 * Gap 8: submission dependencies.  The wait-value field is the
 * split-barrier consumer half (the producer half is the signal value
 * every graph witness carries).  Absent a declared dependency,
 * submission order is genuinely unconstrained — hazards gate
 * SCHEDULING (CB9) — so the negative fixture exercises that freedom
 * adversarially: it runs the consumer to completion before the
 * producer is even submitted.  Stale bytes, deterministically.
 */
static const char gfx_add_one_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Root { ulong in_addr; ulong out_addr; uint count; };\n"
    "kernel void shrine_add_one(uint tid [[thread_position_in_grid]],\n"
    "                           const device Root* d [[buffer(0)]]) {\n"
    "  if (tid >= d->count) return;\n"
    "  const device float4* in = (const device float4*)(d->in_addr);\n"
    "  device float4* out = (device float4*)(d->out_addr);\n"
    "  out[tid] = in[tid] + 1.0f;\n"
    "}\n";

/* Build + alloc an add-one root {in_addr, out_addr, count}. */
static void chain_root(pl_thread* t, uint64_t session, uint64_t in_a,
                       uint64_t out_a, uint32_t count, uint64_t* out_idx,
                       uint64_t* out_gen) {
  uint8_t root[32] = {0};
  memcpy(root + 0, &in_a, 8);
  memcpy(root + 8, &out_a, 8);
  memcpy(root + 16, &count, 4);
  *out_idx = alloc_bytes(t, session, root, sizeof(root), out_gen);
}

/* Submit an add-one dispatch graph; returns its signal value. */
static uint64_t chain_dispatch(pl_thread* t, uint64_t session, uint64_t kernel,
                               uint64_t root_idx, uint64_t root_gen,
                               uint64_t wait) {
  size_t args = t->vsp;
  pl_vpush(t, graph_compute_record(t, 1, 1, 1, 64, 1, 0, wait));
  pl_vpush(t, session);
  pl_vpush(t, kernel);
  pl_vpush(t, 0);
  pl_vpush(t, pointer_record(t, root_idx, 0, 32, root_gen));
  pl_vpush(t, 0);
  return witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                             "gpu.command-graph", 2);
}

Test(hostcall, gfx_dependency_facts_gate_scheduling) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during dependency slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; dependency slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  pl_val msl =
      bar_nat(t, (const uint8_t*)gfx_add_one_msl, sizeof(gfx_add_one_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t kernel = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-compute", args), "gpu.pipeline-compute", 0);

  enum { ROWS = 4 };
  float src[ROWS * 4];
  for (int i = 0; i < ROWS * 4; i++)
    src[i] = (float)(i * 10);
  uint8_t zeros[ROWS * 16] = {0};

  uint64_t src_gen, x_gen, y_gen;
  uint64_t src_idx = alloc_bytes(t, session, src, sizeof(src), &src_gen);
  uint64_t x_idx = alloc_bytes(t, session, zeros, sizeof(zeros), &x_gen);
  uint64_t y_idx = alloc_bytes(t, session, zeros, sizeof(zeros), &y_gen);
  uint64_t src_a = dev_addr(t, session, src_idx, sizeof(src), src_gen);
  uint64_t x_a = dev_addr(t, session, x_idx, sizeof(zeros), x_gen);
  uint64_t y_a = dev_addr(t, session, y_idx, sizeof(zeros), y_gen);

  /* NEGATIVE: producer A (src -> X) and consumer B (X -> Y) with NO
   * declared dependency.  Submission order is free — run B to
   * completion before A is even submitted.  Y reads the stale X
   * (zeros), deterministically: Y = 0 + 1. */
  uint64_t ra_idx, ra_gen, rb_idx, rb_gen;
  chain_root(t, session, src_a, x_a, ROWS, &ra_idx, &ra_gen);
  chain_root(t, session, x_a, y_a, ROWS, &rb_idx, &rb_gen);

  (void)chain_dispatch(t, session, kernel, rb_idx, rb_gen, 0); /* B first */
  float y_stale[ROWS * 4];
  read_bytes(t, session, y_idx, sizeof(zeros), y_gen, y_stale);
  for (int i = 0; i < ROWS * 4; i++)
    cr_assert(y_stale[i] == 1.0f, "stale Y[%d] = %f, want 1.0 (= f(0))", i,
              (double)y_stale[i]);

  (void)chain_dispatch(t, session, kernel, ra_idx, ra_gen, 0); /* A late */
  float x_now[ROWS * 4];
  read_bytes(t, session, x_idx, sizeof(zeros), x_gen, x_now);
  for (int i = 0; i < ROWS * 4; i++)
    cr_assert(x_now[i] == src[i] + 1.0f, "X[%d] = %f, want %f", i,
              (double)x_now[i], (double)(src[i] + 1.0f));

  /* POSITIVE: declare the dependency.  A2 (src -> X2) signals; B2
   * (X2 -> Y2) waits A2's signal value; an independent graph C runs
   * between them without waiting.  Y2 = src + 2, guaranteed. */
  uint64_t x2_gen, y2_gen;
  uint64_t x2_idx = alloc_bytes(t, session, zeros, sizeof(zeros), &x2_gen);
  uint64_t y2_idx = alloc_bytes(t, session, zeros, sizeof(zeros), &y2_gen);
  uint64_t x2_a = dev_addr(t, session, x2_idx, sizeof(zeros), x2_gen);
  uint64_t y2_a = dev_addr(t, session, y2_idx, sizeof(zeros), y2_gen);
  uint64_t ra2_idx, ra2_gen, rb2_idx, rb2_gen;
  chain_root(t, session, src_a, x2_a, ROWS, &ra2_idx, &ra2_gen);
  chain_root(t, session, x2_a, y2_a, ROWS, &rb2_idx, &rb2_gen);

  uint64_t sig_a = chain_dispatch(t, session, kernel, ra2_idx, ra2_gen, 0);
  /* independent work between the signal and its wait (the cascading
   * overlap shape): C never waits */
  uint64_t sig_c = chain_dispatch(t, session, kernel, ra_idx, ra_gen, 0);
  uint64_t sig_b = chain_dispatch(t, session, kernel, rb2_idx, rb2_gen, sig_a);
  cr_assert_gt(sig_c, sig_a);
  cr_assert_gt(sig_b, sig_c);

  float y2[ROWS * 4];
  read_bytes(t, session, y2_idx, sizeof(zeros), y2_gen, y2);
  for (int i = 0; i < ROWS * 4; i++)
    cr_assert(y2[i] == src[i] + 2.0f, "ordered Y2[%d] = %f, want %f", i,
              (double)y2[i], (double)(src[i] + 2.0f));

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/*
 * Gap 5's deferred negative RAW pixel fixture, closed by the first
 * multi-pass render chain: pass A renders into target T, pass B samples
 * T through the descriptor heap into target U — the cross-encoder
 * dependency the single-pass fixtures could not construct (Metal
 * guarantees primitive order to the attachment within one pass).  Per
 * CB9 hazards gate scheduling: with no declared dependency, submission
 * order is free, and the fixture exercises that freedom adversarially —
 * B runs to completion before A is even submitted, so B samples the
 * stale T, deterministically and lawfully.  With the dependency
 * declared (B waits A's signal value — Gap 8's consumer half), the
 * same chain reads the produced pixel.  Render targets are sampleable
 * (RenderTarget|ShaderRead), so writing a target handle into a
 * descriptor window is the whole bridge; no new native mechanics.
 */
Test(hostcall, gfx_cross_pass_raw_pixel_divergence) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during cross-pass slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; cross-pass slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  /* producer pipeline: plain colored quad; consumer pipeline: samples
   * heap[0].tex */
  pl_val prod_msl = bar_nat(t, (const uint8_t*)gfx_msl, sizeof(gfx_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, prod_msl);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t producer = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);
  t->vstack[keep] = bar_nat(t, (const uint8_t*)gfx_textured_msl,
                            sizeof(gfx_textured_msl) - 1);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t consumer = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);

  /* T: pass A's attachment, sampled by pass B.  U: pass B's attachment. */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t t_target =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t u_target =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1); /* count */
    pl_vpush(t, 1); /* stride-class: sampled */
    pl_vpush(t, record(t, "descriptor-heap", f));
  }
  uint64_t dheap = witness_payload_u64(
      t, hostcall(t, "gpu.descriptor-heap", args), "gpu.descriptor-heap", 0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, dheap);
  pl_vpush(t, window_record(t, dheap, 0, 1));
  pl_vpush(t, t_target);
  (void)witness_payload_u64(t, hostcall(t, "gpu.descriptor-write", args),
                            "gpu.descriptor-write", 0);

  /* roots: producer draws a full-target red quad; the consumer's color
   * field is unused by the textured artifact */
  float prod_payload[8] = {0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f};
  float cons_payload[8] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, bar_nat(t, (const uint8_t*)prod_payload, sizeof(prod_payload)));
  pl_val prod_w = hostcall(t, "gpu.alloc", args);
  uint64_t prod_idx = witness_payload_u64(t, prod_w, "gpu.alloc", 0);
  uint64_t prod_len = witness_payload_u64(t, prod_w, "gpu.alloc", 2);
  uint64_t prod_gen = witness_payload_u64(t, prod_w, "gpu.alloc", 3);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, bar_nat(t, (const uint8_t*)cons_payload, sizeof(cons_payload)));
  pl_val cons_w = hostcall(t, "gpu.alloc", args);
  uint64_t cons_idx = witness_payload_u64(t, cons_w, "gpu.alloc", 0);
  uint64_t cons_len = witness_payload_u64(t, cons_w, "gpu.alloc", 2);
  uint64_t cons_gen = witness_payload_u64(t, cons_w, "gpu.alloc", 3);

  enum { STALE_BLUE = 0xff0000ffu, RED = 0xffff0000u, BLACK = 0xff000000u };

  /* seed T with a deterministic stale color (clear-only graph) and
   * force completion through the readback wait */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 0, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, producer);
  pl_vpush(t, pass_desc(t, t_target, STALE_BLUE));
  pl_vpush(t, pointer_record(t, prod_idx, 0, prod_len, prod_gen));
  pl_vpush(t, 0);
  (void)witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                            "gpu.command-graph", 0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t_target);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                   "gpu.readback", 0),
               (uint64_t)STALE_BLUE);

  /* NEGATIVE: the A->B RAW dependency (A writes T, B samples T) is NOT
   * declared.  Submission order is free — run B to completion before A
   * is even submitted.  B samples the stale T, every time. */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, consumer);
  pl_vpush(t, pass_desc(t, u_target, BLACK));
  pl_vpush(t, pointer_record(t, cons_idx, 0, cons_len, cons_gen));
  pl_vpush(t, dheap);
  (void)witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                            "gpu.command-graph", 0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, u_target);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  uint64_t u_stale = witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                         "gpu.readback", 0);
  cr_assert_eq(u_stale, (uint64_t)STALE_BLUE,
               "undeclared RAW: U sampled %08llx, want stale %08x",
               (unsigned long long)u_stale, STALE_BLUE);

  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0)); /* A, late */
  pl_vpush(t, session);
  pl_vpush(t, producer);
  pl_vpush(t, pass_desc(t, t_target, BLACK));
  pl_vpush(t, pointer_record(t, prod_idx, 0, prod_len, prod_gen));
  pl_vpush(t, 0);
  (void)witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                            "gpu.command-graph", 0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t_target);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                   "gpu.readback", 0),
               (uint64_t)RED); /* A really ran — after B */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, u_target);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                   "gpu.readback", 0),
               (uint64_t)STALE_BLUE); /* the stale read persisted */

  /* POSITIVE: same chain with the dependency declared.  Re-seed T
   * stale, then submit A2 and B2 back to back — B2 waits A2's signal
   * value, so it samples the produced pixel. */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 0, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, producer);
  pl_vpush(t, pass_desc(t, t_target, STALE_BLUE));
  pl_vpush(t, pointer_record(t, prod_idx, 0, prod_len, prod_gen));
  pl_vpush(t, 0);
  (void)witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                            "gpu.command-graph", 0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t_target);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                   "gpu.readback", 0),
               (uint64_t)STALE_BLUE);

  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0)); /* A2 */
  pl_vpush(t, session);
  pl_vpush(t, producer);
  pl_vpush(t, pass_desc(t, t_target, BLACK));
  pl_vpush(t, pointer_record(t, prod_idx, 0, prod_len, prod_gen));
  pl_vpush(t, 0);
  uint64_t sig_a2 = witness_payload_u64(
      t, hostcall(t, "gpu.command-graph", args), "gpu.command-graph", 2);
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, sig_a2)); /* B2 waits A2 */
  pl_vpush(t, session);
  pl_vpush(t, consumer);
  pl_vpush(t, pass_desc(t, u_target, BLACK));
  pl_vpush(t, pointer_record(t, cons_idx, 0, cons_len, cons_gen));
  pl_vpush(t, dheap);
  uint64_t sig_b2 = witness_payload_u64(
      t, hostcall(t, "gpu.command-graph", args), "gpu.command-graph", 2);
  cr_assert_gt(sig_b2, sig_a2);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, u_target);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  uint64_t u_fresh = witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                         "gpu.readback", 0);
  cr_assert_eq(u_fresh, (uint64_t)RED,
               "declared RAW: U sampled %08llx, want produced %08x",
               (unsigned long long)u_fresh, RED);

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/*
 * Gap 11: the data-path completions.  gpu.write is consume-and-mint —
 * a pointer complement is a linear residual, so the write waits the
 * session timeline (the Producer-side dual of gpu.read), memcpys, and
 * bumps the generation: every prior complement over the allocation is
 * stale (950).  gpu.wait waits a signaled value without reading bytes
 * (the frames-in-flight primitive).  The blit graph (pipeline 0)
 * carries copy steps — buffer→buffer and buffer↔texture — behind the
 * same carrier/wait-value/signal discipline; destination generations
 * are minted at encode and appended to the witness in step order.
 */
Test(hostcall, gfx_write_copy_wait_data_path) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during data-path slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; data-path slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  uint64_t gen_a = 0;
  uint64_t a = alloc_bytes(t, session, "ABCDEFGH", 8, &gen_a);

  /* a write that does not fill the window refuses */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pointer_record(t, a, 0, 8, gen_a));
  pl_vpush(t, bar_nat(t, (const uint8_t*)"xy", 2));
  cr_assert_eq(refusal_kind(hostcall(t, "gpu.write", args), "gpu.write"), 948u);

  /* the linear write: consume the complement, mint the successor */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pointer_record(t, a, 0, 8, gen_a));
  pl_vpush(t, bar_nat(t, (const uint8_t*)"abcdefgh", 8));
  pl_val ww = hostcall(t, "gpu.write", args);
  cr_assert_eq(witness_payload_u64(t, ww, "gpu.write", 0), a);
  uint64_t gen_a2 = witness_payload_u64(t, ww, "gpu.write", 1);
  cr_assert_eq(gen_a2, gen_a + 1);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pointer_record(t, a, 0, 8, gen_a)); /* the consumed residual */
  cr_assert_eq(refusal_kind(hostcall(t, "gpu.read", args), "gpu.read"), 950u);
  uint8_t got[16];
  read_bytes(t, session, a, 8, gen_a2, got);
  cr_assert_eq(memcmp(got, "abcdefgh", 8), 0);

  /* blit graph: one buffer→buffer copy, A → B */
  uint8_t zeros[16] = {0};
  uint64_t gen_b = 0;
  uint64_t b = alloc_bytes(t, session, zeros, 8, &gen_b);
  size_t gbase = t->vsp;
  pl_vpush(t, 0); /* steps list */
  {
    size_t f = t->vsp;
    pl_vpush(t, 7); /* kind: copy */
    pl_vpush(t, pointer_record(t, a, 0, 8, gen_a2));
    pl_vpush(t, pointer_record(t, b, 0, 8, gen_b));
    pl_vpush(t, 0);
    pl_vpush(t, 0);
    pl_val step = record(t, "command-step", f);
    pl_vpush(t, step);
    pl_vpush(t, t->vstack[gbase]);
    t->vstack[gbase] = record(t, "pair", f);
  }
  {
    size_t f = t->vsp;
    pl_vpush(t, t->vstack[gbase]);
    pl_vpush(t, 1);
    pl_vpush(t, 0);
    pl_vpush(t, 0);
    t->vstack[gbase] = record(t, "command-graph", f);
  }
  args = t->vsp;
  pl_vpush(t, t->vstack[gbase]);
  pl_vpush(t, session);
  pl_vpush(t, 0); /* pipeline 0: blit */
  pl_vpush(t, 0);
  pl_vpush(t, 0);
  pl_vpush(t, 0);
  pl_val bw = hostcall(t, "gpu.command-graph", args);
  cr_assert_eq(witness_payload_u64(t, bw, "gpu.command-graph", 0), 1u);
  uint64_t copy_event = witness_payload_u64(t, bw, "gpu.command-graph", 2);
  uint64_t gen_b2 = witness_payload_u64(t, bw, "gpu.command-graph", 3);
  cr_assert_eq(gen_b2, gen_b + 1);
  t->vsp = gbase;

  /* gpu.wait: the copy's signal value, no readback */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, copy_event);
  cr_assert_eq(
      witness_payload_u64(t, hostcall(t, "gpu.wait", args), "gpu.wait", 0),
      copy_event);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, copy_event + 100); /* never signaled */
  cr_assert_eq(refusal_kind(hostcall(t, "gpu.wait", args), "gpu.wait"), 948u);
  read_bytes(t, session, b, 8, gen_b2, got);
  cr_assert_eq(memcmp(got, "abcdefgh", 8), 0);

  /* buffer→texture→buffer round trip in one blit graph */
  uint8_t texsrc[16];
  for (int i = 0; i < 16; i++)
    texsrc[i] = (uint8_t)(0x40 + i);
  uint64_t gen_c = 0, gen_d = 0;
  uint64_t cbuf = alloc_bytes(t, session, texsrc, 16, &gen_c);
  uint64_t dbuf = alloc_bytes(t, session, zeros, 16, &gen_d);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, 2);
  pl_vpush(t, 2);
  pl_vpush(t, bar_nat(t, zeros, 16));
  pl_vpush(t, 1); /* bgra8 */
  uint64_t tex = witness_payload_u64(t, hostcall(t, "gpu.texture-2d", args),
                                     "gpu.texture-2d", 0);
  gbase = t->vsp;
  pl_vpush(t, 0); /* steps, built back to front: [to-texture, from-texture] */
  {
    size_t f = t->vsp;
    pl_vpush(t, 9); /* kind: copy-from-texture */
    pl_vpush(t, tex);
    pl_vpush(t, pointer_record(t, dbuf, 0, 16, gen_d));
    pl_vpush(t, 2);
    pl_vpush(t, 2);
    pl_val step = record(t, "command-step", f);
    pl_vpush(t, step);
    pl_vpush(t, t->vstack[gbase]);
    t->vstack[gbase] = record(t, "pair", f);
  }
  {
    size_t f = t->vsp;
    pl_vpush(t, 8); /* kind: copy-to-texture */
    pl_vpush(t, pointer_record(t, cbuf, 0, 16, gen_c));
    pl_vpush(t, tex);
    pl_vpush(t, 2);
    pl_vpush(t, 2);
    pl_val step = record(t, "command-step", f);
    pl_vpush(t, step);
    pl_vpush(t, t->vstack[gbase]);
    t->vstack[gbase] = record(t, "pair", f);
  }
  {
    size_t f = t->vsp;
    pl_vpush(t, t->vstack[gbase]);
    pl_vpush(t, 2);
    pl_vpush(t, 0);
    pl_vpush(t, 0);
    t->vstack[gbase] = record(t, "command-graph", f);
  }
  args = t->vsp;
  pl_vpush(t, t->vstack[gbase]);
  pl_vpush(t, session);
  pl_vpush(t, 0);
  pl_vpush(t, 0);
  pl_vpush(t, 0);
  pl_vpush(t, 0);
  pl_val tw = hostcall(t, "gpu.command-graph", args);
  cr_assert_eq(witness_payload_u64(t, tw, "gpu.command-graph", 0), 2u);
  uint64_t gen_d2 = witness_payload_u64(t, tw, "gpu.command-graph", 3);
  cr_assert_eq(gen_d2, gen_d + 1);
  t->vsp = gbase;
  read_bytes(t, session, dbuf, 16, gen_d2, got);
  cr_assert_eq(memcmp(got, texsrc, 16), 0, "texture round trip lost the bytes");

  /* a draw step in a blit graph refuses */
  gbase = t->vsp;
  pl_vpush(t, 0);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1); /* kind: draw */
    pl_vpush(t, 6);
    pl_vpush(t, 1);
    pl_vpush(t, 0);
    pl_vpush(t, 0);
    pl_val step = record(t, "command-step", f);
    pl_vpush(t, step);
    pl_vpush(t, t->vstack[gbase]);
    t->vstack[gbase] = record(t, "pair", f);
  }
  {
    size_t f = t->vsp;
    pl_vpush(t, t->vstack[gbase]);
    pl_vpush(t, 1);
    pl_vpush(t, 0);
    pl_vpush(t, 0);
    t->vstack[gbase] = record(t, "command-graph", f);
  }
  args = t->vsp;
  pl_vpush(t, t->vstack[gbase]);
  pl_vpush(t, session);
  pl_vpush(t, 0);
  pl_vpush(t, 0);
  pl_vpush(t, 0);
  pl_vpush(t, 0);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.command-graph", args), "gpu.command-graph"),
      952u);
  t->vsp = gbase;

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/* ── Gap 13 phase 2 helpers ────────────────────────────────────────────── */

/*
 * The depth artifact: box = (x, y, size, z) — box.z is both extents,
 * box.w lands in clip z — so occlusion is decided by the depth test,
 * never by draw order.
 */
static const char gfx_depth_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Row { float4 box; float4 color; };\n"
    "struct VOut { float4 pos [[position]]; float4 color; };\n"
    "vertex VOut shrine_vertex(uint vid [[vertex_id]],\n"
    "                          uint iid [[instance_id]],\n"
    "                          const device Row* rows [[buffer(0)]]) {\n"
    "  Row r = rows[iid];\n"
    "  float2 c[6] = {\n"
    "      float2(r.box.x, r.box.y),\n"
    "      float2(r.box.x + r.box.z, r.box.y),\n"
    "      float2(r.box.x, r.box.y + r.box.z),\n"
    "      float2(r.box.x + r.box.z, r.box.y),\n"
    "      float2(r.box.x + r.box.z, r.box.y + r.box.z),\n"
    "      float2(r.box.x, r.box.y + r.box.z),\n"
    "  };\n"
    "  float2 p = c[vid];\n"
    "  VOut o;\n"
    "  o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, r.box.w, 1.0);\n"
    "  o.color = r.color;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 shrine_fragment(VOut in [[stage_in]]) {\n"
    "  return in.color;\n"
    "}\n";

/* pipeline-request with a raster-state leg: artifact(bar, msl) +
 * root-layout(stride, 1, 0, buffer) + raster-state(depth-format, 0) —
 * the realized PSO carries the declared depth format. */
static pl_val pipeline_request_raster(pl_thread* t, pl_val msl_bar,
                                      uint64_t stride, uint64_t depth_format) {
  size_t base = t->vsp;
  pl_vpush(t, msl_bar); /* root the bar */
  size_t f = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 1); /* language: msl */
  pl_val art = record(t, "artifact", f);
  pl_vpush(t, art); /* base+1 */
  f = t->vsp;
  pl_vpush(t, stride);
  pl_vpush(t, 1); /* count */
  pl_vpush(t, 0); /* segments */
  pl_vpush(t, 1); /* mode: buffer */
  pl_val layout = record(t, "root-layout", f);
  pl_vpush(t, layout); /* base+2 */
  f = t->vsp;
  pl_vpush(t, depth_format);
  pl_vpush(t, 0); /* reserved */
  pl_val rast = record(t, "raster-state", f);
  pl_vpush(t, rast); /* base+3 */
  f = t->vsp;
  pl_vpush(t, t->vstack[base + 1]);
  pl_vpush(t, t->vstack[base + 2]);
  pl_vpush(t, t->vstack[base + 3]);
  pl_val req = record(t, "pipeline-request", f);
  t->vsp = base;
  return req;
}

/* pass-desc with a depth leg: one color attachment (clear + store), a
 * depth attachment (clear to clear_f32bits, declared store op), and a
 * ds-state (compare, write). */
static pl_val pass_desc_depth(pl_thread* t, uint64_t target,
                              uint64_t clear_bgra8, uint64_t depth_target,
                              uint64_t depth_store, uint64_t clear_f32bits,
                              uint64_t compare, uint64_t write) {
  size_t base = t->vsp;
  {
    size_t f = t->vsp;
    pl_vpush(t, target);
    pl_vpush(t, 1); /* load: clear */
    pl_vpush(t, 1); /* store: store */
    pl_vpush(t, clear_bgra8);
    pl_val ca = record(t, "color-attachment", f);
    pl_vpush(t, ca);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "pair", f)); /* colors list at base */
  }
  {
    size_t f = t->vsp;
    pl_vpush(t, depth_target);
    pl_vpush(t, 1); /* load: clear */
    pl_vpush(t, depth_store);
    pl_vpush(t, clear_f32bits);
    pl_vpush(t, record(t, "depth-attachment", f)); /* base+1 */
  }
  {
    size_t f = t->vsp;
    pl_vpush(t, compare);
    pl_vpush(t, write);
    pl_vpush(t, record(t, "ds-state", f)); /* base+2 */
  }
  size_t f = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, t->vstack[base + 1]);
  pl_vpush(t, t->vstack[base + 2]);
  pl_val pd = record(t, "pass-desc", f);
  t->vsp = base;
  return pd;
}

/*
 * Gap 13 phase 2: the depth leg.  Two overlapping quads in ONE draw
 * step — instance 0 near (z 0.2) blue, instance 1 far (z 0.8) red —
 * under a Less/write ds-state against a cleared d32 attachment: the
 * far red draws after the near blue and the depth test rejects it
 * where they overlap.  The depth store op is DONT-CARE — occlusion
 * needs the test during the pass, never the stored bytes — so the
 * correct color pixels prove the criterion without reading depth back.
 */
Test(hostcall, gfx_depth_occludes_and_dont_care_store) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during depth slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; depth slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  /* library from the bare bar; the pipeline realizes the same artifact
   * under a raster-state carrying Depth32Float */
  pl_val msl =
      bar_nat(t, (const uint8_t*)gfx_depth_msl, sizeof(gfx_depth_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pipeline_request_raster(t, t->vstack[keep], 32, 5));
  uint64_t pipeline = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t color =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 5); /* d32 */
  uint64_t depth =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  /* row 0: near blue quad; row 1: far red quad — same footprint */
  float payload[16] = {
      0.25f, 0.25f, 0.5f, 0.2f, 0.0f, 0.0f, 1.0f, 1.0f, /* near blue */
      0.25f, 0.25f, 0.5f, 0.8f, 1.0f, 0.0f, 0.0f, 1.0f, /* far red */
  };
  uint64_t root_gen = 0;
  uint64_t root_idx =
      alloc_bytes(t, session, payload, sizeof(payload), &root_gen);

  /* one draw step, 2 instances; depth clears to 1.0f and stores
   * DONT-CARE; ds-state Less + write */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 2, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t,
           pass_desc_depth(t, color, 0xff000000u, depth, 2, 0x3f800000u, 2, 1));
  pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(payload), root_gen));
  pl_vpush(t, 0);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                                   "gpu.command-graph", 0),
               1u);

  /* center: blue won although red drew after it */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, color);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                   "gpu.readback", 0),
               0xff0000ffu);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, color);
  pl_vpush(t, 1);
  pl_vpush(t, 1);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                   "gpu.readback", 0),
               0xff000000u);

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/* render graph with one draw-indexed step {index-window icount insts
 * fmt}; the index window is (alloc, 0, len, gen). */
static pl_val graph_indexed_record(pl_thread* t, uint64_t alloc, uint64_t len,
                                   uint64_t gen, uint64_t icount,
                                   uint64_t instances, uint64_t format) {
  size_t base = t->vsp;
  pl_vpush(t, 5); /* kind: draw-indexed */
  pl_vpush(t, pointer_record(t, alloc, 0, len, gen));
  pl_vpush(t, icount);
  pl_vpush(t, instances);
  pl_vpush(t, format);
  pl_val step = record(t, "command-step", base);
  pl_vpush(t, step);
  pl_vpush(t, 0);
  pl_val steps = record(t, "pair", base);
  pl_vpush(t, steps);
  size_t gb = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 1); /* step count */
  pl_vpush(t, 0); /* hazards */
  pl_vpush(t, 0); /* wait */
  pl_val graph = record(t, "command-graph", gb);
  t->vsp = base;
  return graph;
}

/*
 * Gap 13 phase 2: indexed geometry through the complement discipline.
 * The index buffer is an arena allocation named by a heap-pointer
 * record inside the step — native validates the window (liveness,
 * generation, format arithmetic) and points Metal at it; it never
 * reads an index.  A window too small for the declared count refuses
 * with bounds; an unknown index format refuses as graph shape.
 */
Test(hostcall, gfx_indexed_draw_through_complement) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during indexed slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; indexed slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  pl_val msl = bar_nat(t, (const uint8_t*)gfx_msl, sizeof(gfx_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t pipeline = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t target =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  /* one full-target red row; six u16le indices walking the quad's own
   * vertex order */
  float payload[8] = {0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f};
  uint64_t root_gen = 0;
  uint64_t root_idx =
      alloc_bytes(t, session, payload, sizeof(payload), &root_gen);
  static const uint8_t indices[12] = {0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0};
  uint64_t index_gen = 0;
  uint64_t index_idx =
      alloc_bytes(t, session, indices, sizeof(indices), &index_gen);

  args = t->vsp;
  pl_vpush(t, graph_indexed_record(t, index_idx, sizeof(indices), index_gen, 6,
                                   1, 1));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(payload), root_gen));
  pl_vpush(t, 0);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                                   "gpu.command-graph", 0),
               1u);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, target);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                                   "gpu.readback", 0),
               0xffff0000u);

  /* an index window too small for the count refuses with bounds
   * (6 u16 = 12 bytes > the declared 4) */
  args = t->vsp;
  pl_vpush(t, graph_indexed_record(t, index_idx, 4, index_gen, 6, 1, 1));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(payload), root_gen));
  pl_vpush(t, 0);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.command-graph", args), "gpu.command-graph"),
      948u);

  /* an unknown index format refuses as graph shape */
  args = t->vsp;
  pl_vpush(t, graph_indexed_record(t, index_idx, sizeof(indices), index_gen, 6,
                                   1, 3));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(payload), root_gen));
  pl_vpush(t, 0);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.command-graph", args), "gpu.command-graph"),
      952u);

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/* One kernel, two function constants: [[function_constant(0)]] selects
 * the branch, [[function_constant(1)]] scales.  The root is the usual
 * device-address-chased {out_addr, count} struct. */
static const char gfx_spec_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constant uint mode [[function_constant(0)]];\n"
    "constant float scale [[function_constant(1)]];\n"
    "struct Root { ulong out_addr; uint count; };\n"
    "kernel void shrine_specialize(uint tid [[thread_position_in_grid]],\n"
    "                              const device Root* d [[buffer(0)]]) {\n"
    "  if (tid >= d->count) return;\n"
    "  device float* out = (device float*)(d->out_addr);\n"
    "  out[tid] = (mode == 2 ? 100.0f : 1.0f) * scale + float(tid);\n"
    "}\n";

/* Gap 14 pipeline-request: artifact + root-layout(stride, buffer mode)
 * + no raster-state + a two-entry fn-constant pair-list (built back to
 * front; ids are sequential by list position, so the front entry is
 * [[function_constant(0)]] and the second [[function_constant(1)]]). */
static pl_val pipeline_request_constants(pl_thread* t, pl_val msl_bar,
                                         uint64_t stride, uint64_t mode_kind,
                                         uint64_t mode_value,
                                         uint64_t scale_kind,
                                         uint64_t scale_bits) {
  size_t base = t->vsp;
  pl_vpush(t, msl_bar); /* root the bar */
  pl_vpush(t, 0);       /* constants list at base+1 */
  {
    size_t f = t->vsp;
    pl_vpush(t, scale_kind);
    pl_vpush(t, scale_bits);
    pl_val c = record(t, "fn-constant", f);
    pl_vpush(t, c);
    pl_vpush(t, t->vstack[base + 1]);
    t->vstack[base + 1] = record(t, "pair", f);
  }
  {
    size_t f = t->vsp;
    pl_vpush(t, mode_kind);
    pl_vpush(t, mode_value);
    pl_val c = record(t, "fn-constant", f);
    pl_vpush(t, c);
    pl_vpush(t, t->vstack[base + 1]);
    t->vstack[base + 1] = record(t, "pair", f);
  }
  size_t f = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 1); /* language: msl */
  pl_val art = record(t, "artifact", f);
  pl_vpush(t, art); /* base+2 */
  f = t->vsp;
  pl_vpush(t, stride);
  pl_vpush(t, 1); /* count */
  pl_vpush(t, 0); /* segments */
  pl_vpush(t, 1); /* mode: buffer */
  pl_val layout = record(t, "root-layout", f);
  pl_vpush(t, layout); /* base+3 */
  f = t->vsp;
  pl_vpush(t, t->vstack[base + 2]);
  pl_vpush(t, t->vstack[base + 3]);
  pl_vpush(t, 0); /* raster-state */
  pl_vpush(t, t->vstack[base + 1]);
  pl_val req = record(t, "pipeline-request", f);
  t->vsp = base;
  return req;
}

/*
 * Gap 14: value specialization.  Function constants ride the
 * pipeline-request as a fn-constant pair-list; they join the pipeline
 * cache key, so one artifact under two constant sets is two pipelines
 * (each cold), the same set again is a cache hit, and the specialized
 * values demonstrably reach the kernel.  A malformed constant kind
 * refuses as carrier shape before any Metal object exists.
 */
Test(hostcall, gfx_spec_constants_specialize_pipeline) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during specialization slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; specialization slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  pl_val msl =
      bar_nat(t, (const uint8_t*)gfx_spec_msl, sizeof(gfx_spec_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl); /* root across calls */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);

  /* set A (mode 1, scale 2.0f): cold pipeline through the 4-field
   * request */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pipeline_request_constants(t, t->vstack[keep], 16, 1, 1, 2,
                                         0x40000000u));
  pl_val aw = hostcall(t, "gpu.pipeline-compute", args);
  uint64_t pipe_a = witness_payload_u64(t, aw, "gpu.pipeline-compute", 0);
  cr_assert_eq(witness_payload_u64(t, aw, "gpu.pipeline-compute", 1), 0u);

  /* out rows + the {out_addr, count} root the kernel chases */
  enum { LANES = 4 };
  uint8_t zeros[LANES * 4] = {0};
  uint64_t out_gen = 0;
  uint64_t out_idx = alloc_bytes(t, session, zeros, sizeof(zeros), &out_gen);
  uint64_t out_addr = dev_addr(t, session, out_idx, sizeof(zeros), out_gen);
  cr_assert_neq(out_addr, 0u);
  uint8_t root[16] = {0};
  memcpy(root + 0, &out_addr, 8);
  uint32_t count = LANES;
  memcpy(root + 8, &count, 4);
  uint64_t root_gen = 0;
  uint64_t root_idx = alloc_bytes(t, session, root, sizeof(root), &root_gen);

  args = t->vsp;
  pl_vpush(t, graph_compute_record(t, 1, 1, 1, LANES, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, pipe_a);
  pl_vpush(t, 0); /* compute graph takes no target */
  pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(root), root_gen));
  pl_vpush(t, 0);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                                   "gpu.command-graph", 0),
               1u);

  float got[LANES];
  read_bytes(t, session, out_idx, sizeof(zeros), out_gen, got);
  for (int i = 0; i < LANES; i++) {
    float diff = got[i] - (2.0f + (float)i);
    if (diff < 0.0f)
      diff = -diff;
    cr_assert(diff < 1e-6f, "set A lane %d: got %f", i, (double)got[i]);
  }

  /* set B (mode 2, same scale): a DIFFERENT key — cold again, and the
   * specialized branch lands 200 + tid in fresh rows */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pipeline_request_constants(t, t->vstack[keep], 16, 1, 2, 2,
                                         0x40000000u));
  pl_val bw = hostcall(t, "gpu.pipeline-compute", args);
  uint64_t pipe_b = witness_payload_u64(t, bw, "gpu.pipeline-compute", 0);
  cr_assert_eq(witness_payload_u64(t, bw, "gpu.pipeline-compute", 1), 0u);
  cr_assert_neq(pipe_b, pipe_a);

  uint64_t out2_gen = 0;
  uint64_t out2_idx = alloc_bytes(t, session, zeros, sizeof(zeros), &out2_gen);
  uint64_t out2_addr = dev_addr(t, session, out2_idx, sizeof(zeros), out2_gen);
  uint8_t root2[16] = {0};
  memcpy(root2 + 0, &out2_addr, 8);
  memcpy(root2 + 8, &count, 4);
  uint64_t root2_gen = 0;
  uint64_t root2_idx =
      alloc_bytes(t, session, root2, sizeof(root2), &root2_gen);

  args = t->vsp;
  pl_vpush(t, graph_compute_record(t, 1, 1, 1, LANES, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, pipe_b);
  pl_vpush(t, 0);
  pl_vpush(t, pointer_record(t, root2_idx, 0, sizeof(root2), root2_gen));
  pl_vpush(t, 0);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                                   "gpu.command-graph", 0),
               1u);

  read_bytes(t, session, out2_idx, sizeof(zeros), out2_gen, got);
  for (int i = 0; i < LANES; i++) {
    float diff = got[i] - (200.0f + (float)i);
    if (diff < 0.0f)
      diff = -diff;
    cr_assert(diff < 1e-6f, "set B lane %d: got %f", i, (double)got[i]);
  }

  /* set A again: the cache key matches — a hit */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pipeline_request_constants(t, t->vstack[keep], 16, 1, 1, 2,
                                         0x40000000u));
  pl_val aw2 = hostcall(t, "gpu.pipeline-compute", args);
  cr_assert_eq(witness_payload_u64(t, aw2, "gpu.pipeline-compute", 1), 1u);

  /* a malformed fn-constant kind refuses as carrier shape */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pipeline_request_constants(t, t->vstack[keep], 16, 3, 1, 2,
                                         0x40000000u));
  cr_assert_eq(refusal_kind(hostcall(t, "gpu.pipeline-compute", args),
                            "gpu.pipeline-compute"),
               951u);

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/*
 * Gap 12: op-83 record/replay through the er_io_hook seam.  Recording
 * runs the jets live and logs each result row; replay substitutes the
 * logged rows without touching Metal.  The substitution proof: the
 * recorded session is CLOSED live at the end of the recording, so a
 * live re-run of gpu.read would refuse 941 (unknown session) — the
 * replay instead returns the recorded bytes, on a fresh runtime where
 * none of these handles exist natively.
 */
Test(hostcall, gfx_hostcall_record_replay_substitutes) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during record/replay slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  er_log* log = er_log_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  (void)er_scheduler_adopt(sys, t);
  er_scheduler_record(sys, log);

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; record/replay slice skipped");
    pl_set_io_hook(NULL);
    er_scheduler_free(sys);
    er_log_free(log);
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);
  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);
  uint64_t gen = 0;
  uint64_t idx = alloc_bytes(t, session, "REPLAYME", 8, &gen);
  uint8_t got[8];
  read_bytes(t, session, idx, 8, gen, got);
  cr_assert_eq(memcmp(got, "REPLAYME", 8), 0);
  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  cr_assert_eq(er_log_events(log), 5u);

  /* replay the first four calls on a fresh runtime */
  test_rt rt2 = test_rt_new();
  pl_thread* t2 = rt2.t;
  t2->hostcall_f = true;
  er_scheduler* sys2 = er_scheduler_new(rt2.store, (er_config){0});
  (void)er_scheduler_adopt(sys2, t2);
  er_scheduler_replay(sys2, log);

  args = t2->vsp;
  pl_vpush(t2, 0);
  cr_assert_eq(witness_payload_u64(t2, hostcall(t2, "gpu.session-open", args),
                                   "gpu.session-open", 0),
               session);
  args = t2->vsp;
  pl_vpush(t2, session);
  {
    size_t f = t2->vsp;
    pl_vpush(t2, 1);
    pl_vpush(t2, 65536);
    pl_vpush(t2, 0);
    pl_vpush(t2, record(t2, "memory-arena", f));
  }
  (void)witness_payload_u64(t2, hostcall(t2, "gpu.arena", args), "gpu.arena",
                            0);
  uint64_t gen2 = 0;
  uint64_t idx2 = alloc_bytes(t2, session, "REPLAYME", 8, &gen2);
  cr_assert_eq(idx2, idx);
  cr_assert_eq(gen2, gen);
  /* the session is closed and this runtime never opened one: a live
   * read would refuse — the recorded bytes come back instead */
  uint8_t got2[8];
  read_bytes(t2, session, idx, 8, gen, got2);
  cr_assert_eq(memcmp(got2, "REPLAYME", 8), 0);
  cr_assert_eq(er_scheduler_log_cursor(sys2), 4u);

  pl_set_io_hook(NULL);
  er_scheduler_free(sys2);
  er_scheduler_free(sys);
  er_log_free(log);
  pl_catch_pop(t, &c);
  test_rt_free(&rt2);
  test_rt_free(&rt);
}

/* pipeline-request record: artifact(bar, msl) + root-layout(stride,
 * count, segments, mode). */
static pl_val pipeline_request(pl_thread* t, pl_val msl_bar, uint64_t stride,
                               uint64_t mode) {
  size_t base = t->vsp;
  pl_vpush(t, msl_bar); /* root the bar */
  size_t f = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 1); /* language: msl */
  pl_val art = record(t, "artifact", f);
  pl_vpush(t, art); /* base+1 */
  f = t->vsp;
  pl_vpush(t, stride);
  pl_vpush(t, 1); /* count */
  pl_vpush(t, 0); /* segments */
  pl_vpush(t, mode);
  pl_val layout = record(t, "root-layout", f);
  pl_vpush(t, layout); /* base+2 */
  f = t->vsp;
  pl_vpush(t, t->vstack[base + 1]);
  pl_vpush(t, t->vstack[base + 2]);
  pl_val req = record(t, "pipeline-request", f);
  t->vsp = base;
  return req;
}

/*
 * Gap 9: the pipeline-request carrier consumed.  One artifact under two
 * root layouts is two pipelines — the cache key includes the layout
 * (CB10's second relation, witnessed separately) — and the inline-root
 * lowering (setBytes, the push-constant analogue) produces bytes
 * identical to the buffer-bound root.
 */
Test(hostcall, gfx_pipeline_request_inline_root) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during inline-root slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; inline-root slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  pl_val msl =
      bar_nat(t, (const uint8_t*)gfx_add_one_msl, sizeof(gfx_add_one_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);

  /* buffer-root pipeline from the bare bar: cold */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  pl_val pw = hostcall(t, "gpu.pipeline-compute", args);
  uint64_t buffer_pipe = witness_payload_u64(t, pw, "gpu.pipeline-compute", 0);
  cr_assert_eq(witness_payload_u64(t, pw, "gpu.pipeline-compute", 1), 0u);

  /* SAME artifact under an inline-root layout: a distinct pipeline —
   * cold again (the key includes the layout) — then a hit on repeat */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pipeline_request(t, t->vstack[keep], 32, 2));
  pl_val iw = hostcall(t, "gpu.pipeline-compute", args);
  uint64_t inline_pipe = witness_payload_u64(t, iw, "gpu.pipeline-compute", 0);
  cr_assert_eq(witness_payload_u64(t, iw, "gpu.pipeline-compute", 1), 0u);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pipeline_request(t, t->vstack[keep], 32, 2));
  pl_val iw2 = hostcall(t, "gpu.pipeline-compute", args);
  cr_assert_eq(witness_payload_u64(t, iw2, "gpu.pipeline-compute", 1), 1u);

  /* identical bytes through both lowerings */
  enum { ROWS = 4 };
  float src[ROWS * 4];
  for (int i = 0; i < ROWS * 4; i++)
    src[i] = (float)i + 0.25f;
  uint8_t zeros[ROWS * 16] = {0};
  uint64_t src_gen, xa_gen, xb_gen;
  uint64_t src_idx = alloc_bytes(t, session, src, sizeof(src), &src_gen);
  uint64_t xa_idx = alloc_bytes(t, session, zeros, sizeof(zeros), &xa_gen);
  uint64_t xb_idx = alloc_bytes(t, session, zeros, sizeof(zeros), &xb_gen);
  uint64_t src_a = dev_addr(t, session, src_idx, sizeof(src), src_gen);
  uint64_t xa_a = dev_addr(t, session, xa_idx, sizeof(zeros), xa_gen);
  uint64_t xb_a = dev_addr(t, session, xb_idx, sizeof(zeros), xb_gen);

  uint64_t ra_idx, ra_gen, rb_idx, rb_gen;
  chain_root(t, session, src_a, xa_a, ROWS, &ra_idx, &ra_gen);
  chain_root(t, session, src_a, xb_a, ROWS, &rb_idx, &rb_gen);
  (void)chain_dispatch(t, session, buffer_pipe, ra_idx, ra_gen, 0);
  (void)chain_dispatch(t, session, inline_pipe, rb_idx, rb_gen, 0);

  float via_buffer[ROWS * 4], via_inline[ROWS * 4];
  read_bytes(t, session, xa_idx, sizeof(zeros), xa_gen, via_buffer);
  read_bytes(t, session, xb_idx, sizeof(zeros), xb_gen, via_inline);
  for (int i = 0; i < ROWS * 4; i++) {
    cr_assert(via_buffer[i] == src[i] + 1.0f, "buffer[%d]", i);
    cr_assert(via_inline[i] == via_buffer[i], "inline[%d] = %f != buffer %f", i,
              (double)via_inline[i], (double)via_buffer[i]);
  }

  /* an inline root window beyond 4 KiB refuses with bounds */
  uint8_t big[5000] = {0};
  uint64_t big_gen;
  uint64_t big_idx = alloc_bytes(t, session, big, sizeof(big), &big_gen);
  args = t->vsp;
  pl_vpush(t, graph_compute_record(t, 1, 1, 1, 64, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, inline_pipe);
  pl_vpush(t, 0);
  pl_vpush(t, pointer_record(t, big_idx, 0, sizeof(big), big_gen));
  pl_vpush(t, 0);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.command-graph", args), "gpu.command-graph"),
      948u);

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/*
 * Gap 10: dispatch-indirect, GPU-fed.  Stage B applies the eq.-17
 * admissibility predicate (spatial norm < temporal norm: subluminal =
 * admitted) and writes the threadgroup counts for the surviving rows
 * into the args window; stage C dispatches indirectly over exactly the
 * admitted count.  Refusal predicates executing on-device — the GPU
 * feeding itself; native only validates the window and points Metal at
 * it.
 */
static const char gfx_admit_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Root { ulong in_addr; ulong args_addr; uint count; };\n"
    "kernel void shrine_admit(uint tid [[thread_position_in_grid]],\n"
    "                         const device Root* d [[buffer(0)]]) {\n"
    "  if (tid != 0) return;\n"
    "  const device float4* in = (const device float4*)(d->in_addr);\n"
    "  device uint* args = (device uint*)(d->args_addr);\n"
    "  uint k = 0;\n"
    "  for (uint i = 0; i < d->count; i++) {\n"
    "    float4 p = in[i];\n"
    "    if (p.x * p.x + p.y * p.y + p.z * p.z < p.w * p.w) k++;\n"
    "  }\n"
    "  args[0] = k;\n"
    "  args[1] = 1;\n"
    "  args[2] = 1;\n"
    "}\n";

static const char gfx_mark_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Root { ulong out_addr; };\n"
    "kernel void shrine_mark(uint tid [[thread_position_in_grid]],\n"
    "                        const device Root* d [[buffer(0)]]) {\n"
    "  device float* out = (device float*)(d->out_addr);\n"
    "  out[tid] = 42.0f;\n"
    "}\n";

/* compute graph with one dispatch-indirect step {alloc offset gen tw}. */
static pl_val graph_indirect_record(pl_thread* t, uint64_t alloc,
                                    uint64_t offset, uint64_t gen, uint64_t tw,
                                    uint64_t wait) {
  size_t base = t->vsp;
  pl_vpush(t, 4); /* kind: dispatch-indirect */
  pl_vpush(t, alloc);
  pl_vpush(t, offset);
  pl_vpush(t, gen);
  pl_vpush(t, tw);
  pl_val step = record(t, "command-step", base);
  pl_vpush(t, step);
  pl_vpush(t, 0);
  pl_val steps = record(t, "pair", base);
  pl_vpush(t, steps);
  size_t gb = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 1);
  pl_vpush(t, 0);
  pl_vpush(t, wait);
  pl_val graph = record(t, "command-graph", gb);
  t->vsp = base;
  return graph;
}

Test(hostcall, gfx_indirect_dispatch_admits_on_device) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during indirect slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; indirect slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  /* two single-kernel artifacts: admit (stage B) and mark (stage C) */
  uint64_t admit_pipe, mark_pipe;
  {
    pl_val msl =
        bar_nat(t, (const uint8_t*)gfx_admit_msl, sizeof(gfx_admit_msl) - 1);
    size_t keep = t->vsp;
    pl_vpush(t, msl);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, t->vstack[keep]);
    (void)witness_payload_u64(t, hostcall(t, "gpu.library", args),
                              "gpu.library", 0);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, t->vstack[keep]);
    admit_pipe =
        witness_payload_u64(t, hostcall(t, "gpu.pipeline-compute", args),
                            "gpu.pipeline-compute", 0);
    t->vsp = keep;
  }
  {
    pl_val msl =
        bar_nat(t, (const uint8_t*)gfx_mark_msl, sizeof(gfx_mark_msl) - 1);
    size_t keep = t->vsp;
    pl_vpush(t, msl);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, t->vstack[keep]);
    (void)witness_payload_u64(t, hostcall(t, "gpu.library", args),
                              "gpu.library", 0);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, t->vstack[keep]);
    mark_pipe =
        witness_payload_u64(t, hostcall(t, "gpu.pipeline-compute", args),
                            "gpu.pipeline-compute", 0);
    t->vsp = keep;
  }

  /* 8 rows; exactly 3 subluminal (|xyz| < |w|) */
  enum { ROWS = 8, ADMITTED = 3 };
  float rows[ROWS * 4] = {
      0.1f, 0.1f, 0.1f, 1.0f, /* admitted */
      2.0f, 0.0f, 0.0f, 1.0f, /* refused */
      0.0f, 0.2f, 0.0f, 2.0f, /* admitted */
      1.0f, 1.0f, 1.0f, 1.0f, /* refused */
      0.0f, 0.0f, 0.0f, 0.5f, /* admitted */
      3.0f, 3.0f, 3.0f, 0.1f, /* refused */
      5.0f, 0.0f, 0.0f, 1.0f, /* refused */
      9.0f, 9.0f, 9.0f, 1.0f, /* refused */
  };
  uint8_t arg_zeros[12] = {0};
  uint8_t out_zeros[ROWS * 4] = {0};

  uint64_t rows_gen, ind_gen, out_gen;
  uint64_t rows_idx = alloc_bytes(t, session, rows, sizeof(rows), &rows_gen);
  uint64_t ind_idx =
      alloc_bytes(t, session, arg_zeros, sizeof(arg_zeros), &ind_gen);
  uint64_t out_idx =
      alloc_bytes(t, session, out_zeros, sizeof(out_zeros), &out_gen);
  uint64_t rows_a = dev_addr(t, session, rows_idx, sizeof(rows), rows_gen);
  uint64_t ind_a = dev_addr(t, session, ind_idx, sizeof(arg_zeros), ind_gen);
  uint64_t out_a = dev_addr(t, session, out_idx, sizeof(out_zeros), out_gen);

  /* stage B root {in, args, count}; stage C root {out} */
  uint64_t rb_idx, rb_gen;
  chain_root(t, session, rows_a, ind_a, ROWS, &rb_idx, &rb_gen);
  uint64_t rc_idx, rc_gen;
  rc_idx = alloc_bytes(t, session, &out_a, 8, &rc_gen);

  /* B: single-thread admissibility pass writing the indirect args */
  args = t->vsp;
  pl_vpush(t, graph_compute_record(t, 1, 1, 1, 1, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, admit_pipe);
  pl_vpush(t, 0);
  pl_vpush(t, pointer_record(t, rb_idx, 0, 32, rb_gen));
  pl_vpush(t, 0);
  uint64_t sig_b = witness_payload_u64(
      t, hostcall(t, "gpu.command-graph", args), "gpu.command-graph", 2);

  /* C: indirect dispatch over the admitted count, waiting B */
  args = t->vsp;
  pl_vpush(t, graph_indirect_record(t, ind_idx, 0, ind_gen, 1, sig_b));
  pl_vpush(t, session);
  pl_vpush(t, mark_pipe);
  pl_vpush(t, 0);
  pl_vpush(t, pointer_record(t, rc_idx, 0, 8, rc_gen));
  pl_vpush(t, 0);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                                   "gpu.command-graph", 0),
               1u);

  /* the GPU-written args admitted exactly ADMITTED rows */
  uint8_t ind_now[12];
  read_bytes(t, session, ind_idx, sizeof(ind_now), ind_gen, ind_now);
  uint32_t k;
  memcpy(&k, ind_now, 4);
  cr_assert_eq(k, (uint32_t)ADMITTED);

  float marks[ROWS];
  read_bytes(t, session, out_idx, sizeof(out_zeros), out_gen, marks);
  for (int i = 0; i < ROWS; i++) {
    float want = i < ADMITTED ? 42.0f : 0.0f;
    cr_assert(marks[i] == want, "mark[%d] = %f, want %f", i, (double)marks[i],
              (double)want);
  }

  /* stale args window refuses 950 before anything encodes */
  args = t->vsp;
  pl_vpush(t, graph_indirect_record(t, ind_idx, 0, ind_gen + 1, 1, 0));
  pl_vpush(t, session);
  pl_vpush(t, mark_pipe);
  pl_vpush(t, 0);
  pl_vpush(t, pointer_record(t, rc_idx, 0, 8, rc_gen));
  pl_vpush(t, 0);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.command-graph", args), "gpu.command-graph"),
      950u);

  /* misaligned / out-of-bounds args window refuses 948 */
  args = t->vsp;
  pl_vpush(t, graph_indirect_record(t, ind_idx, 4, ind_gen, 1, 0));
  pl_vpush(t, session);
  pl_vpush(t, mark_pipe);
  pl_vpush(t, 0);
  pl_vpush(t, pointer_record(t, rc_idx, 0, 8, rc_gen));
  pl_vpush(t, 0);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.command-graph", args), "gpu.command-graph"),
      948u);

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/*
 * Gap 7: the foil-msl emitter.  foil/shrine/psa-emitter.plan computes
 * the R(3,1,1) antiproduct table and welds the mode-uniform table-walker
 * MSL kernel from text fragments + emitted constants — Foil values
 * writing exact backend bytes through the lawful byte leg.  This test
 * only VERIFIES: it execs wisp, decodes the law-emitted artifact,
 * cross-checks the table against an independent C derivation, runs the
 * artifact on hardware, and matches the readback against the C
 * table-walker.  The C side never authors the artifact.
 */

/* (the integer R(3,1,1) table reference — psa_ap, psa_ar_sgn — lives
 * above the Metal guard; the kernel-ir audit test shares it) */

static void psa_antiprod(float* r, const float* a, const float* b) {
  for (int i = 0; i < 32; i++)
    r[i] = 0.0f;
  for (int i = 0; i < 32; i++)
    for (int j = 0; j < 32; j++) {
      int s, idx;
      psa_ap(i, j, &s, &idx);
      if (s != 0)
        r[idx] += (float)s * a[i] * b[j];
    }
}

static void psa_sandwich_cpu(const float* m, const float* x, float* out) {
  float mrev[32], tmp[32];
  for (int i = 0; i < 32; i++)
    mrev[i] = (float)psa_ar_sgn(i) * m[i];
  psa_antiprod(tmp, m, x);
  psa_antiprod(out, tmp, mrev);
}

/* decimal string -> little-endian bytes (multiply-accumulate) */
static size_t decimal_to_bytes(const char* dec, uint8_t* out, size_t cap) {
  size_t n = 0;
  memset(out, 0, cap);
  for (const char* p = dec; *p >= '0' && *p <= '9'; p++) {
    uint32_t carry = (uint32_t)(*p - '0');
    for (size_t i = 0; i < n; i++) {
      uint32_t v = (uint32_t)out[i] * 10 + carry;
      out[i] = (uint8_t)v;
      carry = v >> 8;
    }
    while (carry != 0 && n < cap) {
      out[n++] = (uint8_t)carry;
      carry >>= 8;
    }
  }
  return n;
}

Test(hostcall, gfx_foil_msl_emitter_law_artifact) {
  cr_assert(pl_host_gpu_metal_register());

  /* run the emitter laws; traces arrive on stderr */
  const char* bin = getenv("ENKI_WISP_BIN");
  if (bin == NULL || bin[0] == '\0')
    bin = "./build/debug/bin/wisp";
  char cmd[512];
  (void)snprintf(cmd, sizeof(cmd), "%s foil/shrine psa-emitter main 2>&1", bin);
  FILE* p = popen(cmd, "r");
  cr_assert_not_null(p, "failed to exec %s", cmd);
  enum { OUT_CAP = 65536 };
  char* out = malloc(OUT_CAP);
  cr_assert_not_null(out);
  size_t got = fread(out, 1, OUT_CAP - 1, p);
  out[got] = '\0';
  int st = pclose(p);
  cr_assert_eq(st, 0, "emitter exited %d", st);

  /* the pure-digit lines, in trace order: cs_sgn*256+2, cs_idx*256+2,
   * artifact bar */
  char* digit_lines[8] = {0};
  int nd = 0;
  for (char* line = strtok(out, "\n"); line != NULL;
       line = strtok(NULL, "\n")) {
    size_t len = strlen(line);
    if (len == 0)
      continue;
    bool digits = true;
    for (size_t i = 0; i < len; i++)
      if (line[i] < '0' || line[i] > '9')
        digits = false;
    if (digits && nd < 8)
      digit_lines[nd++] = line;
  }
  cr_assert_geq(nd, 3, "expected 3 numeric trace lines, got %d", nd);
  /* main returns 0 which may print as a final "0" line — take the
   * three substantive ones in order */
  if (strcmp(digit_lines[nd - 1], "0") == 0)
    nd--;
  char* cs_sgn_line = digit_lines[nd - 3];
  char* cs_idx_line = digit_lines[nd - 2];
  char* artifact_line = digit_lines[nd - 1];

  /* table cross-check: law-emitted vs independent C derivation */
  uint64_t ref_sgn = 0, ref_idx = 0;
  for (int k = 0; k < 1024; k++) {
    int s, idx;
    psa_ap(k / 32, k % 32, &s, &idx);
    ref_sgn += (uint64_t)(k + 1) * (uint64_t)(s == 0 ? 0 : (s > 0 ? 1 : 2));
    ref_idx += (uint64_t)(k + 1) * (uint64_t)idx;
  }
  char want[32];
  (void)snprintf(want, sizeof(want), "%llu",
                 (unsigned long long)(ref_sgn * 256 + 2));
  cr_assert_str_eq(cs_sgn_line, want, "sign-table checksum mismatch");
  (void)snprintf(want, sizeof(want), "%llu",
                 (unsigned long long)(ref_idx * 256 + 2));
  cr_assert_str_eq(cs_idx_line, want, "index-table checksum mismatch");

  /* decode the law-emitted artifact (bar: payload + 0x01 terminator) */
  enum { MSL_CAP = 16384 };
  uint8_t* msl_bytes = malloc(MSL_CAP);
  cr_assert_not_null(msl_bytes);
  size_t msl_n = decimal_to_bytes(artifact_line, msl_bytes, MSL_CAP);
  cr_assert_gt(msl_n, 1000);
  cr_assert_eq(msl_bytes[msl_n - 1], 0x01, "missing bar terminator");
  msl_n -= 1;
  cr_assert_eq(memcmp(msl_bytes, "#include", 8), 0,
               "artifact does not start with the include line");

  /* run the law-emitted artifact on hardware */
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;
  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during emitter slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; emitter slice skipped");
    free(out);
    free(msl_bytes);
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  pl_val msl = bar_nat(t, msl_bytes, msl_n);
  size_t keep = t->vsp;
  pl_vpush(t, msl);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t kernel = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-compute", args), "gpu.pipeline-compute", 0);

  /* the Lengyel motor: rotation about z by 2*phi with sin=0.6 cos=0.8
   * (e430 slot 25, antiscalar slot 31); X rows = spacetime points
   * ct e0 + x e1 + y e2 + z e3 + e4 (slots 1, 2, 4, 8, 16) */
  enum { ROWS = 2 };
  float m[32] = {0};
  m[25] = 0.6f;
  m[31] = 0.8f;
  float x[ROWS * 32] = {0};
  x[1] = 5.0f, x[2] = 1.0f, x[8] = 2.0f, x[16] = 1.0f;
  x[32 + 1] = 1.0f, x[32 + 2] = 0.5f, x[32 + 4] = -0.25f, x[32 + 16] = 1.0f;
  uint8_t zeros[ROWS * 128] = {0};

  uint64_t m_gen, x_gen, o_gen;
  uint64_t m_idx = alloc_bytes(t, session, m, sizeof(m), &m_gen);
  uint64_t x_idx = alloc_bytes(t, session, x, sizeof(x), &x_gen);
  uint64_t o_idx = alloc_bytes(t, session, zeros, sizeof(zeros), &o_gen);
  uint64_t m_a = dev_addr(t, session, m_idx, sizeof(m), m_gen);
  uint64_t x_a = dev_addr(t, session, x_idx, sizeof(x), x_gen);
  uint64_t o_a = dev_addr(t, session, o_idx, sizeof(zeros), o_gen);

  uint8_t root[32] = {0};
  uint32_t count = ROWS;
  memcpy(root + 0, &m_a, 8);
  memcpy(root + 8, &x_a, 8);
  memcpy(root + 16, &o_a, 8);
  memcpy(root + 24, &count, 4);
  uint64_t root_gen;
  uint64_t root_idx = alloc_bytes(t, session, root, sizeof(root), &root_gen);

  args = t->vsp;
  pl_vpush(t, graph_compute_record(t, 1, 1, 1, 64, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, kernel);
  pl_vpush(t, 0);
  pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(root), root_gen));
  pl_vpush(t, 0);
  cr_assert_eq(witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                                   "gpu.command-graph", 0),
               1u);

  float gpu_out[ROWS * 32];
  read_bytes(t, session, o_idx, sizeof(zeros), o_gen, gpu_out);

  /* the readback witness must match the independent CPU table-walker */
  float cpu_out[ROWS * 32];
  for (int row = 0; row < ROWS; row++)
    psa_sandwich_cpu(m, &x[row * 32], &cpu_out[row * 32]);
  for (int i = 0; i < ROWS * 32; i++) {
    float diff = gpu_out[i] - cpu_out[i];
    if (diff < 0.0f)
      diff = -diff;
    cr_assert(diff < 1e-5f, "slot %d: gpu %f cpu %f", i, (double)gpu_out[i],
              (double)cpu_out[i]);
  }

  /* reality anchors, convention-free: time, z, and the projective
   * weight are invariant under the z-rotor (within the table-walker's
   * float accumulation); the spatial xy-norm is preserved; the point
   * actually moved */
  static const struct {
    int slot;
    float want;
  } anchors[3] = {{1, 5.0f}, {8, 2.0f}, {16, 1.0f}}; /* ct, z, e4 */
  for (int i = 0; i < 3; i++) {
    float d = gpu_out[anchors[i].slot] - anchors[i].want;
    if (d < 0.0f)
      d = -d;
    cr_assert(d < 1e-5f, "invariant slot %d: %f, want %f", anchors[i].slot,
              (double)gpu_out[anchors[i].slot], (double)anchors[i].want);
  }
  float n0 = x[2] * x[2] + x[4] * x[4];
  float n1 = gpu_out[2] * gpu_out[2] + gpu_out[4] * gpu_out[4];
  float norm_drift = n1 - n0;
  if (norm_drift < 0.0f)
    norm_drift = -norm_drift;
  cr_assert(norm_drift < 1e-5f, "xy norm drifted: %f -> %f", (double)n0,
            (double)n1);
  cr_assert(gpu_out[2] != x[2], "the rotor did not rotate");

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  free(out);
  free(msl_bytes);
  test_rt_free(&rt);
}

/* ── Slice 3 exit criterion ────────────────────────────────────────────── */

/*
 * The admissibility stage: a value-specialized kernel WRITES the
 * indexed-indirect draw arguments on device — the GPU feeding the
 * render graph.  mode == 2 admits both instances; scale (1.0f) lands
 * indexStart 0.  The args window is the 20-byte
 * MTLDrawIndexedPrimitivesIndirectArguments {indexCount, instanceCount,
 * indexStart, baseVertex, baseInstance}; native validates the window
 * and never reads a byte of it.
 */
static const char gfx_exit_admit_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constant uint mode [[function_constant(0)]];\n"
    "constant float scale [[function_constant(1)]];\n"
    "struct Root { ulong args_addr; };\n"
    "kernel void shrine_exit_admit(uint tid [[thread_position_in_grid]],\n"
    "                              const device Root* d [[buffer(0)]]) {\n"
    "  if (tid != 0) return;\n"
    "  device uint* args = (device uint*)(d->args_addr);\n"
    "  args[0] = 6u;                  /* indexCount */\n"
    "  args[1] = mode == 2 ? 2u : 1u; /* instanceCount */\n"
    "  args[2] = uint(scale) - 1u;    /* indexStart */\n"
    "  args[3] = 0u;                  /* baseVertex */\n"
    "  args[4] = 0u;                  /* baseInstance */\n"
    "}\n";

/*
 * The MRT+depth render artifact: the depth-test Row shape (box =
 * (x, y, size, z), box.w in clip z) with a two-output fragment —
 * color(0) is the instance color, color(1) its channel-swapped twin,
 * proving both attachments were written independently by one draw.
 */
static const char gfx_exit_mrt_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Row { float4 box; float4 color; };\n"
    "struct VOut { float4 pos [[position]]; float4 color; };\n"
    "vertex VOut shrine_vertex(uint vid [[vertex_id]],\n"
    "                          uint iid [[instance_id]],\n"
    "                          const device Row* rows [[buffer(0)]]) {\n"
    "  Row r = rows[iid];\n"
    "  float2 c[6] = {\n"
    "      float2(r.box.x, r.box.y),\n"
    "      float2(r.box.x + r.box.z, r.box.y),\n"
    "      float2(r.box.x, r.box.y + r.box.z),\n"
    "      float2(r.box.x + r.box.z, r.box.y),\n"
    "      float2(r.box.x + r.box.z, r.box.y + r.box.z),\n"
    "      float2(r.box.x, r.box.y + r.box.z),\n"
    "  };\n"
    "  float2 p = c[vid];\n"
    "  VOut o;\n"
    "  o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, r.box.w, 1.0);\n"
    "  o.color = r.color;\n"
    "  return o;\n"
    "}\n"
    "struct FOut { float4 a [[color(0)]]; float4 b [[color(1)]]; };\n"
    "fragment FOut shrine_fragment(VOut in [[stage_in]]) {\n"
    "  FOut o;\n"
    "  o.a = in.color;\n"
    "  o.b = float4(in.color.b, in.color.g, in.color.r, 1.0);\n"
    "  return o;\n"
    "}\n";

/* pipeline_request_raster with the color-count leg: raster-state
 * (depth-format, color-count) — color-count > 1 declares an MRT
 * pipeline, every color attachment BGRA8. */
static pl_val pipeline_request_raster2(pl_thread* t, pl_val msl_bar,
                                       uint64_t stride, uint64_t depth_format,
                                       uint64_t color_count) {
  size_t base = t->vsp;
  pl_vpush(t, msl_bar); /* root the bar */
  size_t f = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 1); /* language: msl */
  pl_val art = record(t, "artifact", f);
  pl_vpush(t, art); /* base+1 */
  f = t->vsp;
  pl_vpush(t, stride);
  pl_vpush(t, 1); /* count */
  pl_vpush(t, 0); /* segments */
  pl_vpush(t, 1); /* mode: buffer */
  pl_val layout = record(t, "root-layout", f);
  pl_vpush(t, layout); /* base+2 */
  f = t->vsp;
  pl_vpush(t, depth_format);
  pl_vpush(t, color_count);
  pl_val rast = record(t, "raster-state", f);
  pl_vpush(t, rast); /* base+3 */
  f = t->vsp;
  pl_vpush(t, t->vstack[base + 1]);
  pl_vpush(t, t->vstack[base + 2]);
  pl_vpush(t, t->vstack[base + 3]);
  pl_val req = record(t, "pipeline-request", f);
  t->vsp = base;
  return req;
}

/* pass_desc_depth with TWO color attachments: the colors pair-list is
 * built back to front, so color(0) sits at the front of the list. */
static pl_val pass_desc_mrt_depth(pl_thread* t, uint64_t target0,
                                  uint64_t target1, uint64_t clear_bgra8,
                                  uint64_t depth_target, uint64_t depth_store,
                                  uint64_t clear_f32bits, uint64_t compare,
                                  uint64_t write) {
  size_t base = t->vsp;
  pl_vpush(t, 0); /* colors list at base, built back to front */
  for (int i = 0; i < 2; i++) {
    size_t f = t->vsp;
    pl_vpush(t, i == 0 ? target1 : target0);
    pl_vpush(t, 1); /* load: clear */
    pl_vpush(t, 1); /* store: store */
    pl_vpush(t, clear_bgra8);
    pl_val ca = record(t, "color-attachment", f);
    pl_vpush(t, ca);
    pl_vpush(t, t->vstack[base]);
    t->vstack[base] = record(t, "pair", f);
  }
  {
    size_t f = t->vsp;
    pl_vpush(t, depth_target);
    pl_vpush(t, 1); /* load: clear */
    pl_vpush(t, depth_store);
    pl_vpush(t, clear_f32bits);
    pl_vpush(t, record(t, "depth-attachment", f)); /* base+1 */
  }
  {
    size_t f = t->vsp;
    pl_vpush(t, compare);
    pl_vpush(t, write);
    pl_vpush(t, record(t, "ds-state", f)); /* base+2 */
  }
  size_t f = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, t->vstack[base + 1]);
  pl_vpush(t, t->vstack[base + 2]);
  pl_val pd = record(t, "pass-desc", f);
  t->vsp = base;
  return pd;
}

/* render graph with one draw-indexed-indirect step {index-window
 * args-window 0 fmt} and a timeline wait on a prior graph's signal. */
static pl_val graph_indexed_indirect_record(
    pl_thread* t, uint64_t index_alloc, uint64_t index_len, uint64_t index_gen,
    uint64_t args_alloc, uint64_t args_len, uint64_t args_gen, uint64_t format,
    uint64_t wait) {
  size_t base = t->vsp;
  pl_vpush(t, 6); /* kind: draw-indexed-indirect */
  pl_vpush(t, pointer_record(t, index_alloc, 0, index_len, index_gen));
  pl_vpush(t, pointer_record(t, args_alloc, 0, args_len, args_gen));
  pl_vpush(t, 0);
  pl_vpush(t, format);
  pl_val step = record(t, "command-step", base);
  pl_vpush(t, step);
  pl_vpush(t, 0);
  pl_val steps = record(t, "pair", base);
  pl_vpush(t, steps);
  size_t gb = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 1); /* step count */
  pl_vpush(t, 0); /* hazards */
  pl_vpush(t, wait);
  pl_val graph = record(t, "command-graph", gb);
  t->vsp = base;
  return graph;
}

/* gpu.readback wrapper: one pixel as u32le A<<24|R<<16|G<<8|B. */
static uint64_t readback_pixel(pl_thread* t, uint64_t session, uint64_t target,
                               uint64_t x, uint64_t y) {
  size_t args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, target);
  pl_vpush(t, x);
  pl_vpush(t, y);
  return witness_payload_u64(t, hostcall(t, "gpu.readback", args),
                             "gpu.readback", 0);
}

/* Every witness the exit-criterion chain produces, in call order —
 * captured live under recording, then again under replay, where each
 * field must come back bit-identical from the log. */
typedef struct {
  uint64_t session;
  uint64_t args_idx, args_gen, args_addr;
  uint64_t admit_lib, admit_pipe;
  uint64_t croot_idx, croot_gen;
  uint64_t sig_admit;
  uint64_t mrt_lib, mrt_pipe;
  uint64_t color0, color1, depth;
  uint64_t root_idx, root_gen;
  uint64_t index_idx, index_gen;
  uint64_t sig_draw;
  uint64_t c0_center, c0_corner, c1_center, c1_corner;
} gfx_exit_wit;

/* The whole chain, every call but session-close: session + arena, the
 * constants-specialized admissibility dispatch writing the draw args,
 * the MRT+depth indexed-indirect render waiting its signal, and four
 * pixel readbacks.  Returns false only when the live host has no
 * Metal device. */
static bool gfx_exit_chain(pl_thread* t, gfx_exit_wit* w) {
  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943)
    return false;
  w->session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, w->session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  /* the args window the admissibility stage will fill on device */
  uint8_t arg_zeros[20] = {0};
  w->args_gen = 0;
  w->args_idx =
      alloc_bytes(t, w->session, arg_zeros, sizeof(arg_zeros), &w->args_gen);
  w->args_addr =
      dev_addr(t, w->session, w->args_idx, sizeof(arg_zeros), w->args_gen);

  /* the specialized kernel: mode 2 (u32) admits two instances */
  {
    pl_val msl = bar_nat(t, (const uint8_t*)gfx_exit_admit_msl,
                         sizeof(gfx_exit_admit_msl) - 1);
    size_t keep = t->vsp;
    pl_vpush(t, msl);
    args = t->vsp;
    pl_vpush(t, w->session);
    pl_vpush(t, t->vstack[keep]);
    w->admit_lib = witness_payload_u64(t, hostcall(t, "gpu.library", args),
                                       "gpu.library", 0);
    args = t->vsp;
    pl_vpush(t, w->session);
    pl_vpush(t, pipeline_request_constants(t, t->vstack[keep], 8, 1, 2, 2,
                                           0x3f800000u));
    w->admit_pipe =
        witness_payload_u64(t, hostcall(t, "gpu.pipeline-compute", args),
                            "gpu.pipeline-compute", 0);
    t->vsp = keep;
  }
  uint8_t croot[8];
  memcpy(croot, &w->args_addr, 8);
  w->croot_gen = 0;
  w->croot_idx =
      alloc_bytes(t, w->session, croot, sizeof(croot), &w->croot_gen);
  args = t->vsp;
  pl_vpush(t, graph_compute_record(t, 1, 1, 1, 1, 1, 0, 0));
  pl_vpush(t, w->session);
  pl_vpush(t, w->admit_pipe);
  pl_vpush(t, 0);
  pl_vpush(t, pointer_record(t, w->croot_idx, 0, sizeof(croot), w->croot_gen));
  pl_vpush(t, 0);
  w->sig_admit = witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                                     "gpu.command-graph", 2);

  /* the MRT pipeline under raster-state (d32, 2 colors) */
  {
    pl_val msl = bar_nat(t, (const uint8_t*)gfx_exit_mrt_msl,
                         sizeof(gfx_exit_mrt_msl) - 1);
    size_t keep = t->vsp;
    pl_vpush(t, msl);
    args = t->vsp;
    pl_vpush(t, w->session);
    pl_vpush(t, t->vstack[keep]);
    w->mrt_lib = witness_payload_u64(t, hostcall(t, "gpu.library", args),
                                     "gpu.library", 0);
    args = t->vsp;
    pl_vpush(t, w->session);
    pl_vpush(t, pipeline_request_raster2(t, t->vstack[keep], 32, 5, 2));
    w->mrt_pipe = witness_payload_u64(
        t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);
    t->vsp = keep;
  }
  args = t->vsp;
  pl_vpush(t, w->session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  w->color0 =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);
  args = t->vsp;
  pl_vpush(t, w->session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  w->color1 =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);
  args = t->vsp;
  pl_vpush(t, w->session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 5); /* d32 */
  w->depth =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  /* row 0: near blue quad; row 1: far red quad — same footprint, so
   * Less+write occludes red although the GPU-admitted instanceCount
   * draws it after blue */
  float payload[16] = {
      0.25f, 0.25f, 0.5f, 0.2f, 0.0f, 0.0f, 1.0f, 1.0f, /* near blue */
      0.25f, 0.25f, 0.5f, 0.8f, 1.0f, 0.0f, 0.0f, 1.0f, /* far red */
  };
  w->root_gen = 0;
  w->root_idx =
      alloc_bytes(t, w->session, payload, sizeof(payload), &w->root_gen);
  static const uint8_t indices[12] = {0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0};
  w->index_gen = 0;
  w->index_idx =
      alloc_bytes(t, w->session, indices, sizeof(indices), &w->index_gen);

  /* one kind-6 step consuming the index window and the GPU-written
   * args window, waiting the admissibility signal */
  args = t->vsp;
  pl_vpush(t, graph_indexed_indirect_record(
                  t, w->index_idx, sizeof(indices), w->index_gen, w->args_idx,
                  sizeof(arg_zeros), w->args_gen, 1, w->sig_admit));
  pl_vpush(t, w->session);
  pl_vpush(t, w->mrt_pipe);
  pl_vpush(t, pass_desc_mrt_depth(t, w->color0, w->color1, 0xff000000u,
                                  w->depth, 2, 0x3f800000u, 2, 1));
  pl_vpush(t, pointer_record(t, w->root_idx, 0, sizeof(payload), w->root_gen));
  pl_vpush(t, 0);
  pl_val dw = hostcall(t, "gpu.command-graph", args);
  cr_assert_eq(witness_payload_u64(t, dw, "gpu.command-graph", 0), 1u);
  w->sig_draw = witness_payload_u64(t, dw, "gpu.command-graph", 2);

  w->c0_center = readback_pixel(t, w->session, w->color0, GFX_W / 2, GFX_H / 2);
  w->c0_corner = readback_pixel(t, w->session, w->color0, 1, 1);
  w->c1_center = readback_pixel(t, w->session, w->color1, GFX_W / 2, GFX_H / 2);
  w->c1_corner = readback_pixel(t, w->session, w->color1, 1, 1);
  return true;
}

/*
 * The Slice 3 exit criterion, composed from every landed mechanism: a
 * value-specialized compute stage writes the draw-indexed-indirect
 * arguments on device (instanceCount 2 under mode == 2); a render
 * graph waits its signal and draws indexed geometry indirectly into
 * TWO color attachments under a d32 Less+write depth test; both
 * attachments read back occlusion-correct pixels.  The whole chain is
 * RECORDED on Metal hardware, the session closed live, then REPLAYED
 * on a fresh runtime where none of the handles exist — a live re-run
 * would refuse 941 at the first gpu.arena — so bit-identical witnesses
 * and pixels prove pure substitution: recorded once, replayed with no
 * GPU work.
 */
Test(hostcall, gfx_no_api_floor_exit_criterion) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during exit-criterion slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  er_log* log = er_log_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  (void)er_scheduler_adopt(sys, t);
  er_scheduler_record(sys, log);

  gfx_exit_wit live;
  memset(&live, 0, sizeof live);
  if (!gfx_exit_chain(t, &live)) {
    cr_log_warn("no metal device; exit-criterion slice skipped");
    pl_set_io_hook(NULL);
    er_scheduler_free(sys);
    er_log_free(log);
    test_rt_free(&rt);
    return;
  }

  /* the recorded pixels: occlusion under the GPU-admitted second
   * instance on color(0), the channel-swapped twin on color(1) */
  cr_assert_eq(live.c0_center, 0xff0000ffu); /* near blue won */
  cr_assert_eq(live.c0_corner, 0xff000000u); /* clear */
  cr_assert_eq(live.c1_center, 0xffff0000u); /* swapped blue = red */
  cr_assert_eq(live.c1_corner, 0xff000000u);
  cr_assert_gt(live.sig_draw, live.sig_admit);

  size_t args = t->vsp;
  pl_vpush(t, live.session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  uint64_t events = er_log_events(log);

  /* replay every call but the close on a fresh runtime: the closed
   * session and absent handles make live execution impossible, so the
   * identical rows below are the log substituting for Metal */
  test_rt rt2 = test_rt_new();
  pl_thread* t2 = rt2.t;
  t2->hostcall_f = true;
  er_scheduler* sys2 = er_scheduler_new(rt2.store, (er_config){0});
  (void)er_scheduler_adopt(sys2, t2);
  er_scheduler_replay(sys2, log);

  gfx_exit_wit rep;
  memset(&rep, 0, sizeof rep);
  cr_assert(gfx_exit_chain(t2, &rep));
  cr_assert_eq(rep.session, live.session);
  cr_assert_eq(rep.args_idx, live.args_idx);
  cr_assert_eq(rep.args_gen, live.args_gen);
  cr_assert_eq(rep.args_addr, live.args_addr);
  cr_assert_eq(rep.admit_lib, live.admit_lib);
  cr_assert_eq(rep.admit_pipe, live.admit_pipe);
  cr_assert_eq(rep.croot_idx, live.croot_idx);
  cr_assert_eq(rep.croot_gen, live.croot_gen);
  cr_assert_eq(rep.sig_admit, live.sig_admit);
  cr_assert_eq(rep.mrt_lib, live.mrt_lib);
  cr_assert_eq(rep.mrt_pipe, live.mrt_pipe);
  cr_assert_eq(rep.color0, live.color0);
  cr_assert_eq(rep.color1, live.color1);
  cr_assert_eq(rep.depth, live.depth);
  cr_assert_eq(rep.root_idx, live.root_idx);
  cr_assert_eq(rep.root_gen, live.root_gen);
  cr_assert_eq(rep.index_idx, live.index_idx);
  cr_assert_eq(rep.index_gen, live.index_gen);
  cr_assert_eq(rep.sig_draw, live.sig_draw);
  cr_assert_eq(rep.c0_center, live.c0_center);
  cr_assert_eq(rep.c0_corner, live.c0_corner);
  cr_assert_eq(rep.c1_center, live.c1_center);
  cr_assert_eq(rep.c1_corner, live.c1_corner);
  cr_assert_eq(er_scheduler_log_cursor(sys2), events - 1);

  pl_set_io_hook(NULL);
  er_scheduler_free(sys2);
  er_scheduler_free(sys);
  er_log_free(log);
  pl_catch_pop(t, &c);
  test_rt_free(&rt2);
  test_rt_free(&rt);
}

/*
 * The present/surface arm: gpu.surface-open vends a headless
 * CAMetalLayer surface, gpu.acquire vends a one-shot drawable at the
 * surface's live generation, and gpu.present blits an admitted render
 * target into it, presents, and signals the session timeline.  The
 * surface config is linear — resize consumes it and mints the next
 * generation, so a stale-generation acquire refuses (950); the
 * drawable is a one-shot residual — present consumes it, so a second
 * present refuses the same way; a source/drawable extent mismatch is
 * bounds (948).  A headless CI host may vend no drawables at all: the
 * present arm skips on the acquire ENCODE refusal (947).
 */
Test(hostcall, gfx_present_blits_and_consumes_drawable) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during present slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; present slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);     /* scope: session */
    pl_vpush(t, 65536); /* capacity */
    pl_vpush(t, 0);     /* generation-policy: bump-on-free */
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  /* the surface: a headless CAMetalLayer at the slice extent */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_val sw = hostcall(t, "gpu.surface-open", args);
  uint64_t s1 = witness_payload_u64(t, sw, "gpu.surface-open", 0);
  uint64_t g1 = witness_payload_u64(t, sw, "gpu.surface-open", 1);
  cr_assert_eq(witness_payload_u64(t, sw, "gpu.surface-open", 2),
               (uint64_t)GFX_W);
  cr_assert_eq(witness_payload_u64(t, sw, "gpu.surface-open", 3),
               (uint64_t)GFX_H);

  /* the orange frame: the slice artifact drawn once into a target */
  pl_val msl = bar_nat(t, (const uint8_t*)gfx_msl, sizeof(gfx_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl); /* root across calls */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t pipeline = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t target =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  /* root payload: one row, box {.25 .25 .5 .5}, color {1 .5 0 1} */
  float payload[8] = {0.25f, 0.25f, 0.5f, 0.5f, 1.0f, 0.5f, 0.0f, 1.0f};
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, bar_nat(t, (const uint8_t*)payload, sizeof(payload)));
  pl_val alloc_w = hostcall(t, "gpu.alloc", args);
  uint64_t alloc_idx = witness_payload_u64(t, alloc_w, "gpu.alloc", 0);
  uint64_t alloc_len = witness_payload_u64(t, alloc_w, "gpu.alloc", 2);
  uint64_t alloc_gen = witness_payload_u64(t, alloc_w, "gpu.alloc", 3);

  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, alloc_idx, 0, alloc_len, alloc_gen));
  pl_vpush(t, 0); /* no descriptor heap */
  (void)witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                            "gpu.command-graph", 0);

  /* acquire a one-shot drawable at the live generation.  A headless
   * host may vend none: skip the present arm on 947 (ENCODE). */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, s1);
  pl_vpush(t, g1);
  pl_val aw = hostcall(t, "gpu.acquire", args);
  if (refusal_kind(aw, "gpu.acquire") == 947) {
    cr_log_warn("no drawable vended; present arm skipped");
    args = t->vsp;
    pl_vpush(t, session);
    (void)hostcall(t, "gpu.session-close", args);
    pl_catch_pop(t, &c);
    test_rt_free(&rt);
    return;
  }
  uint64_t d1 = witness_payload_u64(t, aw, "gpu.acquire", 0);
  cr_assert_eq(witness_payload_u64(t, aw, "gpu.acquire", 1), g1);
  cr_assert_eq(witness_payload_u64(t, aw, "gpu.acquire", 2), (uint64_t)GFX_W);
  cr_assert_eq(witness_payload_u64(t, aw, "gpu.acquire", 3), (uint64_t)GFX_H);

  /* present blits the target into the drawable and signals */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, d1);
  pl_vpush(t, target);
  pl_val pw = hostcall(t, "gpu.present", args);
  cr_assert_eq(witness_payload_u64(t, pw, "gpu.present", 0), s1);
  cr_assert_eq(witness_payload_u64(t, pw, "gpu.present", 1), d1);
  cr_assert_eq(witness_payload_u64(t, pw, "gpu.present", 2), g1);
  uint64_t present_event = witness_payload_u64(t, pw, "gpu.present", 3);
  cr_assert_gt(present_event, 0u);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, present_event);
  cr_assert_eq(
      witness_payload_u64(t, hostcall(t, "gpu.wait", args), "gpu.wait", 0),
      present_event);

  /* the drawable is consumed: a second present refuses (950) */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, d1);
  pl_vpush(t, target);
  cr_assert_eq(refusal_kind(hostcall(t, "gpu.present", args), "gpu.present"),
               950u);

  /* resize consumes the surface config: the generation bumps, the old
   * generation is stale (950), and the fresh drawable's extent no
   * longer matches the 64x64 source (948) */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, s1);
  pl_vpush(t, GFX_W / 2);
  pl_vpush(t, GFX_H / 2);
  pl_val rw = hostcall(t, "gpu.surface-resize", args);
  uint64_t g2 = witness_payload_u64(t, rw, "gpu.surface-resize", 1);
  cr_assert_eq(g2, g1 + 1);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, s1);
  pl_vpush(t, g1);
  cr_assert_eq(refusal_kind(hostcall(t, "gpu.acquire", args), "gpu.acquire"),
               950u);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, s1);
  pl_vpush(t, g2);
  uint64_t d2 = witness_payload_u64(t, hostcall(t, "gpu.acquire", args),
                                    "gpu.acquire", 0);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, d2);
  pl_vpush(t, target);
  cr_assert_eq(refusal_kind(hostcall(t, "gpu.present", args), "gpu.present"),
               948u);

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));

  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/* ── The visual dump (opt-in) ──────────────────────────────────────────── */

enum { DUMP_W = 256, DUMP_H = 256 };

/* Binary PPM (P6): header, then per-pixel RGB pulled out of the BGRA
 * byte order the bgra8 targets read back (bytes B, G, R, A). */
static void dump_ppm(const char* path, const uint8_t* bgra, uint64_t w,
                     uint64_t h) {
  FILE* f = fopen(path, "wb");
  cr_assert_not_null(f, "fopen %s failed", path);
  (void)fprintf(f, "P6\n%llu %llu\n255\n", (unsigned long long)w,
                (unsigned long long)h);
  for (uint64_t i = 0; i < w * h; i++) {
    const uint8_t rgb[3] = {bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4]};
    cr_assert_eq(fwrite(rgb, 1, 3, f), 3u);
  }
  cr_assert_eq(fclose(f), 0);
}

/* The admitted frame-extraction path: alloc a zeroed w*h*4 window, run
 * a blit graph (pipeline 0) with one copy-from-texture step waiting the
 * render's signal value — witness payload 3 is the window's minted
 * generation — then gpu.read the bytes and write dir/name as P6. */
static void dump_frame(pl_thread* t, uint64_t session, uint64_t target,
                       uint64_t w, uint64_t h, uint64_t wait, const char* dir,
                       const char* name) {
  size_t len = (size_t)(w * h * 4);
  uint8_t* bytes = calloc(1, len);
  cr_assert_not_null(bytes);
  uint64_t dst_gen = 0;
  uint64_t dst_idx = alloc_bytes(t, session, bytes, len, &dst_gen);

  size_t gbase = t->vsp;
  pl_vpush(t, 0); /* steps list */
  {
    size_t f = t->vsp;
    pl_vpush(t, 9); /* kind: copy-from-texture */
    pl_vpush(t, target);
    pl_vpush(t, pointer_record(t, dst_idx, 0, len, dst_gen));
    pl_vpush(t, w);
    pl_vpush(t, h);
    pl_val step = record(t, "command-step", f);
    pl_vpush(t, step);
    pl_vpush(t, t->vstack[gbase]);
    t->vstack[gbase] = record(t, "pair", f);
  }
  {
    size_t f = t->vsp;
    pl_vpush(t, t->vstack[gbase]);
    pl_vpush(t, 1); /* step count */
    pl_vpush(t, 0); /* hazards */
    pl_vpush(t, wait);
    t->vstack[gbase] = record(t, "command-graph", f);
  }
  size_t args = t->vsp;
  pl_vpush(t, t->vstack[gbase]);
  pl_vpush(t, session);
  pl_vpush(t, 0); /* pipeline 0: blit */
  pl_vpush(t, 0);
  pl_vpush(t, 0);
  pl_vpush(t, 0);
  pl_val bw = hostcall(t, "gpu.command-graph", args);
  cr_assert_eq(witness_payload_u64(t, bw, "gpu.command-graph", 0), 1u);
  uint64_t dst_gen2 = witness_payload_u64(t, bw, "gpu.command-graph", 3);
  cr_assert_eq(dst_gen2, dst_gen + 1);
  t->vsp = gbase;

  read_bytes(t, session, dst_idx, len, dst_gen2, bytes);
  char path[512];
  (void)snprintf(path, sizeof(path), "%s/%s", dir, name);
  dump_ppm(path, bytes, w, h);
  free(bytes);
}

/*
 * The visual dump: opt-in (ENKI_GFX_DUMP=<dir>) — three landed scenes
 * re-rendered at 256x256 and extracted through the admitted path (a
 * blit-graph copy-from-texture into an arena window, then gpu.read)
 * into binary PPMs: the slice quad, the sampled quadrant texture, and
 * the depth-occlusion pair.  Pure observation for eyeballs; every
 * behavioral assertion is already made at 64x64 by the fixtures above.
 */
Test(hostcall, gfx_visual_dump) {
  const char* dir = getenv("ENKI_GFX_DUMP");
  if (dir == NULL || dir[0] == '\0')
    cr_skip("ENKI_GFX_DUMP not set; visual dump skipped");
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during visual dump: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; visual dump skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  /* three 256 KiB dump windows plus root payloads: a roomier arena */
  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);       /* scope: session */
    pl_vpush(t, 1 << 21); /* capacity */
    pl_vpush(t, 0);       /* generation-policy: bump-on-free */
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  /* scene 1: the slice root payload shape — orange quad on black */
  {
    pl_val msl = bar_nat(t, (const uint8_t*)gfx_msl, sizeof(gfx_msl) - 1);
    size_t keep = t->vsp;
    pl_vpush(t, msl);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, t->vstack[keep]);
    (void)witness_payload_u64(t, hostcall(t, "gpu.library", args),
                              "gpu.library", 0);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, t->vstack[keep]);
    uint64_t pipeline = witness_payload_u64(
        t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);
    t->vsp = keep;

    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, DUMP_W);
    pl_vpush(t, DUMP_H);
    pl_vpush(t, 1); /* bgra8 */
    uint64_t target = witness_payload_u64(t, hostcall(t, "gpu.target", args),
                                          "gpu.target", 0);

    /* one row, box {.25 .25 .5 .5}, color {1 .5 0 1} */
    float payload[8] = {0.25f, 0.25f, 0.5f, 0.5f, 1.0f, 0.5f, 0.0f, 1.0f};
    uint64_t root_gen = 0;
    uint64_t root_idx =
        alloc_bytes(t, session, payload, sizeof(payload), &root_gen);

    args = t->vsp;
    pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0));
    pl_vpush(t, session);
    pl_vpush(t, pipeline);
    pl_vpush(t, pass_desc(t, target, 0xff000000u));
    pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(payload), root_gen));
    pl_vpush(t, 0); /* no descriptor heap */
    uint64_t sig = witness_payload_u64(
        t, hostcall(t, "gpu.command-graph", args), "gpu.command-graph", 2);
    dump_frame(t, session, target, DUMP_W, DUMP_H, sig, dir, "slice.ppm");
  }

  /* scene 2: the 2x2 quadrant texture sampled full-screen through the
   * descriptor heap */
  {
    /* 2x2 BGRA8 quadrants: TL red, TR green, BL blue, BR white */
    static const uint8_t texels[16] = {
        0,   0, 255, 255, 0,   255, 0,   255, /* row 0: red, green */
        255, 0, 0,   255, 255, 255, 255, 255, /* row 1: blue, white */
    };
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, 2);
    pl_vpush(t, 2);
    pl_vpush(t, bar_nat(t, texels, sizeof(texels)));
    pl_vpush(t, 1); /* bgra8 */
    uint64_t texture = witness_payload_u64(
        t, hostcall(t, "gpu.texture-2d", args), "gpu.texture-2d", 0);

    args = t->vsp;
    pl_vpush(t, session);
    {
      size_t f = t->vsp;
      pl_vpush(t, 1); /* count */
      pl_vpush(t, 1); /* stride-class: sampled */
      pl_vpush(t, record(t, "descriptor-heap", f));
    }
    uint64_t dheap = witness_payload_u64(
        t, hostcall(t, "gpu.descriptor-heap", args), "gpu.descriptor-heap", 0);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, dheap);
    pl_vpush(t, window_record(t, dheap, 0, 1));
    pl_vpush(t, texture);
    (void)witness_payload_u64(t, hostcall(t, "gpu.descriptor-write", args),
                              "gpu.descriptor-write", 0);

    pl_val msl = bar_nat(t, (const uint8_t*)gfx_textured_msl,
                         sizeof(gfx_textured_msl) - 1);
    size_t keep = t->vsp;
    pl_vpush(t, msl);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, t->vstack[keep]);
    (void)witness_payload_u64(t, hostcall(t, "gpu.library", args),
                              "gpu.library", 0);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, t->vstack[keep]);
    uint64_t pipeline = witness_payload_u64(
        t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);
    t->vsp = keep;

    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, DUMP_W);
    pl_vpush(t, DUMP_H);
    pl_vpush(t, 1); /* bgra8 */
    uint64_t target = witness_payload_u64(t, hostcall(t, "gpu.target", args),
                                          "gpu.target", 0);

    /* full-target quad; color field unused by the textured artifact */
    float payload[8] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    uint64_t root_gen = 0;
    uint64_t root_idx =
        alloc_bytes(t, session, payload, sizeof(payload), &root_gen);

    args = t->vsp;
    pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0));
    pl_vpush(t, session);
    pl_vpush(t, pipeline);
    pl_vpush(t, pass_desc(t, target, 0xff000000u));
    pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(payload), root_gen));
    pl_vpush(t, dheap);
    uint64_t sig = witness_payload_u64(
        t, hostcall(t, "gpu.command-graph", args), "gpu.command-graph", 2);
    dump_frame(t, session, target, DUMP_W, DUMP_H, sig, dir, "textured.ppm");
  }

  /* scene 3: depth occlusion — near blue beats far red under Less
   * although red draws after it */
  {
    pl_val msl =
        bar_nat(t, (const uint8_t*)gfx_depth_msl, sizeof(gfx_depth_msl) - 1);
    size_t keep = t->vsp;
    pl_vpush(t, msl);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, t->vstack[keep]);
    (void)witness_payload_u64(t, hostcall(t, "gpu.library", args),
                              "gpu.library", 0);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, pipeline_request_raster(t, t->vstack[keep], 32, 5));
    uint64_t pipeline = witness_payload_u64(
        t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);
    t->vsp = keep;

    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, DUMP_W);
    pl_vpush(t, DUMP_H);
    pl_vpush(t, 1); /* bgra8 */
    uint64_t color = witness_payload_u64(t, hostcall(t, "gpu.target", args),
                                         "gpu.target", 0);
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, DUMP_W);
    pl_vpush(t, DUMP_H);
    pl_vpush(t, 5); /* d32 */
    uint64_t depth = witness_payload_u64(t, hostcall(t, "gpu.target", args),
                                         "gpu.target", 0);

    /* row 0: near blue quad; row 1: far red quad — same footprint */
    float payload[16] = {
        0.25f, 0.25f, 0.5f, 0.2f, 0.0f, 0.0f, 1.0f, 1.0f, /* near blue */
        0.25f, 0.25f, 0.5f, 0.8f, 1.0f, 0.0f, 0.0f, 1.0f, /* far red */
    };
    uint64_t root_gen = 0;
    uint64_t root_idx =
        alloc_bytes(t, session, payload, sizeof(payload), &root_gen);

    /* one draw step, 2 instances; depth clears to 1.0f and stores
     * DONT-CARE; ds-state Less + write */
    args = t->vsp;
    pl_vpush(t, graph_record(t, 6, 2, 1, 0, 0));
    pl_vpush(t, session);
    pl_vpush(t, pipeline);
    pl_vpush(
        t, pass_desc_depth(t, color, 0xff000000u, depth, 2, 0x3f800000u, 2, 1));
    pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(payload), root_gen));
    pl_vpush(t, 0);
    uint64_t sig = witness_payload_u64(
        t, hostcall(t, "gpu.command-graph", args), "gpu.command-graph", 2);
    dump_frame(t, session, color, DUMP_W, DUMP_H, sig, dir, "depth.ppm");
  }

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/*
 * Aaltonen's frames-in-flight pattern over the admitted mechanics:
 * FRAMES_IN_FLIGHT root allocations cycled round-robin, each frame
 * pacing on the present event from two frames back before rewriting
 * its slot — the per-frame consume-and-mint bump IS the bump-allocator
 * pattern — then drawing, acquiring, presenting.  The loop is recorded
 * live through the er_io_hook seam and replayed on a fresh runtime:
 * the recorded session was closed live and the replay runtime never
 * opened a layer, so identical present witnesses prove the whole paced
 * loop — waits, writes, acquires, presents — replays with no drawable
 * and no GPU.
 */
Test(hostcall, gfx_frame_loop_paces_and_replays) {
  enum { FRAMES_IN_FLIGHT = 2, FRAME_COUNT = 6 };
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during frame-loop slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  er_log* log = er_log_new();
  er_scheduler* sys = er_scheduler_new(rt.store, (er_config){0});
  (void)er_scheduler_adopt(sys, t);
  er_scheduler_record(sys, log);

  /* ── Phase 1: record the paced loop live ── */
  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; frame-loop slice skipped");
    pl_set_io_hook(NULL);
    er_scheduler_free(sys);
    er_log_free(log);
    pl_catch_pop(t, &c);
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);     /* scope: session */
    pl_vpush(t, 65536); /* capacity */
    pl_vpush(t, 0);     /* generation-policy: bump-on-free */
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  pl_val msl = bar_nat(t, (const uint8_t*)gfx_msl, sizeof(gfx_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl); /* root across calls */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t pipeline = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t target =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_val sw = hostcall(t, "gpu.surface-open", args);
  uint64_t s1 = witness_payload_u64(t, sw, "gpu.surface-open", 0);
  uint64_t g1 = witness_payload_u64(t, sw, "gpu.surface-open", 1);
  cr_assert_eq(witness_payload_u64(t, sw, "gpu.surface-open", 2),
               (uint64_t)GFX_W);
  cr_assert_eq(witness_payload_u64(t, sw, "gpu.surface-open", 3),
               (uint64_t)GFX_H);

  /* the ring: FRAMES_IN_FLIGHT root allocations, one slice row each */
  float payload0[8] = {0.25f, 0.25f, 0.5f, 0.5f, 1.0f, 0.5f, 0.0f, 1.0f};
  uint64_t root_idx[FRAMES_IN_FLIGHT];
  uint64_t root_gen0[FRAMES_IN_FLIGHT]; /* as minted by gpu.alloc */
  uint64_t root_gen[FRAMES_IN_FLIGHT];  /* live, bumped per write */
  for (int slot = 0; slot < FRAMES_IN_FLIGHT; slot++) {
    root_idx[slot] =
        alloc_bytes(t, session, payload0, sizeof(payload0), &root_gen0[slot]);
    root_gen[slot] = root_gen0[slot];
  }

  uint64_t write_gen[FRAME_COUNT];
  uint64_t drawable[FRAME_COUNT];
  uint64_t present_event[FRAME_COUNT];
  for (int i = 0; i < FRAME_COUNT; i++) {
    int slot = i % FRAMES_IN_FLIGHT;

    /* the pacing wait: frame i blocks on frame i-2's present event
     * before rewriting that frame's root slot */
    if (i >= FRAMES_IN_FLIGHT) {
      args = t->vsp;
      pl_vpush(t, session);
      pl_vpush(t, present_event[i - FRAMES_IN_FLIGHT]);
      cr_assert_eq(
          witness_payload_u64(t, hostcall(t, "gpu.wait", args), "gpu.wait", 0),
          present_event[i - FRAMES_IN_FLIGHT]);
    }

    /* rewrite the slot with the per-frame payload: consume the live
     * generation, mint the successor (the bump-allocator pattern) */
    float payload[8];
    memcpy(payload, payload0, sizeof(payload));
    payload[5] = 0.125f * (float)i; /* vary the color G lane */
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, pointer_record(t, root_idx[slot], 0, sizeof(payload),
                               root_gen[slot]));
    pl_vpush(t, bar_nat(t, (const uint8_t*)payload, sizeof(payload)));
    pl_val ww = hostcall(t, "gpu.write", args);
    cr_assert_eq(witness_payload_u64(t, ww, "gpu.write", 0), root_idx[slot]);
    write_gen[i] = witness_payload_u64(t, ww, "gpu.write", 1);
    cr_assert_eq(write_gen[i], root_gen[slot] + 1);
    root_gen[slot] = write_gen[i];

    /* draw the frame from the freshly minted generation */
    args = t->vsp;
    pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0));
    pl_vpush(t, session);
    pl_vpush(t, pipeline);
    pl_vpush(t, pass_desc(t, target, 0xff000000u));
    pl_vpush(t, pointer_record(t, root_idx[slot], 0, sizeof(payload),
                               root_gen[slot]));
    pl_vpush(t, 0); /* no descriptor heap */
    (void)witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                              "gpu.command-graph", 0);

    /* acquire + present.  A headless host may vend no drawable: skip
     * on the FIRST 947 only — after one success every later frame must
     * acquire, because the pacing waits keep the in-flight count at
     * FRAMES_IN_FLIGHT, under the layer's drawable pool. */
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, s1);
    pl_vpush(t, g1);
    pl_val aw = hostcall(t, "gpu.acquire", args);
    if (i == 0 && refusal_kind(aw, "gpu.acquire") == 947) {
      cr_log_warn("no drawable vended; frame-loop slice skipped");
      args = t->vsp;
      pl_vpush(t, session);
      (void)hostcall(t, "gpu.session-close", args);
      pl_set_io_hook(NULL);
      er_scheduler_free(sys);
      er_log_free(log);
      pl_catch_pop(t, &c);
      test_rt_free(&rt);
      return;
    }
    drawable[i] = witness_payload_u64(t, aw, "gpu.acquire", 0);
    cr_assert_eq(witness_payload_u64(t, aw, "gpu.acquire", 1), g1);

    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, drawable[i]);
    pl_vpush(t, target);
    pl_val pw = hostcall(t, "gpu.present", args);
    cr_assert_eq(witness_payload_u64(t, pw, "gpu.present", 0), s1);
    cr_assert_eq(witness_payload_u64(t, pw, "gpu.present", 1), drawable[i]);
    cr_assert_eq(witness_payload_u64(t, pw, "gpu.present", 2), g1);
    present_event[i] = witness_payload_u64(t, pw, "gpu.present", 3);
    if (i > 0)
      cr_assert_gt(present_event[i], present_event[i - 1]);
    else
      cr_assert_gt(present_event[i], 0u);
  }

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  size_t events = er_log_events(log);

  /* ── Phase 2: replay the loop on a fresh runtime, no GPU ── */
  test_rt rt2 = test_rt_new();
  pl_thread* t2 = rt2.t;
  t2->hostcall_f = true;
  er_scheduler* sys2 = er_scheduler_new(rt2.store, (er_config){0});
  (void)er_scheduler_adopt(sys2, t2);
  er_scheduler_replay(sys2, log);

  args = t2->vsp;
  pl_vpush(t2, 0);
  cr_assert_eq(witness_payload_u64(t2, hostcall(t2, "gpu.session-open", args),
                                   "gpu.session-open", 0),
               session);
  args = t2->vsp;
  pl_vpush(t2, session);
  {
    size_t f = t2->vsp;
    pl_vpush(t2, 1);
    pl_vpush(t2, 65536);
    pl_vpush(t2, 0);
    pl_vpush(t2, record(t2, "memory-arena", f));
  }
  (void)witness_payload_u64(t2, hostcall(t2, "gpu.arena", args), "gpu.arena",
                            0);

  pl_val msl2 = bar_nat(t2, (const uint8_t*)gfx_msl, sizeof(gfx_msl) - 1);
  size_t keep2 = t2->vsp;
  pl_vpush(t2, msl2); /* root across calls */
  args = t2->vsp;
  pl_vpush(t2, session);
  pl_vpush(t2, t2->vstack[keep2]);
  (void)witness_payload_u64(t2, hostcall(t2, "gpu.library", args),
                            "gpu.library", 0);
  args = t2->vsp;
  pl_vpush(t2, session);
  pl_vpush(t2, t2->vstack[keep2]);
  cr_assert_eq(witness_payload_u64(t2,
                                   hostcall(t2, "gpu.pipeline-render", args),
                                   "gpu.pipeline-render", 0),
               pipeline);

  args = t2->vsp;
  pl_vpush(t2, session);
  pl_vpush(t2, GFX_W);
  pl_vpush(t2, GFX_H);
  pl_vpush(t2, 1);
  cr_assert_eq(witness_payload_u64(t2, hostcall(t2, "gpu.target", args),
                                   "gpu.target", 0),
               target);

  args = t2->vsp;
  pl_vpush(t2, session);
  pl_vpush(t2, GFX_W);
  pl_vpush(t2, GFX_H);
  pl_val sw2 = hostcall(t2, "gpu.surface-open", args);
  cr_assert_eq(witness_payload_u64(t2, sw2, "gpu.surface-open", 0), s1);
  cr_assert_eq(witness_payload_u64(t2, sw2, "gpu.surface-open", 1), g1);

  uint64_t regen[FRAMES_IN_FLIGHT];
  for (int slot = 0; slot < FRAMES_IN_FLIGHT; slot++) {
    uint64_t g = 0;
    cr_assert_eq(alloc_bytes(t2, session, payload0, sizeof(payload0), &g),
                 root_idx[slot]);
    cr_assert_eq(g, root_gen0[slot]);
    regen[slot] = g;
  }

  /* the frame loop again, witness for witness — this runtime holds no
   * session, no surface, no drawable, yet every present answers */
  for (int i = 0; i < FRAME_COUNT; i++) {
    int slot = i % FRAMES_IN_FLIGHT;
    if (i >= FRAMES_IN_FLIGHT) {
      args = t2->vsp;
      pl_vpush(t2, session);
      pl_vpush(t2, present_event[i - FRAMES_IN_FLIGHT]);
      cr_assert_eq(witness_payload_u64(t2, hostcall(t2, "gpu.wait", args),
                                       "gpu.wait", 0),
                   present_event[i - FRAMES_IN_FLIGHT]);
    }

    float payload[8];
    memcpy(payload, payload0, sizeof(payload));
    payload[5] = 0.125f * (float)i;
    args = t2->vsp;
    pl_vpush(t2, session);
    pl_vpush(t2, pointer_record(t2, root_idx[slot], 0, sizeof(payload),
                                regen[slot]));
    pl_vpush(t2, bar_nat(t2, (const uint8_t*)payload, sizeof(payload)));
    pl_val ww2 = hostcall(t2, "gpu.write", args);
    cr_assert_eq(witness_payload_u64(t2, ww2, "gpu.write", 0), root_idx[slot]);
    cr_assert_eq(witness_payload_u64(t2, ww2, "gpu.write", 1), write_gen[i]);
    regen[slot] = write_gen[i];

    args = t2->vsp;
    pl_vpush(t2, graph_record(t2, 6, 1, 1, 0, 0));
    pl_vpush(t2, session);
    pl_vpush(t2, pipeline);
    pl_vpush(t2, pass_desc(t2, target, 0xff000000u));
    pl_vpush(t2, pointer_record(t2, root_idx[slot], 0, sizeof(payload),
                                regen[slot]));
    pl_vpush(t2, 0);
    (void)witness_payload_u64(t2, hostcall(t2, "gpu.command-graph", args),
                              "gpu.command-graph", 0);

    args = t2->vsp;
    pl_vpush(t2, session);
    pl_vpush(t2, s1);
    pl_vpush(t2, g1);
    pl_val aw2 = hostcall(t2, "gpu.acquire", args);
    cr_assert_eq(witness_payload_u64(t2, aw2, "gpu.acquire", 0), drawable[i]);
    cr_assert_eq(witness_payload_u64(t2, aw2, "gpu.acquire", 1), g1);

    args = t2->vsp;
    pl_vpush(t2, session);
    pl_vpush(t2, drawable[i]);
    pl_vpush(t2, target);
    pl_val pw2 = hostcall(t2, "gpu.present", args);
    cr_assert_eq(witness_payload_u64(t2, pw2, "gpu.present", 0), s1);
    cr_assert_eq(witness_payload_u64(t2, pw2, "gpu.present", 1), drawable[i]);
    cr_assert_eq(witness_payload_u64(t2, pw2, "gpu.present", 2), g1);
    cr_assert_eq(witness_payload_u64(t2, pw2, "gpu.present", 3),
                 present_event[i]);
  }

  /* every recorded event but the live session-close was consumed */
  cr_assert_eq(er_scheduler_log_cursor(sys2), events - 1);

  pl_set_io_hook(NULL);
  er_scheduler_free(sys2);
  er_scheduler_free(sys);
  er_log_free(log);
  pl_catch_pop(t, &c);
  test_rt_free(&rt2);
  test_rt_free(&rt);
}

/* pipeline_request_raster2 with the writemask leg: the 4-field
 * raster-state (depth-format, color-count, writemask-nibbles,
 * reserved).  The mask is 4 bits per color target, low nibble =
 * target 0, Metal bit order (Alpha=1, Blue=2, Green=4, Red=8);
 * nibble 0 means full RGBA.  The mask joins the pipeline cache key. */
static pl_val pipeline_request_raster4(pl_thread* t, pl_val msl_bar,
                                       uint64_t stride, uint64_t depth_format,
                                       uint64_t color_count, uint64_t wmask) {
  size_t base = t->vsp;
  pl_vpush(t, msl_bar); /* root the bar */
  size_t f = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 1); /* language: msl */
  pl_val art = record(t, "artifact", f);
  pl_vpush(t, art); /* base+1 */
  f = t->vsp;
  pl_vpush(t, stride);
  pl_vpush(t, 1); /* count */
  pl_vpush(t, 0); /* segments */
  pl_vpush(t, 1); /* mode: buffer */
  pl_val layout = record(t, "root-layout", f);
  pl_vpush(t, layout); /* base+2 */
  f = t->vsp;
  pl_vpush(t, depth_format);
  pl_vpush(t, color_count);
  pl_vpush(t, wmask);
  pl_vpush(t, 0); /* reserved */
  pl_val rast = record(t, "raster-state", f);
  pl_vpush(t, rast); /* base+3 */
  f = t->vsp;
  pl_vpush(t, t->vstack[base + 1]);
  pl_vpush(t, t->vstack[base + 2]);
  pl_vpush(t, t->vstack[base + 3]);
  pl_val req = record(t, "pipeline-request", f);
  t->vsp = base;
  return req;
}

/* pass-desc with TWO color attachments and no depth leg; the colors
 * pair-list is built back to front, so color(0) heads the list. */
static pl_val pass_desc_mrt(pl_thread* t, uint64_t target0, uint64_t target1,
                            uint64_t clear_bgra8) {
  size_t base = t->vsp;
  pl_vpush(t, 0); /* colors list at base, built back to front */
  for (int i = 0; i < 2; i++) {
    size_t f = t->vsp;
    pl_vpush(t, i == 0 ? target1 : target0);
    pl_vpush(t, 1); /* load: clear */
    pl_vpush(t, 1); /* store: store */
    pl_vpush(t, clear_bgra8);
    pl_val ca = record(t, "color-attachment", f);
    pl_vpush(t, ca);
    pl_vpush(t, t->vstack[base]);
    t->vstack[base] = record(t, "pair", f);
  }
  size_t f = t->vsp;
  pl_vpush(t, t->vstack[base]);
  pl_vpush(t, 0); /* no depth attachment */
  pl_vpush(t, 0); /* no ds-state */
  pl_val pd = record(t, "pass-desc", f);
  t->vsp = base;
  return pd;
}

/*
 * Format-parameterized sampled textures + the color write mask.  The
 * R8 leg uploads a 2x2 single-channel diagonal (format 3, 1 byte/px)
 * and samples it full-screen through the descriptor heap: r8 sampling
 * yields (r, 0, 0, 1), so the quadrants land red/black/black/red.  A
 * payload sized for 4 bytes/px refuses bounds (948); an unknown
 * format refuses as carrier shape (951).  The writemask leg drives
 * the exit-criterion MRT fragment under the 4-field raster-state: a
 * full-mask control shows color(1) receives the channel-swapped twin
 * (red quad -> blue), then a green-only mask on target 1 suppresses
 * that write — blue has zero green, so the pixel stays the clear
 * black.  The cache leg proves the mask joined the pipeline key: the
 * same artifact+layout under wmask 0 and wmask 0x40 are both cold,
 * and re-requesting the second is a hit.
 */
Test(hostcall, gfx_r8_texture_and_writemask) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during r8/writemask slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; r8/writemask slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  /* ── R8 leg ── */

  /* 2x2 R8 diagonal: row 0 {255, 0}, row 1 {0, 255} — 1 byte/px */
  static const uint8_t r8_texels[4] = {255, 0, 0, 255};
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, 2);
  pl_vpush(t, 2);
  pl_vpush(t, bar_nat(t, r8_texels, sizeof(r8_texels)));
  pl_vpush(t, 3); /* r8 */
  uint64_t r8_tex = witness_payload_u64(t, hostcall(t, "gpu.texture-2d", args),
                                        "gpu.texture-2d", 0);

  /* a payload sized w*h*4 for the 1-byte/px format refuses bounds */
  static const uint8_t wrong[16] = {0};
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, 2);
  pl_vpush(t, 2);
  pl_vpush(t, bar_nat(t, wrong, sizeof(wrong)));
  pl_vpush(t, 3); /* r8 */
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.texture-2d", args), "gpu.texture-2d"),
      948u);

  /* an unknown format refuses as carrier shape */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, 2);
  pl_vpush(t, 2);
  pl_vpush(t, bar_nat(t, r8_texels, sizeof(r8_texels)));
  pl_vpush(t, 9);
  cr_assert_eq(
      refusal_kind(hostcall(t, "gpu.texture-2d", args), "gpu.texture-2d"),
      951u);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1); /* count */
    pl_vpush(t, 1); /* stride-class: sampled */
    pl_vpush(t, record(t, "descriptor-heap", f));
  }
  uint64_t dheap = witness_payload_u64(
      t, hostcall(t, "gpu.descriptor-heap", args), "gpu.descriptor-heap", 0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, dheap);
  pl_vpush(t, window_record(t, dheap, 0, 1));
  pl_vpush(t, r8_tex);
  (void)witness_payload_u64(t, hostcall(t, "gpu.descriptor-write", args),
                            "gpu.descriptor-write", 0);

  pl_val msl = bar_nat(t, (const uint8_t*)gfx_textured_msl,
                       sizeof(gfx_textured_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl); /* root across calls */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t sampler_pipe = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t r8_target =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  /* full-target quad; color field unused by the textured artifact */
  float payload[8] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  uint64_t root_gen = 0;
  uint64_t root_idx =
      alloc_bytes(t, session, payload, sizeof(payload), &root_gen);

  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, sampler_pipe);
  pl_vpush(t, pass_desc(t, r8_target, 0xff000000u));
  pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(payload), root_gen));
  pl_vpush(t, dheap);
  (void)witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                            "gpu.command-graph", 0);

  /* quadrant centers: r8 sampling yields (r, 0, 0, 1) */
  static const struct {
    uint64_t x, y;
    uint64_t bgra;
  } probes[4] = {
      {GFX_W / 4, GFX_H / 4, 0xffff0000u},         /* 255 -> red */
      {3 * GFX_W / 4, GFX_H / 4, 0xff000000u},     /* 0 -> black */
      {GFX_W / 4, 3 * GFX_H / 4, 0xff000000u},     /* 0 -> black */
      {3 * GFX_W / 4, 3 * GFX_H / 4, 0xffff0000u}, /* 255 -> red */
  };
  for (int i = 0; i < 4; i++) {
    uint64_t pixel =
        readback_pixel(t, session, r8_target, probes[i].x, probes[i].y);
    cr_assert_eq(pixel, probes[i].bgra, "r8 quadrant %d: got %08llx", i,
                 (unsigned long long)pixel);
  }

  /* ── Writemask + cache leg ── */

  t->vstack[keep] = bar_nat(t, (const uint8_t*)gfx_exit_mrt_msl,
                            sizeof(gfx_exit_mrt_msl) - 1);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);

  /* full-mask control: cold */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pipeline_request_raster4(t, t->vstack[keep], 32, 0, 2, 0));
  pl_val fw = hostcall(t, "gpu.pipeline-render", args);
  uint64_t full_pipe = witness_payload_u64(t, fw, "gpu.pipeline-render", 0);
  cr_assert_eq(witness_payload_u64(t, fw, "gpu.pipeline-render", 1), 0u);

  /* target 0 full (nibble 0), target 1 green-only (0x4): a DIFFERENT
   * key — cold again, so the mask demonstrably joined the key */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pipeline_request_raster4(t, t->vstack[keep], 32, 0, 2, 0x4 << 4));
  pl_val mw = hostcall(t, "gpu.pipeline-render", args);
  uint64_t mask_pipe = witness_payload_u64(t, mw, "gpu.pipeline-render", 0);
  cr_assert_eq(witness_payload_u64(t, mw, "gpu.pipeline-render", 1), 0u);

  /* the same masked request again: a hit */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, pipeline_request_raster4(t, t->vstack[keep], 32, 0, 2, 0x4 << 4));
  pl_val mw2 = hostcall(t, "gpu.pipeline-render", args);
  cr_assert_eq(witness_payload_u64(t, mw2, "gpu.pipeline-render", 1), 1u);

  /* four bgra8 attachments: a control pair and a masked pair */
  uint64_t targets[4];
  for (int i = 0; i < 4; i++) {
    args = t->vsp;
    pl_vpush(t, session);
    pl_vpush(t, GFX_W);
    pl_vpush(t, GFX_H);
    pl_vpush(t, 1); /* bgra8 */
    targets[i] = witness_payload_u64(t, hostcall(t, "gpu.target", args),
                                     "gpu.target", 0);
  }

  /* one RED quad; the MRT fragment writes color(0) = red and
   * color(1) = the channel-swapped twin (blue) */
  float red_quad[8] = {0.25f, 0.25f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f};
  uint64_t red_gen = 0;
  uint64_t red_idx =
      alloc_bytes(t, session, red_quad, sizeof(red_quad), &red_gen);

  /* control: full mask on both targets */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, full_pipe);
  pl_vpush(t, pass_desc_mrt(t, targets[0], targets[1], 0xff000000u));
  pl_vpush(t, pointer_record(t, red_idx, 0, sizeof(red_quad), red_gen));
  pl_vpush(t, 0);
  (void)witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                            "gpu.command-graph", 0);
  cr_assert_eq(readback_pixel(t, session, targets[0], GFX_W / 2, GFX_H / 2),
               0xffff0000u); /* color(0): red */
  cr_assert_eq(readback_pixel(t, session, targets[1], GFX_W / 2, GFX_H / 2),
               0xff0000ffu); /* color(1): swapped red = blue */

  /* masked: green-only on target 1 — the swapped twin is blue, its
   * green channel is zero, so the write lands nothing: the pixel
   * stays the clear black although the control just proved the
   * fragment produces blue there */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, mask_pipe);
  pl_vpush(t, pass_desc_mrt(t, targets[2], targets[3], 0xff000000u));
  pl_vpush(t, pointer_record(t, red_idx, 0, sizeof(red_quad), red_gen));
  pl_vpush(t, 0);
  (void)witness_payload_u64(t, hostcall(t, "gpu.command-graph", args),
                            "gpu.command-graph", 0);
  cr_assert_eq(readback_pixel(t, session, targets[2], GFX_W / 2, GFX_H / 2),
               0xffff0000u); /* target 0 nibble 0 = full RGBA: red */
  uint64_t masked =
      readback_pixel(t, session, targets[3], GFX_W / 2, GFX_H / 2);
  cr_assert_eq(masked, 0xff000000u,
               "green-only mask: target 1 center %08llx, want clear black",
               (unsigned long long)masked);
  cr_assert_eq(readback_pixel(t, session, targets[3], 1, 1), 0xff000000u);

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

/*
 * A GPU-written descriptor: descriptors are bytes in an admitted
 * buffer, so a kernel can mint one on device.  The descriptor heap is
 * bound at COMPUTE buffer index 2 (the gm_command_graph compute path);
 * the kernel copies heap[0] — the host-written quadrant-texture
 * descriptor — into heap[1], which the host never wrote.  A render
 * graph waits the compute signal (the Gap 8 consumer half) and
 * samples heap[1] full-screen: the quadrant pixels arriving through
 * index 1 prove the sampled descriptor was GPU-minted bytes, not a
 * host binding call.
 */
static const char gfx_heap_copy_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void shrine_heap_copy(uint tid [[thread_position_in_grid]],\n"
    "                             const device ulong* root [[buffer(0)]],\n"
    "                             device ulong* heap [[buffer(2)]]) {\n"
    "  (void)root;\n"
    "  if (tid != 0) return;\n"
    "  heap[1] = heap[0];\n"
    "}\n";

/* gfx_textured_msl reading heap index 1 instead of 0. */
static const char gfx_textured1_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Row { float4 box; float4 color; };\n"
    "struct DHeap { texture2d<float> tex; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut shrine_vertex(uint vid [[vertex_id]],\n"
    "                          uint iid [[instance_id]],\n"
    "                          const device Row* rows [[buffer(0)]]) {\n"
    "  Row r = rows[iid];\n"
    "  float2 c[6] = {\n"
    "      float2(r.box.x, r.box.y),\n"
    "      float2(r.box.x + r.box.z, r.box.y),\n"
    "      float2(r.box.x, r.box.y + r.box.w),\n"
    "      float2(r.box.x + r.box.z, r.box.y),\n"
    "      float2(r.box.x + r.box.z, r.box.y + r.box.w),\n"
    "      float2(r.box.x, r.box.y + r.box.w),\n"
    "  };\n"
    "  float2 p = c[vid];\n"
    "  VOut o;\n"
    "  o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "  o.uv = (p - r.box.xy) / r.box.zw;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 shrine_fragment(VOut in [[stage_in]],\n"
    "                                const device DHeap* heap [[buffer(1)]]) "
    "{\n"
    "  constexpr sampler s(filter::nearest);\n"
    "  return heap[1].tex.sample(s, in.uv);\n"
    "}\n";

Test(hostcall, gfx_gpu_written_descriptor) {
  cr_assert(pl_host_gpu_metal_register());
  test_rt rt = test_rt_new();
  pl_thread* t = rt.t;
  t->hostcall_f = true;

  pl_catch c;
  pl_catch_init(t, &c);
  if (setjmp(c.jb) != 0) {
    pl_catch_unwind(t, &c);
    cr_assert_fail("raise during gpu-descriptor slice: %s",
                   t->exn_msg != NULL ? t->exn_msg : "PLAN exception");
  }

  size_t args = t->vsp;
  pl_vpush(t, 0);
  pl_val r = hostcall(t, "gpu.session-open", args);
  if (refusal_kind(r, "gpu.session-open") == 943) {
    cr_log_warn("no metal device; gpu-descriptor slice skipped");
    test_rt_free(&rt);
    return;
  }
  uint64_t session = witness_payload_u64(t, r, "gpu.session-open", 0);

  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 1);
    pl_vpush(t, 65536);
    pl_vpush(t, 0);
    pl_vpush(t, record(t, "memory-arena", f));
  }
  (void)witness_payload_u64(t, hostcall(t, "gpu.arena", args), "gpu.arena", 0);

  /* 2x2 BGRA8 quadrants: TL red, TR green, BL blue, BR white */
  static const uint8_t texels[16] = {
      0,   0, 255, 255, 0,   255, 0,   255, /* row 0: red, green */
      255, 0, 0,   255, 255, 255, 255, 255, /* row 1: blue, white */
  };
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, 2);
  pl_vpush(t, 2);
  pl_vpush(t, bar_nat(t, texels, sizeof(texels)));
  pl_vpush(t, 1); /* bgra8 */
  uint64_t texture = witness_payload_u64(t, hostcall(t, "gpu.texture-2d", args),
                                         "gpu.texture-2d", 0);

  /* two descriptor slots: the host writes index 0 only — index 1
   * stays the zeroed bytes until the kernel mints it */
  args = t->vsp;
  pl_vpush(t, session);
  {
    size_t f = t->vsp;
    pl_vpush(t, 2); /* count */
    pl_vpush(t, 1); /* stride-class: sampled */
    pl_vpush(t, record(t, "descriptor-heap", f));
  }
  uint64_t dheap = witness_payload_u64(
      t, hostcall(t, "gpu.descriptor-heap", args), "gpu.descriptor-heap", 0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, dheap);
  pl_vpush(t, window_record(t, dheap, 0, 1));
  pl_vpush(t, texture);
  (void)witness_payload_u64(t, hostcall(t, "gpu.descriptor-write", args),
                            "gpu.descriptor-write", 0);

  /* the copier kernel; its root is unused but the root discipline
   * still demands an admitted window */
  pl_val msl = bar_nat(t, (const uint8_t*)gfx_heap_copy_msl,
                       sizeof(gfx_heap_copy_msl) - 1);
  size_t keep = t->vsp;
  pl_vpush(t, msl); /* root across calls */
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t kernel = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-compute", args), "gpu.pipeline-compute", 0);

  uint8_t croot[8] = {0};
  uint64_t croot_gen = 0;
  uint64_t croot_idx =
      alloc_bytes(t, session, croot, sizeof(croot), &croot_gen);

  /* one {1,1,1,1} dispatch with the descriptor heap bound: the kernel
   * writes heap[1] = heap[0] on device */
  args = t->vsp;
  pl_vpush(t, graph_compute_record(t, 1, 1, 1, 1, 1, 0, 0));
  pl_vpush(t, session);
  pl_vpush(t, kernel);
  pl_vpush(t, 0); /* compute graph takes no target */
  pl_vpush(t, pointer_record(t, croot_idx, 0, sizeof(croot), croot_gen));
  pl_vpush(t, dheap);
  pl_val cw = hostcall(t, "gpu.command-graph", args);
  cr_assert_eq(witness_payload_u64(t, cw, "gpu.command-graph", 0), 1u);
  uint64_t sig_copy = witness_payload_u64(t, cw, "gpu.command-graph", 2);

  /* the heap[1] sampler */
  t->vstack[keep] = bar_nat(t, (const uint8_t*)gfx_textured1_msl,
                            sizeof(gfx_textured1_msl) - 1);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  (void)witness_payload_u64(t, hostcall(t, "gpu.library", args), "gpu.library",
                            0);
  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, t->vstack[keep]);
  uint64_t pipeline = witness_payload_u64(
      t, hostcall(t, "gpu.pipeline-render", args), "gpu.pipeline-render", 0);

  args = t->vsp;
  pl_vpush(t, session);
  pl_vpush(t, GFX_W);
  pl_vpush(t, GFX_H);
  pl_vpush(t, 1); /* bgra8 */
  uint64_t target =
      witness_payload_u64(t, hostcall(t, "gpu.target", args), "gpu.target", 0);

  /* full-target quad; color field unused by the textured artifact */
  float payload[8] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  uint64_t root_gen = 0;
  uint64_t root_idx =
      alloc_bytes(t, session, payload, sizeof(payload), &root_gen);

  /* the render waits the copy's signal value: heap[1] holds the
   * GPU-minted descriptor before any fragment reads it */
  args = t->vsp;
  pl_vpush(t, graph_record(t, 6, 1, 1, 0, sig_copy));
  pl_vpush(t, session);
  pl_vpush(t, pipeline);
  pl_vpush(t, pass_desc(t, target, 0xff000000u));
  pl_vpush(t, pointer_record(t, root_idx, 0, sizeof(payload), root_gen));
  pl_vpush(t, dheap);
  pl_val dw = hostcall(t, "gpu.command-graph", args);
  cr_assert_eq(witness_payload_u64(t, dw, "gpu.command-graph", 0), 1u);
  cr_assert_gt(witness_payload_u64(t, dw, "gpu.command-graph", 2), sig_copy);

  /* quadrant centers sampled through the descriptor the GPU wrote */
  static const struct {
    uint64_t x, y;
    uint64_t bgra;
  } probes[4] = {
      {GFX_W / 4, GFX_H / 4, 0xffff0000u},         /* red */
      {3 * GFX_W / 4, GFX_H / 4, 0xff00ff00u},     /* green */
      {GFX_W / 4, 3 * GFX_H / 4, 0xff0000ffu},     /* blue */
      {3 * GFX_W / 4, 3 * GFX_H / 4, 0xffffffffu}, /* white */
  };
  for (int i = 0; i < 4; i++) {
    uint64_t pixel =
        readback_pixel(t, session, target, probes[i].x, probes[i].y);
    cr_assert_eq(pixel, probes[i].bgra,
                 "gpu-descriptor quadrant %d: got %08llx", i,
                 (unsigned long long)pixel);
  }

  args = t->vsp;
  pl_vpush(t, session);
  cr_assert_not_null(result_row(hostcall(t, "gpu.session-close", args),
                                "witness", "gpu.session-close"));
  pl_catch_pop(t, &c);
  test_rt_free(&rt);
}

#endif /* PL_HOST_GPU_METAL */
