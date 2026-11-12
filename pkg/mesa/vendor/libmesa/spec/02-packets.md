# libmesa spec — 2: Packet semantics and protection

> **Skeleton, one level deeper than its siblings** (it's the newest and had
> no prior outline to inherit). Chapter 1 defines how bits are laid out; this
> chapter defines what a conforming peer *does* with a packet: the validation
> pipeline, the request/response semantics of the three packet types, the
> space (SDU-protection) regimes, fragment integrity, and the attestation
> fast-path. Processing that requires per-peer or per-flow *state* belongs to
> chapters 4 (flows) and 5 (identity); forwarding belongs to chapter 6.

The implemented protection subset is the explicit
[v2 signed-public profile](08-signed-public.md). Its fixed proof-first exchange
and signing context take precedence over the broader space/Init proposals below
for that profile. The remaining spaces and attestation paths remain proposals.

## 1. Roles per packet

Validate the packet under §2 before selecting its role. Dispatch depends on
packet type:

1. **Peek:** serve if the name's ship is us; otherwise forward under chapter
   6 if we are a relay, or drop.
2. **Poke:** receive if `ack.her` is us; otherwise forward under chapter 6
   if we are a relay, or drop. The payload name identifies the publisher.
3. **Page:** consume for a matching local interest, or forward through
   matching PIT state when acting as a relay. These may both apply to an
   aggregated interest. The name identifies the publisher, so a remote
   publisher's name is expected. Apply the attestation exception in §7;
   otherwise drop pages without an interest.

Drops are silent on the wire and SHOULD be observable through local counters.

## 2. The validation pipeline (normative order)

Order matters: each stage bounds the work a hostile input can cause before
the next stage runs. A conforming implementation MUST NOT reorder crypto
before shape checks or allocation before bounds checks.

1. **Length gate.** Datagram within N-1 max-SDU; ≥ 8 bytes.
2. **Cookie + version.** `tok = 0x67e0.0200`, `ver = 2`, reserved bits zero.
   Else: not a mesa packet — drop (a dual-stack host MAY hand it to another
   protocol; libmesa itself never does).
3. **Header mug.** Low 20 bits of Urbit mug over the exact transmitted body
   bytes (ch. 1); mismatch ⇒ drop before any body
   parse. V2 has no alternate checksum convention.
4. **Bounded structural sift.** Decode per chapter 1 with every
   length/count field bounds-checked against the remaining buffer *before*
   allocation. Includes the `+mat` bounded-scan rule and jam-shape
   validation before any cue in later noun-processing layers.
5. **Type dispatch.** `%peek` / `%page` / `%poke` per §4–§6; reserved type ⇒
   drop.
6. **Admission.** Hard/soft peer policy for authenticated sender paths
   (identity ch. 5 §6); preserve the attestation recovery path through soft
   blocks. Anonymous peeks remain subject to namespace and resource policy.
   Apply rate limits (§8).
7. **Authentication/decryption** as required by the packet's role and space
   (§5) — always last, and only for packets that survived 1–6.

## 3. Names as requests: the interest model

A `name` is simultaneously an address (`her`, `rif`), a data locator
(`pat`), and a fragment coordinate (`boq`, `wan`). Semantics:

- **Rift gate.** A packet naming a rift other than the current known rift of
  `her` is dropped for ordinary traffic (stale epoch). Recognized proofs must
  reach the bounded identity-verification path (§7) to learn authorized changes;
  an unverified rift in a packet never updates accepted peer state. Unknown
  ships follow chapter 5 admission rules.
- **Interest.** A `%peek` registers interest in a name; interest is
  soft-state with a bounded lifetime (the PIT bound, ch. 4 timer algebra).
  Matching of responses to interests is **byte-equality of the serialized
  name** — normative, because relays match the same way (ch. 6 §3). This is
  why deterministic path sealing is load-bearing (see §5 and doc
  04-crypto.md gap 7 / FS2 epoch salting).
- **`wan = ~` (init)** requests/carries the whole message when it fits one
  fragment; `[%auth fag]` and `[%data fag]` address proof and data leaves of
  a fragmented message (§6).

## 4. Packet types

### 4.1 `%peek` — request

Carries a name only. **Unauthenticated by design** (known limitation: DoS/
amplification primitive — doc 04-crypto.md gap D; mitigations are §8 rate
limits and the response-size policy below). A conforming server:

