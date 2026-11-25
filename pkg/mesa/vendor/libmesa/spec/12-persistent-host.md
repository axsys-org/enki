# Implemented persistent host profile

`mesa-store` stores a flow checkpoint and its application disposition in one
SQLite transaction. Accepted delivery appends an inbox row; rejection appends a
terminal disposition. A successful database commit returns the token that the
host passes to `Node::step(Committed(token))`. The transport never acknowledges
storage on the application's behalf.

This is a reference inbox application over preconfigured peers and public
signed objects. The store adds process-crash recovery to the flow transaction contract.
[Continuous profile 2](13-continuous-flows.md) now adds protocol reclamation
and a separate sender-outcome table. Private spaces, enrollment, generalized
authority provisioning, and unchanged-Urbit interoperability remain deferred.

## Atomic state and application effect

One database owns one node checkpoint. Its `state` row contains a schema format,
random database tag, persistent writer generation, durable revision, and the
serialized checkpoint. Format 2 adds `settlements` rows for sender outcomes,
with stable message IDs and commit revisions. The `deliveries` table contains stable message IDs,
commit revisions, exact payloads, and accepted or rejected outcomes. Message IDs
are primary keys; delivery revisions are unique. Endpoint IDs retain all 128
bits, and flow IDs retain all 64 bits.

The accepted inbox row is the reference application effect. A terminal rejection
records its payload and outcome without an accepted effect. This implementation
does not make a payment, send an email, or invoke an external application inside
the transaction. Such effects need their own atomic integration or idempotence
under the stable delivery ID, as required by [chapter 10](10-durable-flows.md).

| Change | Writes in the same transaction | Completion permits |
|---|---|---|
| Enqueue | Exact outbox payload, next-send sequence, checkpoint revision | Publishing the message |
| Deliver | Inbox/disposition row, receipt digest/outcome, receive prefix, checkpoint revision | Publishing the ACK or terminal NACK |
| Settle | Sender-outcome row, outbox removal, checkpoint revision | Releasing the send window; publishing release at a full batch |
| RetireReceived | Receive floor, receipt removal, checkpoint revision | Publishing the retirement acknowledgment |
| RetireSent | Send floor, settlement removal, checkpoint revision | Enqueueing the next batch |

`Store::prepare` starts an IMMEDIATE transaction, checks the writer fence and
base revision, validates the proposed transition, and stages every write.
`Prepared::commit` commits and returns the original token. `Store::commit`
combines these calls. Dropping an uncommitted `Prepared` rolls back its changes;
process termination leaves SQLite to recover the transaction.

Transition validation constructs the expected next checkpoint from the stored
one and compares every field. An enqueue advances exactly one sequence and adds
one payload. A delivery advances exactly one receive prefix and binds the exact
payload digest and outcome. Settlement removes one retained outbox entry and
records its outcome. The proposed revision and token revision must both equal
the stored revision plus one. The store accepts trusted engine requests; object
authentication remains the engine's responsibility.

A storage error returns no completion token. The host withholds the engine
callback and resolves the authoritative database state before proceeding.
The loopback executable exits on storage errors; restart opens the latest
committed checkpoint. A successful database commit can therefore survive the
loss of its callback without losing an accepted effect or duplicating it.

## Creation, restore, and writer fencing

`Store::create(path, empty_checkpoint, limits)` creates a new path exclusively
and persists the initial empty state. Every existing path is refused, including
an empty or partially initialized file. Initialization failure can leave such a
file for inspection; opening it never silently creates a replacement flow.
On Unix, creation also syncs the containing directory entry.

`Store::open(path, expected_empty_checkpoint, limits)` requires an existing
database. It validates the format, checkpoint structure, delivery order,
retained receipt digests, complete outcome history, batch size, and endpoint/flow scope before claiming a writer
generation. The caller then constructs the node with the stored checkpoint and
`Store::incarnation()`. The node separately validates its configured engine
limits and peer keys. Missing files, changed endpoint epochs/domains, invalid
state, and incompatible scope fail closed.

A successful open increments the generation in the same IMMEDIATE transaction
that reads and validates the snapshot. Incarnation tokens combine the database's
random 64-bit tag with its monotonically increasing generation. The generation
never wraps. Its uniqueness guarantee is within an intact database history;
the random tag distinguishes independently created databases probabilistically.
It is a host bookkeeping value, not an authentication credential.

Every commit compares the handle's incarnation, the request token, and the
current database generation under the write transaction. A newer owner fences
older handles and processes. A writer already holding an IMMEDIATE transaction
commits or rolls back before another owner can claim the database. Lock waits
have a 250 ms timeout. Read-only inspection takes a consistent snapshot without
claiming ownership or fencing an active writer.

Database rollback, copying a live database into an independently writable fork,
and recreating an existing flow with an empty database violate this contract.
The store has no external anti-rollback authority. Preserve the authoritative
database and its required SQLite sidecar files; do not reset it to clear a
storage quota.

## Storage configuration and bounds

