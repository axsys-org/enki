# Executed suite-C cryptographic fixtures

`suite-c.json` records five cases evaluated from the pinned `gwbtc/urbit`
Lull and Zuse sources at `734ac17cfb88e571081b84406319a17d3d5e79dd`, the composed
key-rotation/snob stack. Vere's standalone evaluator supplies the embedded Ivory
Hoon environment; the generator does not rebuild `sys/hoon.hoon`. Jets may run.
The JSON records the source, Hoon generator, and Vere binary SHA-256 hashes.

The live seeds are public test data: 1, 2, 0, 4, and 2^256−1, each padded to
32 bytes. Four passes retain the genesis key from seed 1. The seed-4 pass uses
genesis seed 18, producing a tweaked name key with a high zero byte: hashing
its canonical atom bytes must differ from hashing all 32 bytes. All cases use
the same synthetic `dat/xtr`. The independent peer uses seed 3. The domain tag is
`gw-btc`; its payload is synthetic and establishes no Bitcoin authority.

The Hoon body invokes the current `nol`/`com` imports and `sec`/`pub` exports,
`nam`/`fig`, `sigh`, `sign-octs-raw`, and `slar`. It asserts ring export equality
and public/private name equality. The Python driver independently decodes the
returned jam. It uses no Rust crypto to calculate expected results.

```sh
python3 tools/crypto-vectors.py \
  --urbit-source /path/to/gwbtc/urbit \
  --vere-binary /path/to/vere/urbit --check
cargo test -p mesa-crypto -p mesa-identity
```

Omit `--check` to regenerate. The checkout must contain the pinned commit;
working-tree edits do not affect extraction. A different Vere build may
reproduce the fixture bytes; `--check` prints its hash. CI runs Rust tests against
checked-in vectors; fresh Hoon extraction requires the explicit local tools.

Tests compare live public keys, tweaked name keys, 128-bit fingerprints,
signatures, and both agreement directions. One message uses canonical atom
bytes; the other appends a zero byte and uses explicit octet length. Rotation
preserves the name while changing both live keys; cross-life verification fails.
Evidence refresh preserves the name but invalidates an old exact ring/pass match.

Public keys must be canonical, nonidentity prime-subgroup Edwards points.
Signature verification uses Dalek's
[`verify_strict`](https://docs.rs/ed25519-dalek/latest/ed25519_dalek/struct.VerifyingKey.html#method.verify_strict).
Negative tests cover weak/mixed-torsion/noncanonical keys, noncanonical signature
scalars, weak signature points, changed messages, and wrong-life keys. This is
a deliberate stricter input policy, not a claim of identical acceptance behavior
for every historical Hoon/Vere input.

These tests establish primitive and profile compatibility. They do not exercise
Jael activation, verifier readiness, chain currency, first contact, or packet
protection. Agreement output is raw secret material, not a channel key. V2 still
needs a protected transcript, KDF, authority boundary, and activation state machine.
