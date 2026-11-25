# Implemented v2 signed-public profile 1

This profile authenticates immutable public named data using preconfigured
Ed25519 keys and BLAKE3 LSS. It is explicitly selected by constructing
`Engine<SignedPublic>`. It provides integrity and publisher authentication,
without confidentiality, authority discovery, request authentication, or
algorithm negotiation. Only `Access::Public` binds are accepted. Private binds
fail with `UnsupportedAccess`; there is no automatic downgrade to plaintext.

This profile supplies the step-3 protection contract in the
[peer-engine handoff](../doc/11-peer-engine-handoff.md). It narrows the broader
space proposals in chapter 2. It does not implement `%chum`, `%shut`, the old
Hoon v1 signature transcript, or native Ames interoperability. The separate
[Urbit prototype](../interop/README.md) implements this profile in a Hoon Gall
agent inside a running fake ship. The outer packet format
remains experimental v2 with its existing explicit version and checksum.

## Configuration and exchange

The host installs a local signing key and an explicit remote key table. Handles
are local configuration, not wire identifiers. Each remote view binds one
`PeerId`, canonical `DomainTag`, current positive life, rift, and key handle.
The core rejects missing handles, domain changes, and regressing coordinates.
Key ownership and authority provisioning remain host responsibilities.

The supported fragment size is bloq 13: 1024 bytes. Each object has at most
1024 leaves and 1 MiB of logical data. Configurations requesting larger objects,
more leaves, or another bloq fail for this profile. Smaller host limits apply
in addition to these protocol bounds.

1. The reader peeks the exact object name at `Auth(0)`.
2. The publisher returns a page with that name, the logical object length,
   a 64-byte Ed25519 signature, and the exact initial LSS spine as fragment data.
3. The reader checks the bounded spine shape, reconstructs its root, and verifies
   the signature under the current configured key. Only then does it reserve
   message storage and schedule its bounded window of `Data(i)` interests.
4. Data pages carry `Auth::Pair` when the LSS schedule requires a pair, and
   `Auth::None` otherwise. Every data page repeats the authenticated total length.
   Data reaches the verified reassembly buffer only after successful LSS ingestion.
5. The host receives exactly one completed value after every leaf verifies.

This exchange uses a separate proof page even for one-leaf and empty values.
`Init`, proof fragmentation, jumbo leaves, pokes, MAC auth, and next-hop trailers
are unsupported in this profile. A successful peek elicits one page. Replies
for an unchanged binding are byte-identical.

Public peeks identify no requester. The initial closed deployment still requires
a configured peer/route and an enabled service gate for dispatch, but these are
traffic policy, not authentication. Claiming another source or peer cannot
grant access to a private binding. Data authentication uses the named publisher's
configured key and the protected context below.

## Protected context

Ed25519 signs the following concatenation without prehashing, atom normalization,
or implicit terminators. All integer fields use fixed-width little-endian bytes.
The initial ASCII label includes exactly one final zero byte.

| Field | Encoding |
|---|---|
| Profile label | ASCII `libmesa/v2/public-lss/1` followed by `00` |
| Domain length | `u8`, 1–63 |
| Domain | Canonical ASCII bytes |
| Publisher | `u128`, exactly 16 bytes |
| Rift | `u32` |
| Signing life | `u32`, from the current configured peer view |
| Bloq | `u8`, exactly 13 |
| Path length | `u16` |
| Path | Exact opaque bytes, bounded by configured `max_path` |
| Total logical bytes | `u64` |
| LSS root | Exactly 32 bytes |

The domain, publisher, rift, bloq, and path must also match a live interest.
Fragment coordinates are excluded from this object-level transcript; the root
and LSS verifier bind each leaf's index and content. Hop count, route, source
address, host key handle, and local read ID are excluded. The object is public
and recipient-independent. There is no freshness nonce: replay of the same
immutable named object within its rift is intentional.

The proof signature is verified once when the read establishes its root.
A same-rift key change preserves an already authenticated root and partial read;
subsequent leaves must still match that root. New reads require the newly
configured signing life/key. A host requiring withdrawal of in-flight objects
must block the peer or advance its rift; both terminate those reads. This is
object authentication, not per-datagram authentication or proof of a sender's
current key possession on every packet.

## LSS construction