The crate uses rusqlite 0.40.2 with bundled SQLite; the committed lockfile selects
SQLite 3.53.2. Writable connections require WAL mode and verify
`synchronous=FULL`. They also request `fullfsync` and `checkpoint_fullfsync`.
SQLite documents the additional commit synchronization for FULL in WAL mode;
its guarantees depend on the filesystem and storage honoring synchronization.
See [SQLite's synchronous contract](https://www.sqlite.org/pragma.html#pragma_synchronous).

Use a local filesystem supported by SQLite. WAL requires cooperating processes
on one host and does not support a network filesystem. Database backup and
sidecar handling must follow SQLite's rules; a live `.sqlite` file alone is not
a complete backup. See [SQLite's WAL contract](https://www.sqlite.org/wal.html).

| Host limit | Default | Maximum accepted configuration |
|---|---:|---:|
| Serialized checkpoint | 8 MiB | 64 MiB |
| One message/disposition payload | 64 KiB | 1 MiB |
| Retained records in each application table | 16384 | 16384 |
| Total retained delivery payloads | 64 MiB | 1 GiB |

Checkpoint serialization stops at the configured byte limit. Reads check the
serialized length before JSON parsing, and SQLite's value-length limit bounds
individual SQL values. Restore checks record count and total payload size
before collecting the inbox. A failed limit check commits neither the
checkpoint nor its application row. These are host data limits, not a hard
quota for SQLite pages, WAL files, caches, or allocator overhead.

The engine's configured limits can be smaller and still apply. The store also
checks the protocol maxima: at most 16 flows and 1024 messages per retained
batch. The default batch contains 64 sequences per direction. Checkpoint
histories and namespace entries are reclaimed by authenticated watermarks;
application rows remain until a future explicit host retention API. A full
application table fails the transaction even when protocol history is empty.

## Executable slice and crash evidence

Run the complete subprocess matrix:

```sh
cargo test -p mesa-store
```

`persistent-peer` is a loopback-only executable with public fixture keys. It
opens existing stores, sends four messages in each direction, records three
accepted inbox effects and one terminal rejection per peer, and continues
serving receipts after settlement. Message sizes are 0, 1024, 1025, and 17425
bytes. The application rejects sequence 1 with code 23.

For a manual UDP run, build and initialize two fresh database paths:

```sh
cargo build -p mesa-store --bin persistent-peer
mkdir -p /tmp/libmesa-persistent-demo
target/debug/persistent-peer init 0 /tmp/libmesa-persistent-demo/0.sqlite
target/debug/persistent-peer init 1 /tmp/libmesa-persistent-demo/1.sqlite
```

Run each peer in a separate terminal:

```sh
target/debug/persistent-peer run 0 udp /tmp/libmesa-persistent-demo/0.sqlite 127.0.0.1:61001 127.0.0.1:61002 none /tmp/libmesa-persistent-demo/run0
target/debug/persistent-peer run 1 udp /tmp/libmesa-persistent-demo/1.sqlite 127.0.0.1:61002 127.0.0.1:61001 none /tmp/libmesa-persistent-demo/run1
```

Replace `udp` with `tcp` for the stream path; peer 0 accepts the connection.
Stop both processes and repeat the run commands to recover the same stores.
Use fresh marker prefixes for each run; `.done` files report completion of
that run. `persistent-peer inspect <db>` returns the checkpoint and inbox as
JSON without claiming a writer generation. Existing database paths remain
intact; rerunning `init` against them fails.

The original crash matrix runs enqueue, delivery, and settlement at four
boundaries on both UDP and TCP: 24 cases. Profile 2 adds another 24 cases for
watermark publication and retirement; see [chapter 13](13-continuous-flows.md). The child pauses before preparation, after staging
uncommitted writes, after the database commit, or after the engine callback.
The parent reads the authoritative snapshot while the child is paused, then
kills it without destructors. On Unix the test requires SIGKILL. The other peer
also stops; both restart on fresh socket routes using the same database files.

Before an enqueue callback, the remote store contains no delivery of the
unpublished message. Before a delivery callback, the remote sender has no
settlement. Before commit, recovery returns the old revision and no partial
application row. After commit, recovery returns the new revision even if the
callback was lost. Both peers eventually reach revision 12, four ordered
inbox/disposition records, four settlements, and empty outboxes. A further
restart leaves every checkpoint and application record unchanged.

A separate subprocess test pauses an old writer, opens a new owner, then
releases the old process. Its storage attempt fails `Fenced`; no outbox message
is published. Unit tests also cover lock contention during a prepared
transaction, duplicate revision rejection, malformed transitions, full-width
IDs, generation exhaustion, storage limits, and restore validation. An injected
SQL trigger fails the checkpoint update after insertion of the application row;
the transaction rolls back both writes.

These tests establish process-crash recovery under their schedules. They do
not simulate kernel crashes, power removal, torn sectors, dishonest flushes,
or arbitrary database corruption. The reference store also lacks migration,
backup orchestration, a disk quota, and arbitrary application transactions.
Those boundaries remain explicit despite the use of real persistent storage.

## Next phase

Protocol reclamation is implemented in [chapter 13](13-continuous-flows.md).
Next add host-managed application consumption and retention with durable cursors
and explicit quota release. Preserve protocol floors and application delivery
identity across restart. Format-1 databases require an explicit future migration;
current code fails closed rather than resetting existing state.
