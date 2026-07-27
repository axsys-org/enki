#include "store_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "axsys/assume.h"

#define PL_STORE_LOCK_HISTOGRAM_BUCKETS 65

typedef struct pl_store_lock_metric {
  uint64_t count;
  uint64_t total_ns;
  uint64_t max_ns;
  uint64_t histogram[PL_STORE_LOCK_HISTOGRAM_BUCKETS];
} pl_store_lock_metric;

typedef struct pl_store_lock_stats {
  uint64_t acquisitions;
  uint64_t outer_acquisitions;
  uint64_t recursive_acquisitions;
  uint64_t contended_acquisitions;
  uint64_t trylock_failures;
  uint64_t max_depth;
  pl_store_lock_metric wait;
  pl_store_lock_metric owner_hold;
  pl_store_lock_metric recursive_section;
} pl_store_lock_stats;

typedef struct pl_store_lock_shard {
  struct pl_store_lock_shard* next;
  pl_store_lock_stats stats[PL_STORE_LOCK_KIND_COUNT]
                           [PL_STORE_LOCK_CONTEXT_COUNT]
                           [PL_STORE_LOCK_SITE_COUNT];
} pl_store_lock_shard;

struct pl_store_lock_profile {
  FILE* out;
  struct timespec start;
  pthread_mutex_t shards_mu;
  pl_store_lock_shard* shards;
  uint64_t generation;
};

typedef struct pl_store_lock_frame {
  pl_store* store;
  pl_store_lock_kind kind;
  pl_store_lock_context context;
  pl_store_lock_site site;
  uint64_t acquired_ns;
  uint64_t wait_ns;
  uint64_t depth;
  pl_store_lock_shard* shard;
  bool recursive;
  bool contended;
} pl_store_lock_frame;

typedef struct pl_store_lock_completed {
  pl_store_lock_frame frame;
  uint64_t elapsed_ns;
} pl_store_lock_completed;

typedef struct pl_store_lock_tls {
  pl_store_lock_frame* frames;
  size_t count;
  size_t capacity;
  pl_store_lock_completed* completed;
  size_t completed_count;
  size_t completed_capacity;
  pl_store_lock_context context;
  pl_store* context_store;
  pl_store_lock_shard* shard;
  uint64_t shard_generation;
  bool cleanup_registered;
} pl_store_lock_tls;

typedef struct pl_store_lock_metric_snapshot {
  uint64_t count;
  uint64_t total_ns;
  uint64_t max_ns;
  uint64_t histogram[PL_STORE_LOCK_HISTOGRAM_BUCKETS];
} pl_store_lock_metric_snapshot;

typedef struct pl_store_lock_stats_snapshot {
  uint64_t acquisitions;
  uint64_t outer_acquisitions;
  uint64_t recursive_acquisitions;
  uint64_t contended_acquisitions;
  uint64_t trylock_failures;
  uint64_t max_depth;
  pl_store_lock_metric_snapshot wait;
  pl_store_lock_metric_snapshot owner_hold;
  pl_store_lock_metric_snapshot recursive_section;
} pl_store_lock_stats_snapshot;

typedef struct pl_store_lock_aggregate {
  pl_store_lock_stats_snapshot stats[PL_STORE_LOCK_KIND_COUNT]
                                    [PL_STORE_LOCK_CONTEXT_COUNT]
                                    [PL_STORE_LOCK_SITE_COUNT];
} pl_store_lock_aggregate;

static _Thread_local pl_store_lock_tls pl_store_lock_thread;
static _Atomic uint64_t pl_store_lock_next_generation = 1;
static pthread_key_t pl_store_lock_cleanup_key;
static pthread_once_t pl_store_lock_cleanup_once = PTHREAD_ONCE_INIT;

static const char* const pl_store_lock_kind_names[] = {
    [PL_STORE_LOCK_GENERAL] = "mu",
    [PL_STORE_LOCK_SAVE] = "save_mu",
};

