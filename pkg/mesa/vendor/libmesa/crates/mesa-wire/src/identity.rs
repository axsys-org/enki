//! Names and authority selectors are distinct. Neither type proves authority.
use crate::Error;

/// A globally unique claimed peer name, encoded as exactly 16 little-endian
/// bytes. Equality and hashing of identities depend on this value alone.
/// Allocation, domain association, and key ownership require the identity layer.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PeerId(pub u128);

/// Canonical ASCII authority selector: `[a-z][a-z0-9]*(?:-[a-z0-9]+)*`,
/// 1..=63 bytes. Unknown domains remain structurally valid. No normalization,
/// registry lookup, fallback, or authorization is performed here.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct DomainTag<'a>(&'a str);

impl<'a> DomainTag<'a> {
    pub fn new(value: &'a str) -> Result<Self, Error> {
        let bytes = value.as_bytes();
        if !(1..=63).contains(&bytes.len()) || !bytes[0].is_ascii_lowercase() {
            return Err(Error::InvalidDomain);
        }
        let mut after_hyphen = false;
        for &byte in &bytes[1..] {
            if byte.is_ascii_lowercase() || byte.is_ascii_digit() {
                after_hyphen = false;
            } else if byte == b'-' && !after_hyphen {
                after_hyphen = true;
            } else {
                return Err(Error::InvalidDomain);
            }
        }
        if after_hyphen {
            return Err(Error::InvalidDomain);
        }
        Ok(Self(value))
    }

    pub fn as_str(self) -> &'a str {
        self.0
    }
}
