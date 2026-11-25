//! Atoms as little-endian byte strings.
//!
//! Numeric atoms are distinct from explicit-length wire byte strings.

/// A natural number as canonical little-endian bytes (no trailing zeros).
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub struct Atom(Vec<u8>);

impl Atom {
    /// Normalize little-endian bytes. Zero has an empty representation.
    pub fn from_le_bytes(mut bytes: Vec<u8>) -> Self {
        while bytes.last() == Some(&0) {
            bytes.pop();
        }
        Self(bytes)
    }

    /// Read the canonical representation without permitting mutation.
    pub fn as_le_bytes(&self) -> &[u8] {
        &self.0
    }

    /// Byte length (`met 3` in Hoon terms).
    pub fn met3(&self) -> usize {
        self.0.len()
    }
}

#[cfg(test)]
mod tests {
    use super::Atom;

    #[test]
    fn canonical_numbers() {
        assert_eq!(Atom::from_le_bytes(vec![0, 0]), Atom::default());
        assert_eq!(Atom::from_le_bytes(vec![0]).met3(), 0);
        let one = Atom::from_le_bytes(vec![1, 0, 0]);
        assert_eq!(one, Atom::from_le_bytes(vec![1]));
        assert_eq!(one.met3(), 1);
        assert_eq!(Atom::from_le_bytes(vec![0, 1]).as_le_bytes(), &[0, 1]);
    }
}
