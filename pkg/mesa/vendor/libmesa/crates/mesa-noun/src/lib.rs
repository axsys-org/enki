//! Noun primitives for libmesa.
//!
//! Scope (spec ch. 5 governs the identity hardening requirements):
//! - `jam`/`cue` noun serialization with bounded shape and reference validation
//! - `mug` (murmur3-based noun hash; also the packet-header checksum)
//! - `mat`/`rub` length-prefixed atom encoding, with the bounded
//!   zero-run scan (kernel commit 382243cd7c) enforced unconditionally
//! - atom views: `@p`/`@q`/`@uv` rendering
//!
//! Implemented: canonical atoms, byte mug, bounded mat/rub over borrowed bit
//! spans, and canonical jam/cue with shared arena nodes. Wire fixtures exercise
//! mug against Hoon/C checksums. Atom rendering remains planned.

pub mod atom;
pub mod bits;
pub mod mat;
pub mod mug;
pub mod serial;
