# Durable flow foundations: profile 1

Profile 1 is superseded by [continuous profile 2](13-continuous-flows.md). This
chapter records the original transaction contract and finite-history design.
Profile 2 preserves the commit boundary and replaces the namespace version,
checkpoint layout, and finite epoch cap. Current code rejects profile-1 state.

`mesa-flow` provides ordered message delivery over the signed-public read
engine, with host-owned durable state. A sender publishes a message only after
its outbox commit. A receiver publishes an ACK or terminal NACK only after its
delivery commit. The sender removes the message only after authenticating that
receipt and committing the settlement. These three boundaries survive restart
in the fake atomic host tests.

This is a bounded, preconfigured profile. Messages and receipts are public
plaintext with Ed25519/LSS authentication. Flow IDs and both endpoint epochs
are configured before construction. Dynamic allocation, private spaces,
upstream bone conventions, continuous-log reclamation, and epoch migration
remain unimplemented. No generalized PKI or enrollment is introduced.

## Host contract

The host persists an initial `Checkpoint::empty` before starting a new flow
instance. `Node::new` restores the latest committed checkpoint, checks its
version and bounds, and checks both endpoint names, domains, and rifts against
configuration. It rebuilds public bindings from the committed outbox and receive
receipts. Read state, congestion state, timers, advertisements, and incomplete
verification are disposable soft state.

The host supplies a fresh, nonzero `incarnation: u128` on every construction.
Commit tokens combine that incarnation with the proposed durable revision.
An old callback cannot confirm a new process's proposal, even if both proposals
use the same revision after an uncommitted crash. Generating unique incarnation
values, fencing old writers, and enforcing the revision guard belong to the host.

Each `Action::Commit` carries a base revision, the proposed checkpoint, and a
`Change`. The host performs one atomic transaction against that base revision,
then sends `Event::Committed(token)` only after durable success. There is at most
one pending transaction per node. Neither its proposal nor an ordinary delivery
callback authorizes a network acknowledgment.

| Change | Host transaction | Engine behavior after completion |
|---|---|---|
| `Enqueue` | Persist exact message bytes and advance `next_send` | Bind the message, announce availability, emit `Enqueued` |
| `Deliver` | Apply the accepted effect, or record the rejection; record the stable delivery ID and receive checkpoint atomically | Bind the ACK/NACK receipt and advance ordered delivery |
| `Settle` | Record the outcome and remove the message from the durable outbox | Unbind message bytes, release the send-window slot, emit `Settled` |

For accepted delivery, the application effect and its deduplication record must
commit with the checkpoint. An alternative is an external effect made idempotent
under the stable delivery ID. Applying an effect at `Action::Deliver` and storing
the checkpoint later violates this contract. A recorded rejection advances the
receive prefix without applying an accepted application effect.

A failed or uncertain storage operation receives no completion event. The node
keeps the proposal pending; it does not infer abort from time. The host resolves
the transaction or restarts from its authoritative store. `Node::checkpoint()`
reflects completed callbacks, so it can lag the store when a commit succeeds
before its callback. It must not overwrite that newer durable state. Loading an
older checkpoint, concurrent unfenced writers, and storage corruption are
outside the crash-recovery guarantee.

`Enqueued`, `CommittedDelivery`, and `Settled` are process notifications. They
may be lost in a crash after storage success. Applications recover acceptance,
delivery records, and settlement outcomes from durable state; notification
callbacks do not promise exactly-once execution.

## Identity, sequencing, and namespace

A `MessageId` contains sender name/rift, receiver name/rift, flow ID, and sequence.
Names use the existing globally unique 128-bit representation. Each flow ID is
unique within a node's configured table and identifies two independent ordered
directions between the configured endpoints. Sequence numbers start at zero.
Both endpoints must agree on the flow ID and epochs.

Enqueue requires the caller's expected sequence. A retry of an already committed
enqueue fails `Sequence`; the caller inspects `next_send`, the outbox, and settled
outcomes to resolve it. The caller must not replace an uncertain enqueue with a
new sequence automatically. A message name never changes contents within its
epoch. Reusing a flow ID with an empty checkpoint in the same epochs violates
that rule; creation and retirement of flow IDs remain host responsibilities.

