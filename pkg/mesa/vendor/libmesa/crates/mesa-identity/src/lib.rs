//! Identity profiles. Cryptographically bound claims are not authorized peers.
//!
//! Suite-C framing, curve validation, name derivation, and exact ring/pass
//! correspondence are implemented. Byte-level signing and agreement live in
//! mesa-crypto. Domain authority, activation, and handshakes are not implemented.
pub mod suite_c;
