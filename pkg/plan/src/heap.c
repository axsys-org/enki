#include "plan/heap.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "axsys/assume.h"
#include "axsys/perf.h"
#include "internal.h"
#include "plan/store.h"

typedef struct pl_root_entry {
  pl_root_source fn;
  void* ctx;
} pl_root_entry;

struct pl_heap {
  pl_cell* from; /* active semispace; bump frontier lives here */
  pl_cell* to;
  pl_cell* free;
  pl_cell* limit;
  size_t cells; /* per-space size */
  size_t live_cells;
  pl_store* store;
  pl_root_entry* roots;
  size_t nroots, rootcap;
#ifndef NDEBUG
  int forbid_depth;
#endif
};

/* ── Heap lifecycle ────────────────────────────────────────────────────── */

static pl_cell* pl_space_alloc(size_t cells) {
  pl_cell* p = malloc(cells * sizeof(pl_cell));
  ax_assume(p != NULL, "heap semispace allocation failed (%zu cells)", cells);
  ax_assume(((uintptr_t)p & 7u) == 0, "semispace not 8-aligned");
  ax_assume(((uintptr_t)(p + cells) & ~PL_ADDR_MASK) == 0,
            "heap address exceeds 56 bits");
  return p;
}

pl_heap* pl_heap_new(size_t cells, pl_store* store) {
  if (cells < 4096)
    cells = 4096;
  pl_heap* h = calloc(1, sizeof(*h));
  ax_assume(h != NULL, "oom");
  h->cells = cells;
  h->from = pl_space_alloc(cells);
  h->to = pl_space_alloc(cells);
  h->free = h->from;
  h->limit = h->from + cells;
  h->store = store;
  return h;
}

void pl_heap_free(pl_heap* h) {
  if (h == NULL)
    return;
  free(h->from);
  free(h->to);
  free(h->roots);
  free(h);
}

pl_store* pl_heap_store(pl_heap* h) {
  return h->store;
}

#ifndef NDEBUG
void pl_gc_forbid(pl_heap* h) {
  h->forbid_depth++;
}
void pl_gc_allow(pl_heap* h) {
  ax_assume(h->forbid_depth > 0, "GC_ALLOW without matching GC_FORBID");
  h->forbid_depth--;
}
#endif

/* ── Root sources ──────────────────────────────────────────────────────── */

void pl_gc_add_root_source(pl_heap* h, pl_root_source src, void* src_ctx) {
  if (h->nroots == h->rootcap) {
    h->rootcap = h->rootcap ? h->rootcap * 2 : 8;
    h->roots = realloc(h->roots, h->rootcap * sizeof(*h->roots));
    ax_assume(h->roots != NULL, "oom");
  }
  h->roots[h->nroots++] = (pl_root_entry){src, src_ctx};
}

void pl_gc_del_root_source(pl_heap* h, pl_root_source src, void* src_ctx) {
  for (size_t i = 0; i < h->nroots; i++) {
    if (h->roots[i].fn == src && h->roots[i].ctx == src_ctx) {
      h->roots[i] = h->roots[h->nroots - 1];
      h->nroots--;
      return;
    }
  }
  ax_abort("pl_gc_del_root_source: source not registered");
}

/* ── Cheney collection ─────────────────────────────────────────────────── */

typedef struct pl_gc_ctx {
  pl_heap* h;
  pl_cell* target;
  pl_cell* target_free;
} pl_gc_ctx;

#ifndef NDEBUG
/* Store values are collector terminals.  A store-resident proxy is therefore
 * valid only when its unresolved body (or its published target) is itself a
 * store value.  Checking the direct edge here catches the dangerous
 * store-to-moving-heap case at the first collection that observes it. */
static void pl_gc_check_store_pin(pl_gc_ctx* gc, pl_val v) {
  pl_cell* p = pl_ptr(v);
  if (pl_hdr_kind(p[0]) != PL_K_PIN || !pl_pin_is_proxy(p))
    return;
  ax_assume(pl_hdr_meta(p[0]) == 0 && pl_hdr_cells(p[0]) == PL_PIN_CELLS(0),
            "store PIN proxy has a non-proxy layout");
  pl_val target = pl_pin_proxy_target(p);
  pl_val edge = target != 0 ? target : (pl_val)p[5];
  ax_assume(pl_is_nat63(edge) || pl_store_owns(gc->h->store, edge),
            "store PIN proxy points into a moving heap");
  if (target != 0) {
    ax_assume(pl_tag(target) == PL_TAG_PIN,
              "store PIN proxy target is not a PIN");
    pl_cell* canonical = pl_ptr(target);
    ax_assume(pl_hdr_kind(canonical[0]) == PL_K_PIN &&
                  !pl_pin_is_proxy(canonical) &&
                  (pl_hdr_flags(canonical[0]) & PL_F_PIN_HASHED) != 0,
              "store PIN proxy target is not canonical");
  }
}

static void pl_gc_check_local_pointer(pl_gc_ctx* gc, pl_cell* p) {
  uintptr_t addr = (uintptr_t)p;
  uintptr_t lo = (uintptr_t)gc->h->from;
  uintptr_t hi = (uintptr_t)gc->h->free;
  ax_assume((addr & (sizeof(pl_cell) - 1u)) == 0 && addr >= lo && addr < hi,
            "collector observed a pointer owned by another heap");
}
#endif

