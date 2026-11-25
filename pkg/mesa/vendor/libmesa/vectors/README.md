# Golden vectors

The v2 prototype fixtures (`pact/v2-basic.json`) contain eight hand-specified
bodies with checksums from an independent Python implementation. They cover
peek/page/poke, explicit domains, fixed names, variable signatures, MAC/hash
fields, and terminal zeros. Signature fixtures establish framing only.
`python3 tools/v2-vectors.py --check` verifies the checked-in bytes without
calling the Rust encoder; `tools/conformance-v2.py` exercises the v2 runner.

The noun fixtures cover `mat`/`rub`, canonical jam, and the bit boundaries of
suite-C public passes and current rings. They come from an independent Python
transcription of pinned kernel source, with synthetic keys and evidence.
Jam conformance also uses the historical live-Hoon extractions and an optional
freshly compiled Vere reference. See [provenance and limits](noun/README.md).

Historical v1 cross-implementation fixtures for the Hoon kernel, C runtime, and Rust codec.
`pact/v0-basic.json` contains ten live Hoon extractions. The divergence and
checksum files record known incompatibilities; `pact/v1-reassembly.json`
defines logical-byte reconstruction cases, not LSS proof verification.
See `pact/NOTES.md` for provenance and executable checks. Remaining classes:

- `pact/` — packet etch/sift roundtrip + adversarial (extract via `pact.c`
  `PACT_TEST` and fakeship dojo `++etch`/`++sift`)
- `name/` — name↔beam mapping
- `lss/` — blake3 lockstep-streaming Merkle proofs
- `pass/` — suite %a/%b/%c pass + `dat` canonical and non-canonical encodings
- `mnemonyms/` — shared with the `mnemonyms` repo's `test-vectors.json`
- `attestation/` — accept/reject cases against a fake `%test-pki` domain
- `space/` — %publ/%chum/%shut seal/open

Aligns with `groundwire@hd/cc-snob`'s `vectors/gw-kelvin-9.json` pin.