Paths use fixed-width lowercase hexadecimal fields:

```text
flow/1/<sender:32>/<sender-rift:8>/<receiver:32>/<receiver-rift:8>/<flow:16>/msg/<seq:16>
flow/1/<sender:32>/<sender-rift:8>/<receiver:32>/<receiver-rift:8>/<flow:16>/receipt/<seq:16>
```

The message path is 128 bytes; the receipt path is 132. A message binding belongs
to the sender, and its receipt binding belongs to the receiver. Their outer v2
names supply the respective publisher domain/rift, bloq 13, and `Auth(0)` or
`Data(i)` fragment coordinate. The signed-public profile authenticates the full
name and exact object bytes. Endpoint domains are pinned in the checkpoint;
domain migration requires a separate protocol.

The sender retains at most `window` unsettled messages per flow. The receiver
may verify the next `window` sequences concurrently, but proposes only the next
uncommitted sequence for application delivery. It waits for that sequence's
commit before offering the next. One application decision is outstanding per
node; a slow decision can therefore delay other flows. The outbox and buffered
receive windows provide explicit backpressure throughout that delay.

## Availability and recovery

A committed sender announces message availability with a v2 `Poke`. Its
`payload` name identifies the message proof; its `ack` name identifies the
receipt proof. Both use `Fragment::Auth(0)`. The data section has zero logical
length, absent auth, and an empty fragment. The receiver uses the same names
when announcing receipt availability. Direction follows the configured source
claim and the two endpoint identities.

A hint is untrusted scheduling input. It cannot supply message bytes, approve a
delivery, settle an outbox entry, or bypass signature/LSS verification. The node
checks the configured source attachment and service gate, both complete names,
canonical paths, and the active send/receive window before scheduling a read.
A source address still does not authenticate the sender. Duplicate hints share
one read slot. Failed reads require host `Resume`; repeated hints do not reset
their budgets.

Only advertised objects are read. Polling for nonexistent future messages or
uncommitted receipts would consume congestion slots and classify ordinary
application delay as packet loss. An availability hint starts the existing
signed read mechanism, which supplies fragmentation, retries, bounded congestion
control, and full-object verification.

Each unsettled outgoing message has a bounded announcement attempt count. Hints
use the injected transfer policy's retry delays and maximum attempts. They stop
when a verified receipt is ready, when the message settles, or when the budget
expires. Expiry emits `AnnouncementStalled` and retains the durable outbox.
Duplicate message hints for an already committed receive sequence elicit another
receipt hint, without redelivery. This repairs lost receipt announcements and
receiver restarts without a separate acknowledgment-of-acknowledgment exchange.

A failed named read emits `Stalled`. `Resume(flow)` rearms failed reads and
announcements for that flow. Restoring a route or unblocking service also rearms
that peer. The host decides when to retry; there is no hidden infinite polling
loop or silent deletion of failed messages. Hint retries and data reads have
separate bounded counters. Hints do not consume the core's congestion window;
they remain subject to outbox bounds and retry intervals. Per-source admission
and production pacing remain future work.

The host advances time before delivering packets at a deadline. The node emits
one replacement `SetTimer`, combining core read deadlines with announcement
retry deadlines. A clock jump processes at most one due announcement per
message; it does not replay missed intervals. Missing routes suppress hints and
spend no additional attempts. Pending storage and application decisions have
no automatic timeout because a timeout cannot establish transaction abort.

## ACK and NACK contents

Receipts are immutable signed-public objects with this exact value:

| Field | Encoding |
|---|---|
| Label | ASCII `libmesa/flow-receipt/1` followed by one zero byte |
| Message digest | SHA-256 of the exact message bytes, 32 bytes |
| Outcome tag | `00` for accepted, `01` for rejected |
| Rejection code | `u16` little-endian; must be zero for accepted |

The value is exactly 58 bytes. Other tags, lengths, labels, or accepted codes
are invalid. The authenticated receipt name binds both endpoint epochs, flow,
and sequence; its digest must match the retained outbox message. A valid
signature alone cannot approve a wrong digest or malformed receipt.

