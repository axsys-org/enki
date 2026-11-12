#include "gc2_internal.h"
#include "axsys/assume.h"
#include "axsys/buddy.h"
#include "axsys/ds.h"
#include "batch.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Buddy blocks are page multiples on both 4 KiB and 16 KiB hosts. Small
 * objects share 64 KiB slabs; larger objects own a rounded buddy extent. */
#define GC2_PAGE ((size_t)65536)
#define GC2_NONE UINT32_MAX
static const size_t classes[] = {16,  32,  48,  64,   96,   128,  192, 256,
                                 384, 512, 768, 1024, 1536, 2048, 4096};
#define GC2_CLASSES (sizeof(classes) / sizeof(classes[0]))

typedef struct gc2_finalizer {
  struct gc2_finalizer* next;
  void (*drop)(void*);
  void* context;
  size_t bytes;
} gc2_finalizer;

typedef struct gc2_slot {
  size_t serial;
  gc2_finalizer* finalizer;
  uint32_t next;
  bool marked;
} gc2_slot;

typedef struct gc2_span {
  struct gc2_span *next, *prev, *avail_next, *avail_prev;
  size_t offset, bytes, stride, klass;
  uint32_t count, used, free_head;
  bool available;
  gc2_slot slots[];
} gc2_span;

struct pl_gc2 {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  pthread_t sweeper;
  bool shutdown, sweeping;
  ax_buddy buddy;
  gc2_span** pages;
  gc2_span* spans;
  gc2_span* available[GC2_CLASSES];
  gc2_span** work;
  size_t work_n;
  size_t serial, allocated_since, threshold, budget;
  pl_gc2_stats stats;
  gc2_finalizer* finalizers;
  pthread_mutex_t world_mu;
  pthread_cond_t world_cv;
  size_t active, blocking;
  bool stopped;
  bool requested; /* atomic via __atomic builtins */
  pl_root_entry* roots;
};

typedef struct gc2_scope {
  pl_store* store;
  size_t depth;
  bool blocking;
  struct gc2_scope* next;
} gc2_scope;
static _Thread_local gc2_scope* scopes;

static gc2_scope* scope_for(pl_store* s) {
  for (gc2_scope* sc = scopes; sc; sc = sc->next)
    if (sc->store == s)
      return sc;
  return NULL;
}

void pl_store_mutator_enter(pl_store* s) {
  if (!s)
    return;
  gc2_scope* sc = scope_for(s);
  if (sc) {
    sc->depth++;
    return;
  }
  sc = malloc(sizeof(*sc));
  ax_assume(sc != NULL, "oom");
  struct pl_gc2* g = s->gc2;
  pthread_mutex_lock(&g->world_mu);
  while (g->stopped)
    pthread_cond_wait(&g->world_cv, &g->world_mu);
  g->active++;
  pthread_mutex_unlock(&g->world_mu);
  *sc = (gc2_scope){.store = s, .depth = 1, .next = scopes};
  scopes = sc;
}

void pl_store_mutator_leave(pl_store* s) {
  if (!s)
    return;
  gc2_scope** at = &scopes;
  while (*at && (*at)->store != s)
    at = &(*at)->next;
  ax_assume(*at != NULL, "gc2 leave without enter");
  gc2_scope* sc = *at;
  if (--sc->depth)
    return;
  *at = sc->next;
  bool blocking = sc->blocking;
  free(sc);
  struct pl_gc2* g = s->gc2;
  pthread_mutex_lock(&g->world_mu);
  ax_assume(g->active != 0, "gc2 active count underflow");
  g->active--;
  if (blocking)
    g->blocking--;
  pthread_cond_broadcast(&g->world_cv);
  pthread_mutex_unlock(&g->world_mu);
}

void pl_store_mutator_blocking(pl_store* s) {
  if (!s)
    return;
  gc2_scope* sc = scope_for(s);
  if (!sc || sc->blocking)
    return;
  struct pl_gc2* g = s->gc2;
  pthread_mutex_lock(&g->world_mu);
  sc->blocking = true;
  g->blocking++;
  /* A collector already waiting for active scopes must reopen the gate:
   * another actor may be needed to complete this operation. */
  pthread_cond_broadcast(&g->world_cv);
  pthread_mutex_unlock(&g->world_mu);
}