For nonempty values, construction follows the pinned Vere LSS source. A leaf
hash is its BLAKE3 chunk chaining value at the leaf's actual chunk counter,
without the ROOT flag. Parent hashes use unkeyed BLAKE3 parent compression,
also without ROOT. The resulting LSS root is a chaining value, not the ordinary
`blake3::hash(message)` digest. Rust uses the upstream
[BLAKE3 subtree API](https://docs.rs/blake3/1.8.7/blake3/hazmat/index.html).

For `n` leaves, split each internal node after the largest power of two strictly
less than its leaf count. The initial spine contains leaf 0's hash followed by
the right siblings along that leaf's path, from bottom to top. Its exact length
is `32 * (1 + ceil(log2(n)))` bytes: at most 352 bytes. Reconstruct the root by
folding parent compression from left to right through that spine.

Data leaf `i` carries a pair precisely when
`i != 0 && i + 2^(1 + trailing_zeros(i)) < n`.
For `w = 2^trailing_zeros(i)`, the pair reveals the children of the known future
subtree starting at `i + w`, split at `i + 2w`. The verifier checks both the
current leaf and that pair's parent against its known frontier before advancing.
Failed ingestion leaves verifier state unchanged. The frontier contains
O(log n) hashes; the implementation supports in-order verification.

Out-of-order leaves may enter a distinct bounded unverified buffer. They never
enter verified payload storage or reach the host until preceding leaves establish
their expected hashes. A failed buffered leaf is discarded and remains an
outstanding interest, so a subsequent retry can repair it. Missing or unexpected
hash pairs are rejected. Omitted terminal zeros are reconstructed to the logical
leaf length before hashing; proof spines are exact bytes and are never zero-padded.

The v2 empty-value extension uses one logical empty leaf and a one-hash spine
containing the ordinary BLAKE3 hash of the empty string. The signed total length
is zero. This rule is explicitly separate from Vere's nonempty LSS construction;
no upstream empty-message compatibility is claimed.

## Bounds and failure behavior

The existing peer, path, binding, interest, read, retry, and deadline limits
remain active. The aggregate verified-payload reservation defaults to 8 MiB.
The separate unverified payload budget defaults to 128 KiB across all reads,
with at most one buffered copy per outstanding data interest. Each buffered
fragment has at most 64 additional pair bytes and fixed metadata. Excess
unverified data drops without terminating the request or spending an interest.

Malformed or unauthenticated proof claims do not reserve message storage or
terminate reads. An authenticated proof that exceeds the host's read or aggregate
budget terminates with `ResourceLimit`. Cancellation, blocking, breach, timeout,
and retry exhaustion release the verified reservation, unverified buffer, LSS
verifier, and interests. An incomplete read never returns a partial payload.

Binding construction precomputes its spine, pairs, and root signature. Proof
storage is bounded by namespace slots and the 1024-leaf limit; `Usage` reports
it separately from bound payload bytes. Read verifier hash metadata is bounded
by `max_reads` and the tree depth. Usage counts exclude allocator overhead.
Unbind frees payload, tree, and signature while retaining the existing bounded
retired-name slot.

## UDP host and acceptance evidence

`mesa-udp` owns sockets and accepts only `Engine<SignedPublic>`. It installs
explicit routes, bounds receive buffers to `max_packet + 1`, rejects oversized
datagrams and unknown sources, and processes at most 64 datagrams per poll.
The extra byte detects truncation beyond the accepted packet size. Expiry runs
before receive at the supplied tick. A nonblocking send that would block is
counted as loss; the core supplies recovery. There is no unbounded output queue.
Host polling frequency determines CPU admission; this adapter does not yet
provide an Internet-facing per-source rate or congestion policy.

`python3 tools/test-protected-udp.py` runs two independent loopback processes
with public deterministic test seeds. It checks exact bytes and SHA-256 receipts
in both directions for lengths 0, 1, 1024, 1025, 17425, and 1048576. These are
libmesa-to-libmesa tests. The separate virtual-link tests inject loss, duplication,
delay, reordering, corrupt signatures/spines/pairs/data, context changes, and
resource pressure. Network confidentiality and native Ames interoperability remain
unimplemented. The separate Hoon endpoint now exercises this profile across
the Rust/Urbit boundary; its narrower fixture bounds are documented in the
prototype README. The separate [durable flow profile](10-durable-flows.md) uses
these reads for message and receipt objects with explicit host transactions.
Its [transport profile](11-flow-transports.md) adds a bounded two-leg relay and
preserves endpoint recovery across a lossy downstream attachment.
