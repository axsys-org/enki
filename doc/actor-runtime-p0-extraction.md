# P0 Extraction Addendum — Suspension & Actor Runtime

**Status:** P0 deliverable per §11/§12 of the Suspension & Actor Runtime spec.
**Sources examined:**

- reaver (primary, semantic oracle): `reaver/src/hs/Plan.hs`, `Types.hs`, `Repl.hs`, `reaver/src/reaver/actor-demo.rvr`
- `io-work` branch (secondary, naming precedent): `include/enki/run.h`, `src/op82.c`, `src/run.c`, `src/fd.c`, `src/store.c`, `tests/unit/test_op82_runtime.c` (tip `bd8e1bd`; key commit `92ac333` "Actor system and IO")

Per spec priority, reaver wins on observable semantics; io-work supplies naming and C-side structure. Divergences are logged inline and summarized in §D.

______________________________________________________________________

## 1. Step protocol

**Reaver:** `Plan.hs:opSpawn` (156–163): an actor is `fn % N 0` — the spawned function applied to nat 0, then forced to **normal form** (`evaluate $ force …`) on its own GHC thread, wrapped in `try @SomeException` whose result is discarded.

**There is no (state′, effects) decomposition.** Reaver has no step function returning a state/effect pair. Effects are op-82 primop applications performed *during* evaluation (via `unsafePerformIO`, `Plan.hs:471`). Long-lived actors are written as tail-recursive loops carrying state in arguments (`actor-demo.rvr:26–32, 53–66` — `echo-loop`, `(loop 0)`).

**io-work agrees:** `er_actor_create` stores `task_v`; the scheduler calls `plan_eval(&actor->vm, actor->task_v, ER_EVAL_WHNF)` per timeslice; result checked with `eo_op82_is_blocked` → `ER_ACTOR_BLOCKED`, else `ER_ACTOR_DONE` (`src/op82.c:er_scheduler_run`).

**Rule:** the "step protocol" is: spawn payload `fn`; the actor's whole life is one normalization of `(fn 0)`. `R_DONE` from `thread_run` ⇒ the value reached normal form ⇒ actor transitions to `A_HALTED`. The spec's §8 table row for `R_DONE` ("harvest new state/effects, deliver next mailbox event") does **not** apply — there is nothing to harvest; the resulting value is discarded (reaver discards it). The Actor `state` field (§7.1) is not driven by a harvest protocol; the meaningful per-actor "state" for snapshots is the suspended Thread continuation itself.

**Saturation convention:** a pinned nat has arity 1 (`Plan.hs:arity`, 237–242); `exec (P _ _ (N o)) e = op o (unapp (e!!0))` (`Plan.hs:259`) — the primop pin is applied to exactly one argument, which is un-app'd into a row whose head names the operation.

## 2. Effect vocabulary

Normative source: `Plan.hs:rplan` (481–513) and actor ops (156–233). All requests are `(<pin 82> [OpName, args…])`, OpName a string nat. io-work additionally assigns numeric tags (enum `er_op82_sub`, `include/enki/run.h`); `eo_op82_from_tag` accepts both numeric and string forms.

