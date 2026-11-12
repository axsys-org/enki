# Target Mesa interface: processes, flows, and capabilities

Status: proposed target design. This document describes the intended replacement
for the current 199-call bridge. It does not describe functionality already
implemented. [API.md](API.md) remains the reference for the current bridge.

## 1. Goal

An application should ask for communication with a named application, receive a
flow capability, and exchange messages through that capability. It should use
the same operations whether the flow runs locally, over UDP/TCP, or over another
Mesa process.

The native boundary should have four forms: **create, step, inspect, close**.
Configuration, messages, events, and snapshots should be ordinary PLAN values.
Wormholes should represent live resources, authority, and mutable native state.

“RINA-shaped” means that the flow interface recurs between layers, names are
separate from transport addresses, and policy is separate from the mechanism
that executes it. It does not mean that this binding implements a complete RINA
protocol suite or is wire-compatible with an existing RINA implementation.

The expected application/control vocabulary is about 15–25 commands. This is a
design budget, not a reason to disguise 199 existing methods as message tags.
Low-level codec and cryptographic utilities are a separate library surface.

## 2. The objects an application sees

| Kind | Purpose | Ordinary data around it |
| --- | --- | --- |
| Process | Provides flows within a configured naming and policy scope | Configuration, peers, routes, policy, statistics, checkpoint |
| Flow | One communication relationship, including an incoming or connecting relationship | Destination name, requirements, negotiated properties, message IDs |
| Key | Holds secret material and authority to perform cryptographic operations | Public key, public identity claims, signatures |
| Store | Provides durable storage for protocol state and an application inbox | Storage configuration, snapshots, revisions |
| Transaction | Holds a prepared commit, then proof of its committed outcome | Proposed changes, receipt after commit |

A process is a native state machine. An actor drives it. Those are distinct
roles: creating a process does not silently spawn an actor, OS thread, event
loop, or clock.

These are not public resource kinds: Config, Policy, Peer, Packet, Actions,
Checkpoint, Atom, or Bits. They are normal data. A packet decoder returns a
record; a configuration constructor is a PLAN function; a checkpoint is a
versioned record that can be persisted.

Secret-containing rings remain protected resource material. Converting public
identity information to data must not accidentally export a ring's secret seed.

Implementations may keep native packet views, LSS trees, parsed nouns, caches,
and large buffers internally. An optional buffer capability is an optimization
extension, with the same byte semantics as the ordinary-data interface. An
application must not need it to use networking.

## 3. One native calling convention

The proposed registered names are `MesaCreateV2`, `MesaStepV2`,
`MesaInspectV2`, and `MesaCloseV2`. Every call takes exactly two arguments:

```
call(capabilities, request) -> [status, response, capabilities]
```

The capability argument is a shallow row of wormholes. The request is normal
PLAN data. The returned capability row is also shallow. All other returned data
has a normal form.

Logical request shapes are:

```
create:  [2, request_id, kind, configuration]
step:    [2, request_id, command, payload]
inspect: [2, request_id, query, payload]
close:   [2, request_id, reason]
```

For step, inspect, and close, input capability 0 is the target. Other input
capabilities are named by the command schema. For create, the schema specifies
all dependencies, such as a signing key or store.

A capability reference inside normal data is `[cap, index]`. It refers only to
the capability row of that request or response. The schema distinguishes a
capability reference from an ordinary row; arbitrary rows are never interpreted
as references. Indices are local to one envelope and cannot be saved and used in
a later call. They are not object IDs or an actor-to-ID registry.

Successful response:

```
[0, [2, events, more], returned_capabilities]
```

Rejected request:

```
[1, [2, request_id, error_code, details], []]
```

Each event has one shape:

```
[tag, request_id, subject, payload]
```

`subject` is `[]` or a capability reference into the returned capability row.
That row contains every capability referenced by the batch, including existing
capabilities when necessary. Repeated references share one slot. Returning an
existing capability preserves its identity and does not create a new owner.

A request ID is an unsigned 64-bit correlation value chosen by the caller.
Zero is reserved for unsolicited events. An accepted asynchronous request ID
cannot be reused on that resource until its terminal event has been delivered.
A rejected request can be retried with the same ID. These IDs provide correlation,
not deduplication, durable identity, or authorization.

