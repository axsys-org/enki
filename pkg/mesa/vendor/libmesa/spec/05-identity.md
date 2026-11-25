# libmesa spec — 5: Identity

**Status: domain-first design contract, 2026-10-22.** Rust implements bounded
noun/suite-C parsing and suite-C name derivation, signing, and agreement.
Identity authorization and lifecycle remain unimplemented. The C ABI exposes
wire validation only. This chapter replaces the earlier
Azimuth convention and immutable-signer sketch. It follows the composed
key-rotation and snob kernel stack pinned in
[the baseline](../doc/00-kernel-baseline.md). The selected compatibility and bootstrap policies are explicit in §4;
remaining release gates are listed in §10.

## 1. Authority and names

Native libmesa names are fixed 128-bit values. Azimuth short names may later
alias these underlying identities. Width expresses no rank or authority.
Identity equality is the globally unique `name128`; the explicit domain selector
identifies its claimed authority and is part of the full wire context. Initial
association must be authenticated; a conflicting selector cannot overwrite an
accepted association or create a second peer for the same name. Domain migration
awaits an explicit rule. The v2 codec represents these claims but does not yet
authorize them. See [fixed names and domains](../doc/08-names-and-domains.md).

A registered domain authorizes the binding between a name and its current
key/point. The domain may validate evidence carried by the peer or query its
own identity ledger directly. The peer proves possession of the authorized
key. The core does not require every domain to use a key tweak, `dat`, or
`xtr`; those are the current suite-C profile's mechanism. Receiving a pass,
verifying a signature under an unapproved key, or learning a route cannot
substitute for authorization.

Only the registered authority for the selected domain may authorize or
withdraw its points. The selector is untrusted input until its relationship
to the name and key is authenticated. A remote packet cannot register a
verifier or inject a trusted verdict. A query-backed domain needs a defined
trust root and current chain view just as an evidence-backed domain does.
Direct observation and re-attestation enter the same accepted-state boundary;
they do not create two competing sources of authority.

Groundwire's `%gw-btc` suite-C implementation remains the first concrete
profile. The following sections document that profile's formats and reviewed
Jael/Ames lifecycle mechanics. Their `ugn/dat/cry/xtr` checks apply to suite C;
other domains implement equivalent name/key authority and continuity guarantees
without being forced into that representation. Opt-in life-1 fallback is
specific to suite C, not a generic bypass for an unknown domain.

## 2. Current suite-C profile: identity and live keys

`mesa_identity::suite_c` implements bounded public-pass and explicitly selected
current-ring framing. It validates canonical atom bytes and the committed domain
against an expected selector, then returns immutable unverified claims.
`mesa_identity::suite_c::crypto::BoundPass` separately checks curve points,
derives the name, and checks exact ring/pass correspondence including `xtr`.
Evidence semantics, key activation, and authority remain separate requirements.
A bound pass or matching ring must never be treated as an authorized key.

The implemented Edwards profile accepts canonical, nonidentity prime-subgroup
public keys and uses strict Ed25519 verification. It deliberately rejects weak
or noncanonical inputs rather than mirroring a permissive historical verifier.
The [executed Hoon vectors](../vectors/crypto/README.md) establish valid-input
parity; they do not establish identical acceptance of all hostile inputs.
Signing consumes exact bytes. Callers must construct the protected message;
atom messages use their minimal byte representation, while octet messages
retain their declared length. Agreement returns raw secret material, with no
channel KDF or packet encryption. These primitives define no v2 handshake.

The reviewed kernel encodes the following fields, in low-to-high atom order:

```text
public pass:  'c' | ugn[32 bytes] | cry[32 bytes] | mat(dat) | xtr
private ring: 'C' | kes[32 bytes] | ugn[32 bytes] | mat(dat) | xtr
```

`ugn` is the public genesis key. `dat` is immutable domain-committed data.
Together they determine the permanent name through `+nam` and `+fig` in
`zuse.hoon`. The precise Hoon operations are:

```hoon
nam = (scap:ed ugn (shax (can 3 [32 ugn] [(met 3 dat) dat] ~)))
fig = (shaf %cfig nam)
```

These expressions pin the construction; byte-order, scalar/tweak, and hash
vectors must accompany its implementation. This is an Edwards-key name
construction. Bitcoin's secp256k1 custody signatures are a separate domain
concern.

`kes` is the current life's private seed. It derives `cry`, which supplies
both the live signing key (`sgn == cry`) and the agreement key in the kernel's
Edwards/Montgomery forms. At life 1, `cry == ugn`; after rotation, `ugn` remains
public name material and `cry` changes. The tweaked name key signs nothing in
this layout. Structured signature messages and their domain separation must
match the kernel; the core must not expose a bare peer-chosen-atom signing API.

