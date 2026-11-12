# Implemented transfer policies

`mesa-core` separates interest scheduling from host-selected recovery, timer,
and congestion policies. `Engine::with_policy` validates one `TransferPolicy`
at construction. Peers receive independent congestion state under that policy.
The implementation supports fixed windows and a bounded AIMD prototype. The
policy layer does not implement flow persistence, RTT estimation, pacing, or
complete-path policy selection.

## Configuration

`Config` retains resource limits, including the hard per-read `window` cap.
`TransferPolicy` contains three independent choices:

| Component | Implemented choices |
|---|---|
| `RecoveryPolicy` | Maximum transmissions per interest, including the first |
| `TimerPolicy` | Absolute read duration; fixed or capped exponential retry delay |
| `CongestionPolicy` | Fixed aggregate peer window, or AIMD with initial and maximum windows |

`Engine::new` derives a compatibility policy from `Config`: fixed retry delay,
the existing attempt budget and read duration, and a peer window of
`window * max_reads`. That ceiling admits all the previous per-read windows.
`Engine::with_policy` uses the explicit policy's timers and attempt budget;
the corresponding legacy `Config` fields remain validated but have no effect.
Zero durations, zero attempt/window limits, inverted ranges, overflowing
resource products, and a peer window above `window * max_reads` are rejected.

Policy selection belongs to the host. Packet fields, peer names, and attachment
handles cannot change it. Every implemented policy retains endpoint retries.
A future stream attachment must establish complete-path guarantees before
disabling them; a reliable first leg to a relay supplies no such guarantee.

## Interest lifecycle and admission

An interest starts queued with zero transmissions. Emitting `Action::Send`
spends one attempt, occupies one peer flight slot, and starts its retry timer.
The host must dispatch that action once. A host send failure counts as loss;
this boundary does not confirm socket or downstream delivery.

The scheduler runs after each successful engine step. It visits peers in key
order and fills available peer slots. Within a peer, it visits reads in cyclic
`ReadId` order, beginning after the last read served. Each visit emits one
interest. Within a read, the lowest queued fragment index goes first, allowing
an LSS predecessor to recover before later leaves. Fairness applies to newly
available slots; a new read does not preempt an existing transmission.

A response must match an interest emitted at least once. Unsent queued entries
cannot consume unsolicited pages, reserve payload storage, or earn congestion
credit. A delayed response can satisfy a previously emitted interest while it
waits for retransmission. A consumed interest cannot earn credit twice.

Successful verification removes the interest. Expiry releases its flight slot
and queues a retry with its attempt count intact. All newly expired interests
are processed before scheduling retransmissions. An expired final attempt
terminates the whole read with `RetriesExhausted`. Cancellation, blocking,
breach, completion, and resource failure remove that read's queued and active
interests; other reads can use the released capacity in the same step.

Removing a route suppresses transmissions. Existing timers expire once, after
which only the read deadline remains while the route is absent. Repeated polls
spend no attempts. Restoring a route immediately schedules eligible queued work.
Route replacement and same-rift key updates preserve congestion state and read
deadlines. Remote breach terminates the old reads and resets that peer's
controller. Blocking terminates reads while retaining the controller.

## Congestion feedback

The fixed policy supplies a constant peer ceiling. The AIMD prototype starts
at `initial` slots and counts newly verified responses. Each full window of
credits adds one slot, up to `maximum`. Each `AdvanceTo` event that observes
one or more expired transmissions halves that peer's window, rounded down
with a minimum of one, and clears its accumulated credit. Expirations for
other peers have no effect on this controller.

For signed-public reads, a valid proof page earns one credit after successful
resource admission. Each verified LSS leaf earns one credit. Out-of-order bytes
earn no credit until the LSS verifier accepts them; invalid signatures, invalid
leaves, duplicates, and unsolicited pages earn none. The controlled fixture
profile uses its existing test verification boundary.

Older active transmissions can temporarily exceed a reduced window. They keep
their timers and remain eligible for valid replies. The scheduler emits no new
interest until active transmissions fall below the current window. A retry
competes for the same peer budget as a first transmission.

This is an explicit, deterministic congestion prototype. Feedback reflects
verified-object progress, not raw packet arrival or an RTT sample. Batched LSS
verification and host polling affect its response. It supplies neither byte-rate
fairness nor pacing, Internet congestion safety, or per-source server admission.
It controls requested responses, not all traffic sent by a serving peer.

## Timer contract

Ticks use a host-defined unit. For an interest transmitted for the `n`th time
at tick `t`, its retry deadline is the saturating sum of `t` and:

- Fixed: `after`.
- Exponential: `min(maximum, initial * 2^(n - 1))`, with saturating arithmetic.

Each fragment starts at attempt one. Queue time consumes no attempt or retry
timer. The read deadline is `admission_tick + read_timeout`, checked for overflow
before allocating read state. It includes all queue time and never extends.
Absolute expiry takes precedence over retries at the same tick. The host must
advance time before delivering packets at a deadline.

`SetTimer` reports the earliest read deadline or active retry deadline; queued
interests add no timer. Every reported timer is in the future after a completed
step. A large clock jump processes one expiry batch and schedules from the
new time, without sending missed retries in a burst. If the absolute deadline
has passed, the engine returns `Timeout` without retransmitting.

For `A` allowed transmissions and an absolute duration `D`, a read emits at
most `A * (1 + maximum_leaves)` interests in the signed-public profile and
retains read state for at most `D` host ticks, provided the host services time.
The extra interest obtains the proof. At most `max_reads * window` interest
entries exist. These are implementation bounds, not a delta-t proof: maximum
packet lifetime, durable flow state, and acknowledgment-delay algebra remain
unspecified. Same-name immutable-object replay remains intentional.

## Inspection and validation

`Engine::transfer_state(peer)` exposes the current window, active transmissions,
and queued interests. `Usage::pending_interests` counts both queued and active
entries. Neither counter includes a host-owned output queue.

Run `cargo test -p mesa-core --test protected policies` for exact timer traces,
shared-window fairness, invalid-feedback rejection, route outages, peer
isolation, lifecycle cleanup, and deterministic replay. Eight seeded schedules
exercise three simultaneous signed reads in each direction with loss,
duplication, delay, reordering, and corrupt packets. Replaying a schedule must
produce identical actions and exact returned bytes.

Run `python3 tools/test-protected-udp.py` for two independent loopback processes
under both policies, at 0, 1, 1024, 1025, 17425, and 1048576 bytes. The executable
accepts an optional final `fixed` or `adaptive` argument. Socket tests cover
dispatch of retries released when the host restores a route at construction.

## Durable flow integration

The [bounded durable flow profile](10-durable-flows.md) now reuses these
policies for message and receipt reads. It supplies a separate transaction
boundary for enqueue, delivery, and settlement, tested with a fake atomic host.
Its availability hints have their own bounded retry counters; they do not
consume the core congestion window. Production hint pacing and complete-path
policy selection remain future work.