`more` means additional events are already queued and can be obtained by a drain
command. It does not mean that a pending request has completed. A successful
call means the command was admitted; its completion semantics come from its
terminal event.

For example, attaching an existing lower flow uses its position in this call:

```
MesaStepV2([process, lower],
          [2, 7, attach, [[cap, 1], attachment_configuration]])

-> [0, [2, [[attached, 7, [cap, 0], attachment_key]], 0], [process]]
```

The returned index 0 refers to the returned process capability. It has no
relationship to index 0 in another reply. The old lower capability is now moved.

All symbolic names in this document are readable notation for schema-defined
numeric tags. Bytes and UTF-8 text use the existing lossless bar convention:
append byte 1 and interpret the result as a little-endian nat. Rows are headed
by 0; the empty row is nat 0. Optional values are `[]` or `[value]`. Examples are
pseudocode, not executable PLAN syntax.

## 4. Commands and their meaning

### Process

| Command | Payload/dependencies | Result |
| --- | --- | --- |
| allocate | Destination name and requirements | A connecting Flow; later `ready` or `failed` |
| attach | A lower Flow capability and attachment configuration | The lower flow becomes owned by this process |
| detach | An attachment selector local to this process | Releases the attachment; affected upper flows receive state changes |
| configure | A versioned, validated configuration change | `configured` after application; admission and failure rules below apply |
| advance | A monotonic timestamp | Runs due protocol work and updates the requested timer |
| drain | Event and byte budgets | Returns queued events without waiting |

Allocate first emits `created` for the connecting Flow; `ready` or `failed`
completes that allocation request. A process also receives unsolicited
`incoming` events containing pending Flow capabilities. They arise from its
configured service bindings and admission policy. Accept has its own request ID
and completes with `ready` or `failed`.

Attachment selectors are ordinary local configuration keys. They are never a
substitute for possessing a Flow capability. The implementation rejects local
ownership cycles before accepting an attachment.

### Flow

| Command | Payload | Result |
| --- | --- | --- |
| accept | Requirements for an incoming flow | `ready` with negotiated properties, or `failed` |
| write | One message and optional application correlation data | `write-accepted` with a message ID, or admission rejection |
| read | A maximum message size | One `message`, or `would-block`; never waits |
| decide | A delivered message ID and accept/reject outcome | Begins or completes a durable application decision |
| drain | Event and byte budgets | Returns queued notifications without waiting |

A Flow has explicit state:

```
connecting -> ready -> closed
incoming   -> ready -> closed
connecting/incoming/ready -> failed
connecting/incoming/ready/failed -> closed by explicit close
```

Moved is a capability state, not a new network state: the resource continues
under its new owner. A pending incoming flow is rejected by closing it. A
connecting flow may be cancelled by closing it. Writes before ready are rejected.
The implementation may have internal substates, but inspect must use this public
lifecycle vocabulary.

Reading consumes one complete message. An insufficient size limit returns
`message-too-large` with the required size and leaves the message queued. Stream
adapters must preserve message boundaries through framing. They must not turn
one write into an arbitrary number of application reads.

`write-accepted` means the process owns the bytes needed to continue the send.
The caller can release its input immediately. It does not imply remote receipt,
remote application acceptance, or durable storage. A durable flow reports later
settlement using the message ID. The write request itself completes at
`write-accepted`; later settlement does not keep its request ID reserved.

### Store and Transaction

| Target | Command | Result |
| --- | --- | --- |
| Store | prepare | A Transaction holding a validated commit proposal |
| Transaction | commit | A durable commit receipt, or a classified failure |
| Transaction | rollback | Releases the transaction without acknowledging the process |
| Process | committed, borrowing a committed Transaction | Applies its verified receipt to the pending proposal |

The first Store profile permits one prepared transaction per connection. Other
operations needing that connection return `would-block` until it finishes. A
Transaction is terminal after successful commit or rollback. Repeating rollback
after rollback is harmless; rollback after commit is rejected. Repeated commit
returns the retained receipt when success is known. An uncertain outcome requires recovery and cannot be treated as a new
transaction.