A NACK records a terminal application rejection. It lets the sender retire that
message after settlement commit, while preserving the rejection code. Missing
fragments use named-read recovery rather than a terminal NACK. Gap repair,
naxplanations, and compatibility with upstream flow conventions remain separate.

## Checkpoint and bounds

Checkpoint version 1 stores local identity, the ordered flow table, remote
identities, `next_send`, pending outbox bytes, sender settlement outcomes, and a
contiguous receive receipt vector. Outbox and settled sequences partition
`[0, next_send)`. The revision equals total enqueues plus committed receives plus
settlements across all flows. Restoration rejects incompatible configuration,
invalid partitions, oversized payloads, exhausted integer revisions, and bounds
violations. Serialization is host-selected; the fake host roundtrips JSON bytes.
The host must bound serialized input before parsing it.

Defaults are four configured flows, four outstanding messages per direction,
64 KiB per message, a 1 MiB aggregate durable outbox, and 64 sequences per flow
per direction. Configuration permits at most 16 flows and 1024 sequences. The
constructor reserves enough core read, binding, and reassembly capacity for its
chosen bounds. Enqueue returns `Busy` during a pending commit, `Backpressure`
at window/byte limits, or `Limit` at the finite sequence cap.

For `F` configured flows, window `W`, and sequence cap `M`, active named reads
are bounded by `2FW`, buffered incoming messages by `FW`, and namespace slots
by `2FM`. Committed receiver receipts and sender outcomes remain bounded by
`2FM` records. A pending commit adds one checkpoint copy and, for delivery,
one message copy. `Node::usage()` reports those payload counts and transaction
state separately from `core_usage()`. Host-owned action copies and application
storage are outside those counts.

Sender settlement frees payload and LSS storage; the core retains the retired
name until restart. After restart, durable sequence state prevents reuse of that
name. Receiver receipt bindings survive restart so a sender with a lost ACK can
still settle. These records are deliberately retained for the finite epoch log.
Receipt compaction and continuous operation beyond the sequence cap require a
future durable reclamation protocol.

## Key updates and epoch changes

Same-rift key updates preserve flow sequences, outboxes, receipts, and completed
application effects. Active reads retain the signed-public profile's existing
root-pinning behavior. Blocking cancels reads and gates service while retaining
durable flow state. An already proposed application transaction must still be
resolved by the host; blocking cannot undo committed effects.

`InvalidateEpoch(peer, higher_rift)` halts the entire node and disarms its timer.
It publishes no new receipts and accepts no further commit callbacks. This is a
conservative fail-closed boundary, not an epoch-migration implementation. The
host must fence the node, resolve any storage transaction already in progress,
and reconcile the old epoch's outbox and delivery records. Restarting the old
checkpoint under a different local or remote epoch fails `EpochMismatch`.
Nothing silently discards the old durable evidence or replays it into a new epoch.

## Executable evidence

```sh
cargo run -p mesa-flow --example two-peer-flows
cargo test -p mesa-flow
```

The example uses two preconfigured peers, real signatures/LSS, an in-memory
packet link, and fake atomic stores. It delivers four messages, settles all
outboxes, restarts both peers, and repeats no application effects. The stores
fence stale process incarnations, enforce revision guards, serialize checkpoints,
and atomically replace checkpoint, delivery records,
application effects, and delivery order. No production disk backend is claimed.

Tests cover ordered windows in both directions; empty and fragmented payloads;
terminal NACKs; malformed but correctly signed receipts; untrusted duplicate
hints; admission limits; route loss; service blocking; stale callbacks; snapshot
validation; epoch invalidation; and multiple flows sharing a commit revision.
The crash matrix covers three transactions at three storage/callback boundaries,
plus a crash before the application decision. Eight seeded fault schedules mix
loss, duplication, reordering, corruption, and restart, then require identical
action traces on replay and one committed effect per delivery ID.

The separate [flow transport profile](11-flow-transports.md) connects this node
to UDP and framed TCP. It tests endpoint recovery through a bounded two-leg
relay, downstream loss, disconnects, and a receiver restart after commit.
The [persistent host](12-persistent-host.md) implements a transactional SQLite
inbox with writer fencing and subprocess crash tests. Arbitrary application
transactions and continuous-log reclamation remain later work.
