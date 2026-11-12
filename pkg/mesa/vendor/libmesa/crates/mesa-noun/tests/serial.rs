use mesa_noun::{
    bits::Bits,
    mat,
    serial::{self, Error, Limits, Node},
};
use serde_json::Value;

fn hex(s: &str) -> Vec<u8> {
    s.as_bytes()
        .as_chunks::<2>()
        .0
        .iter()
        .map(|p| u8::from_str_radix(std::str::from_utf8(p).unwrap(), 16).unwrap())
        .collect()
}
fn cases() -> Vec<Value> {
    serde_json::from_str::<Value>(include_str!("../../../vectors/noun/jam.json")).unwrap()["cases"]
        .as_array()
        .unwrap()
        .clone()
}
fn check_tree(noun: &serial::Noun<'_>, id: usize, expected: &Value) {
    match noun.node(id).unwrap() {
        Node::Atom(atom) => assert_eq!(
            atom.to_atom(4096).unwrap().as_le_bytes(),
            hex(expected.as_str().unwrap())
        ),
        Node::Cell { head, tail } => {
            assert!(head < id && tail < id);
            check_tree(noun, head, &expected[0]);
            check_tree(noun, tail, &expected[1]);
        }
    }
}

#[test]
fn canonical_vectors_match_structure_and_bytes() {
    for case in cases() {
        let bytes = hex(case["encoded"].as_str().unwrap());
        let input = Bits::from_atom_bytes(&bytes).unwrap();
        assert_eq!(input.len(), case["bits"].as_u64().unwrap() as usize);
        let noun = serial::cue(input, Limits::default()).unwrap();
        check_tree(&noun, noun.root(), &case["noun"]);
        assert_eq!(serial::jam(&noun, Limits::default()).unwrap().bytes, bytes);
    }
    assert_eq!(
        serial::cue(Bits::from_atom_bytes(&[2]).unwrap(), Limits::default())
            .unwrap()
            .node_count(),
        1
    );
}

#[test]
fn historical_live_hoon_jams_roundtrip() {
    let cases: Vec<Value> =
        serde_json::from_str(include_str!("../../../vectors/pact/v0-basic.json")).unwrap();
    assert_eq!(cases.len(), 10);
    for case in cases {
        let bytes = hex(case["jam"].as_str().unwrap());
        let noun = serial::cue(Bits::from_atom_bytes(&bytes).unwrap(), Limits::default()).unwrap();
        assert_eq!(
            serial::jam(&noun, Limits::default()).unwrap().bytes,
            bytes,
            "{}",
            case["name"]
        );
    }
}

#[test]
fn all_bit_truncations_and_trailing_bits_fail() {
    for case in cases() {
        let mut bytes = hex(case["encoded"].as_str().unwrap());
        let len = case["bits"].as_u64().unwrap() as usize;
        for end in 0..len {
            assert!(serial::cue(Bits::new(&bytes, end).unwrap(), Limits::default()).is_err());
        }
        bytes.push(0);
        assert_eq!(
            serial::cue(Bits::new(&bytes, len + 1).unwrap(), Limits::default()).unwrap_err(),
            Error::TrailingBits
        );
    }
    assert_eq!(
        Bits::from_atom_bytes(&[2, 0]).unwrap_err(),
        mat::Error::NonCanonical
    );
}

// Test builder deliberately permits noncanonical/reference-invalid streams.
#[derive(Default)]
struct Stream(Vec<bool>);
impl Stream {
    fn bit(&mut self, bit: bool) {
        self.0.push(bit);
    }
    fn cell(&mut self) {
        self.bit(true);
        self.bit(false);
    }
    fn field(&mut self, value: u64) {
        let atom = mesa_noun::atom::Atom::from_le_bytes(value.to_le_bytes().to_vec());
        let enc = mat::mat(&atom, 64).unwrap();
        for i in 0..enc.bit_len {
            self.bit(enc.bytes[i / 8] & (1 << (i % 8)) != 0);
        }
    }
    fn atom(&mut self, value: u64) {
        self.bit(false);
        self.field(value);
    }
    fn reference(&mut self, offset: usize) {
        self.bit(true);
        self.bit(true);
        self.field(offset as u64);
    }
    fn bytes(&self) -> Vec<u8> {
        let mut bytes = vec![0; self.0.len().div_ceil(8)];
        for (i, bit) in self.0.iter().enumerate() {
            if *bit {
                bytes[i / 8] |= 1 << (i % 8);
            }
        }
        bytes
    }
    fn error(&self, limits: Limits) -> Error {
        serial::cue(Bits::new(&self.bytes(), self.0.len()).unwrap(), limits).unwrap_err()
    }
}

#[test]
fn reject_invalid_references_and_alternate_encodings() {
    for offset in [0, 1, 2, 999] {
        let mut stream = Stream::default();
        stream.reference(offset);
        assert_eq!(stream.error(Limits::default()), Error::InvalidReference);
    }
    // Pending root, atom interior, and future offsets are not completed nouns.
    for offset in [0, 1, 3, 99] {
        let mut stream = Stream::default();
        stream.cell();
        stream.atom(255);
        stream.reference(offset);
        assert_eq!(stream.error(Limits::default()), Error::InvalidReference);
    }
    // Canonical [255 255] uses a reference to the first atom, not a second literal.
    let mut stream = Stream::default();
    stream.cell();
    stream.atom(255);
    stream.atom(255);
    assert_eq!(stream.error(Limits::default()), Error::NonCanonical);
    // Canonical [0 0] repeats the short literal rather than using a reference.
    let mut stream = Stream::default();
    stream.cell();
    stream.atom(0);
    stream.reference(2);
    assert_eq!(stream.error(Limits::default()), Error::NonCanonical);
    // A reference token does not itself enter the kernel's offset dictionary.
    let mut stream = Stream::default();
    stream.cell();
    stream.atom(255);
    stream.cell();
    let reference_at = stream.0.len();
    stream.reference(2);
    stream.reference(reference_at);
    assert_eq!(stream.error(Limits::default()), Error::InvalidReference);
}