Store creation distinguishes creating a new database from reopening an existing
scope. Inspection can return a snapshot or bounded snapshot pages. Reopening
recovers committed state and establishes the writer incarnation used to fence
old writers.

### Key and pure utilities

Secret-dependent operations such as sign and agree use a Key capability through
step. Key resources have an algorithm/use subtype. Agreement returns a Key
resource of the shared-secret subtype when the result is intended to remain
opaque. Secret export, if supported, is a separate, explicit privileged command.

Hashing, packet encoding/decoding, public-key verification, identity-field
inspection, and noun/bit manipulation belong in ordinary PLAN libraries, with
native acceleration where useful. They do not need constructor/getter/clone/
close sequences. Public verification is distinct from deciding that an identity
is authorized in a scope.

## 5. Names, requirements, and policy

An application destination is a structured name, initially:

```
[domain, application, optional_instance]
```

The process resolves that name within its configured scope. UDP addresses, TCP
addresses, attachment routes, and native key slots stay in process/adapter
configuration. They are not the application's destination namespace.

The initial implementation may resolve only preconfigured names. Unknown names
fail explicitly. Discovery, routing, and enrollment can be added behind the
same interface; their existence must not be implied by this API alone.

A requirements record distinguishes:

- Maximum message size.
- Required ordering.
- Delivery/reliability behavior.
- Durability and application acknowledgement behavior.
- Peer authentication and confidentiality requirements.

A ready event returns the properties actually provided. Unsupported requirements
are rejected. Required properties cannot be silently downgraded. In particular,
TCP delivery is not an application commit, and a signature is not encryption.

Policy is normal, versioned data: admission, retry, congestion, resource bounds,
identity authorization, persistence, and route selection. Initially it selects
built-in implementations with validated parameters. Arbitrary PLAN callbacks
from inside a Rust call are outside this profile. A future external policy
exchange must use ordinary requests/events, preserving the same scheduling and
ownership rules.

Configuration changes declare which fields are mutable. Changes to identity,
wire format, or guarantees of an existing flow require explicit reallocation
unless the schema defines a safe transition.

## 6. Recursion and the driver

A process provides upper flows and consumes lower flows. Both use the same
message and capability contract:

```
application
    uses Flow A
Mesa process A
    owns an attachment to Flow B
Mesa process B
    owns an attachment to Flow C
UDP/TCP/local adapter
```

UDP and framed TCP are bottom-level implementations of Flow. An upper process
must not branch on whether its lower flow is a UDP socket, a TCP connection, or
a flow provided by another Mesa process.

A process may implement stronger guarantees over weaker lower flows, for
example retransmission over datagrams. Its advertised properties must follow
from its mechanism, policy, and lower-flow constraints. It cannot advertise
properties it does not implement.

Attaching transfers the lower flow into the receiving process. Former aliases
observe `moved`. The parent retains the native resource independently of the
old wrapper. Detach closes that attachment; it does not revive old aliases.
Reusing a detached connection would require an explicit future hand-back
operation, not an accidental alias.

The driver belongs to the executor or an owning service actor. It services lower
flow input/output, transport readiness, persistence requests, and requested
timers. A process step runs bounded state-machine work and queues effects;
it does not recursively call PLAN or synchronously run an unbounded chain of
lower processes. The driver schedules each process fairly.

Protocol time arrives only through advance. The driver uses a documented clock
unit and epoch for a live run, initially monotonic microseconds. Time cannot move
backwards. A timer event requests the next wakeup; `[]` cancels it. Each new timer
request supersedes the previous request from that process. No native object
creates a hidden timer thread.

Process step and flow read/write never wait for a network peer. Storage commit
and other filesystem operations may block on the storage backend; they execute
using the host's blocking-work path, outside the scheduler's shared execution capacity and allocator locks.

The local strong-reference graph must remain acyclic. This restriction prevents
native ownership leaks; it does not require the network topology to be acyclic.

## 7. Events, bounds, and backpressure

The common event vocabulary includes:

