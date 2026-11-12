# Noun and suite-C layout fixtures

`mat.json` contains 23 length-prefixed atom cases, including zero, numeric
width boundaries, `%gw-btc`, and values above 1,024 bits.
`jam.json` contains 12 canonical noun cases, including repeated large atoms
and repeated cells. `../pass/suite-c-layout.json` contains three public-pass
and current-ring layouts covering an empty evidence tail, unaligned evidence,
and a different domain selector.

The source is `gwbtc/urbit` commit
`734ac17cfb88e571081b84406319a17d3d5e79dd`: `++mat`/`++rub` and `++jam`/`++cue`
in `pkg/arvo/sys/hoon.hoon`; `++pub:ex:cric`, `++sec:ex:cric`, and
`++nol:nu:cric` in `pkg/arvo/sys/zuse.hoon`.
`tools/noun-vectors.py` transcribes the source using Python integer arithmetic;
it never invokes the Rust implementation. These are source-derived fixtures,
not results extracted from a running Hoon kernel.

Run `python3 tools/noun-vectors.py --check` to compare the checked-in bytes,
or omit `--check` to regenerate them. `cargo test -p mesa-noun` checks both
Rust directions against the fixtures and exercises hostile inputs.
`cargo test -p mesa-identity` validates the suite-C parsers against the layouts.

Run `python3 tools/conformance-noun.py` for the 12 generated jams and ten
historical live-Hoon jams from `pact/v0-basic.json`. Add
`--vere-root /path/to/vere` to compile `tools/noun-reference.c` against Vere's
unmodified `pkg/ur` and bundled Murmur3. Both executables must reproduce the
canonical bytes and reject six malformed or false-expectation corpora. The
script prints source hashes for the C reference. CI runs the Rust path; the
Vere checkout is an explicit optional dependency.

On 2026-10-21, all 22 cases and six harness checks passed against a freshly
compiled Vere reference at `879216b82154c7d8645976fcf20b5f9c9c4094d6`, with clean
serializer sources. Its `pkg/ur/serial.c` SHA-256 was
`65e4fd3af8cf30f926bb46bc77ac9de8c55a045ec8d14ac9872fa1d8506227c4`.
This checks noun serialization, not suite-C cryptography or current domain state.

All integers and bit spans use little-endian order, least significant bit
first. The public pass begins with `'c'`, 32 bytes of genesis public-key
material, and 32 bytes of live public-key material. `mat(dat)` begins at bit
520. The evidence begins immediately after that field, without byte alignment.
Within `dat`, the initial `mat(domain)` leaves the remaining bits to the domain.
The current ring substitutes `'C'`, a 32-byte live seed, and the 32-byte genesis
public key in those fixed slots. Test seeds are public deterministic bytes.

Keys are deterministic byte sequences, not validated curve points. Domain
tails and evidence are arbitrary test atoms, not valid Groundwire custody
records. These fixtures establish no name derivation, signature validity,
key agreement, or authorization. Separate [executed Hoon crypto fixtures](../crypto/README.md) now cover those
primitives; neither fixture set establishes authority.

The Rust parser deliberately rejects inputs that would rely on Hoon's implicit
zero extension, and rejects nonminimal atom payloads. `rub` consumes one field;
`decode` requires its exact bit span. Neither strips byte padding or validates
jam/cue structure. Enclosing parsers must bound input and preserve explicit
field lengths before invoking these primitives.

The separate `serial::cue` API enforces canonical jam and exact consumption,
accepts only references to completed literal nouns, and retains a bounded DAG.
Resource tests cover literal count, reference count, atom size, expanded depth,
input/output limits, 2,049 levels of nesting, and a 65-node graph representing
2^64 leaf occurrences. Consumers must not expand that graph into a tree.