static const char* const pl_store_lock_context_names[] = {
    [PL_STORE_LOCK_CONTEXT_OTHER] = "other",
    [PL_STORE_LOCK_CONTEXT_SAVE_SILO] = "save_silo",
    [PL_STORE_LOCK_CONTEXT_SAVE_LEGACY] = "save_legacy",
    [PL_STORE_LOCK_CONTEXT_LOAD_SILO] = "load_silo",
    [PL_STORE_LOCK_CONTEXT_LOAD_LEGACY] = "load_legacy",
    [PL_STORE_LOCK_CONTEXT_COMPILE] = "compile",
    [PL_STORE_LOCK_CONTEXT_COMPILE_EXISTING] = "compile_existing",
    [PL_STORE_LOCK_CONTEXT_COMPILER_INSTALL] = "compiler_install",
    [PL_STORE_LOCK_CONTEXT_SNAPSHOT] = "snapshot",
    [PL_STORE_LOCK_CONTEXT_ACTOR_MESSAGE] = "actor_message",
    [PL_STORE_LOCK_CONTEXT_LAZY_ROW] = "lazy_row",
    [PL_STORE_LOCK_CONTEXT_DIRECT_IO] = "direct_io",
};

static const char* const pl_store_lock_site_names[] = {
    [PL_STORE_LOCK_SITE_ARENA_ALLOC] = "arena_alloc",
    [PL_STORE_LOCK_SITE_ARENA_MARK] = "arena_mark",
    [PL_STORE_LOCK_SITE_ARENA_RELEASE] = "arena_release",
    [PL_STORE_LOCK_SITE_LAW_INDEX] = "law_index",
    [PL_STORE_LOCK_SITE_INTERN_GET] = "intern_get",
    [PL_STORE_LOCK_SITE_BACKEND_PUT] = "backend_put",
    [PL_STORE_LOCK_SITE_BACKEND_GET] = "backend_get",
    [PL_STORE_LOCK_SITE_ROOT_PUT] = "root_put",
    [PL_STORE_LOCK_SITE_ROOT_GET] = "root_get",
    [PL_STORE_LOCK_SITE_CODE_LOOKUP] = "code_lookup",
    [PL_STORE_LOCK_SITE_CODE_PUBLISH] = "code_publish",
    [PL_STORE_LOCK_SITE_COMPILE_EXISTING_SCAN] = "compile_existing_scan",
    [PL_STORE_LOCK_SITE_COMPILER_INSTALL] = "compiler_install",
    [PL_STORE_LOCK_SITE_CANONICAL_REGISTER] = "canonical_register",
    [PL_STORE_LOCK_SITE_LAZY_ROW_INIT] = "lazy_row_init",
    [PL_STORE_LOCK_SITE_SAVE_ROOT] = "save_root",
    [PL_STORE_LOCK_SITE_SAVE_PUBLISH] = "save_publish",
    [PL_STORE_LOCK_SITE_LOAD] = "load",
    [PL_STORE_LOCK_SITE_SILO_LOAD_CLOSURE] = "silo_load_closure",
    [PL_STORE_LOCK_SITE_LEGACY_LOAD_PUBLISH] = "legacy_load_publish",
    [PL_STORE_LOCK_SITE_INSTALL_SELF_CHECK] = "install_self_check",
};

static void pl_store_lock_tls_cleanup(void* value) {
  pl_store_lock_tls* tls = value;
  free(tls->frames);
  free(tls->completed);
  *tls = (pl_store_lock_tls){0};
}

static void pl_store_lock_cleanup_key_init(void) {
  ax_assume(pthread_key_create(&pl_store_lock_cleanup_key,
                               pl_store_lock_tls_cleanup) == 0,
            "store lock profile cleanup key");
}

static void pl_store_lock_tls_register_cleanup(pl_store_lock_tls* tls) {
  if (tls->cleanup_registered)
    return;
  ax_assume(pthread_once(&pl_store_lock_cleanup_once,
                         pl_store_lock_cleanup_key_init) == 0,
            "store lock profile cleanup once");
  ax_assume(pthread_setspecific(pl_store_lock_cleanup_key, tls) == 0,
            "store lock profile cleanup registration");
  tls->cleanup_registered = true;
}