- MUST answer only names it can bind under the namespace rules (ch. 3),
  applying the space's authorization (§5) — never leaking existence
  differences observably beyond what `%publ` already publishes (a `%chum`
  request that fails authorization and one for an unbound path are
  indistinguishable: both are silence).
- MUST answer byte-identically for the same name while the binding and
  epoch are unchanged (retransmit-safety, relay caching, LSS coherence).
- SHOULD bound amplification: a single `%peek` datagram elicits at most one
  `%page` datagram (one leaf); bulk transfer is receiver-driven, one
  interest per leaf (or per window, ch. 4 policy).

### 4.2 `%page` — response

Name + data + next-hop trailer (relay-written; endpoints MUST ignore its
content for authentication purposes — it is outside all auth envelopes).
A receiver:

- MUST hold a matching live interest (byte-equal name), else drop. Unsolicited
  pages are drops, with one exception: the attestation fast-path (§7).
- MUST verify per §5/§6 **before** the page affects flow or application
  state, and before any fragment is stored against reassembly (no
  unverified-buffer poisoning).

### 4.3 `%poke` — request-with-payload

Two names (`ack`, `pok`) + data. Semantically: "here is (a fragment of) the
message bound at `pok`; I hold an interest in `ack`". Rules:

- `her.ack` MUST be the receiver and `her.pok` the sender; mismatch ⇒ drop.
- The data carried MUST be exactly the binding at `pok` (receiver verifies
  as if it had peeked `pok`; a multi-fragment poke's remainder is pulled by
  ordinary peeks against `pok` — receiver-driven, ch. 4).
- Processing the payload registers the receiver's obligation to bind an ack
  at its own `/flow` region (ch. 4); the *sender* then peeks `ack`. An ack
  is a binding, never a distinct packet type.
- Single-fragment pokes are self-contained; the unverified-sender DoS bound
  (attestation pokes from unknown peers MUST be single-fragment) is in §7.

## 5. Spaces (SDU protection)

A space is the protection regime under which a binding is served on the
wire. The space is recoverable from the on-wire path prefix (ch. 1 name
encoding; sealed elements are opaque). Normative table:

| space | path on wire | payload | auth | keys |
|---|---|---|---|---|
| `%publ life` | `/publ/<life>/<inner…>` plaintext | plaintext | ed25519 sig | publisher's signing key |
| `%chum slife her clife` | `/chum/<slife>/<her>/<clife>/<sealed>` | XChaCha8 | keyed-BLAKE3 MAC (16 B) | pairwise key (§5.2) |
| `%shut kid` | `/shut/<kid>/<sealed>` | XChaCha8 | ed25519 sig | group key `kid` |

- **What is authenticated** (all spaces): the pair *(full beam path,
  LSS root)* — §6 — binding content to its namespace location. For `%chum`
  the MAC is computed over the root of the **ciphertext** (encrypt-then-MAC);
  a conforming receiver MUST verify before decrypting.
- **Sealing** (`%chum`/`%shut`): AEAD over the inner path — 16-byte keyed-
  BLAKE3 tag ‖ XChaCha8 ciphertext ‖ terminator. Deterministic (synthetic
  IV) so interests match; the determinism-vs-fingerprinting tension and the
  epoch-salt remedy are doc 04-crypto.md gap 7 / 06-forward-secrecy.md FS2.
- **Nonce invariant (normative).** The XChaCha IV is derived from the sealed
  path: a conforming publisher MUST NEVER serve two distinct plaintexts for
  the same (key, sealed-path) pair. Immutable bindings satisfy this by
  construction; any mutable-binding extension MUST switch to an explicit
  nonce first.
- **Defaults (E2E-by-default, Tier 1):** peer-to-peer reads and all flow
  traffic default to `%chum`; `%publ` is an explicit opt-*out* for genuinely
  public data; `%shut` requires explicit group configuration. *(This is the
  hardened default — stricter than current kernel behavior, tracked as an
  upstream default-flip; doc 04-crypto.md A.)*
- **Constant-time.** All MAC/tag comparisons MUST be constant-time.

### 5.2 Pairwise key schedule