void pl_store_gc_add_roots(pl_store* s, pl_root_source fn, void* ctx) {
  pl_store_mutator_enter(s);
  pthread_mutex_lock(&s->mu);
  ax_arrpush(s->gc2->roots, ((pl_root_entry){fn, ctx}));
  pthread_mutex_unlock(&s->mu);
  pl_store_mutator_leave(s);
}

void pl_store_gc_del_roots(pl_store* s, pl_root_source fn, void* ctx) {
  pl_store_mutator_enter(s);
  pthread_mutex_lock(&s->mu);
  ptrdiff_t i;
  for (i = 0; i < ax_arrlen(s->gc2->roots); i++)
    if (s->gc2->roots[i].fn == fn && s->gc2->roots[i].ctx == ctx)
      break;
  ax_assume(i < ax_arrlen(s->gc2->roots), "unknown gc2 root source");
  ax_arrdelswap(s->gc2->roots, i);
  pthread_mutex_unlock(&s->mu);
  pl_store_mutator_leave(s);
}

static void unavailable(struct pl_gc2* g, gc2_span* sp) {
  if (!sp->available)
    return;
  if (sp->avail_prev)
    sp->avail_prev->avail_next = sp->avail_next;
  else
    g->available[sp->klass] = sp->avail_next;
  if (sp->avail_next)
    sp->avail_next->avail_prev = sp->avail_prev;
  sp->avail_next = sp->avail_prev = NULL;
  sp->available = false;
}

static void available(struct pl_gc2* g, gc2_span* sp) {
  if (sp->klass == GC2_CLASSES || sp->available || sp->free_head == GC2_NONE)
    return;
  sp->avail_prev = NULL;
  sp->avail_next = g->available[sp->klass];
  if (sp->avail_next)
    sp->avail_next->avail_prev = sp;
  g->available[sp->klass] = sp;
  sp->available = true;
}

static void span_free(pl_store* s, gc2_span* sp) {
  struct pl_gc2* g = s->gc2;
  unavailable(g, sp);
  if (sp->prev)
    sp->prev->next = sp->next;
  else
    g->spans = sp->next;
  if (sp->next)
    sp->next->prev = sp->prev;
  for (size_t p = sp->offset / GC2_PAGE;
       p < (sp->offset + sp->bytes) / GC2_PAGE; p++)
    g->pages[p] = NULL;
  /* Discard only whole, empty extents. Never discard live slab neighbours. */
  (void)madvise(s->lo + sp->offset, sp->bytes, MADV_DONTNEED);
  ax_buddy_free(&g->buddy, sp->offset, sp->bytes);
  g->stats.span_bytes -= sp->bytes;
  free(sp);
}

static void slot_free(struct pl_gc2* g, gc2_span* sp, uint32_t i) {
  gc2_finalizer* f = sp->slots[i].finalizer;
  if (f) {
    f->next = g->finalizers;
    g->finalizers = f;
    sp->slots[i].finalizer = NULL;
  }
  sp->slots[i].serial = 0;
  sp->slots[i].marked = false;
  sp->slots[i].next = sp->free_head;
  sp->free_head = i;
  sp->used--;
  g->stats.allocated_bytes -= sp->stride;
}

static void sweep_span(pl_store* s, gc2_span* sp) {
  struct pl_gc2* g = s->gc2;
  for (uint32_t i = 0; i < sp->count; i++) {
    if (sp->slots[i].serial && !sp->slots[i].marked) {
      slot_free(g, sp, i);
      g->stats.reclaimed_bytes += sp->stride;
    }
  }
  if (!sp->used)
    span_free(s, sp);
  else
    available(g, sp);
}

static void* sweep_main(void* arg) {
  pl_store* s = arg;
  struct pl_gc2* g = s->gc2;
  pthread_mutex_lock(&g->mu);
  for (;;) {
    while (!g->sweeping && !g->shutdown)
      pthread_cond_wait(&g->cv, &g->mu);
    if (g->shutdown)
      break;
    while (g->work_n) {
      gc2_span* sp = g->work[--g->work_n];
      sweep_span(s, sp);
      /* One slab/extent per lock hold; allocation can also assist below. */
      pthread_mutex_unlock(&g->mu);
      pthread_mutex_lock(&g->mu);
    }
    free(g->work);
    g->work = NULL;
    g->sweeping = false;
    pthread_cond_broadcast(&g->cv);
  }
  pthread_mutex_unlock(&g->mu);
  return NULL;
}

