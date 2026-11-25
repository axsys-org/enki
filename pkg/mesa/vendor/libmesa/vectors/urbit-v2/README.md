# Hoon v2 page captures

These page bytes came from `%mesa-v2`, running in a fake Urbit booted with the
pill recorded in `provenance.json`. The Hoon agent constructed the Ed25519
signatures and LSS proofs. The capture bridge only preserved the datagrams.
The files contain the Auth(0) page followed by each Data(i) page for lengths
0, 1, and 17425. All bytes are hex in transmission order.

`cargo test -p mesa-core --test urbit_v2` authenticates and reassembles these
captures, then proves that a different configured key cannot authenticate them.
This is a captured-packet regression, not a live Urbit test.

Regenerate through `interop/boot.py`, whose full run writes clean-case captures
to `target/urbit-interop/packets/`. Compare the three files before replacing them,
and update provenance from the corresponding source receipts. Never regenerate
these vectors with the Rust peer or a Python signing implementation.