static uint64_t pl_store_lock_now_ns(void) {
  struct timespec now;
  ax_assume(clock_gettime(CLOCK_MONOTONIC, &now) == 0,
            "store lock profile clock_gettime");
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static uint64_t pl_store_lock_timespec_ns(const struct timespec* ts) {
  return (uint64_t)ts->tv_sec * UINT64_C(1000000000) + (uint64_t)ts->tv_nsec;
}

static size_t pl_store_lock_bucket(uint64_t ns) {
  size_t bucket = 0;
  while (ns != 0 && bucket + 1 < PL_STORE_LOCK_HISTOGRAM_BUCKETS) {
    bucket++;
    ns >>= 1;
  }
  return bucket;
}

static void pl_store_lock_metric_record(pl_store_lock_metric* metric,
                                        uint64_t ns) {
  metric->count++;
  metric->total_ns += ns;
  if (metric->max_ns < ns)
    metric->max_ns = ns;
  metric->histogram[pl_store_lock_bucket(ns)]++;
}

static pl_store_lock_shard* pl_store_lock_shard_for(pl_store* s) {
  pl_store_lock_profile* profile = s->lock_profile;
  pl_store_lock_tls* tls = &pl_store_lock_thread;
  if (tls->shard != NULL && tls->shard_generation == profile->generation)
    return tls->shard;
  pl_store_lock_shard* shard = calloc(1, sizeof(*shard));
  ax_assume(shard != NULL, "store lock profile shard allocation");
  ax_assume(pthread_mutex_lock(&profile->shards_mu) == 0,
            "store lock profile shard mutex lock");
  shard->next = profile->shards;
  profile->shards = shard;
  ax_assume(pthread_mutex_unlock(&profile->shards_mu) == 0,
            "store lock profile shard mutex unlock");
  tls->shard = shard;
  tls->shard_generation = profile->generation;
  return shard;
}

static pl_store_lock_stats*
pl_store_lock_stats_for(pl_store_lock_shard* shard, pl_store_lock_kind kind,
                        pl_store_lock_context context,
                        pl_store_lock_site site) {
  ax_assume(kind < PL_STORE_LOCK_KIND_COUNT, "invalid store lock kind");
  ax_assume(context < PL_STORE_LOCK_CONTEXT_COUNT,
            "invalid store lock context");
  ax_assume(site < PL_STORE_LOCK_SITE_COUNT, "invalid store lock site");
  return &shard->stats[kind][context][site];
}

static pl_store_lock_context pl_store_lock_current_context(pl_store* s) {
  return pl_store_lock_thread.context_store == s ? pl_store_lock_thread.context
                                                 : PL_STORE_LOCK_CONTEXT_OTHER;
}

static void pl_store_lock_record_frame(const pl_store_lock_frame* frame,
                                       uint64_t elapsed_ns) {
  pl_store_lock_stats* stats = pl_store_lock_stats_for(
      frame->shard, frame->kind, frame->context, frame->site);
  stats->acquisitions++;
  if (stats->max_depth < frame->depth)
    stats->max_depth = frame->depth;
  if (frame->recursive) {
    stats->recursive_acquisitions++;
    pl_store_lock_metric_record(&stats->recursive_section, elapsed_ns);
  } else {
    stats->outer_acquisitions++;
    pl_store_lock_metric_record(&stats->owner_hold, elapsed_ns);
    if (frame->contended) {
      stats->contended_acquisitions++;
      pl_store_lock_metric_record(&stats->wait, frame->wait_ns);
    }
  }
}

static void pl_store_lock_tls_reserve(void) {
  pl_store_lock_tls* tls = &pl_store_lock_thread;
  pl_store_lock_tls_register_cleanup(tls);
  if (tls->count == tls->capacity) {
    size_t capacity = tls->capacity == 0 ? 16 : tls->capacity * 2;
    pl_store_lock_frame* frames =
        realloc(tls->frames, capacity * sizeof(*frames));
    ax_assume(frames != NULL, "store lock profile frame allocation");
    tls->frames = frames;
    tls->capacity = capacity;
  }
  if (tls->completed_count == tls->completed_capacity) {
    size_t capacity =
        tls->completed_capacity == 0 ? 16 : tls->completed_capacity * 2;
    pl_store_lock_completed* completed =
        realloc(tls->completed, capacity * sizeof(*completed));
    ax_assume(completed != NULL,
              "store lock profile completed-frame allocation");
    tls->completed = completed;
    tls->completed_capacity = capacity;
  }
}

static uint64_t pl_store_lock_depth(pl_store* s, pl_store_lock_kind kind) {
  uint64_t depth = 0;
  for (size_t i = 0; i < pl_store_lock_thread.count; i++)
    if (pl_store_lock_thread.frames[i].store == s &&
        pl_store_lock_thread.frames[i].kind == kind)
      depth++;
  return depth;
}

static pthread_mutex_t* pl_store_lock_mutex(pl_store* s,
                                            pl_store_lock_kind kind) {
  return kind == PL_STORE_LOCK_GENERAL ? &s->mu : &s->save_mu;
}

static void pl_store_lock_profiled(pl_store* s, pl_store_lock_kind kind,
                                   pl_store_lock_site site) {
  pl_store_lock_tls_reserve();
  pl_store_lock_shard* shard = pl_store_lock_shard_for(s);
  uint64_t previous_depth = pl_store_lock_depth(s, kind);
  pthread_mutex_t* mutex = pl_store_lock_mutex(s, kind);
  bool contended = false;
  uint64_t wait_ns = 0;
  uint64_t acquired_ns;
  if (previous_depth != 0) {
    ax_assume(pthread_mutex_lock(mutex) == 0,
              "recursive store pthread_mutex_lock");
    acquired_ns = pl_store_lock_now_ns();
  } else {
    int rc = pthread_mutex_trylock(mutex);
    if (rc == EBUSY) {
      contended = true;
      uint64_t wait_start_ns = pl_store_lock_now_ns();
      ax_assume(pthread_mutex_lock(mutex) == 0, "store pthread_mutex_lock");
      acquired_ns = pl_store_lock_now_ns();
      wait_ns = acquired_ns - wait_start_ns;
    } else {
      ax_assume(rc == 0, "store pthread_mutex_trylock");
      acquired_ns = pl_store_lock_now_ns();
    }
  }
  pl_store_lock_thread.frames[pl_store_lock_thread.count++] =
      (pl_store_lock_frame){
          .store = s,
          .kind = kind,
          .context = pl_store_lock_current_context(s),
          .site = site,
          .acquired_ns = acquired_ns,
          .wait_ns = wait_ns,
          .depth = previous_depth + 1,
          .shard = shard,
          .recursive = previous_depth != 0,
          .contended = contended,
      };
}

static bool pl_store_trylock_profiled(pl_store* s, pl_store_lock_kind kind,
                                      pl_store_lock_site site) {
  pl_store_lock_tls_reserve();
  pl_store_lock_shard* shard = pl_store_lock_shard_for(s);
  uint64_t previous_depth = pl_store_lock_depth(s, kind);
  pthread_mutex_t* mutex = pl_store_lock_mutex(s, kind);
  int rc = pthread_mutex_trylock(mutex);
  if (rc == EBUSY) {
    pl_store_lock_stats* stats = pl_store_lock_stats_for(
        shard, kind, pl_store_lock_current_context(s), site);
    stats->trylock_failures++;
    return false;
  }
  ax_assume(rc == 0, "store pthread_mutex_trylock");
  pl_store_lock_thread.frames[pl_store_lock_thread.count++] =
      (pl_store_lock_frame){
          .store = s,
          .kind = kind,
          .context = pl_store_lock_current_context(s),
          .site = site,
          .acquired_ns = pl_store_lock_now_ns(),
          .depth = previous_depth + 1,
          .shard = shard,
          .recursive = previous_depth != 0,
      };
  return true;
}

static void pl_store_unlock_profiled(pl_store* s, pl_store_lock_kind kind) {
  pl_store_lock_tls* tls = &pl_store_lock_thread;
  ax_assume(tls->count != 0, "store lock profile stack underflow");
  pl_store_lock_frame frame = tls->frames[tls->count - 1];
  ax_assume(frame.store == s && frame.kind == kind,
            "store locks released out of order");
  uint64_t elapsed_ns = pl_store_lock_now_ns() - frame.acquired_ns;
  tls->count--;
  ax_assume(pthread_mutex_unlock(pl_store_lock_mutex(s, kind)) == 0,
            "store pthread_mutex_unlock");
  if (frame.recursive) {
    ax_assume(tls->completed_count < tls->completed_capacity,
              "store lock completed-frame overflow");
    tls->completed[tls->completed_count++] =
        (pl_store_lock_completed){.frame = frame, .elapsed_ns = elapsed_ns};
    return;
  }
  pl_store_lock_record_frame(&frame, elapsed_ns);
  for (size_t i = 0; i < tls->completed_count;) {
    pl_store_lock_completed* completed = &tls->completed[i];
    if (completed->frame.store != s || completed->frame.kind != kind) {
      i++;
      continue;
    }
    pl_store_lock_record_frame(&completed->frame, completed->elapsed_ns);
    tls->completed[i] = tls->completed[--tls->completed_count];
  }
}

void pl_store_lock(pl_store* s, pl_store_lock_site site) {
  if (s->lock_profile == NULL) {
    ax_assume(pthread_mutex_lock(&s->mu) == 0, "pthread_mutex_lock");
    return;
  }
  pl_store_lock_profiled(s, PL_STORE_LOCK_GENERAL, site);
}

bool pl_store_trylock(pl_store* s, pl_store_lock_site site) {
  if (s->lock_profile != NULL)
    return pl_store_trylock_profiled(s, PL_STORE_LOCK_GENERAL, site);
  int rc = pthread_mutex_trylock(&s->mu);
  if (rc == 0)
    return true;
  ax_assume(rc == EBUSY, "pthread_mutex_trylock");
  return false;
}

void pl_store_unlock(pl_store* s) {
  if (s->lock_profile == NULL) {
    ax_assume(pthread_mutex_unlock(&s->mu) == 0, "pthread_mutex_unlock");
    return;
  }
  pl_store_unlock_profiled(s, PL_STORE_LOCK_GENERAL);
}

void pl_store_save_lock(pl_store* s, pl_store_lock_site site) {
  if (s->lock_profile == NULL) {
    ax_assume(pthread_mutex_lock(&s->save_mu) == 0, "save pthread_mutex_lock");
    return;
  }
  pl_store_lock_profiled(s, PL_STORE_LOCK_SAVE, site);
}

void pl_store_save_unlock(pl_store* s) {
  if (s->lock_profile == NULL) {
    ax_assume(pthread_mutex_unlock(&s->save_mu) == 0,
              "save pthread_mutex_unlock");
    return;
  }
  pl_store_unlock_profiled(s, PL_STORE_LOCK_SAVE);
}

pl_store_lock_context_scope
pl_store_lock_context_begin(pl_store* s, pl_store_lock_context context) {
  if (s->lock_profile == NULL)
    return (pl_store_lock_context_scope){0};
  (void)pl_store_lock_shard_for(s);
  pl_store_lock_context_scope scope = {
      .previous = pl_store_lock_thread.context,
      .previous_store = pl_store_lock_thread.context_store,
      .active = true,
  };
  if (pl_store_lock_thread.context_store != s ||
      pl_store_lock_thread.context == PL_STORE_LOCK_CONTEXT_OTHER) {
    pl_store_lock_thread.context_store = s;
    pl_store_lock_thread.context = context;
  }
  return scope;
}

void pl_store_lock_context_end(pl_store_lock_context_scope* scope) {
  if (!scope->active)
    return;
  pl_store_lock_thread.context = scope->previous;
  pl_store_lock_thread.context_store = scope->previous_store;
  scope->active = false;
}

static void
pl_store_lock_metric_snapshot_load(pl_store_lock_metric_snapshot* out,
                                   const pl_store_lock_metric* metric) {
  out->count = metric->count;
  out->total_ns = metric->total_ns;
  out->max_ns = metric->max_ns;
  for (size_t i = 0; i < PL_STORE_LOCK_HISTOGRAM_BUCKETS; i++)
    out->histogram[i] = metric->histogram[i];
}

static void
pl_store_lock_stats_snapshot_load(pl_store_lock_stats_snapshot* out,
                                  const pl_store_lock_stats* stats) {
  *out = (pl_store_lock_stats_snapshot){0};
  out->acquisitions = stats->acquisitions;
  out->outer_acquisitions = stats->outer_acquisitions;
  out->recursive_acquisitions = stats->recursive_acquisitions;
  out->contended_acquisitions = stats->contended_acquisitions;
  out->trylock_failures = stats->trylock_failures;
  out->max_depth = stats->max_depth;
  pl_store_lock_metric_snapshot_load(&out->wait, &stats->wait);
  pl_store_lock_metric_snapshot_load(&out->owner_hold, &stats->owner_hold);
  pl_store_lock_metric_snapshot_load(&out->recursive_section,
                                     &stats->recursive_section);
}

static void
pl_store_lock_metric_snapshot_add(pl_store_lock_metric_snapshot* out,
                                  const pl_store_lock_metric_snapshot* add) {
  out->count += add->count;
  out->total_ns += add->total_ns;
  if (out->max_ns < add->max_ns)
    out->max_ns = add->max_ns;
  for (size_t i = 0; i < PL_STORE_LOCK_HISTOGRAM_BUCKETS; i++)
    out->histogram[i] += add->histogram[i];
}

static void
pl_store_lock_stats_snapshot_add(pl_store_lock_stats_snapshot* out,
                                 const pl_store_lock_stats_snapshot* add) {
  out->acquisitions += add->acquisitions;
  out->outer_acquisitions += add->outer_acquisitions;
  out->recursive_acquisitions += add->recursive_acquisitions;
  out->contended_acquisitions += add->contended_acquisitions;
  out->trylock_failures += add->trylock_failures;
  if (out->max_depth < add->max_depth)
    out->max_depth = add->max_depth;
  pl_store_lock_metric_snapshot_add(&out->wait, &add->wait);
  pl_store_lock_metric_snapshot_add(&out->owner_hold, &add->owner_hold);
  pl_store_lock_metric_snapshot_add(&out->recursive_section,
                                    &add->recursive_section);
}

static uint64_t
pl_store_lock_metric_percentile(const pl_store_lock_metric_snapshot* metric,
                                uint64_t percentile) {
  if (metric->count == 0)
    return 0;
  uint64_t target = (metric->count * percentile + 99) / 100;
  uint64_t seen = 0;
  for (size_t i = 0; i < PL_STORE_LOCK_HISTOGRAM_BUCKETS; i++) {
    seen += metric->histogram[i];
    if (seen < target)
      continue;
    if (i == 0)
      return 0;
    if (i == 64)
      return UINT64_MAX;
    return (UINT64_C(1) << i) - 1;
  }
  return metric->max_ns;
}

static void
pl_store_lock_write_metric(FILE* out,
                           const pl_store_lock_metric_snapshot* metric) {
  fprintf(out,
          "{\"count\":%" PRIu64 ",\"total_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64
          ",\"p50_upper_ns\":%" PRIu64 ",\"p95_upper_ns\":%" PRIu64
          ",\"p99_upper_ns\":%" PRIu64 ",\"histogram\":[",
          metric->count, metric->total_ns, metric->max_ns,
          pl_store_lock_metric_percentile(metric, 50),
          pl_store_lock_metric_percentile(metric, 95),
          pl_store_lock_metric_percentile(metric, 99));
  for (size_t i = 0; i < PL_STORE_LOCK_HISTOGRAM_BUCKETS; i++)
    fprintf(out, "%s%" PRIu64, i == 0 ? "" : ",", metric->histogram[i]);
  fputs("]}", out);
}

static void
pl_store_lock_write_stats(FILE* out,
                          const pl_store_lock_stats_snapshot* stats) {
  fprintf(out,
          "\"acquisitions\":%" PRIu64 ",\"outer_acquisitions\":%" PRIu64
          ",\"recursive_acquisitions\":%" PRIu64
          ",\"contended_acquisitions\":%" PRIu64
          ",\"trylock_failures\":%" PRIu64 ",\"max_depth\":%" PRIu64
          ",\"wait\":",
          stats->acquisitions, stats->outer_acquisitions,
          stats->recursive_acquisitions, stats->contended_acquisitions,
          stats->trylock_failures, stats->max_depth);
  pl_store_lock_write_metric(out, &stats->wait);
  fputs(",\"owner_hold\":", out);
  pl_store_lock_write_metric(out, &stats->owner_hold);
  fputs(",\"recursive_section\":", out);
  pl_store_lock_write_metric(out, &stats->recursive_section);
}

static bool pl_store_lock_profile_write(pl_store* s) {
  pl_store_lock_profile* profile = s->lock_profile;
  uint64_t duration_ns =
      pl_store_lock_now_ns() - pl_store_lock_timespec_ns(&profile->start);
  pl_store_lock_aggregate* aggregate = calloc(1, sizeof(*aggregate));
  ax_assume(aggregate != NULL, "store lock profile aggregate allocation");
  for (pl_store_lock_shard* shard = profile->shards; shard != NULL;
       shard = shard->next)
    for (size_t kind = 0; kind < PL_STORE_LOCK_KIND_COUNT; kind++)
      for (size_t context = 0; context < PL_STORE_LOCK_CONTEXT_COUNT; context++)
        for (size_t site = 0; site < PL_STORE_LOCK_SITE_COUNT; site++) {
          pl_store_lock_stats_snapshot row;
          pl_store_lock_stats_snapshot_load(&row,
                                            &shard->stats[kind][context][site]);
          pl_store_lock_stats_snapshot_add(
              &aggregate->stats[kind][context][site], &row);
        }
  fprintf(profile->out,
          "{\"schema\":\"enki.store-lock-profile.v1\","
          "\"clock\":\"CLOCK_MONOTONIC\",\"duration_ns\":%" PRIu64
          ",\"histogram\":\"bucket 0 is zero; bucket n is "
          "[2^(n-1),2^n-1] ns; bucket 64 saturates\","
          "\"wait_note\":\"wait begins after trylock reports EBUSY\","
          "\"recursive_section_note\":\"recursive sections overlap and are "
          "excluded from owner_hold totals\",\"totals\":[",
          duration_ns);
  pl_store_lock_stats_snapshot totals[PL_STORE_LOCK_KIND_COUNT] = {0};
  for (size_t kind = 0; kind < PL_STORE_LOCK_KIND_COUNT; kind++)
    for (size_t context = 0; context < PL_STORE_LOCK_CONTEXT_COUNT; context++)
      for (size_t site = 0; site < PL_STORE_LOCK_SITE_COUNT; site++) {
        pl_store_lock_stats_snapshot_add(
            &totals[kind], &aggregate->stats[kind][context][site]);
      }
  for (size_t kind = 0; kind < PL_STORE_LOCK_KIND_COUNT; kind++) {
    fprintf(profile->out, "%s{\"lock\":\"%s\",", kind == 0 ? "" : ",",
            pl_store_lock_kind_names[kind]);
    pl_store_lock_write_stats(profile->out, &totals[kind]);
    fputc('}', profile->out);
  }
  fputs("],\"rows\":[", profile->out);
  bool first = true;
  for (size_t kind = 0; kind < PL_STORE_LOCK_KIND_COUNT; kind++)
    for (size_t context = 0; context < PL_STORE_LOCK_CONTEXT_COUNT; context++)
      for (size_t site = 0; site < PL_STORE_LOCK_SITE_COUNT; site++) {
        const pl_store_lock_stats_snapshot* row =
            &aggregate->stats[kind][context][site];
        if (row->acquisitions == 0 && row->trylock_failures == 0)
          continue;
        fprintf(profile->out,
                "%s{\"lock\":\"%s\",\"context\":\"%s\",\"site\":\"%s\",",
                first ? "" : ",", pl_store_lock_kind_names[kind],
                pl_store_lock_context_names[context],
                pl_store_lock_site_names[site]);
        pl_store_lock_write_stats(profile->out, row);
        fputc('}', profile->out);
        first = false;
      }
  fputs("]}\n", profile->out);
  bool ok = ferror(profile->out) == 0 && fflush(profile->out) == 0;
  free(aggregate);
  return ok;
}

bool pl_store_profile_locks(pl_store* s, const char* path) {
  if (s == NULL || path == NULL || path[0] == '\0') {
    errno = EINVAL;
    return false;
  }
  if (s->lock_profile != NULL) {
    errno = EBUSY;
    return false;
  }
  FILE* out = fopen(path, "w");
  if (out == NULL)
    return false;
  pl_store_lock_profile* profile = calloc(1, sizeof(*profile));
  if (profile == NULL) {
    int saved_errno = errno;
    (void)fclose(out);
    errno = saved_errno;
    return false;
  }
  profile->out = out;
  int mutex_rc = pthread_mutex_init(&profile->shards_mu, NULL);
  if (mutex_rc != 0) {
    free(profile);
    (void)fclose(out);
    errno = mutex_rc;
    return false;
  }
  profile->generation = atomic_fetch_add_explicit(
      &pl_store_lock_next_generation, 1, memory_order_relaxed);
  ax_assume(profile->generation != 0,
            "store lock profile generation exhausted");
  if (clock_gettime(CLOCK_MONOTONIC, &profile->start) != 0) {
    int saved_errno = errno;
    (void)pthread_mutex_destroy(&profile->shards_mu);
    free(profile);
    (void)fclose(out);
    errno = saved_errno;
    return false;
  }
  s->lock_profile = profile;
  return true;
}

void pl_store_profile_locks_prepare_thread(pl_store* s) {
  if (s == NULL || s->lock_profile == NULL)
    return;
  pl_store_lock_tls_reserve();
  (void)pl_store_lock_shard_for(s);
}

bool pl_store_profile_locks_finish(pl_store* s) {
  if (s == NULL || s->lock_profile == NULL)
    return true;
  ax_assume(pl_store_lock_depth(s, PL_STORE_LOCK_GENERAL) == 0 &&
                pl_store_lock_depth(s, PL_STORE_LOCK_SAVE) == 0,
            "finish store lock profile while current thread owns a store lock");
  for (size_t i = 0; i < pl_store_lock_thread.completed_count; i++)
    ax_assume(pl_store_lock_thread.completed[i].frame.store != s,
              "finish store lock profile with deferred recursive records");
  pl_store_lock_profile* profile = s->lock_profile;
  bool ok = pl_store_lock_profile_write(s);
  if (fclose(profile->out) != 0)
    ok = false;
  s->lock_profile = NULL;
  if (pl_store_lock_thread.shard_generation == profile->generation) {
    pl_store_lock_thread.shard = NULL;
    pl_store_lock_thread.shard_generation = 0;
  }
  pl_store_lock_shard* shard = profile->shards;
  while (shard != NULL) {
    pl_store_lock_shard* next = shard->next;
    free(shard);
    shard = next;
  }
  ax_assume(pthread_mutex_destroy(&profile->shards_mu) == 0,
            "store lock profile shard mutex destroy");
  free(profile);
  return ok;
}
