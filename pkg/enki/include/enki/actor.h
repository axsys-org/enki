#ifndef ER_ACTOR_H
#define ER_ACTOR_H

/*
 * Actor runtime (actor spec §7–§8): one er_actor = one pl_thread with a
 * private heap; an actor's whole life is one deep normalization of
 * (fn 0).  The single-OS-thread deterministic executor drives actors in
 * FIFO run-queue order with a fixed fuel quantum and services the
 * coordination effects (op 82 Spawn/Send/SendCaps/Recv/CloseHandle)
 * that pl_thread_run parks as PL_RUN_BLOCKED requests.
 *
 * Messaging (M1–M4): payloads cross actors only as store-resident
 * values — the sender pins at send, the mailbox holds store addresses,
 * the receiver's heap points into the shared immutable store.  PLAN
 * code addresses actors through per-actor handle tables; handle 0 is
 * self, fresh handles are minted by Spawn and by cap transfer in Recv.
 */

#include "plan/eval.h"
#include "plan/heap.h"
#include "plan/store.h"
#include "plan/value.h"

typedef struct er_actor er_actor;
typedef struct er_scheduler er_scheduler;

typedef enum {
  ER_ACTOR_RUNNABLE = 0, /* in the run queue (or not yet started) */
  ER_ACTOR_BLOCKED,      /* parked on Recv against an empty mailbox */
  ER_ACTOR_HALTED,       /* (fn 0) reached normal form */
  ER_ACTOR_CRASHED,      /* uncaught exception or invalid effect (L2) */
} er_actor_status;

typedef struct er_config {
  uint64_t quantum;        /* fuel per slice (>= 2); 0 = default */
  size_t heap_cells;       /* per-actor semispace cells; 0 = default (8192) */
  const char* file_root_c; /* spawned actors' ReadFile jail (may be NULL) */
} er_config;

er_scheduler* er_scheduler_new(pl_store* store, er_config cfg);
void er_scheduler_free(er_scheduler* sys);

/*
 * Create an actor (unstarted, not in the run queue).  Its thread is
 * exposed so the embedder can build the boot function in the actor's
 * own heap; arm and enqueue with er_actor_start.
 */
er_actor* er_scheduler_actor(er_scheduler* sys);
pl_thread* er_actor_thread(er_actor* a);
void er_actor_start(er_actor* a, pl_val fn);

/*
 * Adopt an existing thread (and its heap) as an actor — the reference
 * withNewRts: the embedder's own thread becomes an actor.  Ownership
 * stays with the embedder (er_scheduler_free will not free it), and the
 * scheduler never marks it HALTED: the embedder arms a computation on
 * it (pl_thread_start* family) and runs it with er_scheduler_drive, any
 * number of times.
 */
er_actor* er_scheduler_adopt(er_scheduler* sys, pl_thread* t);

typedef enum {
  ER_DRIVE_DONE,     /* the driven computation completed (pl_thread_result) */
  ER_DRIVE_EXN,      /* it raised; t->exn / t->exn_msg carry the payload */
  ER_DRIVE_DEADLOCK, /* it parked on Recv and nothing can ever wake it */
} er_drive_status;

/* Run the system until the adopted actor's armed computation leaves the
 * runnable set; spawned actors are scheduled and serviced along the
 * way and survive (parked or runnable) across drives. */
er_drive_status er_scheduler_drive(er_scheduler* sys, er_actor* root);

typedef enum {
  ER_RUN_IDLE,      /* no live actors: every actor halted or crashed */
  ER_RUN_QUIESCENT, /* live actors remain, all receive-blocked (§8) */
} er_run_reason;

/* Drive the system until nothing is runnable. */
er_run_reason er_scheduler_run(er_scheduler* sys);

/*
 * Host injection: append a message to an actor's mailbox (waking it if
 * receive-blocked).  The payload must already be shareable — a nat63 or
 * a store-resident value (e.g. obtained via pl_pin) — since it enters
 * the actor's heap without copying.
 */
void er_scheduler_inject(er_scheduler* sys, er_actor* to, pl_val payload);

er_actor_status er_actor_state(const er_actor* a);
uint64_t er_actor_id(const er_actor* a);
/* Creation-ordered lookup (spawn order defines ids); NULL if unknown. */
er_actor* er_scheduler_actor_by_id(er_scheduler* sys, uint64_t id);
/* Normal-form result of a HALTED actor (the reference discards it; we
 * keep the heap until er_scheduler_free so embedders/tests can read). */
pl_val er_actor_result(er_actor* a);

#endif