| op | args | response | errors | class (§10 below) |
|---|---|---|---|---|
| `Input` | `n` (max bytes) | bar-encoded bytes from stdin (`bytesBar`: length-terminated nat, `Plan.hs:520–525`) | — | direct |
| `Output` | `x` (nat) | `0`; writes `natBytes x` to stdout, flushes | — | direct |
| `Warn` | `x` | `0`; as Output but stderr | — | direct |
| `ReadFile` | `p` (nat path, resolved as `src/<natStr p>`) | bar-nat of contents | `0` on IOException | direct |
| `Print` | `s` (nat) | `0`; `putStr (natStr s)` | — | direct |
| `Stamp` | `n` (nat path) | mtime as POSIX seconds nat | `0` on IOException | direct |
| `Now` | `_` (ignored) | POSIX seconds nat | — | direct |
| `Spawn` | `fn` (unevaluated function) | actor handle (nat) | — | coordination |
| `Send` | `h` (nat handle), `msg` | `0` | invalid handle ⇒ runtime error | coordination |
| `SendCaps` | `h`, `msg`, `caps` (row of sender-local handles) | `0` | invalid handle ⇒ error | coordination |
| `Recv` | `0` (literal nat 0 required) | `[msg, capsRow]` — row of payload and row of fresh receiver-local handle nats (`Plan.hs:opRecv`, 179–184) | — | coordination (blocks on empty) |
| `CloseHandle` | `h` | `0`; removes handle from table | — | coordination |
| `CloseFd` | `h` (socket handle) | `0` | invalid fd ⇒ error | direct |
| `Listen` | `port` | socket handle nat (bind 0.0.0.0, backlog 128, SO_REUSEADDR) | — | direct |
| `Accept` | `h` | new connection handle nat | — | direct |
| `Read` | `h`, `n` | bar-nat of ≤ n bytes; `0` on EOF (`Plan.hs:opRead`, 215–222) | — | direct |
| `Write` | `h`, `dat` (nat bytes) | `0`; `sendAll` then **closes the socket** (`Plan.hs:opWrite`, 224–233) | — | direct |

Field strictness: handle/port/count args are pattern-matched as `N h` in `rplan` (504–512) ⇒ forced to WHNF at initiation. **Message payloads are NOT forced before send** in reaver (`opSend` writes `msg` to the Chan lazily) — see divergence D5.

Unknown op ⇒ Haskell `error` (`Plan.hs:513`), i.e. a runtime crash, not a PLAN exception.

io-work error convention: failures return a *tank* (`er_tank_make(gc, val_v, msg_c)`) with messages like `"io error"`, `"bounds error"`; blocking is signaled by tank `"actor blocked"`. The spec's §6.2.4 (errors are data in the response) should follow the reaver shapes above (`0` on IOException, `0` on EOF), not invented PLAN exceptions.

## 3. Effect initiation convention

**Reaver:** distinguished primop number. A pin whose body is `N 82`, arity 1, applied to a row: `op 82 x = unsafePerformIO (rplan x)` (`Plan.hs:471`). Recognition point is `exec` on a saturated pinned-nat application (`Plan.hs:259`). A mode flag gates it: `rplan` errors unless `vMode == RPLAN` (`Plan.hs:483–485`).

**io-work:** same shape — `FORCE_XPRIM` in `src/run.c` dispatches when the prim-set tag is 82, evaluating the single argument to WHNF and calling `eo_exec_op82_app`.

**Rule:** the machine recognizes an effect as *saturated application of the op-82 primop pin to a row*, at a depth-0 safepoint (spec C3). The row head (string nat, optionally numeric per io-work) selects the operation.

## 4. Mailbox semantics

**Reaver:** `Actor = Chan (Val, [Actor])` (`Plan.hs:84`). GHC `Chan` ⇒ **unbounded FIFO per receiver in global arrival order** (all senders interleaved by arrival; per-sender order preserved). No selective receive — `Recv` always takes the head. No overflow policy (unbounded).

**io-work:** `er_mailbox { head, tail, inbound_s }` intrusive FIFO; push to tail, pop from head; only limit is `ER_ACTOR_CAPS_MAX = 16` caps per message.

**Rule (replaces the M3 default):** FIFO **per receiver in arrival order**, which is stronger than the spec's "FIFO per (sender→receiver) pair" default — adopt arrival-order FIFO. Unbounded; spec's watermark warning is an acceptable addition. No selective receive. Messages carry `(payload, caps)`; caps cap of 16 is an io-work artifact, not reaver semantics (reaver is unbounded) — reaver wins, treat 16 as a tunable implementation limit if kept.

## 5. Actor identity

