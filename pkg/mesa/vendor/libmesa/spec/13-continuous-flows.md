# Implemented continuous flow profile 2

`mesa-flow` reclaims protocol history after a signed, durably committed batch
handshake. Message sequences increase across reclamation and restart. The
checkpoint retains one batch per direction; SQLite retains application records
separately. This replaces the finite epoch limit in [profile 1](10-durable-flows.md).

The profile uses the existing preconfigured peers, public signed reads, host
commit boundary, and UDP/TCP transports. Batch size is fixed in the checkpoint
and must match both endpoint configurations. `Config::max_messages` now means
messages per batch: default 64, maximum 1024. `Checkpoint::empty` chooses 64;
`empty_with_batch` pins another size. The send/prefetch window remains independent
and cannot exceed the batch size.

## Durable evidence and reclamation

The sender stops enqueueing at the batch boundary until the receiver confirms
retirement. For a batch `[S, F)`, where `F = S + batch_size`, the protocol has
three publication boundaries:

| Boundary | Durable evidence | Publication after callback |
|---|---|---|
| Final settlement in the batch | Every sequence in `[S, F)` has a committed accepted or rejected outcome; the outbox is empty | Sender binds `release/F` |
| `RetireReceived` | Receiver authenticated `release/F` and already committed every delivery below `F` | Receiver advances `receive_floor` to `F`, removes batch receipts, binds `released/F` |
| `RetireSent` | Sender authenticated `released/F` and still has the complete settled batch | Sender advances `send_floor` to `F`, removes batch settlements, permits the next batch |

The final settlement transaction itself authorizes `release/F`. A separate
publication transaction is unnecessary: restoring a fully settled batch
reconstructs that immutable binding. Both retirement transitions use the same
revision guard and incarnation token as ordinary enqueue/delivery/settlement.
A proposal, verified watermark, or unfinished SQLite transaction permits no
retirement acknowledgment. Publication waits for `Committed(token)`.

The receiver retains `released/F` until its next receive retirement. The sender
cannot publish the next batch until it commits the acknowledgment, so receipt
of the next batch proves the previous handshake completed. Keeping one receiver
acknowledgment repairs loss after receiver retirement and across restart.

Accepted and rejected dispositions have the same retirement rules. A rejection
remains a terminal application outcome. Retirement never changes that outcome.

## Names, signatures, and untrusted hints

The immutable path is:

```text
flow/2/<sender:32hex>/<sender-rift:8hex>/<receiver:32hex>/<receiver-rift:8hex>/<flow:16hex>/<role>/<sequence:16hex>
```

Roles are `msg`, `receipt`, `release`, and `released`. Message/receipt sequences
identify one delivery; release/released sequences identify an exclusive batch
floor. All numeric components use fixed-width lowercase hexadecimal. The longest
path is 133 bytes. The sender publishes `msg` and `release`; the receiver
publishes `receipt` and `released`. Both control objects contain exactly the
bytes `libmesa/flow-retirement/2\0`. The existing signed-root context commits to
the publisher, domain, epoch, entire path, length, and content root. A control
object cannot be substituted across roles, directions, flows, or floors.

Empty Pokes advertise a `msg`/`receipt` pair or a `release`/`released` pair.
They remain untrusted scheduling hints. A receiver schedules a release read
only for the end of its fully received current batch. A sender schedules a
released read only for its fully settled current batch. Far-future hints,
retired message hints, and unexpected control coordinates create no work.
Replayed hints for the current receive floor may repeat its acknowledgment
hint; they cannot advance state.

Control reads use the existing authentication, finite retry budget, absolute
read timeout, and peer congestion window. Release announcements also have a
finite retry budget. Exhaustion retains the durable batch and reports a stall;
it never resets the flow or discards outcomes. `Resume` or route/service
restoration restarts failed work. An absent attachment emits no packets.

## Bounded checkpoint and namespace

Each flow stores `next_send`, `send_floor`, `receive_floor`, the current outbox,
the current settled outcomes, and a receive-receipt suffix. `next_receive()` is
`receive_floor + received.len()`. Both floors are batch multiples. Outbox and
settlements partition `[send_floor, next_send)`; receipts cover
`[receive_floor, next_receive())`. Each interval contains at most one batch.
The receive floor permanently rejects earlier delivery IDs, including after
all their receipt bodies disappear.

The revision equals the sum across flows of:

```text
next_send + send_floor + settled.len()
+ receive_floor + received.len()
+ send_floor / batch_size + receive_floor / batch_size
```

The two quotients count retirement commits. Validation uses checked arithmetic;
revision and sequence exhaustion fail closed. This is a bounded-history
protocol over finite-width counters, not an infinite counter space.

`mesa-core::Event::RetireBefore { prefix, suffix }` replaces individual namespace
entries with a monotonic prefix fence. It removes live objects and tombstones
whose suffix sorts before the cutoff. Every future Bind checks all fences.
The cutoff can only advance. A new fence consumes one namespace slot, and
insufficient capacity leaves the namespace unchanged. The host restores fences
before bindings after restart. This primitive accepts trusted host input; it
does not authenticate retirement evidence itself.

