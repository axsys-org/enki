# Suspension & Actor Runtime Specification

**Status:** Draft 1 · **Audience:** a coding agent with repository access and no prior design context.

**Normative sources, in priority order:**

1. The Haskell reference implementation in the `./reaver/` submodule — the semantic oracle for actor behavior, effect vocabulary, and PLAN evaluation. Where this spec and observable reaver behavior disagree, reaver wins and this spec gets amended.
2. The `io-work` branch — the existing C-side actor/IO work. Treat it as design intent and naming precedent, **not** as code to preserve; this spec supersedes its mechanics where they conflict.
3. This document — normative for the runtime *mechanics*: suspension, scheduling, heap topology, messaging, determinism, and testing.

This spec deliberately does not define the actor-level *semantics* (the step-function type, the effect constructors, supervision rules); those live in the sources above. §12 is the extraction checklist: the questions the agent MUST answer from those sources before implementation, recorded as an addendum table committed alongside this spec.

Normative keywords MUST / MUST NOT / SHOULD / MAY are RFC-2119.

---

## 1. Design thesis

A green thread in this runtime is **not** a saved C stack. The interpreter is an explicit state machine whose complete continuation lives in two arrays owned by a `Thread` struct (a value stack and a frame stack). Suspension is therefore *returning from a C function*; resumption is *calling it again*. There is no `ucontext`, no stack switching, no segmented stacks, and no platform-specific assembly anywhere in this spec.

This is cheap because the substrate (§2) already forbids the two things that would make it expensive: live values in C locals across safepoints, and unbounded C recursion in the evaluator. Those rules exist for the garbage collector; suspension reuses them verbatim. **The GC safepoint discipline and the suspension discipline are the same discipline.** Every design decision below follows from protecting that identity.

The actor model is then: one `Thread` per actor, one small private heap per actor, and one shared immutable content-addressed store through which all inter-actor data flows. Concurrency is cooperative interleaving of actors at safepoints on (initially) a single OS thread, scheduled deterministically by fuel.

This is **deliberately not an async IO runtime**. Unix-facing effects (file, socket, process syscalls) execute inline and block the OS thread (§6.2). Suspension exists for actor coordination — `receive` above all — not for IO multiplexing. Actors that make sustained blocking calls get their own OS thread later (§13), instead of this runtime growing a poller, completion queues, and the rest of an async stack.

---

## 2. Substrate requirements (restated, self-contained)

This spec layers on a PLAN runtime with the following properties. If the codebase the agent finds does not yet satisfy one, building it is in scope as a prerequisite. They are stated as invariants S1–S8; the suspension layer's correctness proofs in this document cite them by name.

**Values.** `Val` is a tagged 64-bit word. Bit 63 clear ⇒ a direct natural number in the low 63 bits. Bit 63 set ⇒ a pointer: a 7-bit kind tag in bits 62–56 (`T_NAT`, `T_APP`, `T_LAW`, `T_PIN`, `T_ENV`, and `T_DEFER` for the in-place-mutating kinds THUNK/BLACKHOLE/INDIRECTION, whose true kind is read from the object header) and a 56-bit address. Heap objects carry a one-word header: kind, flags (incl. a NORMAL bit set by deep normalization), size in cells.

**Heap and allocation discipline.**

- **S1 — Single collection point.** The copying (Cheney semispace) collector runs only inside `gc_reserve(Thread*, size_t cells)`, the *safepoint*. `gc_reserve` guarantees headroom; it allocates nothing.
- **S2 — Allocation never collects.** All construction goes through bump allocation that hard-asserts previously reserved headroom. Constructors are bump-only.
- **S3 — Measure, then build.** Every allocation sequence is preceded by a reserve sized before the build; within the no-collect window between reserve points, the heap does not move and bare pointers are stable.
- **S4 — No `Val` survives in a C local across a safepoint.** Anything needed after a `gc_reserve` is re-fetched from a *root location*.
- **S5 — Closed root set.** The collector scans exactly the registered root sources — the Thread's value stack, frame stack, and a small set of named Thread slots — and never the C stack.

