# libmesa native syscalls

See [the proposed target interface](TARGET_SPEC.md) for the planned process/flow
API and migration from this bridge. That specification is not implemented yet.

This package exposes libmesa through 199 opset-83 syscalls. Native values are
resource-owning wormholes, with Rust type checks at each operation. The vendored
source is pinned to [gwbtc/libmesa at 2e94a135](https://github.com/gwbtc/libmesa/tree/2e94a135961f99e3522d31ddae08a6a8a67c055b);
see [provenance](vendor/libmesa/UPSTREAM). No upstream source was modified.

The binding covers noun/bit operations, v1/v2 packets, suite-c identity,
cryptography and LSS, the signed-public transfer engine, durable flow nodes,
SQLite transactions/snapshots, UDP/TCP lanes, hosts and relays. Small native
records and enums travel as normal PLAN data; stateful or opaque native values
travel as wormholes. Rust generic traits are instantiated with the shipped
`SignedPublic`, UDP and TCP implementations. Arbitrary PLAN callbacks for
`Protection`, `PacketIo` or Rust `Read`/`Write` are not provided.

## Calling convention

Every syscall takes `(handles, data)` and returns `[status, data, handles]`.
Use the ordinary RPLAN opset-83 call mechanism with a `Mesa...` name. The handle
argument is a shallow row of wormholes; the data argument is forced to normal
form. Replies containing wormholes must be consumed through host evaluation,
not forced to normal form or persisted. [API.md](API.md) lists every operation,
field order and ownership transfer.

For example, in language-neutral notation (not a PLAN source program):

```
[0, 0, [key]]    = MesaKeyFromSeed([], [bar(seed32)])
[0, 0, [public]] = MesaKeyPublic([key], [])
[0, sig, []]     = MesaSign([key], [bar(message)])
[0, 0, []]       = MesaVerify([public], [bar(message), sig])
[0, 0, []]       = MesaClose([key], [])
```

No host address or numeric object-ID map is exposed. The C host entry is
`pl_mesa_call` in `plan/mesa.h`; embedding callers must retain roots and obey the
store's mutator-scope contract. The PLAN path also enforces RPLAN mode and marks
these calls as host effects, including worker-thread blocking coordination.

## Lifetime and collection

The wormhole and its inline opaque pointer live in nonmoving gc2. Rust owns the
pointed-to object and its internal buffers; these are reclaimed by a native
finalizer attached to the gc2 allocation. This preserves upstream allocator and
Drop contracts. It does not relocate libmesa's Vecs or SQLite allocations into
gc2. Ordinary byte-buffer wormholes still keep their backing bytes inline in
gc2 as before.

Aliases refer to the same handle. `MesaClose` drops the native object immediately;
later uses of any alias return an error. Consuming constructors move native
objects and leave their old handles closed, even if the native constructor then
fails. Wrapper memory is collected later. Parsed bit/noun/identity/packet views
own their backing storage. Prepared transactions retain their parent connection.

Unreachable resources are queued by sweeping or rollback, and destroyed outside
the allocator mutex at a host GC poll, explicit finalizer drain, GC wait, or store
teardown. Finalizers are exactly once and may run on different host threads.
They must never reenter PLAN, publish roots or resurrect objects. Native destructors
close sockets, roll back transactions and run the upstream secret-zeroizing Drop
implementations. Use explicit close/commit/rollback when timing matters.

Each handle has a mutex; calls acquire multiple handles in address order.
Duplicate occurrences of one handle in a single call are rejected. Foreign
memory estimates contribute to gc2 collection pressure and `external_bytes`.
These estimates include payloads/proofs/checkpoints/action queues, but are not a
hard quota or exact accounting of SQLite, crypto and transport internals.

## Boundaries

- Normalization, hashing and persistence reject reachable wormholes. Only explicit
  exports such as packet bytes and checkpoint JSON produce persistable data.
- Native resources are unavailable during actor record/replay: native state and
  external effects have no replay encoding. Calls return a normal error.
- Existing actor `SendCaps` transfers actor capabilities only. Keep a native Mesa
  owner in a service actor and send it normal commands/results; generic native
  capability transfer between actors is not added here.
- Input/reply encoding is limited to 16 MiB, nesting depth 64, one million input
  values and 64 input handles. Engine/flow constructors add bounded resource
  limits on top of libmesa validation. Large snapshots can remain native and be
  inspected incrementally; a whole JSON export can exceed the reply limit.
- Native errors are returned as diagnostic bars. Panics are caught at the Rust
  FFI boundary and affected input handles become poisoned; fatal allocation
  failure is not recoverable. Calls are not transactions unless explicitly
  using the storage transaction API.
- File operations use the existing RPLAN file-root policy, including canonical
  read/write paths. SQLite may create journal/WAL files next to the database.
- Suite-c binding verifies cryptographic consistency, not external authority.
  The engine uses upstream signed-public policy. This binding adds no private
  payload encryption or authority service. Exporting ring seeds/shared secrets
  explicitly creates ordinary PLAN bytes, whose lifetime follows PLAN.

## Build and validation

The default Make build links the bridge. Cargo and a compatible Rust toolchain
are required; Cargo.lock pins dependencies and `vendor/libmesa` pins native code.

```
make BUILD_TYPE=debug
make BUILD_TYPE=debug test
make test-mesa
```

`MESA_LIB=/absolute/path/libenki_mesa.a` uses a prebuilt bridge. Nix builds the
locked Rust dependency separately and supplies it to each C configuration.
`make MESA=0 BUILD_DIR=build/debug-no-mesa BUILD_TYPE=debug` builds C-only tooling;
the opcode names remain registered and return an unavailable error. Use separate
build directories when changing this compile-time option. Installation includes
`libenki_mesa.a` alongside the C archives; static embedders must link that archive
and the platform libraries listed in the Makefile.

The C tests exercise ordinary and known-op dispatch, closed aliases, both GCs,
normal-form rejection, every registered opcode, and a signed fragmented
5,000-byte two-peer exchange through PLAN. Rust tests cover codecs, crypto/LSS,
invalid inputs and FFI alias rejection, protocol transfer, durable storage
recovery/fencing/transaction ownership, UDP/TCP and relay behavior. The networking
tests require loopback socket access. Resource tests also verify finalization on
collection, rollback and store destruction.

## Maintenance

`include/ops.def` is append-only because opcode indices appear in compiled
bytecode. `src/operations.rs` routes names to modules; both registries and this
reference are checked by tests. Keep the internal FFI representation opaque;
changes to native Rust layouts do not change the PLAN representation.
