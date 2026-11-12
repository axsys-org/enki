use mesa_noun::{
    atom::Atom,
    bits::Bits,
    mat::{self, Error},
};
use serde::Deserialize;

const MAX_BITS: usize = 16 * 1024;

fn hex(s: &str) -> Vec<u8> {
    s.as_bytes()
        .as_chunks::<2>()
        .0
        .iter()
        .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
        .collect()
}

#[derive(Deserialize)]
struct Corpus<T> {
    cases: Vec<T>,
}
#[derive(Deserialize)]
struct Case {
    atom: String,
    bits: usize,
    encoded: String,
}
fn cases() -> Vec<Case> {
    serde_json::from_str::<Corpus<Case>>(include_str!("../../../vectors/noun/mat.json"))
        .unwrap()
        .cases
}

#[test]
fn independent_source_vectors_and_exact_consumption() {
    for case in cases() {
        let atom = Atom::from_le_bytes(hex(&case.atom));
        let bytes = hex(&case.encoded);
        let encoded = mat::mat(&atom, MAX_BITS).unwrap();
        assert_eq!(encoded.bytes, bytes);
        assert_eq!(encoded.bit_len, case.bits);
        let decoded = mat::decode(Bits::new(&bytes, case.bits).unwrap(), MAX_BITS).unwrap();
        assert_eq!(decoded.consumed, case.bits);
        assert_eq!(decoded.atom.to_atom(MAX_BITS / 8).unwrap(), atom);
    }
    // Fixed examples independent of the generated JSON or Rust encoder.
    for (n, bits, byte) in [(0, 1, 1), (1, 3, 6), (2, 6, 36), (3, 6, 52), (4, 7, 76)] {
        let encoded = mat::mat(&Atom::from_le_bytes(vec![n]), 8).unwrap();
        assert_eq!((encoded.bit_len, encoded.bytes), (bits, vec![byte]));
    }
}

#[test]
fn every_bit_truncation_fails_without_implicit_zero_extension() {
    for case in cases() {
        let bytes = hex(&case.encoded);
        for len in 0..case.bits {
            assert!(
                mat::rub(Bits::new(&bytes, len).unwrap(), MAX_BITS).is_err(),
                "{len}/{}",
                case.bits
            );
        }
    }
}

#[test]
fn unaligned_nested_fields_preserve_next_bit() {
    for case in cases() {
        let source = hex(&case.encoded);
        for offset in 0..8 {
            // Surround the field with set bits so parsing cannot rely on zero padding.
            let mut bytes = vec![255; (offset + case.bits + 1).div_ceil(8)];
            for i in 0..case.bits {
                let bit = (source[i / 8] >> (i % 8)) & 1;
                let pos = offset + i;
                bytes[pos / 8] = (bytes[pos / 8] & !(1 << (pos % 8))) | (bit << (pos % 8));
            }
            let field = Bits::new(&bytes, offset + case.bits + 1)
                .unwrap()
                .slice(offset, case.bits + 1)
                .unwrap();
            let decoded = mat::rub(field, MAX_BITS).unwrap();
            assert_eq!(decoded.consumed, case.bits);
            assert_eq!(
                decoded.atom.to_atom(MAX_BITS / 8).unwrap().as_le_bytes(),
                hex(&case.atom)
            );
            assert_eq!(field.bit(decoded.consumed), Some(true));
            assert_eq!(
                mat::decode(field, MAX_BITS).unwrap_err(),
                Error::TrailingBits
            );
        }
    }
}

#[test]
fn reject_overlong_atoms_and_bound_scan_and_declared_length() {
    // The long form for zero and an overdeclared two-bit encoding of one.
    for (bytes, bits) in [(vec![2], 3), (vec![20], 6)] {
        assert_eq!(
            mat::rub(Bits::new(&bytes, bits).unwrap(), MAX_BITS).unwrap_err(),
            Error::NonCanonical
        );
    }
    let zeros = [0; 128];
    assert_eq!(
        mat::rub(Bits::new(&zeros, zeros.len() * 8).unwrap(), MAX_BITS).unwrap_err(),
        Error::Limit
    );
    assert_eq!(
        mat::rub(Bits::new(&zeros, 8).unwrap(), MAX_BITS).unwrap_err(),
        Error::Truncated
    );
    // c=7 describes 64 bits with no payload. Reject the limit before taking data.
    assert_eq!(
        mat::rub(Bits::new(&[128, 0], 14).unwrap(), 63).unwrap_err(),
        Error::Limit
    );
    assert_eq!(
        mat::rub(Bits::new(&[128, 0], 14).unwrap(), 64).unwrap_err(),
        Error::Truncated
    );
    // A machine-word-sized prefix must not overflow shifts, lengths or cursors.
    let width = usize::BITS as usize;
    let mut hostile = vec![0; (2 * width).div_ceil(8)];
    hostile[width / 8] |= 1 << (width % 8);
    let input = Bits::new(&hostile, 2 * width).unwrap();
    assert_eq!(mat::rub(input, MAX_BITS).unwrap_err(), Error::Limit);
    assert_eq!(mat::rub(input, usize::MAX).unwrap_err(), Error::Truncated);
    for index in width + 1..2 * width {
        hostile[index / 8] |= 1 << (index % 8);
    }
    assert_eq!(
        mat::rub(Bits::new(&hostile, 2 * width).unwrap(), usize::MAX).unwrap_err(),
        Error::Limit
    );
}