static pl_val pl_forward(pl_gc_ctx* gc, pl_val v) {
  for (;;) {
    if (pl_is_nat63(v))
      return v;
    if (gc->h->store != NULL && pl_store_owns(gc->h->store, v)) {
#ifndef NDEBUG
      pl_gc_check_store_pin(gc, v);
#endif
      return v; /* store region is non-moving and closed */
    }
    pl_cell* p = pl_ptr(v);
#ifndef NDEBUG
    pl_gc_check_local_pointer(gc, p);
#endif
    pl_cell hdr = p[0];
    pl_kind kind = pl_hdr_kind(hdr);
    if (kind == PL_K_FWD)
      return (pl_val)p[1];
    if (kind == PL_K_IND) {
      /* Short-circuit indirections during evacuation; the slot gets the
       * target's stable tag for free. */
      v = (pl_val)p[1];
      continue;
    }
    uint32_t cells = pl_hdr_cells(hdr);
#ifndef NDEBUG
    ax_assume(cells != 0 &&
                  (size_t)cells <=
                      ((uintptr_t)gc->h->free - (uintptr_t)p) / sizeof(pl_cell),
              "collector observed an invalid heap object size");
#endif
    pl_cell* np = gc->target_free;
    gc->target_free += cells;
    memcpy(np, p, cells * sizeof(pl_cell));
    pl_val nv = pl_make(pl_tag_for_kind(kind), np);
    p[0] = pl_hdr_make(PL_K_FWD, 0, 0, cells);
    p[1] = nv;
    return nv;
  }
}

static void pl_gc_visit(pl_val* slot, void* gc_ctx) {
  pl_gc_ctx* gc = gc_ctx;
  *slot = pl_forward(gc, *slot);
}

static void pl_cheney_scan(pl_gc_ctx* gc) {
  pl_cell* scan = gc->target;
  while (scan < gc->target_free) {
    pl_cell hdr = scan[0];
    pl_kind kind = pl_hdr_kind(hdr);
    uint32_t cells = pl_hdr_cells(hdr);
    uint32_t first = 0, count = 0;
    switch (kind) {
    case PL_K_NAT:
      break;
    case PL_K_APP:
    case PL_K_ENV:
      first = 1;
      count = cells - 1;
      break;
    case PL_K_LAW:
      first = 2;
      count = 2;
      break;
    case PL_K_THUNK:
      first = 1;
      count = 2;
      break;
    case PL_K_THKE: {
      pl_val* env = (pl_val*)&scan[1];
      *env = pl_forward(gc, *env);
      for (uint32_t i = 3; i < cells; i++) {
        pl_val* f = (pl_val*)&scan[i];
        *f = pl_forward(gc, *f);
      }
      scan += cells;
      continue;
    }
    case PL_K_IND:
    case PL_K_BH:
      first = 1;
      count = 1;
      break;
    case PL_K_PIN: {
      ax_assume(pl_pin_is_proxy(scan) && pl_hdr_meta(hdr) == 0 &&
                    cells == PL_PIN_CELLS(0),
                "canonical PIN appeared in a moving heap");
      pl_val target = pl_pin_proxy_target(scan);
      if (target == 0) {
        /* The body is the proxy's only live edge until Save publishes a
         * canonical target. */
        pl_val* body = (pl_val*)&scan[5];
        *body = pl_forward(gc, *body);
      } else {
        /* Resolved targets are canonical store PINs.  Check that forwarding
         * treats the target as terminal, then discard the obsolete heap-body
         * edge so it is not retained by this collection. */
        ax_assume(gc->h->store != NULL && pl_tag(target) == PL_TAG_PIN &&
                      pl_store_owns(gc->h->store, target),
                  "resolved PIN proxy target is not store-resident");
        pl_val stable = pl_forward(gc, target);
        ax_assume(stable == target,
                  "resolved PIN proxy target moved during collection");
        scan[5] = 0;
        pl_pin_set_target(scan, stable);
      }
      scan += cells;
      continue;
    }
    default:
      ax_abort("cheney_scan: bad kind %d", (int)kind);
    }
    for (uint32_t i = first; i < first + count; i++) {
      pl_val* f = (pl_val*)&scan[i];
      *f = pl_forward(gc, *f);
    }
    scan += cells;
  }
}

static void pl_collect_into(pl_heap* h, pl_cell* target) {
#ifndef NDEBUG
  ax_assume(h->forbid_depth == 0, "collection inside a no-collect window (I1)");
#endif
  pl_gc_ctx gc = {.h = h, .target = target, .target_free = target};
  for (size_t i = 0; i < h->nroots; i++)
    h->roots[i].fn(pl_gc_visit, &gc, h->roots[i].ctx);
  pl_cheney_scan(&gc);
  h->live_cells = (size_t)(gc.target_free - target);
  h->free = gc.target_free;
}