#[test]
fn every_resource_limit_is_enforced() {
    let mut stream = Stream::default();
    stream.cell();
    stream.atom(255);
    stream.reference(2);
    let bytes = stream.bytes();
    let input = Bits::new(&bytes, stream.0.len()).unwrap();
    let exact = Limits {
        max_input_bits: input.len(),
        max_output_bits: input.len(),
        max_nodes: 2,
        max_depth: 2,
        max_atom_bits: 8,
        max_backrefs: 1,
    };
    let noun = serial::cue(input, exact).unwrap();
    for limits in [
        Limits {
            max_input_bits: input.len() - 1,
            ..exact
        },
        Limits {
            max_output_bits: input.len() - 1,
            ..exact
        },
        Limits {
            max_nodes: 1,
            ..exact
        },
        Limits {
            max_depth: 1,
            ..exact
        },
        Limits {
            max_atom_bits: 7,
            ..exact
        },
        Limits {
            max_backrefs: 0,
            ..exact
        },
    ] {
        assert_eq!(serial::cue(input, limits).unwrap_err(), Error::Limit);
    }
    for limits in [
        Limits {
            max_output_bits: input.len() - 1,
            ..exact
        },
        Limits {
            max_nodes: 1,
            ..exact
        },
        Limits {
            max_depth: 1,
            ..exact
        },
        Limits {
            max_atom_bits: 7,
            ..exact
        },
        Limits {
            max_backrefs: 0,
            ..exact
        },
    ] {
        assert_eq!(serial::jam(&noun, limits).unwrap_err(), Error::Limit);
    }
}

#[test]
fn references_preserve_sharing_and_enforce_expanded_depth() {
    // [[0 1] [reference-to-[0 1] 2]] has nesting depth 3 in its literal stream
    // but an expanded depth of 4 because the referenced cell is itself nested.
    let mut stream = Stream::default();
    stream.cell();
    stream.cell();
    stream.atom(0);
    stream.atom(1);
    stream.cell();
    stream.reference(2);
    stream.atom(2);
    let bytes = stream.bytes();
    let input = Bits::new(&bytes, stream.0.len()).unwrap();
    assert_eq!(
        serial::cue(
            input,
            Limits {
                max_depth: 3,
                ..Limits::default()
            }
        )
        .unwrap_err(),
        Error::Limit
    );
    let noun = serial::cue(
        input,
        Limits {
            max_depth: 4,
            ..Limits::default()
        },
    )
    .unwrap();
    let Node::Cell {
        head: shared,
        tail: outer,
    } = noun.node(noun.root()).unwrap()
    else {
        panic!()
    };
    let Node::Cell {
        head: reference, ..
    } = noun.node(outer).unwrap()
    else {
        panic!()
    };
    assert_eq!(shared, reference);
}

#[test]
fn exponentially_shared_noun_uses_linear_storage_and_work() {
    // Each level is [previous previous]. Expanding this noun would produce
    // 2^64 leaves, but the encoded graph needs only 65 literal nodes.
    let levels = 64;
    let mut stream = Stream::default();
    for _ in 0..levels {
        stream.cell();
    }
    stream.atom(65535);
    for child in (1..=levels).rev() {
        stream.reference(2 * child);
    }
    let bytes = stream.bytes();
    let input = Bits::new(&bytes, stream.0.len()).unwrap();
    let limits = Limits {
        max_nodes: levels + 1,
        max_depth: levels + 1,
        max_backrefs: levels,
        ..Limits::default()
    };
    let noun = serial::cue(input, limits).unwrap();
    assert_eq!(noun.node_count(), levels + 1);
    for index in 1..noun.node_count() {
        let Node::Cell { head, tail } = noun.node(index).unwrap() else {
            panic!()
        };
        assert_eq!(head, tail);
        assert_eq!(head, index - 1);
    }
    assert_eq!(serial::jam(&noun, limits).unwrap().bytes, bytes);
    assert_eq!(
        serial::cue(
            input,
            Limits {
                max_nodes: levels,
                ..limits
            }
        )
        .unwrap_err(),
        Error::Limit
    );
}

#[test]
fn deep_inputs_do_not_recurse_and_mutations_never_escape_bounds() {
    let mut stream = Stream::default();
    for _ in 0..2048 {
        stream.cell();
    }
    for _ in 0..2049 {
        stream.atom(0);
    }
    let bytes = stream.bytes();
    let input = Bits::new(&bytes, stream.0.len()).unwrap();
    assert_eq!(
        serial::cue(input, Limits::default()).unwrap_err(),
        Error::Limit
    );
    let limits = Limits {
        max_depth: 2049,
        ..Limits::default()
    };
    let noun = serial::cue(input, limits).unwrap();
    assert_eq!(noun.node_count(), 4097);
    drop(noun);
    let mut seed = 0x123456789abcdef0u64;
    for length in 0..128 {
        for _ in 0..32 {
            let mut bytes = vec![0; length];
            for byte in &mut bytes {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                *byte = seed as u8;
            }
            let limits = Limits {
                max_nodes: 64,
                max_depth: 16,
                max_atom_bits: 256,
                max_backrefs: 16,
                ..Limits::default()
            };
            let input = Bits::new(&bytes, length * 8).unwrap();
            if let Ok(noun) = serial::cue(input, limits) {
                assert_eq!(serial::jam(&noun, limits).unwrap().bytes, bytes);
            }
        }
    }
}
