//! Mesa LSS at bloq 13: BLAKE3 chunk chaining values (without ROOT), a
//! left spine, and scheduled pairs that reveal future subtrees. No signatures.
//! Nonempty trees follow Vere io/lss.c at 96fae0a. The v2 empty-value extension
//! commits to BLAKE3(empty), explicitly separated by the signed total length.
use blake3::hazmat::{merge_subtrees_non_root, HasherExt, Mode};

pub const LEAF_BYTES: usize = 1024;
pub const MAX_LEAVES: usize = 1024;
pub type Hash = [u8; 32];
pub type Pair = [Hash; 2];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Limit,
    Proof,
    Fragment,
    Pair,
    Complete,
}

fn count(total: usize) -> Result<usize, Error> {
    let n = total.div_ceil(LEAF_BYTES).max(1);
    if n > MAX_LEAVES {
        Err(Error::Limit)
    } else {
        Ok(n)
    }
}
pub fn proof_hashes(total: usize) -> Result<usize, Error> {
    let n = count(total)?;
    Ok(1 + (usize::BITS - (n - 1).leading_zeros()) as usize)
}
fn parent(left: &Hash, right: &Hash) -> Hash {
    merge_subtrees_non_root(left, right, Mode::Hash)
}
fn leaf(bytes: &[u8], index: usize) -> Hash {
    if bytes.is_empty() {
        return *blake3::hash(&[]).as_bytes();
    }
    blake3::Hasher::new()
        .set_input_offset((index * LEAF_BYTES) as u64)
        .update(bytes)
        .finalize_non_root()
}
fn split(count: usize) -> usize {
    1 << (usize::BITS - (count - 1).leading_zeros() - 1)
}
pub fn expects_pair(leaves: usize, index: usize) -> bool {
    index != 0
        && index < leaves
        && leaves <= MAX_LEAVES
        && index + (2usize << index.trailing_zeros()) < leaves
}

pub struct Tree {
    proof: Vec<Hash>,
    pairs: Vec<Option<Pair>>,
}
impl Tree {
    pub fn build(bytes: &[u8]) -> Result<Self, Error> {
        let n = count(bytes.len())?;
        let mut tree = Self {
            proof: vec![[0; 32]; proof_hashes(bytes.len())?],
            pairs: vec![None; n],
        };
        tree.subtree(bytes, 0, n);
        Ok(tree)
    }
    fn subtree(&mut self, bytes: &[u8], start: usize, n: usize) -> Hash {
        if n == 1 {
            let hash = leaf(
                &bytes[start * LEAF_BYTES..bytes.len().min((start + 1) * LEAF_BYTES)],
                start,
            );
            if start == 0 {
                self.proof[0] = hash;
            }
            return hash;
        }
        let width = split(n);
        let left = self.subtree(bytes, start, width);
        let right = self.subtree(bytes, start + width, n - width);
        if start == 0 {
            self.proof[width.trailing_zeros() as usize + 1] = right;
        } else {
            self.pairs[start - width] = Some([left, right]);
        }
        parent(&left, &right)
    }
    pub fn root(&self) -> Hash {
        root(&self.proof)
    }
    pub fn proof(&self) -> Vec<u8> {
        self.proof.iter().flatten().copied().collect()
    }
    pub fn pair(&self, index: usize) -> Option<&Pair> {
        self.pairs.get(index).and_then(Option::as_ref)
    }
    /// Allocated proof/pair payload, excluding Vec bookkeeping.
    pub fn storage_bytes(&self) -> usize {
        self.proof.len() * 32 + self.pairs.len() * std::mem::size_of::<Option<Pair>>()
    }
}
fn root(proof: &[Hash]) -> Hash {
    proof[1..]
        .iter()
        .fold(proof[0], |left, right| parent(&left, right))
}

#[derive(Clone)]
struct Expected {
    start: usize,
    end: usize,
    hash: Hash,
}

/// In-order verifier. A failed ingest leaves its state unchanged. The frontier
/// contains O(log leaves) hashes. The caller separately bounds out-of-order data.
#[derive(Clone)]
pub struct Verifier {
    total: usize,
    leaves: usize,
    next: usize,
    frontier: Vec<Expected>,
    root: Hash,
}
impl Verifier {
    pub fn new(total: usize, proof: &[u8]) -> Result<Self, Error> {
        let leaves = count(total)?;
        if proof.len() != proof_hashes(total)? * 32 {
            return Err(Error::Proof);
        }
        let (hashes, _) = proof.as_chunks::<32>();
        let mut frontier = vec![Expected {
            start: 0,
            end: 1,
            hash: hashes[0],
        }];
        for (i, hash) in hashes.iter().enumerate().skip(1) {
            let start = 1 << (i - 1);
            frontier.push(Expected {
                start,
                end: (start * 2).min(leaves),
                hash: *hash,
            });
        }
        Ok(Self {
            total,
            leaves,
            next: 0,
            frontier,
            root: root(hashes),
        })
    }
    pub fn root(&self) -> Hash {
        self.root
    }
    pub fn next_index(&self) -> usize {
        self.next
    }
    pub fn complete(&self) -> bool {
        self.next == self.leaves
    }
    pub fn ingest(&mut self, bytes: &[u8], pair: Option<&Pair>) -> Result<(), Error> {
        if self.complete() {
            return Err(Error::Complete);
        }
        let expected_len = self
            .total
            .saturating_sub(self.next * LEAF_BYTES)
            .min(LEAF_BYTES);
        if bytes.len() != expected_len {
            return Err(Error::Fragment);
        }
        let current = self
            .frontier
            .iter()
            .position(|e| e.start == self.next && e.end == self.next + 1)
            .ok_or(Error::Proof)?;
        if leaf(bytes, self.next) != self.frontier[current].hash {
            return Err(Error::Fragment);
        }
        if pair.is_some() != expects_pair(self.leaves, self.next) {
            return Err(Error::Pair);
        }
        if let Some(pair) = pair {
            let width = 1 << self.next.trailing_zeros();
            let start = self.next + width;
            let end = (start + width * 2).min(self.leaves);
            let target = self
                .frontier
                .iter()
                .position(|e| e.start == start && e.end == end)
                .ok_or(Error::Pair)?;
            if parent(&pair[0], &pair[1]) != self.frontier[target].hash {
                return Err(Error::Pair);
            }
            self.frontier[target] = Expected {
                start,
                end: start + width,
                hash: pair[0],
            };
            self.frontier.push(Expected {
                start: start + width,
                end,
                hash: pair[1],
            });
        }
        self.frontier.remove(current);
        self.next += 1;
        Ok(())
    }
}