| Event | Meaning |
| --- | --- |
| created | A resource was created |
| incoming | A remote request produced a pending Flow |
| ready | A flow is usable with the reported properties |
| write-accepted | Local ownership of a message was accepted |
| readable | A read can obtain a message without waiting |
| message | A complete message returned by read |
| writable | A write previously blocked by capacity may be retried |
| commit-required | A durable proposal must be persisted |
| settled | A message reached the reported durable application outcome |
| timer | The process requests or cancels a wakeup |
| info | An inspect result |
| configured, attached, detached | A control operation completed |
| failed | An accepted operation failed |
| closed | A resource has stopped providing service |

Each command schema states its immediate results, terminal events, and permitted
unsolicited events. Every queue has event-count and byte limits. Flow data and
control events have separate budgets so unread payloads cannot prevent failure,
close, or commit notifications from being represented.

The bridge must reserve capacity for the necessary result before accepting a
command that creates a resource or consumes a capability. Resource delivery and
publication of its containing event are one transition. A result-encoding error
must not silently discard an accepted commit proposal or orphan a new resource.

Batches are bounded by event count, encoded bytes, capability count, and work
budget. If more output is queued, the caller drains another batch. Large inspect
results use pagination or explicit chunking. Pending request counts, messages,
attachments, peers, and native bytes are independently bounded.

Within a resource, events have a defined order. There is no promised total order
between independent resources. Notifications such as readable/writable may be
coalesced; data, durable proposals, and terminal events may not be silently
coalesced or dropped. Control-event exhaustion stops admission until drained. Resource creation and
request admission reserve their terminal-notification capacity. Close remains
possible when ordinary queues are full; it releases queued payloads and uses
reserved terminal state/notifications to report closure or cancellation.

Expected capacity failures use `would-block` and leave the command unaccepted.
A read or write readiness notification is a hint; the subsequent operation still
checks state and capacity.

## 8. Ownership and lifetime

A wormhole is an unforgeable local capability. Its native resource is not named
by a persistable nat. An ordinary alias grants access to the same capability;
it does not create an independent resource or ownership token.

Every capability position in a schema declares one mode:

| Mode | Rule |
| --- | --- |
| Borrow | Valid only during the call; not retained afterward |
| Retain | The resource keeps a strong reference; the caller keeps its capability |
| Move | On acceptance, ownership transfers and all old aliases become moved |

Parsing, type checks, rights checks, bounds checks, and capacity reservation occur
before acceptance. Rejection leaves input resources and ownership unchanged.
The target API must not expose the current bridge's constructor behavior where
an ordinary rejection can unexpectedly consume inputs.

After acceptance, a command may fail asynchronously. Previously accepted moves
remain effective. Newly acquired resources must then be released or explicitly
returned according to the command schema. There is no general rollback promise
for arbitrary host effects.

Close is idempotent for a valid capability, including one already closed or
moved. Other operations on closed/moved capabilities return stable errors.
Inspect may report their terminal state without reviving them. Wrong-kind or
foreign capabilities are rejected.

Explicit close of a Process stops its service and makes its outstanding Flow
capabilities terminal. Explicit close of a Flow releases that flow. Close does
not promise graceful delivery of queued messages; graceful completion is an
explicit protocol operation or application policy.

Creating a Process retains its Key and optional Store dependencies. Retention
preserves lifetime, not immunity from explicit close: closing a retained Key
revokes later signing through it, and closing a retained Store prevents new
transactions. Dependent operations report the resulting loss of service.
Implementation caches must not bypass this rule.

A live Flow retains the native Process state it needs. Merely collecting the
Process wrapper must not close a Flow still reachable by the application.
Explicit process close and collection of one wrapper are therefore distinct.
A prepared Transaction similarly retains its connection after the Store wrapper
is collected or closed; it can still commit or roll back. This is an explicit
exception: authority to finish that transaction was already granted at prepare.

A Process owns its flow state, not strong references to its public Flow wrappers.
Queued events about its own flows hold local state references until publication,
not self-retaining capability cycles. Releasing the last owner of a Flow releases
its local resources; any peer notification is best-effort driver work and cannot
be required for native destruction to finish.

