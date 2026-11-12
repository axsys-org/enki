# Implemented flow attachments and relay profile

`mesa-transport` connects the durable flow node to nonblocking UDP or a framed
stream. Its deterministic two-leg relay forwards unchanged pacts under a
bounded pending-interest table (PIT). Both endpoints keep their configured
recovery, timer, and congestion policies across every attachment.

The profile uses preconfigured peers, public plaintext messages, and the fixed
v2 signed-public protection. It adds no identity discovery, requester
authentication, encryption, enrollment, or Urbit compatibility. A configured
socket address or stream identifies a local route claim; signatures and LSS
still authenticate objects at the endpoints.

## Host boundary

`Host<I: PacketIo>` owns one `mesa_flow::Node` and one replaceable lane to one
configured remote peer. Every flow in that node must use that peer. The caller
creates sockets, connects or accepts TCP, supplies time, polls the host, and
processes every returned `Batch`. Multiple remote peers, concurrent lanes, and
route selection remain separate work.

`Batch.actions` contains all non-network node actions, including timers,
delivery proposals, storage requests, and completion notifications. The host
consumes `Send` actions through the attachment. Network errors appear in
`Batch.notices`, alongside actions already issued during that call. A socket
reset after receipt verification therefore returns the settlement commit to
the application. It cannot turn that commit into an I/O error with no actions.

The host never acknowledges storage. The application must apply the transaction
contract in [chapter 10](10-durable-flows.md): store each proposed checkpoint and
application disposition atomically under its revision and writer fence, then
submit `Committed(token)` after durable success. `Deliver` remains a proposal.
The tests in this crate use an atomic in-memory model. A separate
[persistent host](12-persistent-host.md) now supplies a transactional SQLite
inbox and subprocess crash tests over these adapters.

Construction and `reconnect` return actions because restoring a route can emit
outbox announcements and pending interests immediately. `disconnect` removes
the route and releases the old attachment. A hard I/O failure does the same and
returns a `Disconnected` notice. Missing routes suppress new sends; absolute
read deadlines can still expire. A fresh lane restores stalled work without
changing committed outboxes, receipts, or sequence numbers. Recovery after a
node restart loads the latest host checkpoint with a fresh fenced incarnation.

`Host::poll(now)` advances node time before processing network input. It makes
one bounded write flush and at most 64 receive calls. It returns node failures
encountered after input as notices so earlier actions remain visible. The
application must continue polling connected lanes: `SetTimer` describes node
protocol deadlines, not socket readiness or queued stream writes. A received
partial frame can require another poll before any complete pact is available.
An application that ignores returned batches forfeits the host contract.

## Attachment bounds and framing

Each `PacketIo` implementation declares its local maximum pact size, reliability,
ordering, and framing. An instance represents one full-duplex lane. A successful
`send` means local admission, not remote receipt. Queue pressure or UDP
`WouldBlock` returns `false`; the host records the lost send and leaves endpoint
recovery active. Finite budgets may exhaust during persistent pressure. The
application can explicitly resume after the cause clears.

| Attachment | Packet boundary | Receive bound | Send bound |
|---|---|---|---|
| `Udp` | One pact per datagram | One datagram per call; buffer is `max_packet + 1` | One nonblocking send per call |
| `Stream<S>` | Four-byte unsigned big-endian length, then exactly that many pact bytes | At most two reads per call; four prefix bytes and one bounded body | Explicit byte capacity; at most 64 writes per flush |

The transport constructors accept packet bounds from 8 through 65535 bytes.
The flow examples use 1472. A host rejects an attachment smaller than its node's
configured maximum pact. The stream queue capacity includes four prefix bytes
per pact and the entire front frame until its final byte is written. It must
hold at least one maximum frame and cannot exceed 16 MiB. Queue contents and
partial receive bodies are disposable soft state.

UDP accepts datagrams only from its configured next-hop address. It discards
unknown sources, short input, and oversized datagrams without passing them to
the node. This gate does not authenticate a peer.

The stream parser validates length before allocating a body. It handles split
prefixes, split bodies, coalesced frames, short writes, and `WouldBlock` without
changing pact bytes. An invalid length, EOF, or hard I/O error poisons the lane
and releases queued frames and the receive body. No bytes from an old stream
carry into a replacement. `Stream::tcp` sets the connected socket nonblocking;
callers of generic `Stream::new` must supply nonblocking `Read + Write` methods.

