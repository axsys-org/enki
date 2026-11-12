//! Borrowed LSB-first bit spans. Explicit lengths distinguish padding from data.
use crate::{atom::Atom, mat::Error};

#[derive(Clone, Copy)]
pub struct Bits<'a> {
    bytes: &'a [u8],
    start: usize,
    len: usize,
}

// The backing buffer can contain private fields outside this view (for example
// a ring seed before dat). Debug must never dump it, even for a public subspan.
impl std::fmt::Debug for Bits<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Bits")
            .field("len", &self.len)
            .finish_non_exhaustive()
    }
}

impl<'a> Bits<'a> {
    /// View a canonical little-endian atom without copying it. Zero is empty;
    /// an explicit high zero byte is noncanonical. The final byte's unused bits
    /// are excluded by the atom's numeric bit length.
    pub fn from_atom_bytes(bytes: &'a [u8]) -> Result<Self, Error> {
        let len = match bytes.last() {
            None => 0,
            Some(0) => return Err(Error::NonCanonical),
            Some(last) => (bytes.len() - 1)
                .checked_mul(8)
                .and_then(|n| n.checked_add(8 - last.leading_zeros() as usize))
                .ok_or(Error::Limit)?,
        };
        Self::new(bytes, len)
    }

    /// Select the low `len` bits. Bits outside that span are not inspected.
    pub fn new(bytes: &'a [u8], len: usize) -> Result<Self, Error> {
        let available = bytes.len().checked_mul(8).ok_or(Error::Limit)?;
        if len > available {
            return Err(Error::Truncated);
        }
        Ok(Self {
            bytes,
            start: 0,
            len,
        })
    }

    pub fn len(self) -> usize {
        self.len
    }

    pub fn is_empty(self) -> bool {
        self.len == 0
    }

    pub fn bit(self, index: usize) -> Option<bool> {
        if index >= self.len {
            return None;
        }
        let absolute = self.start + index;
        Some(self.bytes[absolute / 8] & (1 << (absolute % 8)) != 0)
    }

    pub fn slice(self, start: usize, len: usize) -> Result<Self, Error> {
        if start > self.len || len > self.len - start {
            return Err(Error::Truncated);
        }
        Ok(Self {
            bytes: self.bytes,
            start: self.start + start,
            len,
        })
    }

    /// Read at most one machine word; larger spans are not integers here.
    pub(crate) fn word(self) -> Result<usize, Error> {
        if self.len > usize::BITS as usize {
            return Err(Error::Limit);
        }
        let mut value = 0;
        for index in 0..self.len {
            if self.bit(index) == Some(true) {
                value |= 1 << index;
            }
        }
        Ok(value)
    }

    /// Copy a bounded span into a canonical atom. Parsing itself never allocates.
    /// This can strip high zero bits; it does not validate an encoding.
    pub fn to_atom(self, max_bytes: usize) -> Result<Atom, Error> {
        let len = self.len.div_ceil(8);
        if len > max_bytes {
            return Err(Error::Limit);
        }
        let mut bytes = vec![0; len];
        for index in 0..self.len {
            if self.bit(index) == Some(true) {
                bytes[index / 8] |= 1 << (index % 8);
            }
        }
        Ok(Atom::from_le_bytes(bytes))
    }
}