**Reaver:** an actor's identity *is* its inbox channel (`Plan.hs:82–84`); there is no global ID space. PLAN code addresses actors via **per-actor handle tables**: `rtsHandles :: IORef (IntMap Actor)`, dense ints, **handle 0 = self** (`Plan.hs:newRts`, 111–117; `actor-demo.rvr:39`). Handles are capabilities: they cannot be forged from nats — they are only minted by `Spawn`, by cap transfer in `Recv`, or handle 0; transferring requires `SendCaps`, which translates sender-local handles to actor refs (`opSendCaps`, 171–177) and `Recv` re-mints them as fresh receiver-local handles (`opRecv`, 179–184). `CloseHandle` revokes a table entry.

**io-work:** identical scheme — `er_actor* handles_v[ER_ACTOR_HANDLES_MAX /*1024*/]`, `handles_v[0] = self`, `next_handle_s` bump allocation.

**Rule for §7.1 `ActorId`:** runtime-internal ID may be anything stable (needed for D2 tie-breaks and the event log); *PLAN-visible* addressing is per-actor handle nats with handle 0 = self. The handle table is part of actor state that must survive suspension and appear in snapshots.

## 6. Spawn / link / monitor

**Spawn** (`Plan.hs:opSpawn`, 156–163): arg `fn` is taken **unevaluated** (io-work confirms: spawn arg evaluated with `ER_BC_EVAL_NONE`); child runs `fn % N 0`; child's Rts starts with only handle 0 = self (the parent is *not* automatically a handle in the child — the child knows nobody until told via caps); parent receives a fresh handle to the child. io-work additionally allocates the child its own GC heap (`ER_ACTOR_DEFAULT_HEAP`) and imports `fn` into it — precedent for spec M2/L1 pinning of spawn payloads.

**Link / monitor: absent in both sources.** No linking, no exit signals, no monitors. §12 item 6 answer: spawn only.

## 7. Crash & supervision

**Reaver:** `opSpawn` wraps the child in `try @SomeException` and **discards the result** (`Plan.hs:160–161`) — a crashed actor dies silently; nobody is notified; its handles dangle (sends to it succeed into a Chan nobody reads). PLAN-level `Try`/`Throw` (`op 66`, `Plan.hs:435–436, 515–518`) exist for in-actor exception handling; `PLAN_EXN` carries a forced PLAN value (`Plan.hs:280–284`).

**io-work: absent** — no crash detection, restart, or supervision.

**Rule:** there are no reaver supervision semantics to replicate. Spec §7.4 L2 (mark `A_CRASHED`, pin the exception value, record in event log, free heap, never resume) is runtime-added behavior and stands as written; it is observably compatible with reaver (silence toward other actors). Crash report content: the pinned PLAN exception value plus actor id — nothing more is mandated by any source.

## 8. Snapshot / event-log format

**Reaver** (`Plan.hs:608–631`): `Save x` requires a pin; writes each pin (post-order over sub-pins, dedup by existence check) as canonical text to `snap/<base58btc(sha256)>.plan` and appends `@<base58>` to `snap/root.plan`. `Load 0` is **unimplemented** (`error`, `Plan.hs:610–611`).

**io-work** (`src/store.c`): pack-file store — `pins.pack`, index magic `{'p','i','n','p','a','c','k',1}`, `ER_STORE_VERSION 0x02`, root key `"root"`, SHA-256 via `er_pin_freeze`. Supersedes reaver's flat files for the C runtime (storage format is runtime mechanics, where this codebase's lineage governs; content addressing/hashing must stay reaver-canonical so hashes agree).

**Event log: absent in both sources.** Propose per spec R1–R3; format is ours to define. Minimum per spec: header `{version, QUANTUM}`; records for direct-effect results `(actor, effect-id, args-hash, canonical result bytes)`, host injections (pinned payload hash), timer firings, snapshot records `(actor, state-pin hash)`.

## 9. Timer semantics

**Reaver:** the only time op is `Now` — absolute wall clock, **1-second resolution**, POSIX seconds (`Plan.hs:502`), plus `Stamp` (file mtime, seconds). **There is no sleep/timeout/recurring-timer effect** in either source; nothing blocks on time. io-work matches (`OP82_NOW` = `time(NULL)`; no timers).

