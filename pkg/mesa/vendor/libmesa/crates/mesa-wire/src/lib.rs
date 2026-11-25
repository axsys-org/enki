//! Experimental Mesa v2 pact codec (spec ch. 1).
//!
//! Decodes and encodes the three packet types (%peek, %page, %poke) with
//! the normative validation order of spec ch. 2 §2: cookie/version gate,
//! 20-bit mug check, then bounded structural sift — every length field
//! bounds-checked before allocation. Vector-driven against
//! `../../vectors/pact/`.

mod codec;
mod fragment;
mod identity;
/// Explicit historical v1 codec for compatibility tests; no automatic fallback.
pub mod legacy_v1;
pub use codec::{decode, encode, Auth, Body, Data, Error, Fragment, Limits, Name, Packet};
pub use fragment::reconstruct_data_fragment;
pub use identity::{DomainTag, PeerId};

/// Protocol version carried in every header (3 bits on the wire).
pub const VERSION: u8 = 2;

/// Header magic cookie, little-endian on the wire at byte offset 4.
pub const COOKIE: u32 = 0x67e0_0200;

/// Fixed header size in bytes.
pub const HEADER_LEN: usize = 8;

/// Packet type tag (header bits).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PactType {
    Page = 0b01,
    Peek = 0b10,
    Poke = 0b11,
}

/// Cheap pre-dispatch test: is this datagram shaped like a mesa packet?
/// (Length gate + cookie; mirrors `mesa_is_new_pact` in vere's pact.c.)
pub fn is_mesa(buf: &[u8]) -> bool {
    buf.len() >= HEADER_LEN && buf[4..8] == COOKIE.to_le_bytes()
}

/// Decoded packet header. `gum` is the 20-bit truncated mug of the body,
/// NOT verified against the body by `sift_head`; use `decode` for that check.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Head {
    pub nex: u8,
    pub typ: PactType,
    pub hop: u8,
    pub gum: u32,
}

/// Sift the fixed 8-byte header: length gate, cookie, reserved-zero,
/// version, type tag. Field layout (LSB-first within the first LE word):
/// res(2) nex(2) ver(3) tip(2) hop(3) gum(20), then the 32-bit cookie.
pub fn sift_head(buf: &[u8]) -> Option<Head> {
    sift_head_version(buf, VERSION)
}

pub(crate) fn sift_head_version(buf: &[u8], version: u8) -> Option<Head> {
    if !is_mesa(buf) {
        return None;
    }
    let w = u32::from_le_bytes(buf[0..4].try_into().unwrap());
    if w & 0b11 != 0 {
        return None; // reserved bits
    }
    if (w >> 4) & 0b111 != version as u32 {
        return None;
    }
    let typ = match (w >> 7) & 0b11 {
        0b01 => PactType::Page,
        0b10 => PactType::Peek,
        0b11 => PactType::Poke,
        _ => return None, // reserved type
    };
    Some(Head {
        nex: ((w >> 2) & 0b11) as u8,
        typ,
        hop: ((w >> 9) & 0b111) as u8,
        gum: w >> 12,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cookie_bytes() {
        assert_eq!(COOKIE.to_le_bytes(), [0x00, 0x02, 0xe0, 0x67]);
    }

    #[test]
    fn short_or_cookieless_is_not_mesa() {
        assert!(!is_mesa(&[0u8; 7]));
        assert!(!is_mesa(&[0u8; 64]));
        let mut ok = [0u8; 8];
        ok[4..8].copy_from_slice(&COOKIE.to_le_bytes());
        assert!(is_mesa(&ok));
    }
}
