//! Bounded, canonical jam/cue with a borrowed arena and preserved sharing.
//!
//! No recursive parsing, traversal, or destruction is used. Backreferences may
//! name only completed literal nouns. Limits apply to the expanded noun depth
//! as well as the encoded work; references never expand into copied subtrees.
use crate::{bits::Bits, mat};
use std::collections::{BTreeMap, HashMap};
use std::hash::{Hash, Hasher};

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub max_input_bits: usize,
    pub max_output_bits: usize,
    /// Literal occurrences, including cells awaiting completion.
    pub max_nodes: usize,
    /// Root counts as one; includes the depth hidden behind backreferences.
    pub max_depth: usize,
    pub max_atom_bits: usize,
    pub max_backrefs: usize,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_input_bits: 64 * 1024 * 8,
            max_output_bits: 64 * 1024 * 8,
            max_nodes: 16 * 1024,
            max_depth: 256,
            max_atom_bits: 64 * 1024 * 8,
            max_backrefs: 16 * 1024,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Limit,
    Truncated,
    NonCanonical,
    TrailingBits,
    InvalidReference,
}
impl From<mat::Error> for Error {
    fn from(error: mat::Error) -> Self {
        match error {
            mat::Error::Limit => Self::Limit,
            mat::Error::Truncated => Self::Truncated,
            mat::Error::NonCanonical => Self::NonCanonical,
            mat::Error::TrailingBits => Self::TrailingBits,
        }
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for Error {}

/// Child indices address this noun's arena; they always precede their parent.
#[derive(Clone, Copy, Debug)]
pub enum Node<'a> {
    Atom(Bits<'a>),
    Cell { head: usize, tail: usize },
}

/// Immutable acyclic graph. Atoms borrow the input; cells contain indices,
/// so dropping even a deeply nested noun does not recurse.
/// Consumers must retain sharing and bound their own visits: the expanded tree
/// can contain exponentially more occurrences than this arena has nodes.
#[derive(Debug)]
pub struct Noun<'a> {
    nodes: Vec<Node<'a>>,
    root: usize,
}
impl<'a> Noun<'a> {
    pub fn root(&self) -> usize {
        self.root
    }
    pub fn node(&self, index: usize) -> Option<Node<'a>> {
        self.nodes.get(index).copied()
    }
    pub fn node_count(&self) -> usize {
        self.nodes.len()
    }
}

struct Frame {
    start: usize,
    head: Option<usize>,
}

/// Decode exactly one canonically jammed noun. Limits gate each stack, arena,
/// and reference-table insertion. The final jam comparison also enforces the
/// kernel's literal-versus-backreference choice; no alternate spelling passes.
pub fn cue(input: Bits<'_>, limits: Limits) -> Result<Noun<'_>, Error> {
    if input.len() > limits.max_input_bits || limits.max_depth == 0 {
        return Err(Error::Limit);
    }
    let mut cursor = 0;
    let mut literals = 0usize;
    let mut backrefs = 0usize;
    let mut stack: Vec<Frame> = Vec::new();
    let mut nodes = Vec::new();
    let mut heights: Vec<usize> = Vec::new();
    let mut offsets = BTreeMap::new();
    let root = 'parse: loop {
        let start = cursor;
        let first = input.bit(cursor).ok_or(Error::Truncated)?;
        cursor += 1;
        let mut current = if !first {
            if literals == limits.max_nodes {
                return Err(Error::Limit);
            }
            literals += 1;
            let field = mat::rub(
                input.slice(cursor, input.len() - cursor)?,
                limits.max_atom_bits,
            )?;
            cursor += field.consumed;
            let id = nodes.len();
            nodes.push(Node::Atom(field.atom));
            heights.push(1);
            offsets.insert(start, id);
            id
        } else {
            let second = input.bit(cursor).ok_or(Error::Truncated)?;
            cursor += 1;
            if !second {
                if literals == limits.max_nodes || stack.len() + 1 >= limits.max_depth {
                    return Err(Error::Limit);
                }
                literals += 1;
                stack.push(Frame { start, head: None });
                continue;
            }
            if backrefs == limits.max_backrefs {
                return Err(Error::Limit);
            }
            backrefs += 1;
            let field = mat::rub(
                input.slice(cursor, input.len() - cursor)?,
                usize::BITS as usize,
            )?;
            cursor += field.consumed;
            let offset = field.atom.word()?;
            // Only completed literals are registered: no forward, cyclic,
            // interior-bit, or reference-to-reference addresses are valid.
            *offsets.get(&offset).ok_or(Error::InvalidReference)?
        };
        loop {
            if heights[current] > limits.max_depth - stack.len() {
                return Err(Error::Limit);
            }
            let Some(frame) = stack.last_mut() else {
                break 'parse current;
            };
            let Some(head) = frame.head else {
                frame.head = Some(current);
                break;
            };
            let start = frame.start;
            stack.pop();
            let height = heights[head]
                .max(heights[current])
                .checked_add(1)
                .ok_or(Error::Limit)?;
            let id = nodes.len();
            nodes.push(Node::Cell {
                head,
                tail: current,
            });
            heights.push(height);
            offsets.insert(start, id);
            current = id;
        }
    };
    if cursor != input.len() {
        return Err(Error::TrailingBits);
    }
    let noun = Noun { nodes, root };
    let encoded = jam(&noun, limits)?;
    if encoded.bit_len != input.len()
        || (0..input.len())
            .any(|i| input.bit(i) != Some(encoded.bytes[i / 8] & (1 << (i % 8)) != 0))
    {
        return Err(Error::NonCanonical);
    }
    Ok(noun)
}