The flow uses four fixed prefixes per direction pair: messages, receipts,
releases, and acknowledgments. It retires messages below `send_floor`, receipts
below `receive_floor`, sender releases through `send_floor`, and receiver
acknowledgments below `receive_floor`. The current receiver acknowledgment
stays bound. At most `2 * batch_size + 6` namespace slots are required per flow,
including individual tombstones, four fences, and two control objects. Control
payload capacity is reserved at construction. Reclamation leaves active reads
in the opposite direction intact.

A packet already queued before reclamation can arrive afterward. Its immutable
name still identifies the old sequence; checkpoint floors reject its use for
new delivery or retirement. The core cannot rebind a retired name, even after
its individual tombstone has been replaced by a fence.

## Application retention and format boundary

SQLite format 2 stores sender outcomes in `settlements` and received dispositions
in `deliveries`. Each row has its stable MessageId and commit revision. A Settle
transaction inserts its outcome and updates the checkpoint atomically. Neither
retirement transition deletes an application row. Read-only inspection returns
both tables and the checkpoint. Restore checks complete record coverage, scope,
and retained receipt/outcome agreement.

`Limits::delivery_records` caps each application table separately. The default
is 16384 rows in each; the payload-byte limit applies to deliveries. Exhausting
an application quota fails the whole transaction and yields no callback.
Protocol compaction does not bypass that quota. The reference executable exits
on storage failure and restores from the authoritative database on restart.
It does not implement application consumption, export, or deletion.

This phase changes the flow namespace and checkpoint version from 1 to 2 and
the SQLite format from 1 to 2. Legacy checkpoints and databases fail closed;
there is no automatic migration. Keep existing databases intact. Initializing
an empty replacement under an already used endpoint epoch violates stable
message identity. A future migration must preserve every sequence, outcome,
and writer-fencing boundary. Wire v2 packet framing and the C ABI do not change.

## Runnable slice and acceptance evidence

Run the deterministic continuous tests and the persistent subprocess suite:

```sh
cargo test -p mesa-flow --test continuous
cargo test -p mesa-store
```

For a manual 128-message UDP run with eight-message batches, choose unused
loopback ports and a fresh directory. These commands use public fixture keys:

```sh
cargo build -p mesa-store --bin persistent-peer
mkdir /tmp/libmesa-continuous
target/debug/persistent-peer init 0 /tmp/libmesa-continuous/0.sqlite 8
target/debug/persistent-peer init 1 /tmp/libmesa-continuous/1.sqlite 8
```

Run the two commands in separate terminals:

```sh
target/debug/persistent-peer run 0 udp /tmp/libmesa-continuous/0.sqlite 127.0.0.1:61001 127.0.0.1:61002 none /tmp/libmesa-continuous/run0 128 8
target/debug/persistent-peer run 1 udp /tmp/libmesa-continuous/1.sqlite 127.0.0.1:61002 127.0.0.1:61001 none /tmp/libmesa-continuous/run1 128 8
```

Use `tcp` for the stream path. Each direction cycles through 0, 1024, 1025,
and 17425-byte messages, rejecting sequence 1 with code 23. Each peer finishes
with 128 inbox records, 128 sender outcomes, both floors at 128, empty protocol
histories, and revision 416. The processes remain available to serve control
reads. Stop and restart them with fresh marker prefixes to inspect recovery;
increase the count to continue the same sequence space. Never rerun `init` to
resume an existing flow.

The tests cover:

- Three seeds, each run twice, send 128 messages per direction through loss,
  duplication, reordering, disconnect/reconnect, and process-state restart.
  Per-step assertions bound checkpoints, namespace slots, reads, and buffers;
  action-trace digests match between runs. Old packets are replayed after final
  retirement and restart without changing checkpoint or application records.
- Lost receiver control packets leave the sender backpressured with its outcomes
  intact. Finite retries stop. Resume recovers the retained acknowledgment.
- Untrusted hints, corrupt release signatures, and signed invalid marker bodies
  cannot advance a floor.
- The subprocess suite adds 24 watermark crash cases: final settlement/release,
  receive retirement, and send retirement, each at before/prepared/committed/
  callback boundaries on UDP and TCP. Together with the original 24 cases,
  the suite covers 48 cases. Negative checks prohibit early publication.
- Real UDP and TCP processes each exchange 128 messages per direction across
  16 batches, then restart completed stores without changing their records.
- Storage tests preserve application history through reclamation, enforce its
  quotas, reject malformed advances and missing history, and roll back an
  outcome insert when the checkpoint update fails.

These finite tests establish the stated recovery and resource invariants under
the tested schedules. They are not throughput measurements, arbitrary-duration
soaks, power-loss tests, or a general application-retention implementation.

## Next phase

Add explicit host-managed consumption and retention for inbox records and sender
outcomes. Define durable consumer cursors and the evidence required before a
host can release application history. Keep those decisions separate from the
protocol floors, preserve restart-safe deduplication, and test quota recovery
without resetting peer epochs or sequence numbers. Storage migration and backup
orchestration remain separate operational work. Generalized PKI, Jael,
userspace verifiers, enrollment, and comet interoperability remain deferred.