Stream reliability describes the live local byte connection. It does not
suppress endpoint interest retries, change the peer congestion window, enlarge
pacts, or imply reliable downstream forwarding. No complete-path reliability
claim or automatic policy selection exists in this implementation.

## Two-leg relay

`Relay` has two configured legs and one reachable publisher per leg. The host
maps ingress to a leg and supplies monotonic virtual time. The relay owns no
sockets, retransmissions, durable state, cache, or application effects. Its
`Forward` result either identifies the output leg and original bytes or gives
a drop reason. The caller performs bounded I/O and accounts for output loss.

A Peek goes toward its configured publisher and allocates a PIT entry keyed by
the entire canonical name: publisher, domain, rift, bloq, fragment, and path.
The key is a canonical hop-zero Peek encoding. Duplicate interests forward again
without increasing table size or extending the original expiry. A Page returns
only through a matching interest from the other leg, then removes that entry.
Unsolicited Pages and packets arriving from the wrong publisher leg are dropped.

The relay checks packet size against both legs before allocating state or
forwarding. It never fragments a pact, rewrites its name, raises a path MTU, or
uses a Page's next-hop trailer for routing. The table has a configured cap of
1 through 65536 entries and a nonzero lifetime. Each key is bounded by the wire
packet limit. A full table drops new interests; endpoint retries can recover
after slots expire. Unrepresentable expiry times are rejected. The host calls
`advance_to` before input at that time and uses `next_timer` to expire idle
state. Disconnecting either leg clears all PIT entries, since every entry
crosses both legs.

Flow Pokes are empty availability hints. The relay forwards them only when
both named publishers are the configured endpoints, with no inline payload or
authentication material. They allocate no PIT state. The receiving flow node
still validates exact message and receipt names, epochs, windows, and service
access before reading any advertised object.

PIT matching is a forwarding check, not object authentication. A structurally
valid forged response can consume an interest; endpoint verification rejects
bad data and endpoint retries restore progress. General routing, loops,
fan-out, source rate limits, caching, relay-owned recovery, and admission
protection against hostile configured links require separate policies. This
relay supports a fixed two-endpoint path, not arbitrary network topology.

## Acceptance evidence

Run the live socket slice and deterministic tests:

```sh
cargo run -p mesa-transport --example flow-loopback
cargo test -p mesa-transport
```

The example opens two loopback sockets in one process. For UDP and TCP, each
peer sends messages of 0, 1024, 1025, and 17425 bytes. The fake atomic stores
check exact payloads, ordered application effects, and committed settlement in
both directions. A separate TCP test closes the connection with a retained
outbox, observes route removal on EOF/reset, opens a new pair, and settles it.
These are libmesa-to-libmesa tests, not unchanged-Urbit interoperability.

The deterministic relay test connects a framed reliable byte stream to a lossy
datagram model. Reads take at most 127 bytes and writes at most 113 bytes; each
simulated byte direction holds at most 8192 bytes. The datagram model drops the
first downstream Page, applies seeded loss, duplication, and reordering, and
injects a corrupted signature with a valid packet checksum. Four fixed seeds
run twice and produce identical datagram traces. The test observes explicit
endpoint protection rejection and repeated interests across the stream leg.

Each schedule cuts the stream at tick 25 and reconnects at tick 60, clearing
relay interests. It also restarts the datagram peer after a delivery transaction
commits but before its completion callback. Both stores retain exactly four
ordered application effects and settle all outboxes. The PIT stays within 32
entries and expires to zero. Unit tests cover name-context matching, direction,
capacity, expiry, clock regression/overflow, and size gates.

Additional tests cover every split of a framed packet, bytewise input, coalesced
frames, partial writes, queue limits, invalid lengths, EOF at every truncation,
unknown UDP sources, bounded polling, finite retry exhaustion under persistent
queue pressure, and preservation of commit requests across socket failure.

## Next work

The [reference persistent host](12-persistent-host.md) now implements the
transaction boundary with SQLite, writer fencing, and subprocess crash tests.
It stores accepted inbox effects and rejection records atomically with the
checkpoint. Arbitrary application transactions and storage operations remain
host integration work.

The finite sequence log remains deliberate: receipts, settlements, and retired
names accumulate up to the configured epoch cap. Continuous operation needs a
separate reclamation protocol that proves old names cannot be reused or cause
redelivery after either endpoint restarts. Production congestion/pacing,
multihoming, encrypted attachments, WSS, and general relay service remain
separate milestones.