void pl_gc2_init(pl_store* s, size_t capacity) {
  struct pl_gc2* g = calloc(1, sizeof(*g));
  ax_assume(g != NULL, "oom");
  s->gc2 = g;
  ax_assume(ax_buddy_init(&g->buddy, capacity, GC2_PAGE), "gc2 buddy init");
  /* Reserve address space without charging the entire (normally 256 GiB)
   * window against Linux's commit limit, as the previous arena did. */
  s->lo = mmap(NULL, capacity, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  ax_assume(s->lo != MAP_FAILED,
            "gc2 address reservation of %zu bytes failed: %s", capacity,
            strerror(errno));
  s->hi = s->lo + capacity;
  g->pages = calloc(capacity / GC2_PAGE, sizeof(*g->pages));
  ax_assume(g->pages != NULL, "oom");
  g->stats.capacity_bytes = capacity;
  g->threshold = g->budget = (size_t)64 << 20;
  ax_assume(pthread_mutex_init(&g->mu, NULL) == 0, "gc2 mutex");
  ax_assume(pthread_cond_init(&g->cv, NULL) == 0, "gc2 cond");
  ax_assume(pthread_mutex_init(&g->world_mu, NULL) == 0, "gc2 world mutex");
  ax_assume(pthread_cond_init(&g->world_cv, NULL) == 0, "gc2 world cond");
  ax_assume(pthread_create(&g->sweeper, NULL, sweep_main, s) == 0,
            "gc2 sweeper creation");
}

void pl_store_gc_run_finalizers(pl_store* s) {
  struct pl_gc2* g = s->gc2;
  pthread_mutex_lock(&g->mu);
  gc2_finalizer* list = g->finalizers;
  g->finalizers = NULL;
  pthread_mutex_unlock(&g->mu);
  while (list) {
    gc2_finalizer* f = list;
    list = f->next;
    f->drop(f->context);
    pthread_mutex_lock(&g->mu);
    g->stats.external_bytes -= f->bytes;
    pthread_mutex_unlock(&g->mu);
    free(f);
  }
}

void pl_store_gc_wait(pl_store* s) {
  struct pl_gc2* g = s->gc2;
  pthread_mutex_lock(&g->mu);
  while (g->sweeping)
    pthread_cond_wait(&g->cv, &g->mu);
  pthread_mutex_unlock(&g->mu);
  pl_store_gc_run_finalizers(s);
}

void pl_gc2_destroy(pl_store* s) {
  struct pl_gc2* g = s->gc2;
  pl_store_gc_wait(s);
  ax_assume(!g->active && !g->stopped && !s->heaps && !ax_arrlen(g->roots),
            "gc2 teardown with live participants");
  pthread_mutex_lock(&g->mu);
  g->shutdown = true;
  pthread_cond_signal(&g->cv);
  pthread_mutex_unlock(&g->mu);
  ax_assume(pthread_join(g->sweeper, NULL) == 0, "gc2 sweeper join");
  while (g->spans) {
    gc2_span* next = g->spans->next;
    for (uint32_t i = 0; i < g->spans->count; i++)
      if (g->spans->slots[i].serial)
        slot_free(g, g->spans, i);
    free(g->spans);
    g->spans = next;
  }
  pl_store_gc_run_finalizers(s);
  (void)munmap(s->lo, g->buddy.capacity);
  ax_buddy_destroy(&g->buddy);
  free(g->pages);
  ax_arrfree(g->roots);
  pthread_cond_destroy(&g->cv);
  pthread_mutex_destroy(&g->mu);
  pthread_cond_destroy(&g->world_cv);
  pthread_mutex_destroy(&g->world_mu);
  free(g);
}

pl_cell* pl_gc2_alloc(pl_store* s, size_t cells) {
  struct pl_gc2* g = s->gc2;
  if (!cells || cells > SIZE_MAX / sizeof(pl_cell))
    return NULL;
  size_t bytes = cells * sizeof(pl_cell), klass = 0;
  while (klass < GC2_CLASSES && classes[klass] < bytes)
    klass++;
  pthread_mutex_lock(&g->mu);
  gc2_span* sp = klass < GC2_CLASSES ? g->available[klass] : NULL;
  /* Sweep before growing/reusing a span. No new allocation is ever placed
   * in a span still on this epoch's sweep queue. */
  while (!sp && g->work_n) {
    sweep_span(s, g->work[--g->work_n]);
    sp = klass < GC2_CLASSES ? g->available[klass] : NULL;
  }
  if (!sp) {
    size_t actual;
    size_t offset =
        ax_buddy_alloc(&g->buddy, bytes < GC2_PAGE ? GC2_PAGE : bytes, &actual);
    if (offset == SIZE_MAX) {
      pthread_mutex_unlock(&g->mu);
      return NULL;
    }
    size_t stride = klass < GC2_CLASSES ? classes[klass] : actual;
    uint32_t count = (uint32_t)(actual / stride);
    sp = calloc(1, sizeof(*sp) + count * sizeof(gc2_slot));
    ax_assume(sp != NULL, "oom");
    sp->offset = offset;
    sp->bytes = actual;
    sp->stride = stride;
    sp->klass = klass;
    sp->count = count;
    sp->next = g->spans;
    if (sp->next)
      sp->next->prev = sp;
    g->spans = sp;
    for (uint32_t i = 0; i < count; i++)
      sp->slots[i].next = i + 1 < count ? i + 1 : GC2_NONE;
    for (size_t p = offset / GC2_PAGE; p < (offset + actual) / GC2_PAGE; p++)
      g->pages[p] = sp;
    g->stats.span_bytes += actual;
    available(g, sp);
  }
  uint32_t i = sp->free_head;
  ax_assume(i != GC2_NONE, "gc2 full slab in allocation list");
  sp->free_head = sp->slots[i].next;
  ax_assume(g->serial < SIZE_MAX, "gc2 allocation serial overflow");
  sp->slots[i].serial = ++g->serial;
  sp->slots[i].marked = false;
  sp->used++;
  if (sp->free_head == GC2_NONE)
    unavailable(g, sp);
  g->stats.allocated_bytes += sp->stride;
  g->allocated_since = sp->stride > SIZE_MAX - g->allocated_since
                           ? SIZE_MAX
                           : g->allocated_since + sp->stride;
  if (g->threshold && g->allocated_since >= g->budget)
    __atomic_store_n(&g->requested, true, __ATOMIC_RELEASE);
  pl_cell* out = (pl_cell*)(s->lo + sp->offset + i * sp->stride);
  pthread_mutex_unlock(&g->mu);
  return out;
}

size_t pl_gc2_checkpoint(pl_store* s) {
  pthread_mutex_lock(&s->gc2->mu);
  size_t serial = s->gc2->serial;
  pthread_mutex_unlock(&s->gc2->mu);
  return serial;
}

void pl_gc2_rollback(pl_store* s, size_t checkpoint) {
  struct pl_gc2* g = s->gc2;
  pthread_mutex_lock(&g->mu);
  ax_assume(checkpoint <= g->serial, "invalid gc2 checkpoint");
  /* Load transactions retain store.mu throughout. Other allocations take
   * that same lock; post-checkpoint objects are unpublished and swept/fresh. */
  for (gc2_span* sp = g->spans; sp;) {
    gc2_span* next = sp->next;
    bool changed = false;
    for (uint32_t i = 0; i < sp->count; i++)
      if (sp->slots[i].serial > checkpoint) {
        slot_free(g, sp, i);
        changed = true;
      }
    if (changed) {
      if (!sp->used)
        span_free(s, sp);
      else
        available(g, sp);
    }
    sp = next;
  }
  g->serial = checkpoint;
  pthread_mutex_unlock(&g->mu);
}

static gc2_slot* find_slot(pl_store* s, pl_val v, gc2_span** span) {
  uintptr_t addr = pl_addr(v);
  if (addr < (uintptr_t)s->lo || addr >= (uintptr_t)s->hi)
    return NULL;
  size_t offset = addr - (uintptr_t)s->lo;
  gc2_span* sp = s->gc2->pages[offset / GC2_PAGE];
  if (!sp || (offset - sp->offset) % sp->stride ||
      (offset - sp->offset) / sp->stride >= sp->count)
    return NULL;
  gc2_slot* slot = &sp->slots[(offset - sp->offset) / sp->stride];
  if (!slot->serial)
    return NULL;
  if (span)
    *span = sp;
  return slot;
}

bool pl_gc2_live(pl_store* s, pl_val v) {
  if (pl_is_nat63(v))
    return true;
  uintptr_t addr = pl_addr(v);
  if (addr >= s->native_lo && addr < s->native_hi)
    return true;
  gc2_slot* slot = find_slot(s, v, NULL);
  return slot && slot->marked;
}

typedef struct gc2_seen {
  uintptr_t key;
  bool value;
} gc2_seen;
typedef struct gc2_range {
  uintptr_t lo, hi;
} gc2_range;
typedef struct gc2_trace {
  pl_store* store;
  pl_val* todo;
  gc2_seen* seen;
  gc2_seen* codes;
  gc2_range* heaps;
  size_t nheaps;
} gc2_trace;

static int range_cmp(const void* a, const void* b) {
  uintptr_t x = ((const gc2_range*)a)->lo, y = ((const gc2_range*)b)->lo;
  return x < y ? -1 : x > y;
}

static uintptr_t heap_end(gc2_trace* tr, uintptr_t addr) {
  size_t lo = 0, hi = tr->nheaps;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    gc2_range r = tr->heaps[mid];
    if (addr < r.lo)
      hi = mid;
    else if (addr >= r.hi)
      lo = mid + 1;
    else
      return r.hi;
  }
  return 0;
}

