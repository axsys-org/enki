//! Current suite-C cryptographic binding, separate from domain authorization.
//!
//! A valid point, derived name, or matching ring does not prove a pass is current,
//! authorize its life/rift, verify its evidence, or admit its holder to a scope.
use super::{UnverifiedPass, UnverifiedRing};
use mesa_crypto::{sha256, KeyPair, PublicKey};
use mesa_noun::bits::Bits;
use mesa_wire::PeerId;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    InvalidKey,
    NameMismatch,
    RingMismatch,
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for Error {}

/// Immutable parsed pass with curve-valid keys and its derived name. Its domain
/// evidence and claimed live key remain unauthorized. There is no activation API.
#[derive(Debug)]
pub struct BoundPass<'a> {
    pass: UnverifiedPass<'a>,
    live: PublicKey,
    name_key: PublicKey,
    name: PeerId,
}
impl<'a> BoundPass<'a> {
    pub fn new(pass: UnverifiedPass<'a>) -> Result<Self, Error> {
        let genesis = PublicKey::from_bytes(*pass.genesis_key).map_err(|_| Error::InvalidKey)?;
        let live = PublicKey::from_bytes(*pass.live_key).map_err(|_| Error::InvalidKey)?;
        // The parser bounds dat. Preserve genesis at exactly 32 bytes, while
        // dat is an atom (minimal bytes), as in +nam:ex:cric.
        let dat = pass
            .claims
            .dat
            .to_atom(pass.claims.dat.len().div_ceil(8))
            .unwrap();
        let mut input = Vec::with_capacity(32 + dat.met3());
        input.extend_from_slice(genesis.as_bytes());
        input.extend_from_slice(dat.as_le_bytes());
        let name_key = genesis
            .add_scalar(sha256(atom_bytes(&input)))
            .map_err(|_| Error::InvalidKey)?;
        let name = fingerprint(name_key.as_bytes());
        Ok(Self {
            pass,
            live,
            name_key,
            name,
        })
    }
    pub fn name(&self) -> PeerId {
        self.name
    }
    pub fn name_key(&self) -> &PublicKey {
        &self.name_key
    }
    pub fn live_key(&self) -> &PublicKey {
        &self.live
    }
    pub fn unverified(&self) -> &UnverifiedPass<'a> {
        &self.pass
    }
    pub fn require_name(&self, expected: PeerId) -> Result<(), Error> {
        if self.name == expected {
            Ok(())
        } else {
            Err(Error::NameMismatch)
        }
    }

    /// Check exact current-layout private/public correspondence, including xtr.
    /// Trusted layout provenance is still required at parse_current_ring. This
    /// does not select a life or install the resulting key anywhere.
    pub fn check_ring(&self, ring: &UnverifiedRing<'_>) -> Result<(), Error> {
        if ring.genesis_key != self.pass.genesis_key
            || !same_bits(ring.claims.dat, self.pass.claims.dat)
            || !same_bits(ring.claims.evidence, self.pass.claims.evidence)
            || KeyPair::from_seed(ring.live_seed).public_key().as_bytes() != self.live.as_bytes()
        {
            return Err(Error::RingMismatch);
        }
        Ok(())
    }
}

fn same_bits(a: Bits<'_>, b: Bits<'_>) -> bool {
    a.len() == b.len() && (0..a.len()).all(|i| a.bit(i) == b.bit(i))
}
fn atom_bytes(bytes: &[u8]) -> &[u8] {
    let end = bytes.iter().rposition(|b| *b != 0).map_or(0, |i| i + 1);
    &bytes[..end]
}
fn fingerprint(name_key: &[u8; 32]) -> PeerId {
    // +shaf %cfig nam: shax hashes minimal atom bytes; shas hashes exactly
    // 32 bytes after XOR with the little-endian salt, even with high zeros.
    let mut salted = sha256(atom_bytes(name_key));
    for (byte, salt) in salted.iter_mut().zip(b"cfig") {
        *byte ^= salt;
    }
    let hash = sha256(&salted);
    let folded = std::array::from_fn(|i| hash[i] ^ hash[i + 16]);
    PeerId(u128::from_le_bytes(folded))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn atom_hashing_retains_internal_zeros_and_removes_only_high_zeros() {
        assert_eq!(atom_bytes(&[1, 0, 2, 0, 0]), &[1, 0, 2]);
        assert_eq!(atom_bytes(&[0; 32]), &[]);
        assert_ne!(fingerprint(&[0; 32]), PeerId(0));
    }
}