`xtr` carries mutable evidence and is excluded from the name. Neither `cry`
nor `xtr` determines the name. Old signatures remain valid under their old
public keys; rejecting them as current requires an accepted current point.
Signature verification is not a currency test.

The `'C'` tag was retained when the ring layout changed from two secret seeds
to a live seed plus a public genesis key. Old suite-C rings must not be silently
loaded under this layout. Layout migration requires a separate rule; tag
recognition alone cannot establish compatibility.

## 3. Domain data and accepted points

The suite-C profile parser reads the bounded leading `mat(domain)` from `dat`. It must
reject malformed lengths and non-term tags before creating peer state or
requesting verification. Everything after that field, and the meaning of
`xtr`, belongs to the domain. Kelvin 9 and a spawn satpoint are `%gw-btc`
fields, not universal suite-C fields.

For `%gw-btc`, the current format is:

```text
dat = mat(%gw-btc) ++ mat(9) ++ mat(jam(spawn-satpoint))
xtr = jam(custody-log)
```

The domain parser must re-encode and compare `dat` to reject noncanonical
representations and trailing data. A foreign domain version is unevaluable;
it is not evidence against the claimed identity. All noun decoding, including
the domain's `xtr` decoder, must bound input size, depth, and backreferences
before allocating from peer-supplied lengths.

`mesa_noun::serial::cue` now validates canonical jam into a bounded arena of
borrowed atoms and shared cells. It limits literal nodes, backreferences, input,
output, atom size, and expanded depth, and compares canonical re-encoding.
It does not recursively expand references. Domain consumers must also bound
their traversal work; a shared graph can denote an exponentially larger tree.
The suite-C framing parser leaves `domain_tail` and `evidence` opaque, because
their noun shape and semantics belong to the selected domain.

An accepted point supplies life, rift, the public-key history and current
pass, sponsor, and fief. The core rechecks every positive verdict:

- the current life is nonzero and has a key entry;
- suite, pass, immutable name, and responding domain agree;
- neither life nor rift decreases relative to an accepted point;
- at the same life, the live key remains unchanged;
- sponsor and fief satisfy the supported routing profile.

A same-life answer may refresh evidence and route metadata while retaining
the live key. A higher life permits a new live key without changing the name.
A higher rift breaches the remote peer's continuity before the replacement
point is installed. The accepted fief must reach the routing engine along with
the keys; merely caching it in the identity table is insufficient.

The kernel currently restricts an explicit comet sponsor to a star or comet
and uses a star fallback for a null sponsor. Those are kernel compatibility
constraints. The standalone routing model uses explicit verified sponsors and
fiefs; it must not reconstruct authority from numeric rank. Exact interoperability
constraints remain visible at the adapter boundary.

## 4. Attestation intake

An attestation is recognized by packet shape even when its sender is already
known. A proof arriving during promotion or after rekey must enter the same
verification path as first contact.

The Mesa intake checks bounded framing and noun structure, sender/name
agreement, recipient and exact recipient life, proof coordinates, pass
structure, the signature under the presented live key, and the immutable name
binding. Signature and name checks both precede a domain request. An attacker
can copy `ugn/dat` and sign with an unrelated `cry`; only the domain can decide
whether that candidate key is authorized.

The proof endpoint uses fixed outer `%publ 1` and `/pawn/proof/1/...`
publisher-discovery coordinates. The signed body carries the publisher's
actual life and pass. These fixed coordinates do not relax Mesa's exact inner
recipient-life check. Ordinary public reads remain life-versioned.

The current kernel requires the complete first-contact proof in one 1,024-byte
Mesa fragment. libmesa's initial compatibility profile must enforce that bound.
An unbounded custody log cannot be assumed to fit; proof transport or evidence
compaction is a separate release decision (§10).

Unknown peers remain unresolved while a verifier evaluates the candidate.
An established peer retains its accepted point while a replacement is checked.
The pinned kernel requests verification for an unknown peer, a higher-life
candidate, or a peer awaiting re-attestation under soft block. An ordinary
same/older-life resend otherwise retains the existing key subscription; it
does not independently authorize new evidence or metadata.

The implementation must bound pending peers, agendas, verification work,
retries, and total evidence. Single-flight verification may bound local work;
it must not be described as a kernel request-ID or causal-ordering protocol.

A registered suspended domain cannot acquire an unregistered-domain fallback.
Lookup failure cannot be interpreted as absence of a verifier.

The following optional admission modes are selected for libmesa. Both require
explicit host opt-in and remain distinguishable from domain verification.