Wormhole wrappers live in nonmoving gc2. Foreign Rust allocations remain under
Rust's ownership and are released when their last native owner is released.
They contribute estimated memory pressure to gc2. Closed control blocks remain
valid for aliases and remain accounted for until collection.

Finalizers release native ownership exactly once, outside allocator locks.
They must not enter PLAN, resurrect resources, or require a running actor to
finish. They are fallback cleanup: application decisions, durable commit, and
graceful network shutdown must not depend on the collector's timing. Dropping an
unfinished Transaction rolls it back.

## 9. Durable messages

Durability is a sequence of explicit transitions:

```
process emits commit-required(proposal)
store prepares proposal -> transaction
transaction commits -> durable receipt, retained by the Transaction
process accepts committed(transaction, receipt)
process emits the resulting delivery/settlement/network events
```

A proposal is normal versioned data containing its scope, writer incarnation,
base revision, proposed next state, and application inbox/outbox changes. A
receipt identifies the successfully committed transition. It is only valid for
that scope, incarnation, revision, and proposal. Repeating a matching committed
callback is idempotent and emits no duplicate delivery or settlement. Conflicting
or wrong-incarnation callbacks are rejected. A committed callback borrows the
committed Transaction capability and validates
its native provenance against the process's configured store and pending
proposal. The normal receipt record alone is not proof of commit. The
Transaction releases its database transaction at commit but retains the native
receipt until closed or collected; no separate Receipt resource kind is needed.
The driver keeps it alive until the process has acknowledged the callback.

The reference store commits protocol state and its application inbox changes in
one transaction. Only successful durable commit permits the committed callback.
Rolling back does not acknowledge the process. Retry and abandonment of pending
proposals must follow the protocol's stated recovery behavior.

If a storage operation has an uncertain outcome, the driver must recover or
inspect committed state before retrying. It must not fabricate success or assume
that an I/O error proves no write occurred.

After restart, reopening recovers the last committed state and fences older
writers. Flow capabilities are newly created runtime values; persisted data
contains names, message IDs, revisions, and checkpoints, never wormholes.

The guarantee covers the store transaction and the protocol's defined message
semantics. It does not make arbitrary external application side effects exactly
once. Applications use the durable inbox/outbox boundary to coordinate those
effects.

## 10. Errors, inspection, and actor ownership

Errors use stable numeric codes with structured details. Initial categories are
invalid-request, wrong-kind, closed, moved, unauthorized, unsupported,
unsupported-requirements, would-block, cancelled, limit, stale-revision, fenced,
invalid-state, transport-failure, storage-failure, outcome-unknown, and
internal-failure. Human-readable text may accompany details, but Rust Debug
strings are not the machine interface.

Inspect returns ordinary snapshots of public state: kind, lifecycle state,
properties, usage, pending work, supported features, or checkpoint pages.
Inspect must not mutate protocol state, reveal native pointers, expose secrets,
or consume an event queue. Its snapshot consistency is defined per resource.

Initially, one service actor owns a process, its flows, and its driver. Other
actors send ordinary commands to that actor. This matches today's actor-only
SendCaps boundary while keeping the new networking contract usable.

Cross-actor native capability transfer is a later, typed extension. Its default
is move, not unrestricted sharing. It must revoke the sender's aliases, preserve
resource lifetime, route notifications to the new owner, and serialize access
to shared process state. It must not convert a native handle into a public nat
lookup table. Merely placing a wormhole in a message does not authorize transfer.

Record/replay remains explicitly unsupported in the first profile. A future
implementation must record external inputs, time, policy decisions, and durable
outcomes, then reconstruct runtime capabilities from normal state. Serializing
pointer values or replaying live socket operations is not a replay design.

## 11. One source of truth

A machine-readable schema defines each resource kind, command, query, event,
error, record version, and field order. It also defines:

- Scalar/byte bounds and capability kinds, rights, and ownership modes.
- Lifecycle states in which a command is accepted.
- Rejection rules and allowed failure-after-acceptance behavior.
- Immediate and terminal events and their correlation rules.
- Backpressure, pagination, and effect/blocking classification.

