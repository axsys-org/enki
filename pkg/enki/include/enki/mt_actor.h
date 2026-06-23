#ifndef ER_MT_ACTOR_H
#define ER_MT_ACTOR_H

/*
 * Multithreaded actor executor — a parallel sibling of the deterministic
 * single-OS-thread executor in actor.h.  Same actor model (one em_actor =
 * one pl_thread with a private heap, whose whole life is one deep
 * normalization of (fn 0)), same op-82 coordination effects
 * (Spawn/Send/SendCaps/Recv/CloseHandle), same store-resident messaging
 * (payloads pinned at send, mailboxes hold store addresses) — but driven
 * by a pool of worker OS threads instead of one FIFO run queue.
 *
 *   - Parallelism lives in pl_thread_run: each actor's reduction work
 *     runs lock-free on whichever worker dequeued it, on that actor's
 *     own heap.  Only the cheap coordination bookkeeping (the run queue,
 *     mailboxes, handle tables, spawn) is serialized, under one scheduler
 *     mutex, plus the shared store's own pin/load lock.
 *   - Determinism is traded away: scheduling and message interleavings
 *     depend on the OS.  There is therefore no record/replay and no
 *     adopt/drive stepping here — for a reproducible run use actor.h.
 *   - The shared pl_store is put in concurrency mode for the scheduler's
 *     lifetime (pl_store_set_concurrent), so workers may pin payloads in
 *     parallel.  Compilation (Compile/SetCompiler) must be quiescent.
 *
 * The per-actor semantics match the reference and the single-threaded
 * executor: payloads forced+pinned at the sender, sends to invalid
 * handles or pin-time exceptions crash the sender, Recv responds
 * valRow [msg, capsRow], caps are re-minted as fresh receiver-local
 * handles, CloseHandle of an unknown handle is a no-op.
 */

#include "enki/actor.h" /* er_config, er_actor_status, er_run_reason */
#include "plan/eval.h"
#include "plan/heap.h"
#include "plan/store.h"
#include "plan/value.h"

typedef struct em_actor em_actor;
typedef struct em_scheduler em_scheduler;

/*
 * Create a multithreaded scheduler over `store` with `nworkers` worker
 * OS threads (nworkers <= 0 selects a default from the CPU count).  The
 * store is switched into concurrency mode for the scheduler's lifetime.
 */
em_scheduler* em_scheduler_new(pl_store* store, er_config cfg, int nworkers);
void em_scheduler_free(em_scheduler* sys);

/*
 * Create an actor (unstarted, not runnable).  Its thread is exposed so
 * the embedder can build the boot function in the actor's own heap;
 * arm and enqueue with em_actor_start.  Both calls must happen before
 * em_scheduler_run (or, during a run, only from inside a worker via the
 * Spawn effect) — the embedder must not race the workers.
 */
em_actor* em_scheduler_actor(em_scheduler* sys);
pl_thread* em_actor_thread(em_actor* a);
void em_actor_start(em_actor* a, pl_val fn);

/*
 * Run the worker pool until the system quiesces: every actor has halted,
 * crashed, or is receive-blocked with nothing able to wake it.  Blocks
 * the caller until then.  Re-runnable: after it returns QUIESCENT, an
 * em_scheduler_inject can wake a blocked actor and a fresh run drains it.
 */
er_run_reason em_scheduler_run(em_scheduler* sys);

/*
 * Host injection: append a message to an actor's mailbox (waking it if
 * receive-blocked).  Thread-safe; may be called between runs or, from
 * the embedder, concurrently with a run.  The payload must already be
 * shareable: a nat63 or a store-resident value (e.g. via pl_pin).
 */
void em_scheduler_inject(em_scheduler* sys, em_actor* to, pl_val payload);

/*
 * Introspection — well-defined only when no run is in progress (between
 * em_scheduler_run calls): during a run an actor's state changes under
 * the workers.
 */
er_actor_status em_actor_state(const em_actor* a);
uint64_t em_actor_id(const em_actor* a);
em_actor* em_scheduler_actor_by_id(em_scheduler* sys, uint64_t id);
/* Normal-form result of a HALTED actor (kept until em_scheduler_free). */
pl_val em_actor_result(em_actor* a);

#endif