static void trace_visit(pl_val* slot, void* ctx) {
  gc2_trace* tr = ctx;
  pl_val v = *slot;
  if (pl_is_nat63(v))
    return;
  pl_store* s = tr->store;
  uintptr_t addr = pl_addr(v);
  if (addr >= s->native_lo && addr < s->native_hi)
    return;
  if (addr >= (uintptr_t)s->lo && addr < (uintptr_t)s->hi) {
    gc2_span* sp = NULL;
    gc2_slot* mark = find_slot(s, v, &sp);
    ax_assume(mark != NULL, "gc2 root points at a freed/non-object address");
    if (mark->marked)
      return;
    mark->marked = true;
    s->gc2->stats.live_bytes += sp->stride;
  } else {
    ax_assume(!(addr & 7) && heap_end(tr, addr), "gc2 unregistered heap root");
    if (ax_hmgeti(tr->seen, addr) >= 0)
      return;
    ax_hmput(tr->seen, addr, true);
  }
  ax_arrpush(tr->todo, v);
}

void pl_gc2_set_finalizer(pl_store* s, pl_val v, void (*drop)(void*),
                          void* context) {
  struct pl_gc2* g = s->gc2;
  gc2_finalizer* f = calloc(1, sizeof(*f));
  ax_assume(f && drop, "invalid finalizer");
  f->drop = drop;
  f->context = context;
  pthread_mutex_lock(&g->mu);
  gc2_slot* slot = find_slot(s, v, NULL);
  ax_assume(slot && slot->serial && !slot->finalizer,
            "invalid resource allocation");
  slot->finalizer = f;
  pthread_mutex_unlock(&g->mu);
}

