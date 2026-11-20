# libmesa spec — 1: Experimental Mesa v2 wire format

**Implemented codec profile, 2026-10-21.** The default Rust API implements
version **2** with fixed 128-bit names, explicit domain selectors, exact-byte
checksums, and length-delimited signatures. This is a libmesa experimental
version assignment, not an upstream allocation. Encoding and structural
validation establish no identity, signature validity, or LSS proof validity.

The historical profile is retained under `mesa_wire::legacy_v1` and documented
in [the v1 reference](reference/mesa-v1.md). There is no automatic version or
checksum fallback. Existing Vere/Hoon packets are v1, not v2 conformance evidence.

## Framing and limits

Integers are little-endian; packed fields run from least to most significant
bit. A datagram contains exactly one pact. A stream adapter supplies a complete
frame and bounds its declared length before allocating.

Decode checks packet size, header/version, checksum, bounded structure,
canonical encodings, and exact consumption. It borrows input and allocates no
memory. Encode first measures and validates, then allocates the complete
output. Defaults remain 1472 packet bytes, 1024 fragment bytes, and 65535 path
bytes. Hosts may configure larger packet/fragment limits for an appropriate
transport; a declared message length never authorizes whole-message allocation.

## Header

The first 32-bit word holds, from its least significant bit:

```text
reserved(2) | next(2) | version(3) | type(2) | hop(3) | gum(20)
```

The second word is cookie `0x67e00200` (bytes `00 02 e0 67`). Reserved bits are
zero; version is 2; type is page=1, peek=2, poke=3. Encode saturates hop at 7.
Next is 0 or 1 for a page and 0 for peek/poke. Next=1 carries the six-byte IPv4
trailer; longer/multiple trailers remain unsupported.

`is_mesa` checks only minimum header length and cookie. `sift_head` additionally
checks header shape and v2, but does not verify the body checksum; use `decode`
for complete structural validation. `legacy_v1::sift_head` accepts only v1.

## Checksum

`gum` is the low 20 bits of the byte mug of **every transmitted body byte**,
including terminal zeros and any next-hop trailer. Mug uses MurmurHash3 x86-32
starting with seed `0xcafebabe`, folded as `(hash >> 31) XOR (hash & 0x7fffffff)`.
If the fold is zero, increment the seed and retry, for at most eight attempts;
the fallback mug is `0x7fff`.

The v2 API has no checksum-mode parameter. It never retries after trimming
zeros. A checksum detects accidental corruption; it does not authenticate the
name, domain selector, or payload. Fragment normalization is separate.

## Names

Identity equality is the globally unique `PeerId(u128)`. Each wire name also
carries a domain selector, which identifies the claimed authority for that
identity. Full wire-name equality includes the selector so interests cannot
match across different authority contexts. The identity layer must authorize
the association; a remote selector cannot replace an accepted association.

A name has this layout:

| Field | Encoding |
|---|---|
| Metadata | `reserved-zero(2) · rift-width-minus-one(2) · init(1) · auth-fragment(1) · fragment-width-tag(2)` |
| Domain length | One byte, 1–63 |
| Domain | Exactly that many canonical ASCII bytes |
| Publisher identity | Exactly 16 little-endian bytes, including high zero bytes |
| Rift | 1–4 bytes, selected by its width field |
| Bloq | One byte: log2 of fragment capacity in bits |
| Fragment index | Absent for init; otherwise 1, 2, 4, or 8 bytes |
| Path length | Two bytes |
| Path | Exactly that many bytes; nonempty |

There is no identity-width selector. The former low two metadata bits must be
zero. A zero or small identity still occupies 16 bytes. Width conveys no rank,
sponsorship, algorithm, or authority. Short aliases resolve before wire encoding.

Domain tags match `[a-z][a-z0-9]*(?:-[a-z0-9]+)*` and are case-sensitive.
There is no normalization, default, numeric registry, or implicit `%gw-btc`.
Empty tags, uppercase, non-ASCII, underscores, leading digits, and leading,
trailing, or repeated hyphens are invalid. A syntactically valid unknown tag
passes the codec; identity dispatch later reports an unavailable/unsupported
authority without trying another domain. `gw-btc` denotes the current reference
domain; its selector must agree with the domain committed in a suite-C pass.

