//! Byte-level cryptographic primitives, without peer admission or network policy.
//!
//! Current suite-C uses Ed25519 and Edwards-to-Montgomery agreement. Protocol
//! callers must supply the specified protected transcript; signatures alone do
//! not establish key currency or domain authorization. No packet signing API or
//! algorithm negotiation is defined here.
use curve25519_dalek::{
    edwards::{CompressedEdwardsY, EdwardsPoint},
    scalar::Scalar,
    traits::IsIdentity,
};
use ed25519_dalek::{Signer, SigningKey, VerifyingKey};
use sha2::{Digest, Sha256, Sha512};
use subtle::ConstantTimeEq;
use zeroize::{Zeroize, Zeroizing};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    InvalidPoint,
    InvalidSignature,
    InvalidAgreement,
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for Error {}

/// Canonically encoded, nonidentity, prime-subgroup Edwards public key.
#[derive(Clone, Debug)]
pub struct PublicKey {
    bytes: [u8; 32],
    point: EdwardsPoint,
}
impl PublicKey {
    pub fn from_bytes(bytes: [u8; 32]) -> Result<Self, Error> {
        let point = CompressedEdwardsY(bytes)
            .decompress()
            .ok_or(Error::InvalidPoint)?;
        if point.compress().to_bytes() != bytes || point.is_identity() || !point.is_torsion_free() {
            return Err(Error::InvalidPoint);
        }
        Ok(Self { bytes, point })
    }
    pub fn as_bytes(&self) -> &[u8; 32] {
        &self.bytes
    }

    /// Strict verification over exact bytes; no trailing-zero normalization.
    /// This rejects noncanonical scalars/points and weak keys rather than
    /// reproducing permissive historical verifier behavior.
    pub fn verify(&self, message: &[u8], signature: &[u8; 64]) -> Result<(), Error> {
        let key = VerifyingKey::from_bytes(&self.bytes).map_err(|_| Error::InvalidPoint)?;
        key.verify_strict(message, &ed25519_dalek::Signature::from_bytes(signature))
            .map_err(|_| Error::InvalidSignature)
    }

    /// Zuse +scap semantics: clear bit 255, then add scalar*B. This is specific
    /// to the current Edwards profile and is not a generic identity requirement.
    pub fn add_scalar(&self, mut scalar: [u8; 32]) -> Result<Self, Error> {
        scalar[31] &= 0x7f;
        let scalar = Scalar::from_bytes_mod_order(scalar);
        let point = self.point + EdwardsPoint::mul_base(&scalar);
        Self::from_bytes(point.compress().to_bytes())
    }
}

/// Seed-derived live key. Dalek zeroizes owned key storage on drop; this wrapper
/// deliberately exposes neither Debug nor Clone nor secret-key export.
pub struct KeyPair(SigningKey);
impl KeyPair {
    /// Seed is exactly 32 little-endian bytes, including high zeros (+luck).
    pub fn from_seed(seed: &[u8; 32]) -> Self {
        Self(SigningKey::from_bytes(seed))
    }
    pub fn public_key(&self) -> PublicKey {
        PublicKey::from_bytes(self.0.verifying_key().to_bytes()).expect("seed-derived public key")
    }
    /// Low-level trusted-caller primitive; callers own transcript construction.
    pub fn sign(&self, message: &[u8]) -> [u8; 64] {
        self.0.sign(message).to_bytes()
    }

    /// Raw +slar output, before any KDF. Must never be used directly as a channel
    /// key. The caller owns the profile's KDF and authenticated peer selection.
    pub fn agree(&self, peer: &PublicKey) -> Result<SharedSecret, Error> {
        let seed = Zeroizing::new(self.0.to_bytes());
        let mut digest = Sha512::digest(seed.as_ref());
        let mut scalar = Zeroizing::new([0; 32]);
        scalar.copy_from_slice(&digest[..32]);
        digest.zeroize();
        let shared = Zeroizing::new(peer.point.to_montgomery().mul_clamped(*scalar).to_bytes());
        if bool::from(shared.ct_eq(&[0; 32])) {
            return Err(Error::InvalidAgreement);
        }
        Ok(SharedSecret(shared))
    }
}

/// Raw shared secret, zeroized on drop and excluded from Debug/Clone.
pub struct SharedSecret(Zeroizing<[u8; 32]>);
impl SharedSecret {
    pub fn as_bytes(&self) -> &[u8; 32] {
        &self.0
    }
}

/// Exact-byte SHA-256. Atom normalization belongs to the calling profile.
pub fn sha256(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}