### Admission status and bootstrap

| Status | Authority and permitted transition |
|---|---|
| Unresolved | No accepted authorization; keep bounded pending state. |
| Self-authenticated fallback | Explicitly unregistered domain, life 1, `ugn == cry`, valid signature and immutable name. Ignore `xtr` as authority. |
| Provisional | Host explicitly trusts a configured sponsor's authenticated introduction while domain verification catches up. Record that source and pending evidence. |
| Domain-verified | The committed registered verifier has authorized the point. |

Fallback is disabled by default. It cannot authorize rotation or prove external
currency. Registering a verifier later must trigger revalidation of fallback
peers before labeling them domain-verified; merely adding a registry entry
cannot confer that status. Suspension, lookup error, and a failed verdict must
never downgrade into fallback. Fallback alone does not auto-edit an allow list.

Provisional bootstrap is an explicit trust delegation, not a successful chain
check. The host chooses the trusted sponsor and enables receipt; a route's
sponsor field alone cannot authorize introductions. The domain adapter still
performs bounded decoding, immutable-name binding, and offline consistency
checks. Direct attestation traffic also proves possession of its presented
key. A sponsor-delivered pass is an introduction, not proof that its subject
is presently online.

The host API must expose the trust status and source so applications can decide
whether provisional peers may carry their traffic. The configuration must also
specify bounds, expiry, and revalidation on verifier readiness. A successful
check upgrades provisional state. Rejection or expiry withdraws provisional
permission and returns the peer to unresolved status without poisoning its
name or removing a separately accepted verified point. Hard blocks always
apply. Provisional trust must not automatically return when a verified peer's
vouch is withdrawn. Exact timeout values and persistence behavior remain part
of the embedding/storage design, rather than hidden protocol constants.

## 5. Verifier events

The standalone interface must preserve the distinctions in Jael's asynchronous
contract. A boolean verification callback cannot express them.

| Event | Effect |
|---|---|
| `%full` / accept point | Validate and install the point and fief; admit through the soft gate; preserve an independent hard block. |
| `%fail` / reject candidate | Drop pending alien candidate state; retain any accepted peer, point, flows, and existing soft/hard policy. |
| `%lost` / verifier unavailable | No authority to accept or revoke. The current kernel handler is a no-op; libmesa needs bounded retry/expiry policy. |
| No verdict / unevaluable | Leave acceptance unchanged; expose local pending/unavailable status and release bounded work appropriately. |
| `%snob` / withdraw vouch | Retain point and flows, soft-block ordinary inbound peer traffic, solicit a fresh attestation. |
| `%anew` / refresh own evidence | Accept only an evidence-tail refresh matching the active identity, domain, and live key (§7). |

A rejected candidate is never evidence that its claimed name should be
hard-blocked. `%fail` must not delete a healthy accepted point, breach its
flows, or add that name to a blocklist. Terminal per-peer revocation and the
old moved/strike punishment ladder are not part of this core contract.

A withdrawal is accepted only from the registered authority for a point it
has vouched for. Domain removal (`%bane`) is a distinct local administrative
action: deregister the authority, remove its accepted points, and breach/block
those peers. Suspension (`%gost`) retains the registry and points; revival
(`%ghul`) is distinct from fresh peer verification. The kernel currently maps
suspension/revival to hard-list edits. Its readiness and policy-provenance
limitations are release gates, not evidence that desk liveness proves currency.

## 6. Re-attestation and admission

A soft block retains peer state, flows, routes, and subscriptions. It drops
ordinary authenticated inbound traffic before receive processing or
acknowledgement, while allowing self-attestations and requests for our proof.
Successful verification applies the new point and releases the soft block.
At unchanged rift, flow sequence state survives and shared keys and encrypted
request paths are rederived. At increased rift, ordinary breach semantics
apply. There is no dual-key grace period after acceptance of the new life.

The kernel's `%poof` solicits one proof and retries every 30 seconds while the
peer remains soft-blocked. The library must bound solicitation and preserve
recovery traffic. It must not infer authorization, a full life, or malicious
behavior from an unauthenticated four-bit tick or decryption failure.
Automatic tick-triggered recovery was deferred in the reviewed stack.

The soft policy supports deny and allow modes. Successful registered-domain
verification removes a deny entry or adds an allow entry. A withdrawal does
the inverse. Neither operation changes the mode. The hard policy remains
independent and also gates attestations. Kernel soft sets carry no provenance:
a successful verdict can reverse an overlapping manual soft exclusion.
Preserving operator exclusions across domain lifecycle events needs a separate
policy representation or an explicit documented compatibility choice.

