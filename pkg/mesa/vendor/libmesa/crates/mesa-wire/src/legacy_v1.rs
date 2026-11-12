//! Bounded, borrowing codec for the v1 none/IPv4-trailer profile.
//! Parsing checks integrity and structure, not signatures or LSS proofs.

pub use crate::{Head, PactType, COOKIE, HEADER_LEN};

/// Historical Mesa wire version. Never selected by the default v2 decoder.
pub const VERSION: u8 = 1;

/// Parse a v1 header only. Does not verify its body checksum.
pub fn sift_head(bytes: &[u8]) -> Option<Head> {
    crate::sift_head_version(bytes, VERSION)
}
use mesa_noun::mug::mug_bytes;

/// The upstream codecs disagree on terminal zero bytes in the whole body.
/// Select explicitly; decoding never retries under another convention.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Checksum {
    HoonAtom,
    VereBytes,
}

impl Checksum {
    pub fn gum(self, mut body: &[u8]) -> u32 {
        if self == Self::HoonAtom {
            while body.last() == Some(&0) {
                body = &body[..body.len() - 1];
            }
        }
        mug_bytes(body) & 0x000f_ffff
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub max_packet: usize,
    pub max_path: usize,
    pub max_fragment: usize,
}

impl Limits {
    pub const DATAGRAM: Self = Self {
        max_packet: 1472,
        max_path: u16::MAX as usize,
        max_fragment: 1024,
    };
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Limit,
    Header,
    Checksum,
    Truncated,
    Reserved,
    UnsupportedNext,
    NonCanonical,
    EmptyPath,
    TrailingBytes,
    FragmentRange,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for Error {}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fragment {
    Init,
    Auth(u64),
    Data(u64),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Name<'a> {
    /// Numeric publisher name (@p). Width encoding grants no identity authority;
    /// the identity layer binds this name to a domain-authorized current point.
    pub ship: u128,
    pub rift: u32,
    /// Log2 of fragment capacity in bits. This codec does not shift by it.
    pub bloq: u8,
    pub fragment: Fragment,
    /// Encoded path cord, without a leading slash. Namespace parsing is separate.
    pub path: &'a [u8],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Auth<'a> {
    Signature(&'a [u8; 64]),
    None,
    Mac(&'a [u8; 16]),
    Pair(&'a [u8; 64]),
}

impl Auth<'_> {
    fn bytes(&self) -> &[u8] {
        match self {
            Self::Signature(b) | Self::Pair(b) => *b,
            Self::Mac(b) => *b,
            Self::None => &[],
        }
    }
    fn tag(&self) -> u8 {
        match self {
            Self::Signature(_) => 0,
            Self::None => 1,
            Self::Mac(_) => 2,
            Self::Pair(_) => 3,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Data<'a> {
    /// Logical message length; never used to allocate by this codec.
    pub total_bytes: u64,
    pub auth: Auth<'a>,
    /// Explicit-length wire bytes, including any terminal zeros.
    pub fragment: &'a [u8],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Body<'a> {
    Peek(Name<'a>),
    Page {
        name: Name<'a>,
        data: Data<'a>,
        /// Four little-endian address bytes followed by two port bytes.
        next: Option<[u8; 6]>,
    },
    Poke {
        ack: Name<'a>,
        payload: Name<'a>,
        data: Data<'a>,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Packet<'a> {
    /// Encoding saturates at seven, matching Hoon.
    pub hop: u8,
    pub body: Body<'a>,
}

struct Reader<'a> {
    rest: &'a [u8],
    limits: Limits,
}

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], Error> {
        let bytes = self.rest.get(..n).ok_or(Error::Truncated)?;
        self.rest = &self.rest[n..];
        Ok(bytes)
    }
    fn byte(&mut self) -> Result<u8, Error> {
        Ok(self.take(1)?[0])
    }
    fn uint(&mut self, n: usize) -> Result<u64, Error> {
        let mut bytes = [0; 8];
        bytes
            .get_mut(..n)
            .ok_or(Error::Limit)?
            .copy_from_slice(self.take(n)?);
        Ok(u64::from_le_bytes(bytes))
    }
    fn name(&mut self) -> Result<Name<'a>, Error> {
        let meta = self.byte()?;
        // Historically called "rank": only an integer-width tag at this layer.
        let ship_len = 2 << (meta & 3);
        let rift_len = ((meta >> 2) & 3) as usize + 1;
        let init = meta & 0x10 != 0;
        if init && meta & 0xe0 != 0 {
            return Err(Error::Reserved);
        }
        let mut ship_bytes = [0; 16];
        ship_bytes[..ship_len].copy_from_slice(self.take(ship_len)?);
        let ship = u128::from_le_bytes(ship_bytes);
        let rift = self.uint(rift_len)? as u32;
        if ship_width(ship) != ship_len || byte_width(rift as u64).max(1) != rift_len {
            return Err(Error::NonCanonical);
        }
        let bloq = self.byte()?;
        let fragment = if init {
            Fragment::Init
        } else {
            let n = 1 << (meta >> 6);
            let index = self.uint(n)?;
            if pow_width(index) != n {
                return Err(Error::NonCanonical);
            }
            if meta & 0x20 != 0 {
                Fragment::Auth(index)
            } else {
                Fragment::Data(index)
            }
        };
        let path_len = self.uint(2)? as usize;
        if path_len == 0 {
            return Err(Error::EmptyPath);
        }
        if path_len > self.limits.max_path {
            return Err(Error::Limit);
        }
        let path = self.take(path_len)?;
        Ok(Name {
            ship,
            rift,
            bloq,
            fragment,
            path,
        })
    }
    fn data(&mut self) -> Result<Data<'a>, Error> {
        let meta = self.byte()?;
        if meta & 0x30 != 0 {
            return Err(Error::Reserved);
        }
        let total_len = 1 << (meta & 3);
        let total_bytes = self.uint(total_len)?;
        if pow_width(total_bytes) != total_len {
            return Err(Error::NonCanonical);
        }
        let auth = match (meta >> 2) & 3 {
            0 => Auth::Signature(self.take(64)?.try_into().unwrap()),
            1 => Auth::None,
            2 => Auth::Mac(self.take(16)?.try_into().unwrap()),
            _ => Auth::Pair(self.take(64)?.try_into().unwrap()),
        };
        let tag = meta >> 6;
        let n = if tag == 3 {
            self.byte()? as usize
        } else {
            tag as usize
        };
        if n > 4 {
            return Err(Error::Limit);
        }
        if tag == 3 && n < 3 {
            return Err(Error::NonCanonical);
        }
        let length = self.uint(n)? as usize;
        if byte_width(length as u64) != n {
            return Err(Error::NonCanonical);
        }
        if length > self.limits.max_fragment {
            return Err(Error::Limit);
        }
        let fragment = self.take(length)?;
        Ok(Data {
            total_bytes,
            auth,
            fragment,
        })
    }
}

/// Parse a full packet with no allocation. Returned slices borrow the input.
/// Success establishes wire structure and checksum only, not authentication.
pub fn decode(bytes: &[u8], checksum: Checksum, limits: Limits) -> Result<Packet<'_>, Error> {
    if bytes.len() > limits.max_packet {
        return Err(Error::Limit);
    }
    let head = sift_head(bytes).ok_or(Error::Header)?;
    if checksum.gum(&bytes[HEADER_LEN..]) != head.gum {
        return Err(Error::Checksum);
    }
    if head.nex > 1 {
        return Err(Error::UnsupportedNext);
    }
    if head.typ != PactType::Page && head.nex != 0 {
        return Err(Error::Reserved);
    }
    let mut r = Reader {
        rest: &bytes[HEADER_LEN..],
        limits,
    };
    let name = r.name()?;
    let body = match head.typ {
        PactType::Peek => Body::Peek(name),
        PactType::Page => {
            let data = r.data()?;
            let next = if head.nex == 1 {
                Some(r.take(6)?.try_into().unwrap())
            } else {
                None
            };
            Body::Page { name, data, next }
        }
        PactType::Poke => {
            let payload = r.name()?;
            let data = r.data()?;
            Body::Poke {
                ack: name,
                payload,
                data,
            }
        }
    };
    if !r.rest.is_empty() {
        return Err(Error::TrailingBytes);
    }
    Ok(Packet {
        hop: head.hop,
        body,
    })
}

fn byte_width(n: u64) -> usize {
    (64 - n.leading_zeros() as usize).div_ceil(8)
}
fn pow_width(n: u64) -> usize {
    byte_width(n).max(1).next_power_of_two()
}
fn ship_width(n: u128) -> usize {
    ((128 - n.leading_zeros() as usize).div_ceil(8))
        .max(2)
        .next_power_of_two()
}
fn fragment_parts(fragment: Fragment) -> (u8, usize, u64) {
    match fragment {
        Fragment::Init => (0x10, 0, 0),
        Fragment::Auth(n) | Fragment::Data(n) => {
            let width = pow_width(n);
            let auth = if matches!(fragment, Fragment::Auth(_)) {
                0x20
            } else {
                0
            };
            (auth | ((width.trailing_zeros() as u8) << 6), width, n)
        }
    }
}

// First pass measures and validates; only then does encode allocate output.
struct Writer {
    bytes: Option<Vec<u8>>,
    len: usize,
    limits: Limits,
}
impl Writer {
    fn put(&mut self, bytes: &[u8]) -> Result<(), Error> {
        self.len = self.len.checked_add(bytes.len()).ok_or(Error::Limit)?;
        if self.len > self.limits.max_packet {
            return Err(Error::Limit);
        }
        if let Some(out) = &mut self.bytes {
            out.extend_from_slice(bytes);
        }
        Ok(())
    }
    fn uint(&mut self, n: u64, width: usize) -> Result<(), Error> {
        self.put(&n.to_le_bytes()[..width])
    }
    fn name(&mut self, name: Name<'_>) -> Result<(), Error> {
        if name.path.is_empty() {
            return Err(Error::EmptyPath);
        }
        if name.path.len() > self.limits.max_path.min(u16::MAX as usize) {
            return Err(Error::Limit);
        }
        let ship_len = ship_width(name.ship);
        let rift_len = byte_width(name.rift as u64).max(1);
        let (flags, frag_len, index) = fragment_parts(name.fragment);
        let meta = (ship_len.trailing_zeros() - 1) as u8 | (((rift_len - 1) as u8) << 2) | flags;
        self.put(&[meta])?;
        self.put(&name.ship.to_le_bytes()[..ship_len])?;
        self.uint(name.rift as u64, rift_len)?;
        self.put(&[name.bloq])?;
        self.uint(index, frag_len)?;
        self.uint(name.path.len() as u64, 2)?;
        self.put(name.path)
    }
    fn data(&mut self, data: Data<'_>) -> Result<(), Error> {
        let len = data.fragment.len();
        if len > self.limits.max_fragment || len > u32::MAX as usize {
            return Err(Error::Limit);
        }
        let total_len = pow_width(data.total_bytes);
        let len_len = byte_width(len as u64);
        let meta = total_len.trailing_zeros() as u8
            | (data.auth.tag() << 2)
            | ((len_len.min(3) as u8) << 6);
        self.put(&[meta])?;
        self.uint(data.total_bytes, total_len)?;
        self.put(data.auth.bytes())?;
        if len_len >= 3 {
            self.put(&[len_len as u8])?;
        }
        self.uint(len as u64, len_len)?;
        self.put(data.fragment)
    }
    fn body(&mut self, body: Body<'_>) -> Result<(), Error> {
        match body {
            Body::Peek(name) => self.name(name),
            Body::Page { name, data, next } => {
                self.name(name)?;
                self.data(data)?;
                if let Some(lane) = next {
                    self.put(&lane)?;
                }
                Ok(())
            }
            Body::Poke { ack, payload, data } => {
                self.name(ack)?;
                self.name(payload)?;
                self.data(data)
            }
        }
    }
}

/// Encode canonical integer widths while preserving explicit fragment bytes.
/// This is not a Hoon noun encoder: it does not normalize fragments to atoms.
pub fn encode(packet: &Packet<'_>, checksum: Checksum, limits: Limits) -> Result<Vec<u8>, Error> {
    let mut w = Writer {
        bytes: None,
        len: 0,
        limits,
    };
    w.put(&[0; HEADER_LEN])?;
    w.body(packet.body)?;
    let size = w.len;
    w.bytes = Some(Vec::with_capacity(size));
    w.len = 0;
    w.put(&[0; HEADER_LEN])?;
    w.body(packet.body)?;
    let mut bytes = w.bytes.unwrap();
    let (typ, next) = match packet.body {
        Body::Peek(_) => (PactType::Peek, 0),
        Body::Page { next, .. } => (PactType::Page, u32::from(next.is_some())),
        Body::Poke { .. } => (PactType::Poke, 0),
    };
    let word = (next << 2)
        | ((VERSION as u32) << 4)
        | ((typ as u32) << 7)
        | ((packet.hop.min(7) as u32) << 9)
        | (checksum.gum(&bytes[HEADER_LEN..]) << 12);
    bytes[..4].copy_from_slice(&word.to_le_bytes());
    bytes[4..8].copy_from_slice(&COOKIE.to_le_bytes());
    Ok(bytes)
}