// Bit spans compare by value, independent of source buffer or alignment.
#[derive(Clone, Copy, Debug)]
struct AtomKey<'a>(Bits<'a>);
impl PartialEq for AtomKey<'_> {
    fn eq(&self, other: &Self) -> bool {
        self.0.len() == other.0.len() && (0..self.0.len()).all(|i| self.0.bit(i) == other.0.bit(i))
    }
}
impl Eq for AtomKey<'_> {}
impl Hash for AtomKey<'_> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.0.len().hash(state);
        for start in (0..self.0.len()).step_by(8) {
            let mut byte = 0u8;
            for bit in 0..8 {
                if self.0.bit(start + bit) == Some(true) {
                    byte |= 1 << bit;
                }
            }
            byte.hash(state);
        }
    }
}
#[derive(Hash, PartialEq, Eq)]
enum Key<'a> {
    Atom(AtomKey<'a>),
    Cell(usize, usize),
}

struct Writer {
    bytes: Vec<u8>,
    len: usize,
    max: usize,
}
impl Writer {
    fn bit(&mut self, value: bool) -> Result<(), Error> {
        if self.len == self.max {
            return Err(Error::Limit);
        }
        if self.len.is_multiple_of(8) {
            self.bytes.push(0);
        }
        if value {
            self.bytes[self.len / 8] |= 1 << (self.len % 8);
        }
        self.len += 1;
        Ok(())
    }
    fn mat(&mut self, atom: Bits<'_>) -> Result<(), Error> {
        if atom.is_empty() {
            return self.bit(true);
        }
        let len = atom.len();
        let width = (usize::BITS - len.leading_zeros()) as usize;
        // Reject the complete write before extending the output buffer.
        let size = len.checked_add(2 * width).ok_or(Error::Limit)?;
        if size > self.max - self.len {
            return Err(Error::Limit);
        }
        for _ in 0..width {
            self.bit(false)?;
        }
        self.bit(true)?;
        for i in 0..width - 1 {
            self.bit(len & (1 << i) != 0)?;
        }
        for i in 0..len {
            self.bit(atom.bit(i) == Some(true))?;
        }
        Ok(())
    }
}

/// Re-encode a decoded noun with Hoon's canonical sharing choices. Structural
/// equivalence is interned bottom-up, so equal but distinct literal nodes share
/// one identity. Traversal and interning stay bounded without expanding the DAG.
pub fn jam(noun: &Noun<'_>, limits: Limits) -> Result<mat::Encoded, Error> {
    if noun.nodes.len() > limits.max_nodes {
        return Err(Error::Limit);
    }
    let mut classes = Vec::new();
    let mut heights: Vec<usize> = Vec::new();
    let mut keys = HashMap::new();
    for node in &noun.nodes {
        let (key, height) = match *node {
            Node::Atom(atom) => {
                if atom.len() > limits.max_atom_bits {
                    return Err(Error::Limit);
                }
                (Key::Atom(AtomKey(atom)), 1)
            }
            Node::Cell { head, tail } => (
                Key::Cell(classes[head], classes[tail]),
                heights[head]
                    .max(heights[tail])
                    .checked_add(1)
                    .ok_or(Error::Limit)?,
            ),
        };
        if height > limits.max_depth {
            return Err(Error::Limit);
        }
        heights.push(height);
        let next = keys.len();
        classes.push(*keys.entry(key).or_insert(next));
    }
    let mut writer = Writer {
        bytes: Vec::new(),
        len: 0,
        max: limits.max_output_bits,
    };
    let mut seen = HashMap::new();
    let mut stack = vec![noun.root];
    let mut backrefs = 0;
    while let Some(id) = stack.pop() {
        let node = noun.nodes[id];
        let class = classes[id];
        if let Some(&offset) = seen.get(&class) {
            let width = (usize::BITS - usize::leading_zeros(offset)) as usize;
            if !matches!(node, Node::Atom(atom) if atom.len() <= width) {
                if backrefs == limits.max_backrefs {
                    return Err(Error::Limit);
                }
                backrefs += 1;
                writer.bit(true)?;
                writer.bit(true)?;
                writer.mat(Bits::new(&offset.to_le_bytes(), width)?)?;
                continue;
            }
        } else {
            seen.insert(class, writer.len);
        }
        match node {
            Node::Atom(atom) => {
                writer.bit(false)?;
                writer.mat(atom)?;
            }
            Node::Cell { head, tail } => {
                writer.bit(true)?;
                writer.bit(false)?;
                stack.push(tail);
                stack.push(head);
            }
        }
    }
    Ok(mat::Encoded {
        bytes: writer.bytes,
        bit_len: writer.len,
    })
}