Baseline: X25519 ECDH between the peers' `cry` keys at the lives named in
the path prefix, expanded via blake3-KDF with the domain-separation labels
of ch. 1. Reserved forward: the FS2 epoch-prekey schedule (mix epoch prekeys
from `/epok` bindings into the KDF when both peers publish them; permit
baseline only under an explicit compatibility policy) — labels and the
`/epok` region are reserved by this
spec even before FS2 is normative (06-forward-secrecy.md).

### 5.3 Authorization to request

`%publ`: anyone. `%chum`: any peer able to complete ECDH — i.e. effectively
anyone with an identity; real authorization is applied at the inner read,
which the server MUST perform *as the requesting ship* (never as local
omniscient — the `peek-shut` gang bug class, doc 04-crypto.md D.9, is a
MUST-NOT here). `%shut`: holders of `kid`; inner reads likewise MUST be
performed as-requester, not as-local.

## 6. Fragmentation and integrity (LSS)

- A byte-aligned data fragment at bloq `b >= 3` holds `2^(b-3)` bytes.
  A nonempty message needs `ceil(tob / 2^(b-3))` data fragments. Bloq 13
  gives 1024 bytes. BLAKE3 base leaves remain 1024 bytes; jumbo fragments
  group subtrees. Single-leaf messages carry their authenticator directly.
  Reconstruct omitted terminal zeros to the logical fragment length before
  hashing; proof bytes follow separate rules (ch. 1).
- Multi-leaf messages are authenticated once at the **root** of a blake3
  lockstep-streaming Merkle tree over the leaves; fragment 0 (`%auth`)
  carries the proof spine; `%data` leaves carry pair hashes sufficient for
  incremental verification in order.
- A receiver MUST verify each leaf against the root chain as it arrives
  (streaming, O(log n) verifier hashes) and MUST NOT deliver a partially
  verified message. Out-of-order leaves MAY be buffered unverified but MUST
  be bounded (§8) and MUST NOT be delivered before in-order verification.
- The authenticated object is *(beam, root)*: integrity is end-to-end and
  transport-independent — stream N-1s (ch. 7) do not relax it.
- `boq` is honored as-requested: a server SHOULD serve any bloq it can
  re-delimit for (this is what makes bloq a per-flow policy with no
  negotiation step — ch. 7 §2).

## 7. Attestation fast-path

Attestation-shaped pages and pokes enter identity processing even when the
sender is known or soft-blocked. Anonymous peeks remain namespace requests;
they do not identify their requester and cannot confer peer admission.

- Recognize proof shape independently of peer status. A hard block still
  applies; a soft block preserves self-attestations and proof requests.
- Require a single-fragment proof in the initial compatibility profile. Bound
  decoding before allocation, verify the presented live-key signature and
  immutable `ugn/dat` name binding, then dispatch to the committed domain.
  Bound pending work per peer and globally; a signature alone is not a verdict.
- Serve the established proof path under fixed `%publ 1` publisher-discovery
  coordinates. Use the sender's actual life/pass inside the signed proof and
  require its exact recipient life. These fixed coordinates are not a general
  exemption from namespace life checks.
- Solicit fresh proof after a domain withdrawal (`%poof` semantics). The pinned
  kernel retries every 30 seconds while soft-blocked. Tick/decrypt failures are
  unauthenticated hints; automatic recovery based on them is not adopted.
- libmesa stays Mesa-only. The kernel's proactive legacy open-packet
  announcements require an upstream Mesa replacement and loss/concurrent-rekey
  tests before full interoperability. No legacy recipient-life sentinel is
  accepted by the Mesa proof path.

The planned attestation-confidentiality upgrade remains a separate wire-visible
proposal. It must preserve these identity checks; it is not implemented by the
current codec.

## 8. Resource bounds (normative minimums)

Implementations MUST bound, with configurable limits and drop-on-exceed:
per-source unverified-traffic rate; interest-table size and lifetime;
unverified reassembly buffers; in-flight verifications (per-ship
single-flight); response amplification (§4.1). Recommended defaults land
with the vector suite.

## 9. Vectors

`../vectors/pact/` (adversarial: truncations, bad mug, non-canonical
lengths, reserved types, oversize paths), `../vectors/space/` (seal/open,
KDF labels, MAC-before-decrypt), `../vectors/lss/` (proof spines, leaf
corruption at each position), attestation accept/reject (shared with
identity vectors).