Generate opcode registration, structural validation, dispatch stubs, PLAN
constructors/helpers, documentation tables, and compatibility coverage checks
from that schema. Protocol behavior and native ownership code remain explicit
implementations, reviewed and tested independently of the generated tables.

Pure PLAN helpers supply application-friendly allocate/accept/read/write/close
functions and record constructors. The four native forms remain stable as new
policies or adapter implementations are added.

## 12. Example application and recursive composition

Pseudocode omitting the common envelope:

```
key     = create(Key, [], secret_configuration)
store   = create(Store, [], storage_configuration)
process = create(Process, [key, store], process_configuration)

lower   = create(Flow, [], udp_adapter_configuration)
step(process, [lower], attach, attachment_configuration)  # moves lower

flow    = step(process, [], allocate, [destination, requirements])
drive_until_ready(flow)
step(flow, [], write, [bar("hello"), application_tag])

# At the receiver:
incoming = next_incoming(receiver_process)
step(incoming, [], accept, requirements)
message  = read_when_ready(incoming)
# A durable profile also services commit-required and application decide.

close(flow)
close(process)
close(store)
close(key)
```

The driver's role is to feed time and lower input, drain work, perform authorized
I/O, and complete the durable commit exchange. `drive_until_ready` and
`read_when_ready` are actor-level asynchronous helpers, not blocking syscalls.

To add a layer, replace the bottom adapter in the example with an allocated flow
from another process. The consuming process and application use the same attach,
allocate, read, write, and close contract. Only configuration and the driver graph
change.

## 13. Migration and completion criteria

Implement this in stages while keeping the current bridge available:

1. Define the schema, envelope, structured errors, and four new syscall entries.
   Append entries; preserve all existing opcode indices and legacy behavior.
1. Move value-like wrappers to PLAN records/helpers. Add compatibility wrappers
   where semantics are equivalent. Preserve low-level utilities in a separate
   library rather than forcing them into the flow protocol.
1. Introduce Process/Flow around existing preconfigured Mesa functionality.
   Unify Engine/Node/Host event handling and put concrete transport adapters
   behind the driver boundary. Keep current wire protocols working.
1. Implement named allocation/acceptance and recursive lower-flow composition.
   Add the control protocol and policy mechanisms actually needed to support
   those behaviors. They cannot be obtained by renaming today's wrappers.
1. Add the Store/Transaction profile, recovery tests, and optional native
   capability transfer. Remove legacy-only implementation paths after parity
   and a documented deprecation period; retain index compatibility as required.

The first complete profile must demonstrate:

- The same application exchanging messages over local, UDP, and framed TCP flows.
- A two-layer Mesa-over-Mesa exchange with no application changes.
- Explicit failure for unsupported properties and unauthorized allocation.
- Bounded operation under a slow reader, exhausted queues, and a stalled peer.
- Correct readiness, event ordering, cancellation, and timer replacement.
- Closed/moved aliases failing consistently under gc1/gc2 and yield stress.
- Rejection preserving ownership, and failure after acceptance cleaning up moves.
- A collected Process wrapper leaving retained flows alive; explicit process
  close terminating them; ownership-cycle rejection.
- Prepared transactions surviving parent wrapper loss and rolling back on drop.
- Recovery before/after durable commit, duplicate callbacks, fencing, uncertain
  outcomes, and crash boundaries, without premature acknowledgement.
- A legacy compatibility suite showing which old operations are preserved,
  replaced by pure helpers, or deliberately retained only as low-level utilities.

## 14. Decisions to settle before freezing the ABI

The structure above is the proposed contract. These details need separate,
reviewable decisions before claiming interoperability or stability:

- Exact numeric tags, canonical record encodings, and default resource budgets.
- Named-flow allocation/admission wire messages and their version negotiation.
- The supported initial requirements profiles and their precise failure and
  delivery guarantees.
- Canonical durable proposal encoding and conformance rules for additional Store backends.
- Whether chunked normal data is sufficient before introducing buffer capabilities.
- The actor transfer protocol and the scheduling ownership of transferred flows.

These decisions should fill in the schema and protocol profiles. They should not
introduce new public Engine/Node/Host/transport-specific syscall families.