void pl_store_gc_charge(pl_store* s, pl_val v, size_t bytes) {
  struct pl_gc2* g = s->gc2;
  pthread_mutex_lock(&g->mu);
  gc2_slot* slot = find_slot(s, v, NULL);
  ax_assume(slot && slot->serial && slot->finalizer, "charge without resource");
  size_t old = slot->finalizer->bytes;
  ax_assume(bytes <= SIZE_MAX - (g->stats.external_bytes - old),
            "foreign memory overflow");
  g->stats.external_bytes = g->stats.external_bytes - old + bytes;
  slot->finalizer->bytes = bytes;
  if (bytes > old) {
    size_t delta = bytes - old;
    g->allocated_since = delta > SIZE_MAX - g->allocated_since
                             ? SIZE_MAX
                             : g->allocated_since + delta;
    if (g->threshold && g->allocated_since >= g->budget)
      __atomic_store_n(&g->requested, true, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&g->mu);
}

void pl_gc2_visit_code(const pl_code* code, pl_root_visit visit, void* ctx) {
  if (!code)
    return;
  for (size_t i = 0; i < code->nops;) {
    size_t width = pl_bytecode_width(code->ops + i);
    ax_assume(width <= code->nops - i, "truncated code during gc2");
    if (code->ops[i] == OP_PUSH_LIT || code->ops[i] == OP_INTERP) {
      pl_val v = (pl_val)code->ops[i + 1];
      visit(&v, ctx);
    }
    i += width;
  }
  for (uint32_t i = 0; i < code->nlawblks; i++) {
    pl_val v = code->lawblks[i].law;
    visit(&v, ctx);
  }
}

static void trace_code(gc2_trace* tr, const pl_code* code) {
  if (!code || ax_hmgeti(tr->codes, (uintptr_t)code) >= 0)
    return;
  ax_hmput(tr->codes, (uintptr_t)code, true);
  pl_gc2_visit_code(code, trace_visit, tr);
}

static void trace_code_root(const pl_code* code, void* ctx) {
  trace_code(ctx, code);
}

static void trace_edge(gc2_trace* tr, pl_val v, bool shared) {
  ax_assume(!shared || pl_is_nat63(v) || pl_store_owns(tr->store, v),
            "gc2 object references an actor heap");
  trace_visit(&v, tr);
}

static void trace_drain(gc2_trace* tr) {
  while (ax_arrlen(tr->todo)) {
    pl_val v = ax_arrpop(tr->todo);
    pl_cell* p = pl_ptr(v);
    bool shared = pl_store_owns(tr->store, v);
    uint32_t cells = pl_hdr_cells(p[0]);
    size_t available_bytes;
    if (shared) {
      gc2_span* sp = NULL;
      ax_assume(find_slot(tr->store, v, &sp) != NULL, "lost gc2 object");
      available_bytes = sp->stride;
    } else {
      available_bytes = heap_end(tr, pl_addr(v)) - pl_addr(v);
    }
    ax_assume(cells && cells <= available_bytes / sizeof(pl_cell),
              "invalid object size during gc2");
    uint32_t first = 0, count = 0;
    switch (pl_hdr_kind(p[0])) {
    case PL_K_NAT:
    case PL_K_WORMHOLE: /* host type and payload bytes are opaque */
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
    case PL_K_THKE:
      first = 2;
      count = cells - 2;
      break;
    case PL_K_IND:
    case PL_K_BH:
      first = 1;
      count = 1;
      break;
    case PL_K_PIN:
      if (pl_pin_is_proxy(p)) {
        pl_val target = pl_pin_proxy_target(p);
        trace_edge(tr, target ? target : (pl_val)p[5], shared);
      } else {
        ax_assume(shared, "canonical PIN in actor heap");
        trace_edge(tr, (pl_val)p[5], true);
        first = 7;
        count = pl_hdr_meta(p[0]);
        trace_code(tr, pl_pin_code(p));
      }
      break;
    default:
      ax_abort("invalid kind during gc2");
    }
    ax_assume(first <= cells && count <= cells - first, "invalid gc2 fields");
    for (uint32_t i = 0; i < count; i++)
      trace_edge(tr, (pl_val)p[first + i], shared);
  }
}

static void mark_world(pl_store* s) {
  struct pl_gc2* g = s->gc2;
  gc2_trace tr = {.store = s};
  g->stats.live_bytes = 0;
  for (gc2_span* sp = g->spans; sp; sp = sp->next)
    for (uint32_t i = 0; i < sp->count; i++)
      sp->slots[i].marked = false;
  for (pl_heap* h = s->heaps; h; h = h->gc2_next)
    ax_arrpush(tr.heaps, ((gc2_range){(uintptr_t)h->from, (uintptr_t)h->free}));
  tr.nheaps = (size_t)ax_arrlen(tr.heaps);
  if (tr.nheaps)
    qsort(tr.heaps, tr.nheaps, sizeof(*tr.heaps), range_cmp);
  for (pl_heap* h = s->heaps; h; h = h->gc2_next) {
#ifndef NDEBUG
    ax_assume(h->forbid_depth == 0, "gc2 inside a no-collect window");
#endif
    for (size_t i = 0; i < h->nroots; i++)
      h->roots[i].fn(trace_visit, &tr, h->roots[i].ctx);
    for (pl_thread* t = h->threads; t; t = t->gc2_next) {
      /* Identity cache entries are weak; address reuse must not hit them. */
      memset(t->op_memo, 0, sizeof(t->op_memo));
      for (size_t i = 0; i < t->fsp; i++)
        if (t->fstack[i].kind == PL_F_EXEC || t->fstack[i].kind == PL_F_EXECV)
          trace_code(&tr, t->fstack[i].code);
    }
  }
  for (ptrdiff_t i = 0; i < ax_arrlen(g->roots); i++)
    g->roots[i].fn(trace_visit, &tr, g->roots[i].ctx);
  pl_store_gc_roots(s, trace_visit, trace_code_root, &tr);
  trace_drain(&tr);
  pl_store_gc_weak(s);
  ax_arrfree(tr.heaps);
  ax_arrfree(tr.todo);
  ax_hmfree(tr.seen);
  ax_hmfree(tr.codes);
}

void pl_store_gc_request(pl_store* s) {
  __atomic_store_n(&s->gc2->requested, true, __ATOMIC_RELEASE);
}

static bool collect(pl_store* s, bool force) {
  struct pl_gc2* g = s->gc2;
  ax_assume(scope_for(s) == NULL, "gc2 collection inside a mutator scope");
  pthread_mutex_lock(&g->world_mu);
retry:
  while (g->stopped)
    pthread_cond_wait(&g->world_cv, &g->world_mu);
  if (!force && !__atomic_load_n(&g->requested, __ATOMIC_ACQUIRE)) {
    pthread_mutex_unlock(&g->world_mu);
    return false;
  }
  if (g->blocking) {
    if (force) {
      pthread_cond_wait(&g->world_cv, &g->world_mu);
      goto retry;
    }
    pthread_mutex_unlock(&g->world_mu);
    return false;
  }
  g->stopped = true;
  while (g->active && !g->blocking)
    pthread_cond_wait(&g->world_cv, &g->world_mu);
  if (g->blocking) {
    g->stopped = false;
    pthread_cond_broadcast(&g->world_cv);
    if (force)
      goto retry;
    pthread_mutex_unlock(&g->world_mu);
    return false;
  }
  pthread_mutex_unlock(&g->world_mu);
  pl_store_gc_wait(s);
  /* No mutator can own save_mu/mu now. Follow the store's lock order. */
  pthread_mutex_lock(&s->save_mu);
  pthread_mutex_lock(&s->mu);
  pthread_mutex_lock(&g->mu);
  mark_world(s);
  size_t n = 0;
  for (gc2_span* sp = g->spans; sp; sp = sp->next)
    n++;
  g->work = n ? malloc(n * sizeof(*g->work)) : NULL;
  ax_assume(!n || g->work != NULL, "oom");
  for (gc2_span* sp = g->spans; sp; sp = sp->next) {
    unavailable(g, sp);
    g->work[g->work_n++] = sp;
  }
  g->stats.collections++;
  g->allocated_since = 0;
  g->budget =
      g->stats.live_bytes > g->threshold ? g->stats.live_bytes : g->threshold;
  __atomic_store_n(&g->requested, false, __ATOMIC_RELEASE);
  g->sweeping = true;
  pthread_cond_signal(&g->cv);
  pthread_mutex_unlock(&g->mu);
  pthread_mutex_unlock(&s->mu);
  pthread_mutex_unlock(&s->save_mu);
  pthread_mutex_lock(&g->world_mu);
  g->stopped = false;
  pthread_cond_broadcast(&g->world_cv);
  pthread_mutex_unlock(&g->world_mu);
  return true;
}

void pl_store_gc_collect(pl_store* s) {
  (void)collect(s, true);
}

bool pl_store_gc_poll(pl_store* s) {
  pl_store_gc_run_finalizers(s);
  if (!__atomic_load_n(&s->gc2->requested, __ATOMIC_ACQUIRE))
    return false;
  return collect(s, false);
}

void pl_store_gc_set_threshold(pl_store* s, size_t bytes) {
  struct pl_gc2* g = s->gc2;
  pthread_mutex_lock(&g->mu);
  g->threshold = bytes;
  g->budget = bytes > g->stats.live_bytes ? bytes : g->stats.live_bytes;
  if (bytes && g->allocated_since >= g->budget)
    pl_store_gc_request(s);
  pthread_mutex_unlock(&g->mu);
}

pl_gc2_stats pl_store_gc_stats(pl_store* s) {
  struct pl_gc2* g = s->gc2;
  pthread_mutex_lock(&g->mu);
  pl_gc2_stats stats = g->stats;
  stats.sweeping = g->sweeping;
  pthread_mutex_unlock(&g->mu);
  return stats;
}
