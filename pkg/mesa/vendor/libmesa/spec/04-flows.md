# libmesa spec — 4: Flows and messages

> **Design outline.** The flow engine is sequencing/window mechanism + policy
> modules (retransmission, congestion) selected from complete-path guarantees — option C in
> `../doc/02-rina.md` §6.1.

The named-read engine now implements the [transfer-policy prerequisite](09-transfer-policies.md).
The [bounded durable profile](10-durable-flows.md) now implements ordered
sequencing, ACK/NACK receipts, host commits, and crash/restart tests with
preconfigured flows. The broader upstream-compatible outline remains future work.

## Outline

1. **Message layer.** `%plea`/`%boon` over flows; `$spar`; payload encoding
   (noun for Arvo interop; opaque bytes otherwise).
2. **Flow identity.** side = [bone dire]; bone allocation parity rules;
   forward/backward/naxplanation bone bits.
3. **Sender state vector.** ordered send queue, next-seq, sliding window;
   pokes reference `/flow/<bone>/poke/...` bindings.
4. **Receiver state vector.** last-acked, pending-ack, nack map; exactly-once
   delivery rule (+(last-acked) only; bounded re-ack of dupes).
5. **Acks as bindings.** `/flow/.../ack/<seq>` pages; nacks; naxplanations as
   separate flow bindings.
6. **Timers (delta-t).** The MPL/A/R mapping for every timer (PIT expiry,
   resend, dead-flow, recork); the exactly-once argument; whether corks are
   necessary or an optimization (open — doc/02-rina.md §7.3).
7. **Backpressure.** clog/flub/halt semantics toward the host.
8. **Policies.** retransmission (on/off), congestion (gauge port / off),
   delimiting bloq; derivation from complete-path guarantees (ch. 7).

## Host durability contract (required before flow implementation)

The engine must expose a delivery identifier containing the peer, epoch,
flow, and sequence. The host must durably record the accepted delivery and
its deduplication state before the engine publishes an acknowledgment that
permits the sender to discard the message.

Exactly-once application effects require the host to atomically commit the
effect and that delivery identifier, or make the effect idempotent under the
identifier. An ordinary callback followed by an ack cannot guarantee this:
a crash between the effect and the durable record permits duplicate effects.
A crash after an early ack can lose the message.

Define recovery of pending deliveries, durable sequence state, outbox state,
and stale epochs before implementing the flow API. Test crashes before and
after each commit/ack boundary. Timer algebra addresses packet lifetime and
state reclamation; it does not replace host transactions. The bounded profile
exercises these requirements with a fake atomic host. The separate
[persistent host](12-persistent-host.md) adds a transactional SQLite inbox,
writer fencing, and subprocess crash tests. Arbitrary application transactions
and continuous-log reclamation remain separate work.