static void pl_gc_collect(pl_heap* h) {
  pl_collect_into(h, h->to);
  pl_cell* old_from = h->from;
  h->from = h->to;
  h->to = old_from;
  h->limit = h->from + h->cells;
}

static void pl_gc_grow(pl_heap* h, size_t need_cells) {
  size_t want = h->cells;
  while (want < h->live_cells + need_cells + (h->live_cells / 2) + 4096)
    want *= 2;
  pl_cell* nfrom = pl_space_alloc(want);
  pl_cell* nto = pl_space_alloc(want);
  /* live data currently sits in h->from; evacuate it into nfrom */
  pl_cell* old_from = h->from;
  pl_cell* old_to = h->to;
  pl_collect_into(h, nfrom);
  h->from = nfrom;
  h->to = nto;
  h->cells = want;
  h->limit = h->from + want;
  free(old_from);
  free(old_to);
}

void pl_gc_reserve(pl_thread* t, size_t cells) {
  pl_heap* h = t->heap;
#ifdef PL_GC_STRESS
  pl_gc_collect(h);
#endif
  if (ax_likely(h->free + cells <= h->limit))
    return;
  pl_gc_collect(h);
  if (h->free + cells > h->limit)
    pl_gc_grow(h, cells);
  ax_assume(h->free + cells <= h->limit, "heap exhausted after grow");
}

pl_cell* pl_bump(pl_thread* t, size_t cells) {
  pl_heap* h = t->heap;
  ax_assume(h->free + cells <= h->limit, "bump without reserved headroom (I2)");
  pl_cell* p = h->free;
  h->free += cells;
  return p;
}

size_t pl_gc_headroom(pl_thread* t) {
  return (size_t)(t->heap->limit - t->heap->free);
}

size_t pl_gc_live_cells(pl_heap* h) {
  return h->live_cells;
}

bool pl_gc_collect_if_pressure(pl_thread* t, size_t allocation_floor_cells) {
  pl_heap* h = t->heap;
  size_t used_cells = (size_t)(h->free - h->from);
  ax_assume(used_cells >= h->live_cells,
            "heap frontier precedes last live-set size");
  size_t allocated_cells = used_cells - h->live_cells;
  size_t threshold = h->live_cells > allocation_floor_cells
                         ? h->live_cells
                         : allocation_floor_cells;
  if (allocated_cells == 0 || allocated_cells < threshold)
    return false;
  pl_gc_collect(h);
  return true;
}

void pl_gc_collect_now(pl_thread* t) {
  pl_gc_collect(t->heap);
}

/* ── Thread ────────────────────────────────────────────────────────────── */

static _Atomic uint64_t pl_next_profile_lane = 1;

static void pl_thread_roots(pl_root_visit visit, void* gc_ctx, void* src_ctx) {
  pl_thread* t = src_ctx;
  for (size_t i = 0; i < t->vsp; i++)
    visit(&t->vstack[i], gc_ctx);
  for (size_t i = 0; i < t->fsp; i++) {
    visit(&t->fstack[i].a, gc_ctx);
    visit(&t->fstack[i].b, gc_ctx);
  }
  visit(&t->exn, gc_ctx);
  visit(&t->resume_val, gc_ctx);
  visit(&t->blocked_on, gc_ctx);
  visit(&t->result, gc_ctx);
  for (size_t i = 0; i < t->profile_zone_n; i++)
    visit(&t->profile_zones[i].handle, gc_ctx);
}

pl_thread* pl_thread_new(pl_heap* h) {
  pl_thread* t = calloc(1, sizeof(*t));
  ax_assume(t != NULL, "oom");
  t->heap = h;
  t->vcap = 4096;
  t->vstack = malloc(t->vcap * sizeof(pl_val));
  t->fcap = 4096;
  t->fstack = malloc(t->fcap * sizeof(pl_frame));
  ax_assume(t->vstack != NULL && t->fstack != NULL, "oom");
  t->fuel = UINT64_MAX; /* fuel is inert outside pl_thread_run */
  t->profile_next_generation = 1;
  t->profile_lane =
      atomic_fetch_add_explicit(&pl_next_profile_lane, 1, memory_order_relaxed);
  ax_assume(t->profile_lane != 0, "profile lane id exhausted");
  pl_gc_add_root_source(h, pl_thread_roots, t);
  return t;
}

void pl_thread_free(pl_thread* t) {
  if (t == NULL)
    return;
  pl_profile_thread_free(t);
  pl_gc_del_root_source(t->heap, pl_thread_roots, t);
  free(t->vstack);
  free(t->fstack);
  free(t);
}

void pl_vstack_grow(pl_thread* t) {
  t->vcap *= 2;
  /* frames hold vstack offsets as u32 */
  ax_assume(t->vcap <= UINT32_MAX, "vstack exceeds u32 frame offsets");
  t->vstack = realloc(t->vstack, t->vcap * sizeof(pl_val));
  ax_assume(t->vstack != NULL, "oom");
}

void pl_fstack_grow(pl_thread* t) {
  t->fcap *= 2;
  t->fstack = realloc(t->fstack, t->fcap * sizeof(pl_frame));
  ax_assume(t->fstack != NULL, "oom");
}