Init requires zero auth-fragment and fragment-width bits. Otherwise auth=1
selects a proof fragment and auth=0 a data fragment. The fragment-width tag
selects `1 << tag` bytes. Rift and fragment index use the smallest allowed width;
each takes at least one byte. Path bytes remain opaque at the codec layer.

A `gw-btc` name adds seven bytes of selector framing relative to a v1 name
whose publisher already occupied 16 bytes. Poke names carry independent tags:
the acknowledgement publisher and payload publisher may use different domains.

## Data

Data starts with:

```text
total-width-tag(2) | auth-kind(2) | reserved-zero(2) | length-width-tag(2)
```

The following fields are:

1. Total logical message length: `1 << total-width-tag` bytes (1, 2, 4, 8).
2. Authentication field, selected below.
3. Fragment length: 0, 1, or 2 bytes directly from its tag; tag 3 first adds
   one byte specifying a length width of 3 or 4. Other extended widths fail.
4. Exactly that many fragment bytes, including zeros.

| Auth kind | Bytes |
|---|---|
| 0: signature | Two-byte length (1–65535), then signature bytes |
| 1: absent | None |
| 2: MAC | Exactly 16 bytes |
| 3: hash pair | Two 32-byte hashes |

The signature length is always two bytes. No zero-length signature is allowed;
use absent auth where the packet role permits it. Signatures borrow input just
like fragments. The frame-size limit and available bytes bound the length
before any use. Structural acceptance does not establish an algorithm or a
valid signature: the accepted point/profile must prescribe those, with no
algorithm inference or downgrade from length alone. Large signatures may exceed
the datagram profile; this encoding does not provide fragmented authentication
or claim PQ support.

Total and fragment-length integer widths must be minimal. Zero fragment length
uses no length bytes; zero total length still uses one. The codec does not
validate semantic consistency between message length, fragment position,
authentication role, and identity state; subsequent layers must do so.

## Bodies and reconstruction

Peek carries one name. Poke carries acknowledgement name, payload name, then
data. Page carries name, data, and the optional six-byte IPv4 trailer: four
address bytes and two port bytes, little-endian. Extra trailing bytes fail.

For bloq `b >= 3`, fragment capacity is `2^(b-3)` bytes; bloq 13 is 1024 bytes.
For data-fragment index `i`, reconstruct logical length as
`min(capacity, total - i*capacity)` with checked arithmetic and a nonempty valid
range. Transmitted data may omit terminal zero bytes but may not exceed that
logical length. Restore omitted zeros before future LSS hashing. Exact-multiple
final fragments retain full capacity. The helper bounds each allocation;
proof fragments require a separate rule and must not use it.

## APIs and verification boundary

Rust defaults to `decode(bytes, limits)` and `encode(packet, limits)` for v2.
`Name` carries `publisher: PeerId` and `domain: DomainTag`; type construction
validates the tag grammar but neither proves global uniqueness nor installs a
trusted domain association. Conflicting claims need identity policy, not a
second peer keyed by `(domain, name)` or a first-arrival-wins rule.

The C ABI adds `mesa_wire_v2_validate` and `mesa_wire_v2_roundtrip` without a
checksum argument. Existing `mesa_wire_validate` and `mesa_wire_roundtrip`
retain their v1-only semantics and explicit checksum argument. Both profiles
preserve caller-owned buffers and errors; no automatic cross-version dispatch.

`vectors/pact/v2-basic.json` contains eight hand-specified body fixtures with
an independent Python checksum. `tools/v2-vectors.py --check` reproduces them
without using Rust. `tools/conformance-v2.py` exercises the v2 CLI's fixtures,
truncations, reserved bits, version separation, and failure reporting. Rust
adds domain grammar, fixed-name width, signature lengths/borrowing, limits,
and canonical mutation coverage. The separate Vere comparison remains v1-only.

The [signed-public profile](08-signed-public.md) now defines a fixed protected
v2 context, including domain/name binding, and LSS verification for preconfigured
peers. Bounded noun decoding and suite-C framing exist in
separate crates. Suite-C name derivation, signing, and agreement now match
executed Hoon fixtures. Domain authority and network handshakes remain
unimplemented; wire validation itself does not invoke crypto. The
[identity milestone](../doc/09-identity-milestone.md) remains a separate integration
track after peer-engine progress.
