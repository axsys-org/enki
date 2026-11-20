# Historical Mesa v1 wire profile

This reference records the explicit `mesa_wire::legacy_v1` module and the
Vere comparison format. The default Rust codec and versioned v2 C functions
implement [Mesa v2](../01-wire.md); neither automatically falls back to v1.
The existing unversioned C functions remain v1-only for compatibility.

This chapter specifies the implemented v1 codec profile: `%peek`, `%page`,
and `%poke`, with absent or short IPv4 next-hop trailers. Long and multiple
trailers return an unsupported-format error; their Hoon and C layouts need
separate conformance work. Wire validation does not establish authentication.

## Framing and limits

Integers are little-endian; packed fields run from least to most significant
bit. A datagram holds exactly one pact. A stream adapter must supply one
complete frame and bound its declared length before allocating it.

The codec checks the packet-size limit, header, selected checksum, structural
bounds, canonical integer widths, and exact consumption, in that order.
Parsing borrows the frame and allocates no memory. Encoding measures the
packet and checks all limits before allocating output. The default datagram
limits are 1472 packet bytes and 1024 fragment bytes. Callers may configure
larger limits for a transport path that supports them; a large `tob` never
authorizes an allocation. Path length cannot exceed 65535 bytes.

## Header

The first word holds `reserved(2) · next(2) · version(3) · type(2) · hop(3)
· gum(20)`. The second word is cookie `0x67e00200`. Reserved bits are zero,
version is 1, and type is page=1, peek=2, poke=3. Encoding saturates hop at 7.
Next is 0 or 1 for a page and must be 0 for a peek or poke.

## Checksum compatibility

`gum` is the low 20 bits of Urbit's byte mug: MurmurHash3 x86-32, starting
with seed `0xcafebabe`, folded as `(hash >> 31) XOR (hash & 0x7fffffff)`.
If the fold is zero, increment the seed and retry, for at most eight attempts;
the fallback mug is `0x7fff`.

The two upstream implementations hash different representations. The API
requires callers to select one convention explicitly:

| Convention | Bytes passed to mug |
|---|---|
| `HoonAtom` | Complete body after removing terminal zero bytes |
| `VereBytes` | Complete body with its exact transmitted length |

The body includes the next-hop trailer. A port below 256 can therefore
trigger the disagreement even when the fragment ends in a nonzero byte.
The decoder never retries under the other convention. Selection belongs to
the host's compatibility configuration, not unauthenticated packet input.
There is no claim that a single convention interoperates with both upstream
implementations for every packet. A common upstream rule remains open.

Checksum normalization never changes fragment bytes or their encoded length.
Hoon's encoder separately normalizes fragment atoms; C's encoder preserves
explicit lengths. Libmesa preserves explicit wire bytes under both checksum
conventions, including packets Hoon cannot re-emit byte-identically.

## Names

A wire name identifies a publisher, its rift, a path, and a fragment. Its
publisher field is the numeric `@p`, represented by `legacy_v1::Name.ship: u128`. For the current suite-C Groundwire profile, that number is the permanent
suite-C name derived from immutable `ugn/dat`; the domain authorizes the
current key through attestation and re-attestation (chapter 5). Rotating the
live key does not change this publisher field.

The wire name carries no separate domain identifier or live public key.
The committed domain is extracted from the attested pass, and the identity
layer maintains the accepted name-to-current-point binding. Successful wire
decoding establishes neither that binding nor authority to update its rift.

A name starts with `ship-width-tag(2) · rift-width-minus-one(2) · init(1) · auth(1)
· fragment-width-tag(2)`, followed by:

1. Ship: 2, 4, 8, or 16 bytes, selected by `2 << ship-width-tag`.
2. Rift: 1–4 bytes, selected by its width field.
3. Bloq: one byte, the log2 of fragment capacity in **bits**.
4. Fragment index: absent for init; otherwise 1, 2, 4, or 8 bytes.
5. Path length: two bytes, followed by that many path bytes.

`ship-width-tag` is the field historically called `rank`; its bits and width
mapping are unchanged. The codec uses it only for integer encoding. It does
not assign galaxy/star/planet/moon/comet privileges, resolve Azimuth, or infer
sponsorship. All four widths remain in the historical v1 codec. The v2 profile is specified
separately in [chapter 1](../01-wire.md).

Init requires zero auth and fragment-width bits. Otherwise auth=1 selects
an authentication fragment, and auth=0 selects a data fragment. Integer
widths must be the smallest allowed width for the value; zero rift and zero
index still take one byte. Ship width has a two-byte minimum. Empty paths
are rejected. The codec preserves path cords as opaque bytes; namespace
syntax and authorization are a later validation stage. Fixed-width path
lengths, signatures, MACs, and hashes retain their prescribed widths.

## Data

Data starts with `total-width-tag(2) · auth-kind(2) · reserved(2)
· length-width-tag(2)`. Reserved bits must be zero. The fields that follow are:

1. Total logical message bytes: 1, 2, 4, or 8 bytes.
2. Auth: kind 0 is a 64-byte signature; 1 is absent; 2 is a 16-byte MAC;
   3 is two 32-byte hashes.
3. Fragment length: the width tag gives 0, 1, or 2 bytes directly. Tag 3
   adds one byte specifying a width of 3 or 4. Wider lengths are rejected.
4. Fragment: exactly the indicated number of bytes, including zeros.

Total and fragment-length widths must be minimal. A zero fragment length
takes zero length bytes. Bounds are checked before accessing any variable
field. Total message length, fragment coordinates, proof interpretation,
and signature validity require checks beyond this structural codec.

## Bodies and reconstruction

`%peek` carries a name. `%poke` carries ack-name, payload-name, and data.
`%page` carries name, data, and optionally six IPv4 lane bytes: four address
bytes followed by two port bytes. The codec rejects unused trailing bytes.

For byte-aligned bloq `b >= 3`, wire-fragment capacity is `2^(b-3)` bytes;
bloq 13 means 1024 bytes. The BLAKE3 tree retains 1024-byte base leaves;
jumbo fragments group subtrees rather than redefine that base leaf size.

For a data fragment at index `i`, its logical length is
`min(capacity, tob - i * capacity)`, with checked arithmetic and a valid
nonempty range. Its transmitted length must not exceed that logical length.
Restore omitted terminal zeros to the logical length **before** LSS hashing.
An exact-multiple final fragment has a full capacity of bytes, not zero.
The reconstruction helper bounds each allocation independently of `tob`.
Proof fragments need their own length rules and must not use this helper.

## Conformance and remaining layers

`vectors/pact/` contains live Hoon fixtures, checksum divergence fixtures,
and source-derived reconstruction cases. `tools/conformance.py` generates
shared truncation and malformed-field cases and checks the Rust runner and
an explicitly supplied patched C binary. Rust additionally rejects
nonminimal integer encodings; the C oracle does not yet enforce all of those
canonicality rules.

Jam/cue, the bounded `mat` scan, name-to-beam mapping, signatures, and LSS
proof verification belong to later layers. None is implied by codec success.