Mesa peeks have no authenticated requester. Their return lane is not an
identity, so peer blocklists cannot enforce identity admission on anonymous
reads. Namespace authorization and resource limits govern those reads.
This exception must remain explicit in any claim about allow-mode admission.

## 7. Local rotation and evidence refresh

The host keeps accepted public authorization, staged private rings, and the
active private life separate. Either the next ring or its authorized point
may arrive first. A staged ring activates only when it reproduces the current
authorized suite and exact pass, including `xtr`, and fingerprints to the
running identity. Same-live-key equality alone is insufficient. Life zero,
rollback, and replacement of an already-active life's ring are rejected.

The kernel publishes the authorized point before the matching private-key
gift. Activation changes both signer and agreement key, then announces the
new attestation to known peers. This ordering is observable; it is not an
atomic network-wide rotation. Each peer independently verifies and advances.

`%anew` changes only our evidence tail. The response must retain `ugn`, `dat`,
domain, name, and the active public key. A response prepared before rekey
cannot reinstall the old `cry`. The kernel does not persist the refreshed
pass to the boot keyfile; libmesa's storage contract must define durable
refresh and recovery before exposing this operation as complete.

Live advancement of our own rift needs a separate quiescent transition that
clears old continuity and prevents old-ring channels from being recreated
before the matching new ring activates. The current kernel does not safely
implement that interval. Routine life rotation must not be used as evidence
that live own-rift recovery works.

## 8. Transport compatibility

The current kernel proactively announces rekeys through legacy Ames open
packets even to known Mesa peers. Mesa proof reads and proof pokes cover
first contact and solicitation, but do not make a strictly Mesa-only receiver
compatible with every kernel identity update.

**Decision: libmesa stays Mesa-only.** Change the kernel to announce activated
keys through Mesa's attestation path. Existing proof poke/page mechanisms are
the starting point, but the upstream patch must specify exact receiver-life
handling, retry/recovery, and simultaneous rotations. Neither packet reuse nor
a successful first-contact proof establishes reliable rekey convergence.
Legacy data flows, identity-control packets, Fine, and `%ahoy` remain outside
libmesa's implementation scope.

Until that upstream change exists and passes interop tests, libmesa must not
claim full rekey compatibility with the pinned kernel. The legacy `%keys`
recipient-life-1 exception is not imported into the Mesa path.

## 9. Required implementation evidence## 9. Required implementation evidence

The next identity milestone needs kernel-derived fixtures and negative tests
through the actual admission and activation path:

1. Preserve the name across rotation; change both live signer and agreement
   key; reject each life's signature under the other life's key.
2. Copy `ugn/dat` with an unauthorized signer: reject the candidate without
   disabling an accepted peer or pre-blocking an unknown name.
3. Reject malformed domain encodings, wrong-domain verdicts, wrong names,
   life/rift rollback, life zero, and same-life key substitution.
4. Exercise both point-first and ring-first activation, including a ring with
   the right key but stale `xtr`, and a delayed `%anew` after rekey.
5. Withdraw, solicit, re-attest, and readmit at the same life and at a higher
   life; preserve flows at unchanged rift; breach on increased remote rift.
6. Test allow/deny modes, hard blocks, anonymous peeks, and proof arrival after
   the peer became known. An ordinary valid signature must not bypass a vouch.
7. Exercise verifier outage/restart, late pre-withdrawal completions, and own-rift
   quiescence once their contracts are settled. Pin proof-size boundary cases.
8. Test fallback opt-in/off, unregistered versus suspended lookup, and verifier
   installation after fallback admission. Test provisional source authentication,
   opt-in, expiry, successful revalidation, rejection, and prohibition on implicit
   fallback after withdrawal.
9. After the upstream Mesa announcement change, rotate both peers concurrently,
   lose announcements, recover through proof exchange, and confirm that no legacy
   control packet is required.

Existing wire-codec roundtrips prove none of these identity properties.

## 10. Open choices and release gates

The selected policy is opt-in life-1 fallback, explicit provisional sponsor
bootstrap, and Mesa-only operation with an upstream rekey-announcement change.
The default native admission path remains registered-domain verification.
Fallback and provisional trust carry their own status and cannot silently
substitute for a lost domain vouch.

The source audit also identifies release gates: verifier readiness distinct
from process liveness; fencing work begun before withdrawal; policy ownership;
own-rift quiescence; evidence growth beyond one fragment; bounded domain noun
decoding; and exact final-ring/evidence agreement in the companion tooling.
The baseline links the upstream records. This chapter neither claims these
repairs exist nor silently adopts old terminal-snob policy to fill the gaps.
