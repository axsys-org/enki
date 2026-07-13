#include "plan/wormhole.h"

#include "axsys/assume.h"
#include "host_internal.h"

bool pl_is_wormhole(pl_val v) {
  if (pl_is_nat63(v) || pl_tag(v) != PL_TAG_DEFER)
    return false;
  pl_cell* p = pl_ptr(v);
  return pl_hdr_kind(p[0]) == PL_K_BH && (pl_hdr_flags(p[0]) & PL_F_WORM) != 0;
}

bool pl_wormhole_is_closed(pl_val wormhole) {
  ax_assume(pl_is_wormhole(wormhole), "pl_wormhole_is_closed: not a wormhole");
  return (pl_hdr_flags(pl_ptr(wormhole)[0]) & PL_F_CLOSED) != 0;
}

uint64_t pl_wormhole_token(pl_val wormhole) {
  ax_assume(pl_is_wormhole(wormhole), "pl_wormhole_token: not a wormhole");
  ax_assume(!pl_wormhole_is_closed(wormhole),
            "pl_wormhole_token: wormhole is closed");
  return pl_ptr(wormhole)[1];
}

static pl_val pl_wormhole_bump(pl_thread* t, uint64_t token) {
  pl_cell* p = pl_bump(t, PL_WORMHOLE_CELLS);
  p[0] = pl_hdr_make(PL_K_BH, PL_F_HOLE | PL_F_WORM, 0, PL_WORMHOLE_CELLS);
  p[1] = token;
  return pl_make(PL_TAG_DEFER, p);
}

pl_val pl_wormhole_adopt(pl_thread* t, uint64_t token) {
  ax_assume(pl_host_is_installed(), "pl_wormhole_adopt: host not installed");
  pl_gc_reserve(t, PL_WORMHOLE_CELLS);
  return pl_wormhole_bump(t, token);
}

pl_val pl_wormhole_clone(pl_thread* t, pl_val wormhole) {
  ax_assume(pl_is_wormhole(wormhole), "pl_wormhole_clone: not a wormhole");
  ax_assume(!pl_wormhole_is_closed(wormhole),
            "pl_wormhole_clone: wormhole is closed");

  size_t base = t->vsp;
  bool same_heap = pl_heap_owns(t->heap, wormhole);
  if (same_heap)
    pl_vpush(t, wormhole);
  pl_gc_reserve(t, PL_WORMHOLE_CELLS);
  if (same_heap)
    wormhole = t->vstack[base];
  uint64_t token = pl_wormhole_token(wormhole);
  pl_host_token_retain(token);
  pl_val out = pl_wormhole_bump(t, token);
  if (same_heap)
    t->vsp = base;
  return out;
}

void pl_wormhole_close(pl_val wormhole) {
  ax_assume(pl_is_wormhole(wormhole), "pl_wormhole_close: not a wormhole");
  pl_cell* p = pl_ptr(wormhole);
  if ((pl_hdr_flags(p[0]) & PL_F_CLOSED) != 0)
    return;
  uint64_t token = p[1];
  p[0] = pl_hdr_set_flag(p[0], PL_F_CLOSED);
  pl_host_token_release(token);
}
