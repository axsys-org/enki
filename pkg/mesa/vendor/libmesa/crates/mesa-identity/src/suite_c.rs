//! Explicit current suite-C layout, pinned in spec/05-identity.md.
//!
//! Pass: 'c' | genesis-public[32] | live-public[32] | mat(dat) | evidence.
//! Ring: 'C' | live-seed[32] | genesis-public[32] | mat(dat) | evidence.
//! Domain evidence is opaque here. A registered domain owns its format and
//! authority checks; successful parsing cannot create an accepted identity.
use mesa_noun::{bits::Bits, mat};
use mesa_wire::DomainTag;

pub mod crypto;

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub max_blob_bytes: usize,
    pub max_dat_bits: usize,
    pub max_evidence_bits: usize,
}
impl Default for Limits {
    fn default() -> Self {
        Self {
            max_blob_bytes: 64 * 1024,
            max_dat_bits: 32 * 1024 * 8,
            max_evidence_bits: 32 * 1024 * 8,
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Limit,
    Truncated,
    NonCanonical,
    WrongSuite,
    InvalidDomain,
    DomainMismatch,
}
impl From<mat::Error> for Error {
    fn from(error: mat::Error) -> Self {
        match error {
            mat::Error::Limit => Self::Limit,
            mat::Error::Truncated => Self::Truncated,
            mat::Error::NonCanonical | mat::Error::TrailingBits => Self::NonCanonical,
        }
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for Error {}

/// Domain-committed data and mutable evidence, still entirely unverified.
#[derive(Debug)]
pub struct Claims<'a> {
    domain: [u8; 63],
    domain_len: usize,
    /// Complete immutable dat atom, including its mat(domain) prefix.
    dat: Bits<'a>,
    /// Opaque remainder after mat(domain), with its exact bit length.
    domain_tail: Bits<'a>,
    /// Mutable xtr atom, beginning immediately after mat(dat), without padding.
    evidence: Bits<'a>,
}
impl<'a> Claims<'a> {
    pub fn dat(&self) -> Bits<'a> {
        self.dat
    }
    pub fn domain_tail(&self) -> Bits<'a> {
        self.domain_tail
    }
    pub fn evidence(&self) -> Bits<'a> {
        self.evidence
    }
    pub fn domain(&self) -> DomainTag<'_> {
        // The parser alone constructs Claims and validates these immutable bytes.
        DomainTag::new(std::str::from_utf8(&self.domain[..self.domain_len]).unwrap()).unwrap()
    }
}
#[derive(Debug)]
pub struct UnverifiedPass<'a> {
    genesis_key: &'a [u8; 32],
    live_key: &'a [u8; 32],
    claims: Claims<'a>,
}

impl<'a> UnverifiedPass<'a> {
    pub fn genesis_key(&self) -> &'a [u8; 32] {
        self.genesis_key
    }
    pub fn live_key(&self) -> &'a [u8; 32] {
        self.live_key
    }
    pub fn claims(&self) -> &Claims<'a> {
        &self.claims
    }
}

/// Borrowed secret material. Deliberately has no Debug/Clone implementation and
/// never copies the seed. The caller owns and clears the original storage.
pub struct UnverifiedRing<'a> {
    live_seed: &'a [u8; 32],
    genesis_key: &'a [u8; 32],
    claims: Claims<'a>,
}
impl<'a> UnverifiedRing<'a> {
    pub fn genesis_key(&self) -> &'a [u8; 32] {
        self.genesis_key
    }
    pub fn claims(&self) -> &Claims<'a> {
        &self.claims
    }
    pub fn live_seed(&self) -> &'a [u8; 32] {
        self.live_seed
    }
}

fn claims<'a>(
    bytes: &'a [u8],
    tag: u8,
    expected_domain: DomainTag<'_>,
    limits: Limits,
) -> Result<Claims<'a>, Error> {
    if bytes.len() > limits.max_blob_bytes {
        return Err(Error::Limit);
    }
    if bytes.len() < 66 {
        return Err(Error::Truncated);
    }
    if bytes[0] != tag {
        return Err(Error::WrongSuite);
    }
    let all = Bits::from_atom_bytes(bytes)?;
    let field = mat::rub(all.slice(520, all.len() - 520)?, limits.max_dat_bits)?;
    let evidence_start = 520 + field.consumed;
    let evidence = all.slice(evidence_start, all.len() - evidence_start)?;
    if evidence.len() > limits.max_evidence_bits {
        return Err(Error::Limit);
    }
    let domain_field = mat::rub(field.atom, 63 * 8)?;
    let domain_len = domain_field.atom.len().div_ceil(8);
    let mut domain = [0; 63];
    for index in 0..domain_field.atom.len() {
        if domain_field.atom.bit(index) == Some(true) {
            domain[index / 8] |= 1 << (index % 8);
        }
    }
    let text = std::str::from_utf8(&domain[..domain_len]).map_err(|_| Error::InvalidDomain)?;
    let parsed = DomainTag::new(text).map_err(|_| Error::InvalidDomain)?;
    if parsed != expected_domain {
        return Err(Error::DomainMismatch);
    }
    Ok(Claims {
        domain,
        domain_len,
        dat: field.atom,
        domain_tail: field.atom.slice(
            domain_field.consumed,
            field.atom.len() - domain_field.consumed,
        )?,
        evidence,
    })
}

/// Parse canonical atom bytes and require the committed domain to match the
/// expected wire/configuration selector. Unknown canonical domains are allowed;
/// no registry lookup or fallback takes place. Keys are not curve-validated.
pub fn parse_pass<'a>(
    bytes: &'a [u8],
    expected_domain: DomainTag<'_>,
    limits: Limits,
) -> Result<UnverifiedPass<'a>, Error> {
    let claims = claims(bytes, b'c', expected_domain, limits)?;
    Ok(UnverifiedPass {
        genesis_key: bytes[1..33].try_into().unwrap(),
        live_key: bytes[33..65].try_into().unwrap(),
        claims,
    })
}

/// Parse only the current live-seed/genesis-public layout. The caller must
/// select this layout from trusted provenance. Historical two-secret-seed
/// rings use the SAME 'C' tag, so bytes alone cannot establish migration safety.
/// This function neither imports a legacy ring nor activates a key.
pub fn parse_current_ring<'a>(
    bytes: &'a [u8],
    expected_domain: DomainTag<'_>,
    limits: Limits,
) -> Result<UnverifiedRing<'a>, Error> {
    let claims = claims(bytes, b'C', expected_domain, limits)?;
    Ok(UnverifiedRing {
        live_seed: bytes[1..33].try_into().unwrap(),
        genesis_key: bytes[33..65].try_into().unwrap(),
        claims,
    })
}
