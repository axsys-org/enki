//! Strict, bounded `mat`/`rub` for length-prefixed natural-number atoms.
//!
//! Hoon scans a zero prefix, then reads the atom bit length and its payload.
//! This implementation bounds the scan by both the input and machine width,
//! checks the declared length before reading the payload, and rejects implicit
//! zero extension and nonminimal payloads. It never invokes cue or allocates
//! while decoding. Embedded fields use `rub`; a standalone field uses `decode`.
use crate::{atom::Atom, bits::Bits};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Limit,
    Truncated,
    NonCanonical,
    TrailingBits,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for Error {}

#[derive(Clone, Copy, Debug)]
pub struct Decoded<'a> {
    /// Borrowed payload bits, excluding the length prefix; empty for zero.
    pub atom: Bits<'a>,
    /// Exact field length, including the prefix, for parsing subsequent fields.
    pub consumed: usize,
}

/// Decode one field at the start of a bounded span. Remaining bits belong to
/// the enclosing format. `max_atom_bits` bounds the declared payload length.
pub fn rub(input: Bits<'_>, max_atom_bits: usize) -> Result<Decoded<'_>, Error> {
    let mut zeros = 0;
    while !input.bit(zeros).ok_or(Error::Truncated)? {
        zeros += 1;
        // A longer prefix cannot describe a representable usize bit length.
        if zeros > usize::BITS as usize {
            return Err(Error::Limit);
        }
    }
    if zeros == 0 {
        return Ok(Decoded {
            atom: input.slice(1, 0)?,
            consumed: 1,
        });
    }
    let lower = input.slice(zeros + 1, zeros - 1)?.word()?;
    let len = (1usize << (zeros - 1)) | lower;
    if len > max_atom_bits {
        return Err(Error::Limit);
    }
    let prefix = 2 * zeros;
    let consumed = prefix.checked_add(len).ok_or(Error::Limit)?;
    let atom = input.slice(prefix, len)?;
    // Nonzero atoms must occupy exactly their declared bit length.
    if atom.bit(len - 1) != Some(true) {
        return Err(Error::NonCanonical);
    }
    Ok(Decoded { atom, consumed })
}

/// Decode a standalone field. Caller-specified bit length excludes byte padding;
/// all bits inside that span must be consumed. No padding is inferred or removed.
pub fn decode(input: Bits<'_>, max_atom_bits: usize) -> Result<Decoded<'_>, Error> {
    let decoded = rub(input, max_atom_bits)?;
    if decoded.consumed != input.len() {
        return Err(Error::TrailingBits);
    }
    Ok(decoded)
}

/// Encoded field with its exact bit length; unused high bits in the final byte
/// are zero. Concatenation must use `bit_len`, not the byte length.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Encoded {
    pub bytes: Vec<u8>,
    pub bit_len: usize,
}

/// Encode a canonical atom. The payload and resulting allocation are bounded
/// before allocating; prefix overhead is at most twice the machine word width.
pub fn mat(atom: &Atom, max_atom_bits: usize) -> Result<Encoded, Error> {
    let bytes = atom.as_le_bytes();
    let len = match bytes.last() {
        None => 0,
        Some(last) => (bytes.len() - 1)
            .checked_mul(8)
            .and_then(|n| n.checked_add(8 - last.leading_zeros() as usize))
            .ok_or(Error::Limit)?,
    };
    if len > max_atom_bits {
        return Err(Error::Limit);
    }
    if len == 0 {
        return Ok(Encoded {
            bytes: vec![1],
            bit_len: 1,
        });
    }
    let width = (usize::BITS - len.leading_zeros()) as usize;
    let bit_len = len.checked_add(2 * width).ok_or(Error::Limit)?;
    let mut out = vec![0; bit_len.div_ceil(8)];
    let mut set = |index: usize| out[index / 8] |= 1 << (index % 8);
    set(width);
    for index in 0..width - 1 {
        if len & (1 << index) != 0 {
            set(width + 1 + index);
        }
    }
    for index in 0..len {
        if bytes[index / 8] & (1 << (index % 8)) != 0 {
            set(2 * width + index);
        }
    }
    Ok(Encoded {
        bytes: out,
        bit_len,
    })
}
