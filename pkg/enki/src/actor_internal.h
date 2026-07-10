#ifndef ER_ACTOR_INTERNAL_H
#define ER_ACTOR_INTERNAL_H

/* Private actor-runtime declarations shared between actor.c and
 * http.c.  Everything here is internal to pkg/enki/src; embedders see
 * only enki/actor.h. */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "enki/actor.h"
#include "plan/eval.h"
#include "plan/heap.h"
#include "plan/value.h"

typedef struct er_msg {
  pl_val payload; /* nat63 or store-resident — terminal for every GC */
  struct er_msg* next;
  uint32_t ncaps;
  er_actor* caps[]; /* actor refs, translated at send */
} er_msg;

typedef struct er_http_xfer er_http_xfer; /* http.c */

struct er_actor {
  er_scheduler* sys;
  uint64_t id; /* creation order; deterministic tie-break key */
  pl_heap* heap;
  pl_thread* t;
  er_actor_status status;
  bool started;
  bool adopted;      /* embedder-owned thread/heap; never HALTED, never freed */
  er_msg* mbox_head; /* arrival-order FIFO (matches reaver's Chan) */
  er_msg* mbox_tail;
  er_actor** handle_v; /* dense handle table; NULL = closed; [0] = self */
  size_t handle_n;     /* next handle to mint (never reused) */
  size_t handle_cap;
  er_http_xfer* http; /* non-NULL: parked on a Fetch, not on Recv */
  er_actor* qnext;
  er_actor* all_next;
};

/* ── Event log ─────────────────────────────────────────────────────────── */

typedef enum {
  ER_EV_IO = 1,
  ER_EV_INJECT = 2,
  ER_EV_HTTP = 3,
} er_ev_kind;

typedef struct er_event {
  uint8_t kind;
  uint64_t actor;        /* er_actor id */
  uint64_t op;           /* IO/HTTP: effect-name mote; INJECT: 0 */
  uint8_t args_hash[32]; /* IO/HTTP: SHA-256 of the request */
  uint8_t* data;         /* IO: result nat bytes; INJECT: payload
                            encoding; HTTP: flat result encoding */
  uint64_t data_n;
} er_event;

struct er_log {
  uint64_t quantum; /* header: replay must use the same quantum */
  er_event* ev;     /* stb_ds array */
};

typedef enum { ER_MODE_LIVE = 0, ER_MODE_RECORD, ER_MODE_REPLAY } er_mode;

struct er_scheduler {
  pl_store* store;
  er_config cfg;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  er_actor* qhead; /* run queue */
  er_actor* qtail;
  er_actor* all_head; /* every actor, creation order */
  er_actor* all_tail;
  uint64_t next_id;
  er_mode mode;
  er_log* rec;        /* RECORD sink */
  const er_log* play; /* REPLAY source */
  size_t cursor;      /* next replay event */

  /* HTTP driver state (http.c).  curlm is a CURLM*, created lazily on
   * the first live/record Fetch and never in replay mode. */
  void* curlm;
  er_http_xfer* http_inflight; /* singly linked; teardown walk */
  size_t http_inflight_n;
  er_http_xfer* http_done_head; /* completion FIFO, DONE-read order */
  er_http_xfer* http_done_tail;
  size_t http_parked_n; /* actors awaiting a fetch (incl. replay) */
  bool http_pumping;    /* MT executors: single CURLM owner */
};

struct er_mt_executor {
  er_scheduler* sys;
  er_actor* root;
  uint32_t workers;
  pthread_t* thread_v;
  size_t active;
  bool stopping;
  er_run_reason reason;
};

/* ── actor.c internals shared with http.c ──────────────────────────────── */

void er_enqueue(er_actor* a);
void er_crash_msg(er_actor* a, const char* msg);
const er_event* er_replay_next(er_scheduler* sys);
uint64_t er_mote(const char* s);

/* ── The HTTP driver (http.c) ──────────────────────────────────────────── */

/* The Fetch branch of er_service: parse/validate the parked request,
 * then dispatch a transfer (live/record) or park a replay stub.  On
 * validation failure deposits (1 BadUrl) — logged in record mode,
 * consumed from the log in replay mode. */
void er_http_service(er_scheduler* sys, er_actor* a, uint32_t argc,
                     pl_val* args);

/* Nonblocking progress: curl_multi_perform + harvest completions.
 * LIVE deposits eagerly; RECORD only accumulates the done FIFO. */
void er_http_pump(er_scheduler* sys);

/* Empty-run-queue hook.  LIVE: wait in curl_multi_poll and deposit
 * everything completed.  RECORD: wait if needed, then deposit exactly
 * ONE completion and append its event.  REPLAY: consume exactly one
 * logged event and deposit it.  Returns true if any actor was made
 * runnable (the caller loops), false if no http work is outstanding. */
bool er_http_idle(er_scheduler* sys);

/* Any transfers inflight, completions undeposited, or actors parked? */
bool er_http_outstanding(const er_scheduler* sys);

/* MT executors: single-owner bounded pump with sys->mu dropped around
 * the curl wait.  True = this call pumped (re-check the queue and call
 * again); false = someone else owns it or there is no http work. */
bool er_http_mt_pump(er_scheduler* sys);

/* Abort every transfer and destroy the multi handle (scheduler free).
 * Nothing is deposited and nothing is logged. */
void er_http_teardown(er_scheduler* sys);

#endif