#[test]
fn bounds_apply_to_views_copying_and_encoding() {
    assert_eq!(Bits::new(&[1], 9).unwrap_err(), Error::Truncated);
    let bits = Bits::new(&[0xff], 8).unwrap();
    assert_eq!(bits.slice(usize::MAX, 1).unwrap_err(), Error::Truncated);
    assert_eq!(bits.slice(1, usize::MAX).unwrap_err(), Error::Truncated);
    assert_eq!(bits.bit(usize::MAX), None);
    assert_eq!(bits.to_atom(0).unwrap_err(), Error::Limit);
    assert_eq!(
        bits.slice(0, 3).unwrap().to_atom(1).unwrap().as_le_bytes(),
        &[7]
    );
    let one = Atom::from_le_bytes(vec![1]);
    assert_eq!(mat::mat(&one, 0).unwrap_err(), Error::Limit);
    assert_eq!(mat::mat(&Atom::default(), 0).unwrap().bytes, [1]);
    let encoded = mat::mat(&one, 1).unwrap();
    assert_eq!(
        mat::decode(Bits::new(&encoded.bytes, 3).unwrap(), 0).unwrap_err(),
        Error::Limit
    );
    assert!(mat::decode(Bits::new(&[1], 1).unwrap(), 0)
        .unwrap()
        .atom
        .is_empty());
    // Byte padding lies outside a three-bit field, but inside an eight-bit frame.
    assert!(mat::decode(Bits::new(&[0xfe], 3).unwrap(), 1).is_ok());
    assert_eq!(
        mat::decode(Bits::new(&[6], 8).unwrap(), 1).unwrap_err(),
        Error::TrailingBits
    );
}

#[derive(Deserialize)]
struct Layout {
    pass_atom: String,
    bits: usize,
    genesis: String,
    live: String,
    dat: String,
    dat_bits: usize,
    domain: String,
    domain_field_bits: usize,
    domain_tail: String,
    domain_tail_bits: usize,
    xtr: String,
    xtr_offset: usize,
}

#[test]
fn suite_c_layout_preserves_domain_tail_and_evidence_boundary() {
    let cases = serde_json::from_str::<Corpus<Layout>>(include_str!(
        "../../../vectors/pass/suite-c-layout.json"
    ))
    .unwrap()
    .cases;
    let mut unaligned = false;
    for case in cases {
        let bytes = hex(&case.pass_atom);
        assert_eq!(bytes[0], b'c');
        assert_eq!(&bytes[1..33], hex(&case.genesis));
        assert_eq!(&bytes[33..65], hex(&case.live));
        let bits = Bits::new(&bytes, case.bits).unwrap();
        let data = mat::rub(bits.slice(520, case.bits - 520).unwrap(), MAX_BITS).unwrap();
        assert_eq!(data.atom.len(), case.dat_bits);
        assert_eq!(
            data.atom.to_atom(MAX_BITS / 8).unwrap().as_le_bytes(),
            hex(&case.dat)
        );
        assert_eq!(520 + data.consumed, case.xtr_offset);
        unaligned |= case.xtr_offset % 8 != 0;
        let evidence = bits
            .slice(case.xtr_offset, case.bits - case.xtr_offset)
            .unwrap();
        assert_eq!(
            evidence.to_atom(MAX_BITS / 8).unwrap().as_le_bytes(),
            hex(&case.xtr)
        );
        let domain = mat::rub(data.atom, 63 * 8).unwrap();
        assert_eq!(domain.consumed, case.domain_field_bits);
        assert_eq!(
            domain.atom.to_atom(63).unwrap().as_le_bytes(),
            case.domain.as_bytes()
        );
        let tail = data
            .atom
            .slice(domain.consumed, data.atom.len() - domain.consumed)
            .unwrap();
        assert_eq!(tail.len(), case.domain_tail_bits);
        assert_eq!(
            tail.to_atom(MAX_BITS / 8).unwrap().as_le_bytes(),
            hex(&case.domain_tail)
        );
    }
    assert!(unaligned);
}

#[test]
fn small_atom_exhaustion_and_hostile_streams_are_canonical() {
    for n in 0u16..=u16::MAX {
        let atom = Atom::from_le_bytes(n.to_le_bytes().to_vec());
        let encoded = mat::mat(&atom, 16).unwrap();
        let decoded = mat::decode(Bits::new(&encoded.bytes, encoded.bit_len).unwrap(), 16).unwrap();
        assert_eq!(decoded.atom.to_atom(2).unwrap(), atom);
    }
    let mut state = 0x123456789abcdef0u64;
    for len in 0..256 {
        for _ in 0..32 {
            let mut bytes = vec![0; len];
            for byte in &mut bytes {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                *byte = state as u8;
            }
            let input = Bits::new(&bytes, bytes.len() * 8).unwrap();
            if let Ok(decoded) = mat::rub(input, MAX_BITS) {
                let atom = decoded.atom.to_atom(MAX_BITS / 8).unwrap();
                let encoded = mat::mat(&atom, MAX_BITS).unwrap();
                assert_eq!(encoded.bit_len, decoded.consumed);
                for bit in 0..encoded.bit_len {
                    assert_eq!(
                        input.bit(bit),
                        Some(encoded.bytes[bit / 8] & (1 << (bit % 8)) != 0)
                    );
                }
            }
        }
    }
}