**Rule:** §8's timer min-heap is **not required** for vocabulary parity — `Now` is a direct effect (logged for replay per R1). Build the timer structure only if/when a sleep-class coordination effect is added; D2's deadline-tie determinism rule then applies.

## 10. Coordination vs direct classification

| effect | class | blocking behavior in sources |
|---|---|---|
| `Recv` | **coordination** | reaver: blocks on `readChan`; io-work: tank "actor blocked" → `ER_ACTOR_BLOCKED`, retried — the only suspending effect in io-work |
| `Spawn`, `Send`, `SendCaps`, `CloseHandle` | **coordination** | never actually block in either source ⇒ prime candidates for spec §6.3.4 inline service while staying semantically uniform |
| `Input`, `Output`, `Warn`, `Print`, `ReadFile`, `Stamp`, `Now` | **direct** | inline, blocking |
| `Listen`, `Accept`, `Read`, `Write`, `CloseFd` | **direct** | inline, blocking (reaver blocks the GHC thread; io-work blocks the scheduler thread) |

No effect is treated asynchronously by either source — nothing to flag under §12 item 10's divergence clause.

______________________________________________________________________

## D. Logged divergences

| # | divergence | resolution |
|---|---|---|
| D1 | Spec §7.1/§8 assumed a (state′, effects)-harvesting step protocol; reaver has none — actors are single normalizations with mid-evaluation effects | Reaver wins. Amend spec: drop `R_DONE` harvest; `R_DONE` ⇒ `A_HALTED`; `Actor.state` is the suspended continuation, not a harvested value (§1 above) |
| D2 | io-work accepts numeric op tags 0–17 and strings; reaver only strings | Support both at recognition (cheap, io-work precedent); strings are canonical |
| D3 | io-work caps/handles limits (16 caps/msg, 1024 handles); reaver unbounded | Reaver semantics (unbounded); limits allowed only as growable defaults |
| D4 | io-work imports message payloads into receiver GC heap (`enki_gc_import`); reaver shares the GHC heap lazily; spec M1/M2 mandates pin-to-store at initiation | Spec mechanics win (M1/M2). Observable consequence vs reaver: payload divergence/exceptions surface at *send* (forcing during pinning), not at receiver — accepted, log here |
| D5 | Reaver sends unforced payloads (no deepseq in `opSend`) | Subsumed by D4; pinning forces. Differential tests must avoid relying on payload laziness across actors |
| D6 | Reaver `Write` closes the socket after every write (`Plan.hs:232`) | Looks accidental but reaver is the oracle: replicate until reaver changes; noted for upstream fix |
| D7 | Reaver scheduling is preemptive GHC threads; spec mandates deterministic fuel | By design (spec §1/Y2); single-actor observables must still match (R4) |
| D8 | Spec M3 default "FIFO per sender-pair"; reaver Chan gives arrival-order FIFO per receiver | Reaver wins: arrival-order FIFO (§4 above) |
| D9 | Reaver snapshot = flat base58 files; io-work = packfile v2 | Packfile (mechanics); canonical serialization + SHA-256 stays reaver-identical |
| D10 | Reaver `ReadFile`/`Stamp` resolve paths under `src/` | Replicate as-is (oracle behavior); revisit with reaver ("should be more unixy" TODO, `Plan.hs:500`) |
| D11 | `Recv` requires literal arg `0`; unknown ops are host errors, not PLAN exceptions | Replicate: malformed effect rows abort the actor (crash path), matching reaver's `error` |

## Naming precedent adopted from io-work

`er_*` structs (`er_actor`, `er_mailbox`, `er_message`, `er_tank`, `er_vm`), `ER_*` constants, `er_op82_sub` effect enum, `eo_*` op implementations, `er_scheduler_*` scheduler entry points, actor states `ER_ACTOR_{RUNNABLE,RUNNING,BLOCKED,DONE,…}` (spec §7.1 `ActorStatus` maps onto these; spec adds `CRASHED`/`HALTED`/`IDLE` distinctions).