**The machine.** Evaluation to WHNF and to normal form runs as a single EVAL/RETURN trampoline over an explicit frame stack (frame kinds for thunk update, pending application argument, strict primop argument, and normalization descent). Thunks blackhole on entry and update (to an indirection) on completion; a blackhole encountered by EVAL raises the PLAN `<<loop>>` exception.

- **S6 — No unbounded C recursion in evaluation.** All reductive recursion goes through frames. Bounded structural recursion (e.g., walking a static law body) and host/jet calls into the evaluator are permitted but are *C-entry regions* (§5.3).
- **S7 — Exceptions.** A PLAN exception stores its value in a rooted Thread slot and `longjmp`s to the trampoline's handler, which unwinds the value/frame stacks to entry watermarks.

**The store.**

- **S8 — Shared immutable store.** Pinning a value deep-normalizes it, **copies the closure into a non-moving store region**, canonically serializes it, hashes it (SHA-256), interns by hash, and persists the bytes through a content-addressed backend. Store-region objects reference only store-region objects (the *closure invariant*); store contents are normal forms only — **no thunk, blackhole, or indirection is ever store-resident**. The collector treats store addresses as terminal. Heap→store pointers are routine; store→heap pointers never exist.

S8 is the load-bearing wall of the actor design: it is what makes the store safely shareable across actors with no synchronization for readers, and what guarantees actors never share mutable evaluation state (§7.4).

---

## 3. Thread: the suspendable unit

```c
typedef enum { R_DONE, R_EXN, R_YIELDED, R_BLOCKED } RunStatus;

typedef enum { RES_EVAL, RES_RETURN, RES_RUN } ResumeKind;

typedef struct Thread {
    Heap   *heap;                 /* private semispace pair (§7) */
    Val    *vstack; size_t vsp, vcap;     /* value stack  — root source */
    Frame  *fstack; size_t fsp, fcap;     /* frame stack  — root source */

    /* exception */
    Val     exn;                  /* rooted */
    jmp_buf trap;

    /* suspension state */
    uint64_t   fuel;              /* reductions remaining this quantum */
    uint32_t   centry_depth;      /* §5.3: >0 ⇒ suspension deferred */
    bool       pending_yield;     /* fuel hit 0 inside a C-entry region */
    ResumeKind resume_kind;
    Val        resume_val;        /* rooted: value to EVAL or RETURN on re-entry */
    uint32_t   resume_pc;         /* RES_RUN: bytecode offset (mirrors F_RUN) */
    Val        blocked_on;        /* rooted: effect request while R_BLOCKED */
} Thread;

RunStatus thread_run(Thread *t, uint64_t fuel);
```

Rules:

- **T1.** `thread_run` is the only public execution entry. It installs the exception trap, loads `t->fuel = fuel`, re-enters the trampoline per `resume_kind`, and returns one of the four statuses. The caller (scheduler) MUST NOT inspect or mutate stacks directly.
- **T2.** All `Val`-typed Thread slots (`exn`, `resume_val`, `blocked_on`) are part of the Thread's root source. Frame `Val` fields likewise. `resume_pc` and all stack indices are offsets, never pointers (the arrays may be realloc'd on growth).
- **T3.** A suspended Thread (`R_YIELDED` or `R_BLOCKED`) is a complete, collectable, *inspectable* continuation: its private heap may be collected at any time (its roots are exactly its registered sources), and it may be resumed any number of safepoints later with identical semantics (§10's transparency property).
- **T4.** Entry watermarks are fields, not C locals: top-level execution always runs with frame base 0; re-entrant evaluator calls from C (§5.3) save/restore their own bases but are never suspension points.

---

## 4. Suspension mechanics

### 4.1 Where suspension can happen

Exactly three machine positions, each of which already coincides with a safepoint and at which, by S4/S6, **no live value exists outside root locations and no evaluator state exists on the C stack** (when `centry_depth == 0`):

| point | captured as | resume action |
|---|---|---|
| about to EVAL a value `v` (incl. forcing a strict primop arg) | `resume_kind=RES_EVAL; resume_val=v` | re-enter loop in EVAL state with `v` |
| about to RETURN a value `v` to the top frame | `resume_kind=RES_RETURN; resume_val=v` | re-enter loop in RETURN state with `v` |
| compiled-law dispatch, before executing the opcode at `pc` | `F_RUN` frame on fstack updated with `pc`; `resume_kind=RES_RUN` | jump back into dispatch at `pc` |

`F_RUN { code_ref /*Val: the law's pin*/, pc, vbase }` is pushed once per compiled-law activation at entry (it doubles as the activation record the collector scans for `code_ref`). During normal execution `pc` MAY live in a C local/register; it is **written back into the frame only at suspension** — so steady-state dispatch pays nothing.

### 4.2 The fuel check rides the reserve

`gc_reserve` is already executed at every safepoint (per machine step and per compiled dispatch). Suspension polling folds into it:

```c
static inline void gc_reserve(Thread *t, size_t cells) {
    if (UNLIKELY(--t->fuel == 0 || t->heap->free + cells > t->heap->limit))
        reserve_slow(t, cells);          /* out-of-line */
}
```

`reserve_slow` distinguishes, in order: (1) genuine heap shortfall → collect (and grow if still short) — **legal at any `centry_depth`**, since GC never unwinds the C stack; (2) `fuel == 0` → suspend with `R_YIELDED` if `centry_depth == 0`, else set `pending_yield` and continue with `fuel = 1`-style grace (decrements keep funneling to the slow path; the yield fires at the first depth-0 safepoint).

- **Y1.** The fast path adds exactly one decrement and one fused branch over the bare heap check. No other per-step cost is permitted.
- **Y2 — Determinism.** Fuel is the *only* trigger for `R_YIELDED`. `thread_run` MUST NOT consult wall-clock time, signals, or any nondeterministic source. Given identical Thread state and fuel, the suspension point is bit-identical. (An async-preemption hook — an external party clamping the heap trigger to force the slow path — is reserved for a future multi-OS-thread executor and MUST stay disabled in the deterministic executor; see §13.)
- **Y3.** Fuel is denominated in safepoints ("reductions"). The quantum is scheduler policy (§8), not Thread policy.

### 4.3 Suspension procedure (depth 0)

1. Capture the resume point per §4.1 (one enum store + one rooted `Val` store, or one `pc` writeback).
2. Return `R_YIELDED` (or `R_BLOCKED`, §6) from `thread_run` through normal C returns — the trampoline is the only live C frame, by §5.3.

Total cost: a handful of stores. **There is no "context" to save** beyond what the machine already keeps in the Thread by S4/S6 — that is the entire point of the design.

### 4.4 Resumption

`thread_run` switches on `resume_kind`: `RES_EVAL` → EVAL `resume_val`; `RES_RETURN` → RETURN `resume_val`; `RES_RUN` → re-enter the dispatch loop of the law in the top `F_RUN` frame at `resume_pc`. For a Thread resumed after `R_BLOCKED`, the scheduler MUST first deposit the effect's response (§6.3) — which is precisely setting `resume_kind=RES_RETURN; resume_val=response`.

---

## 5. What may and may not suspend

### 5.1 The transparency property (the governing requirement)

**Suspension MUST be semantically invisible.** For any program and any fuel schedule, the sequence of PLAN-level results, effects, and exceptions is identical to an infinite-fuel run. The whole section exists to protect this; §10.1 makes it mechanically testable.

### 5.2 GC vs suspension — different rules

Collection requires only S1–S5 and is legal at any safepoint, including under nested C entries (the C stack holds no `Val`s, per S4). Suspension additionally requires that **the trampoline be the only live C frame**, because returning `R_YIELDED` unwinds the C stack. Conflating these two is the most likely implementation bug; the invariant tests in §10.2 target it directly.

### 5.3 C-entry regions

A *C-entry region* is any execution with native frames between the trampoline and the current machine step: re-entrant evaluator calls from host code or jets, bounded structural recursion inside the evaluator, and jet bodies themselves. Rules:

- **C1.** `centry_depth` is incremented/decremented around every such region. Suspension requests are deferred while it is nonzero (§4.2); GC is not.
- **C2.** Jets are **atomic**: they run to completion within one scheduling decision. A *pure* jet whose worst case is unbounded or large (e.g., huge-operand bignum work, hashing megabytes) SHOULD be chunked — restructured so each chunk returns to the machine (frames) between chunks — or MUST be documented as a latency bubble with a measured bound. Direct-effect handlers (§6.2) are exempt from the bound: blocking arbitrarily on a syscall is their specified behavior.
- **C3.** Effects (§6) MUST NOT be initiated from inside a C-entry region. An effect reaching the machine from depth > 0 is a runtime abort in debug builds.
- **C4.** New evaluator code MUST prefer frames over C recursion; C-entry is a tolerated exception for bounded work, not a pattern.

---

## 6. Effects: coordination suspends, unix blocks

### 6.1 Two classes

An *effect* is an actor-initiated request beyond its own heap. **The effect vocabulary — constructors, fields, strictness, response types — is normative in reaver and MUST be extracted per §12.** This spec fixes the runtime contract and a binary classification (the per-effect assignment is §12 item 10):

- **Coordination effects** — scheduler-mediated actor coordination: `receive` (mandatory member), and whatever else reaver requires (spawn, link, …). These suspend the Thread with `R_BLOCKED`.
- **Direct effects** — unix-backed operations: file, socket, process, clock syscalls. These execute **inline, synchronously, blocking the OS thread**, like a side-effecting jet. No suspension, no poller, no completion queue, no readiness events. The actor never leaves RUNNING; every other actor waits.

The cost of the direct class — one actor's slow `read(2)` stalls the executor — is accepted by design. The remedy, when it matters, is moving blocking-prone actors onto dedicated OS threads (§13), not building an async runtime inside this one.

### 6.2 Direct effects

1. Recognized by the machine at a depth-0 safepoint (C3 applies to both classes: effect *initiation* never happens inside a C-entry region — this keeps the event-log order well-defined).
2. The handler runs as its own C-entry region (`centry_depth++`): performs the syscall(s), allocates the response value in the actor's private heap under the normal reserve discipline, returns it; the machine RETURNs it. Results never cross actors here, so no pinning is involved.
3. **Replay hook:** between recognition and execution the handler consults the executor mode. Live → perform the syscall and append `(actor, effect-id, args-hash, result-bytes)` to the event log, result serialized canonically. Replay → return the deserialized logged result and perform nothing. This hook is mandatory; a direct effect without it breaks R2.
4. Errors (errno, EOF, partial reads) are data: surfaced inside the response value per the vocabulary, never as PLAN exceptions invented by the runtime. EINTR is retried in the handler.

### 6.3 Coordination effects

1. The request is materialized as a PLAN value; payloads that cross actors (message bodies, spawn arguments) MUST be pinned at initiation — see M2, §7.3.
2. The request is parked in `t->blocked_on` (rooted) and the Thread suspends with `R_BLOCKED` at a RETURN-class point: the machine is left expecting the effect's *response* as the returned value.
3. The scheduler resumes it by depositing the response — `resume_kind=RES_RETURN; resume_val=response; blocked_on=NIL` — and re-queueing. Responses carrying cross-actor data are store-resident (M1) and flow in without copying.
4. A coordination effect the runtime can satisfy immediately (e.g., a `send`, or a `receive` against a non-empty mailbox) MAY be serviced inline without leaving the runnable set, provided observable semantics match reaver; the uniform `R_BLOCKED` path MUST also remain correct (it is the one YIELD_STRESS exercises).
- **E1.** An `R_BLOCKED` Thread satisfies all the guarantees of an `R_YIELDED` one (T3): initiation only at depth-0 safepoints makes this so.
- **E2.** Effect order within one actor is program semantics (transparency property); completion order across actors is scheduler-determined and captured by the log where external (§9).

---

## 7. Actors, heaps, and the store

### 7.1 Actor

```c
typedef enum { A_RUNNABLE, A_RUNNING, A_BLOCKED, A_IDLE, A_CRASHED, A_HALTED } ActorStatus;

typedef struct Actor {
    ActorId      id;            /* identity scheme: §12 item 5 */
    Thread       thr;           /* owns its private Heap */
    ActorStatus  status;
    Val          state;         /* current actor state value — rooted Thread slot or vstack-resident; see step protocol, §12 item 1 */
    Mailbox      mbox;          /* §7.3 */
    /* supervision / linkage fields per §12 items 6–7 */
} Actor;
```

One actor = one Thread = one private heap. The actor's *state* is a PLAN value; the step protocol (how the state function consumes an event and produces a new state plus effects) is reaver-normative.

### 7.2 Heap topology

- **H1.** Each actor owns a private semispace pair, initially small (default 64 KiB per space; configurable), growing by doubling. Collection of one actor's heap never touches another actor and never blocks the scheduler beyond that actor's pause.
- **H2.** The store (S8) is the single shared region: immutable, non-moving, normal-forms-only, append-interned, content-addressed. Reads need no synchronization on a single-OS-thread executor (and only intern-table locking on a future multi-threaded one — §13).
- **H3 — No shared mutable state, ever.** The only values visible to two actors are store-resident. Since the store contains no thunks (S8), **no thunk is ever shared between actors**. Consequence: a blackhole can only be encountered by the actor that created it, so `BLACKHOLE ⇒ <<loop>>` remains exactly correct and the evaluator needs no blackhole owner fields, wait queues, or wakeup logic. Any future feature that would share a thunk across actors violates H3 and is out of scope.

### 7.3 Messaging

- **M1.** A message is a store-resident value, transmitted by reference: the sender pins the payload (deep-normalize → store-copy → hash → intern), the receiver gets the pin/its body. Cost of sending = cost of pinning; repeated sends of equal content are deduplicated by content addressing; receiving costs nothing.
- **M2.** Pinning at *initiation* (not delivery) is mandatory: it snapshots the payload while the sender's heap is live and severs all references into the sender's moving heap before the message escapes the actor. The mailbox therefore holds only store addresses/hashes and needs no GC integration.
- **M3.** Ordering guarantee: extracted from reaver (§12 item 4). Default if the reference is silent: FIFO per (sender → receiver) pair, no global order. Overflow policy likewise per §12 item 4 (default: unbounded with a watermark warning).
- **M4.** Delivery is an effect completion (§6.3): a receive-blocked actor resumes with the message value as the response.

### 7.4 Lifecycle, crashes

- **L1.** Spawn is an effect; the child's initial state value arrives pinned (M2 applies to spawn payloads identically).
- **L2.** An uncaught PLAN exception at actor top level marks the actor `A_CRASHED`; the exception value is pinned and recorded in the event log. Supervision/restart/link semantics are reaver-normative (§12 items 6–7); the runtime contract is only: a crashed actor's Thread is never resumed, its heap is freed, its pinned crash report and last snapshot survive.
- **L3.** Abandoned blackholes after a crash are unreachable by H3 (no other actor could hold them) and are freed with the heap. No repair pass needed.

---

## 8. Scheduler

Single OS thread (the *deterministic executor*). There is **no IO poller** — direct effects never park anything (§6.2). Components:

- **Run queue** — FIFO of `A_RUNNABLE` actors.
- **Timer structure** — deadline min-heap, *only if* the extracted vocabulary classifies timers as coordination effects; checked between quanta.
- **Receive table** — actors blocked on empty mailboxes.

Loop: pop actor → `status = thread_run(&a->thr, QUANTUM)` →

| status | action |
|---|---|
| `R_YIELDED` | push to run-queue tail (round-robin fairness) |
| `R_BLOCKED` | decode `blocked_on`; park in the matching structure or service inline per §6.3.4 |
| `R_DONE` | per step protocol: harvest new state/effects, deliver next mailbox event or park `A_IDLE` (exact transition is reaver-normative, §12 item 1) |
| `R_EXN` | crash path (L2) |

When the run queue empties: fire due timers; if timers are pending, a single blocking sleep until the earliest deadline is correct (nothing is runnable — blocking the thread is the semantics, not a compromise). If nothing is runnable, no timer is pending, and every live actor is receive-blocked, the system is **quiescent**: the executor returns control to the embedder (wisp host), which may inject events, attach input, or terminate. Quiescence is a normal state, not an error.

- **D1.** `QUANTUM` is a fixed configuration constant per executor instance, recorded in the event log header.
- **D2.** The executor's choices MUST be a pure function of: initial actor set, the logged external-event stream (§9), and `QUANTUM`. Tie-breaks (multiple timers at one deadline, multiple host injections in one batch) MUST use a deterministic total order (actor id), never arrival nondeterminism.

---

## 9. Determinism, event log, replay

- **R1.** *External events* — anything entering from outside the actor system — are recorded in an append-only event log. With inline direct effects there are exactly three sources: **direct-effect results** (logged at the §6.2.3 hook: effect id, args hash, canonical result bytes), **host injections** (events the embedder feeds in, payloads pinned), and **timer firings as observed**. Internal events (actor→actor messages, yields) are **not** logged; they are reproducible.
- **R2.** *Replay* = rerun the executor with the same `QUANTUM`, substituting logged results at direct-effect sites (no syscalls performed) and logged injections at their logged positions. Replay MUST reproduce, bit-for-bit: every actor's state-value hash at every snapshot point, every effect request hash, and the final log. This is the system's persistence/recovery story *and* its strongest integration test (§10.3).
- **R3.** *Snapshots*: an actor's state is a PLAN value; snapshotting = pinning it and logging the hash. Recovery = load pin by hash from the store backend, replay the log suffix. Snapshot cadence is policy (per §12 item 8 if the reference specifies one; otherwise configurable count-of-events).
- **R4.** Fuel-schedule independence: changing `QUANTUM` may change interleavings and therefore the log, but any *single-actor* observable sequence (its events in, its effects out, its state hashes) MUST be unchanged. (Cross-actor message *arrival order* may legitimately differ; per-actor semantics may not.)

---

## 10. Testing

### 10.1 YIELD_STRESS — the transparency oracle

A build flag making every depth-0 safepoint a suspension: effectively `fuel = 1`, with the harness resuming immediately. **The entire existing evaluation test suite (unit, property, differential-vs-reaver) MUST pass under YIELD_STRESS with results identical to infinite fuel.** This single mode converts every latent violation of §5 — a value cached in a C local across a yield, a stale `pc`, a frame field missed by the root source — into a deterministic failure on existing tests, the same way GC-stress modes audit rooting. It is the first thing to build after `thread_run`.

### 10.2 Invariant death tests (debug builds)

Effect initiated at `centry_depth > 0` aborts (C3). Suspension attempted at depth > 0 aborts. `R_BLOCKED` Thread resumed without a deposited response aborts. Collection of a suspended Thread's heap, then resume → correct (run under GC-stress + YIELD_STRESS combined + asan).

### 10.3 System-level

1. **Differential vs reaver:** identical event scripts driven into both runtimes; compare per-actor state hashes and effect-request sequences. Required scenarios: message ordering per M3, crash + supervision per §12, timer ordering ties (D2), receive-while-empty, quiescence and host re-injection, spawn chains, direct-effect replay (logged syscall results substituted, byte-identical state hashes), an actor that loops forever (fairness: others still progress), an actor blocked in a slow direct effect (semantics correct despite the stall).
2. **Replay:** record a randomized run (fuzzed event timings), replay per R2, assert bit-identical. Then re-run live with a different `QUANTUM` and assert R4.
3. **Blackhole isolation:** property test that `<<loop>>` self-detection still fires under heavy cross-actor messaging (guards H3 against regression).
4. **Soak:** thousands of actors, small heaps, sustained messaging; assert bounded RSS apart from store growth, and per-actor GC pause bounds.

---

## 11. Implementation plan

- **P0 — Extraction.** Answer every §12 item from reaver + `io-work`; commit the addendum table. No code before this lands: most of the remaining ambiguity in this spec resolves here.
- **P1 — Suspension core.** `RunStatus`, Thread suspension fields, fuel-in-reserve, resume protocol, `F_RUN`, C-entry depth. Deliverable: YIELD_STRESS green on the full existing suite (§10.1).
- **P2 — Scheduler skeleton.** Run queue + quantum + timers + quiescence handoff, no actors yet (drive bare Threads). Determinism test for D2 tie-breaks.
- **P3 — Actors.** Per-actor heaps, mailboxes over the store (M1–M4), lifecycle, crash path. Differential scenarios begin.
- **P4 — Effects.** Full vocabulary from P0 wired through §6; reaver differential suite green.
- **P5 — Log & replay.** R1–R4, snapshots, recovery test.

Each phase lands green across the sanitizer matrix and YIELD_STRESS before the next begins.

## 12. Extraction checklist (P0 deliverable)

For each item: cite file/function in reaver (primary) and `io-work` (secondary), state the rule, note divergences between the two sources (reaver wins; log the divergence).

1. **Step protocol** — the actor state function's type, saturation convention, and how (state′, effects) are decomposed from its result; what `R_DONE` means for an actor (§8 table).
2. **Effect vocabulary** — every effect constructor: fields, field strictness, response type, error responses.
3. **Effect initiation convention** — how the machine recognizes an effect (primop? distinguished pins? result shape?).
4. **Mailbox semantics** — ordering guarantees, overflow policy, selective receive (if any).
5. **Actor identity** — id scheme, relation to the namespace, addressability from PLAN values.
6. **Spawn/link/monitor** — semantics and payloads.
7. **Crash & supervision** — what a crash report contains; restart semantics; escalation.
8. **Snapshot/event-log format** — if reaver fixes one, adopt it; else propose under R1–R3.
9. **Timer semantics** — absolute vs relative, resolution, coalescing.
10. **Coordination vs direct classification** — for every effect in the vocabulary: coordination (suspends, §6.3) or direct (inline, blocks the OS thread, §6.2). `receive` is coordination by definition; anything wrapping a unix syscall defaults to direct. Flag any effect the reference treats asynchronously — that is a divergence to record, not machinery to build.

## 13. Deferred (named seams)

| item | seam |
|---|---|
| Dedicated OS threads for blocking-prone actors | the named remedy for §6.2's stall cost: pin an actor to its own OS thread running its own executor loop. Prerequisites shared with the row below: store intern-table lock, MPSC wake/mailbox queues. Per-actor heaps and the store closure invariant already make actor state thread-private and shared data immutable |
| Multi-OS-thread executor | same prerequisites; async preemption uses a heap-trigger clamp forced from outside — disabled in the deterministic executor (Y2) |
| Store eviction / demand paging of pins | mailbox + state snapshots already address by hash; eviction = drop resident copy, reload from backend on access |
| Actor migration between executors | a suspended Thread is already a self-contained value+frame snapshot; migration = move the struct + replay heap (or snapshot/restore via pin) |
| Priority scheduling | run queue is the seam; D2 determinism must be preserved per priority class |
