# LSS and signed-public evidence

`vere.json` contains 12 nonempty LSS cases built and verified by compiled C,
without calling Rust. The cases cover short leaves, exact boundaries, irregular
trees, and the 1024-leaf maximum. Every root, spine, and scheduled pair is checked
by the Rust test suite.

The reference inputs were Vere `96fae0a67dfc989cc4628d42d6a499030ad6514e`
(`pkg/vere/io/lss.c` and `.h`) and urcrypt
`146ea2bcb4634b6d553c10415e9c9c9a91ca662f` (BLAKE3 wrappers and portable
compression). The fixture also records exact source SHA-256 hashes. The driver
supplies Vere types/allocation macros, aliases the compression entry points to
portable C, and calls the existing inline-proof completion for single-leaf
objects. It does not alter the LSS algorithm. It uses exact final-leaf lengths,
including 1024 for an exact multiple.

```sh
python3 tools/lss-vectors.py --vere /path/to/vere --urcrypt /path/to/urcrypt --check
cargo test -p mesa-lss
```

`signed-public.json` adds an independently serialized Python v2 context and
an OpenSSL Ed25519 signature over the C-built root. Its seed is public test
material. Rust checks the exact context and signature against this fixture.

```sh
python3 tools/protection-vector.py --check
cargo test -p mesa-core --test protected
```

The empty-object extension is tested separately against `blake3::hash([])`;
it is not C/Vere conformance evidence. These fixtures establish LSS and the new
v2 transcript's behavior, not Hoon signature-context or network interoperability.
