#include "plan/hostcall.h"

#include <string.h>

#include "axsys/assume.h"
#include "internal.h"
#include "plan/build.h"
#include "plan/nat.h"

/*
 * The op-83 binding registry.  Entries share the pl_opdesc shape so the
 * machine drives static and registered ops through one descriptor view
 * (pl_op_desc): indices below pl_nops address pl_ops, indices at or
 * above it address this registry.
 */

#define PL_HOSTCALL_MAX 64

static pl_opdesc g_hostcalls[PL_HOSTCALL_MAX];
static size_t g_nhostcalls;

bool pl_hostcall_register(const char* name_c, uint8_t argc,
                          uint32_t strict_mask, pl_hostcall_body body) {
  ax_assume(name_c != NULL && body != NULL, "hostcall: bad registration");
  if (g_nhostcalls >= PL_HOSTCALL_MAX)
    return false;
  for (size_t i = 0; i < g_nhostcalls; i++) {
    if (g_hostcalls[i].argc == argc &&
        strcmp(g_hostcalls[i].name_c, name_c) == 0)
      return false; /* duplicate-binding refusal */
  }
  g_hostcalls[g_nhostcalls++] = (pl_opdesc){
      .opset = 83,
      .name = 0,
      .name_c = name_c,
      .argc = argc,
      .strict_mask = strict_mask,
      .deep = false,
      .body = body,
  };
  return true;
}

size_t pl_hostcall_count(void) {
  return g_nhostcalls;
}

const pl_opdesc* pl_op_desc(uint32_t idx) {
  if (idx < pl_nops)
    return &pl_ops[idx];
  ax_assume(idx - pl_nops < g_nhostcalls, "hostcall: bad descriptor index");
  return &g_hostcalls[idx - pl_nops];
}

int pl_op_lookup_all(uint64_t opset, pl_val name, uint32_t argc) {
  if (opset != 83)
    return pl_op_lookup(opset, name, argc);
  for (size_t i = 0; i < g_nhostcalls; i++) {
    const pl_opdesc* d = &g_hostcalls[i];
    if (d->argc == argc && pl_nat_name_eq(name, d->name_c))
      return (int)(pl_nops + i);
  }
  return -1;
}

/* ── Witness / refusal rows ────────────────────────────────────────────── */

static pl_val hc_string(pl_thread* t, const char* s) {
  return pl_nat_from_bytes(t, (const uint8_t*)s, strlen(s));
}

/* Assemble (0 elem…) from rooted stack slots [base, vsp): an inert row
 * APP whose elements are [tag, binding, payload…] in slot order with
 * tag/binding pushed last (see callers). */
static pl_val hc_row(pl_thread* t, size_t base, size_t payload_n) {
  uint32_t n = (uint32_t)(t->vsp - base);
  pl_gc_reserve(t, PL_APP_CELLS(n));
  PL_GC_FORBID(t);
  pl_cell* p = pl_bump(t, PL_APP_CELLS(n));
  p[0] = pl_hdr_make(PL_K_APP, 0, 0, PL_APP_CELLS(n));
  p[1] = 0;                               /* head nat 0: row */
  p[2] = t->vstack[base + payload_n];     /* tag */
  p[3] = t->vstack[base + payload_n + 1]; /* binding */
  for (size_t i = 0; i < payload_n; i++)
    p[4 + i] = t->vstack[base + i];
  PL_GC_ALLOW(t);
  pl_val out = pl_make(PL_TAG_APP, p);
  t->vsp = base;
  return out;
}

pl_val pl_hostcall_witness(pl_thread* t, const char* binding_c,
                           size_t payload_base) {
  size_t payload_n = t->vsp - payload_base;
  pl_vpush(t, hc_string(t, "witness"));
  pl_vpush(t, hc_string(t, binding_c));
  return hc_row(t, payload_base, payload_n);
}

pl_val pl_hostcall_refusal(pl_thread* t, const char* binding_c, uint64_t kind,
                           const char* detail_c) {
  size_t base = t->vsp;
  ax_assume(kind <= PL_NAT63_MAX, "hostcall: refusal kind out of range");
  pl_vpush(t, (pl_val)kind);
  pl_vpush(t, hc_string(t, detail_c));
  pl_vpush(t, hc_string(t, "refusal"));
  pl_vpush(t, hc_string(t, binding_c));
  return hc_row(t, base, 2);
}
