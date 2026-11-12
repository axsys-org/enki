//! Golden-vector tests against ../../vectors/pact/.
//!
//! Vectors were extracted from a live zuse-408 ship (en:pact:ames /
//! fax:plot via dojo); see vectors/pact/NOTES.md for provenance and the
//! divergence probes they encode.

use mesa_wire::legacy_v1::{
    decode, encode, sift_head, Body, Checksum, Error, Fragment, Limits, PactType, HEADER_LEN,
};

#[derive(serde::Deserialize)]
struct Vector {
    name: String,
    #[serde(rename = "type")]
    typ: String,
    len: usize,
    wire: String,
    #[serde(default)]
    hop_on_wire: Option<u8>,
}

fn unhex(s: &str) -> Vec<u8> {
    assert_eq!(s.len() & 1, 0);
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

fn load() -> Vec<Vector> {
    let path = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../vectors/pact/v0-basic.json"
    );
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}

#[test]
fn headers_sift() {
    for v in load() {
        let wire = unhex(&v.wire);
        assert_eq!(wire.len(), v.len, "{}: length", v.name);
        let head = sift_head(&wire).unwrap_or_else(|| panic!("{}: no head", v.name));
        let want = match v.typ.as_str() {
            "page" => PactType::Page,
            "peek" => PactType::Peek,
            "poke" => PactType::Poke,
            t => panic!("{}: bad type {t}", v.name),
        };
        assert_eq!(head.typ, want, "{}: type", v.name);
        if let Some(hop) = v.hop_on_wire {
            assert_eq!(head.hop, hop, "{}: clamped hop", v.name);
        }
    }
}

#[test]
fn full_golden_roundtrips() {
    let vectors = load();
    assert_eq!(vectors.len(), 10, "do not silently run an empty corpus");
    for v in vectors {
        let bytes = unhex(&v.wire);
        for checksum in [Checksum::HoonAtom, Checksum::VereBytes] {
            let packet = decode(&bytes, checksum, Limits::DATAGRAM)
                .unwrap_or_else(|e| panic!("{}: {e}", v.name));
            assert_eq!(
                encode(&packet, checksum, Limits::DATAGRAM).unwrap(),
                bytes,
                "{}",
                v.name
            );
            let name = match packet.body {
                Body::Peek(n) => n,
                Body::Page { name, .. } => name,
                Body::Poke { ack, .. } => ack,
            };
            match v.name.as_str() {
                "peek-publ-galaxy" => {
                    assert_eq!((name.ship, name.rift, name.bloq), (0, 1, 13));
                    assert_eq!(name.path, b"publ/1/a/x/1//foo/bar");
                }
                "peek-comet-datafrag" => {
                    assert_eq!(name.ship, u128::MAX);
                    assert_eq!(name.fragment, Fragment::Data(5));
                }
                "peek-rift-1byte" => assert_eq!(name.rift, 255),
                "peek-rift-2byte" => assert_eq!(name.rift, 256),
                "page-short-final-frag" => {
                    let Body::Page { data, .. } = packet.body else {
                        panic!()
                    };
                    assert_eq!(data.total_bytes, 4);
                    assert_eq!(data.fragment, &[0xde]);
                }
                "page-next-if-hop3" => {
                    let Body::Page { next, .. } = packet.body else {
                        panic!()
                    };
                    assert_eq!(packet.hop, 3);
                    assert_eq!(next, Some([10, 1, 168, 192, 0x39, 5]));
                }
                "poke-hmac" => {
                    let Body::Poke { payload, data, .. } = packet.body else {
                        panic!()
                    };
                    assert_eq!(payload.ship, u128::MAX);
                    assert_eq!(payload.path, b"flow/poke");
                    assert_eq!(data.fragment, &[0x42, 0, 0xfe, 0xca]);
                }
                _ => {}
            }
        }
    }
}

fn rehash(bytes: &mut [u8], checksum: Checksum) {
    if bytes.len() >= HEADER_LEN {
        let w = u32::from_le_bytes(bytes[..4].try_into().unwrap()) & 0xfff;
        let gum = checksum.gum(&bytes[HEADER_LEN..]);
        bytes[..4].copy_from_slice(&(w | (gum << 12)).to_le_bytes());
    }
}

#[test]
fn every_truncation_and_trailing_byte_rejected() {
    for v in load() {
        let bytes = unhex(&v.wire);
        for checksum in [Checksum::HoonAtom, Checksum::VereBytes] {
            for n in 0..bytes.len() {
                let mut short = bytes[..n].to_vec();
                // Reach the structural parser even though the body was changed.
                rehash(&mut short, checksum);
                assert!(
                    decode(&short, checksum, Limits::DATAGRAM).is_err(),
                    "{}: {n}",
                    v.name
                );
            }
            let mut trailing = bytes.clone();
            trailing.push(0);
            rehash(&mut trailing, checksum);
            assert_eq!(
                decode(&trailing, checksum, Limits::DATAGRAM),
                Err(Error::TrailingBytes)
            );
        }
    }
}

#[test]
fn malformed_names_and_resource_limits() {
    let bytes = unhex(&load()[0].wire);
    for flags in [0x20, 0x40, 0x80] {
        let mut bad = bytes.clone();
        bad[8] |= flags;
        rehash(&mut bad, Checksum::VereBytes);
        assert_eq!(
            decode(&bad, Checksum::VereBytes, Limits::DATAGRAM),
            Err(Error::Reserved)
        );
    }
    let mut bad = bytes.clone();
    bad[13..15].copy_from_slice(&u16::MAX.to_le_bytes());
    rehash(&mut bad, Checksum::VereBytes);
    assert_eq!(
        decode(&bad, Checksum::VereBytes, Limits::DATAGRAM),
        Err(Error::Truncated)
    );
    let packet = decode(&bytes, Checksum::VereBytes, Limits::DATAGRAM).unwrap();
    for limits in [
        Limits {
            max_packet: bytes.len() - 1,
            ..Limits::DATAGRAM
        },
        Limits {
            max_path: 1,
            ..Limits::DATAGRAM
        },
    ] {
        assert_eq!(
            decode(&bytes, Checksum::VereBytes, limits),
            Err(Error::Limit)
        );
        assert_eq!(
            encode(&packet, Checksum::VereBytes, limits),
            Err(Error::Limit)
        );
    }
}

#[test]
fn body_checksum_failure_precedes_shape_failure() {
    let mut bytes = unhex(&load()[0].wire);
    bytes[8] |= 0x20;
    assert_eq!(
        decode(&bytes, Checksum::VereBytes, Limits::DATAGRAM),
        Err(Error::Checksum)
    );
}

#[test]
fn reject_nonminimal_widths_and_hostile_length_tags() {
    // Widen rift=1 from one byte to two without changing its value.
    let mut wire = unhex(&load()[0].wire);
    wire[8] |= 4;
    wire.insert(12, 0);
    rehash(&mut wire, Checksum::VereBytes);
    assert_eq!(
        decode(&wire, Checksum::VereBytes, Limits::DATAGRAM),
        Err(Error::NonCanonical)
    );

    let path = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../vectors/pact/v0-divergence.json"
    );
    let vectors: Vec<serde_json::Value> =
        serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
    let original = unhex(vectors[0]["wire"].as_str().unwrap());
    // Known 28-byte fixture: data meta at 23, total at 24, length at 25.
    for (width, error) in [
        (0, Error::NonCanonical),
        (2, Error::NonCanonical),
        (5, Error::Limit),
        (255, Error::Limit),
    ] {
        let mut wire = original.clone();
        wire[23] = (wire[23] & 0x3f) | 0xc0;
        wire.insert(25, width);
        rehash(&mut wire, Checksum::VereBytes);
        assert_eq!(
            decode(&wire, Checksum::VereBytes, Limits::DATAGRAM),
            Err(error)
        );
    }
    let mut wire = original;
    wire[23] |= 0x10;
    rehash(&mut wire, Checksum::VereBytes);
    assert_eq!(
        decode(&wire, Checksum::VereBytes, Limits::DATAGRAM),
        Err(Error::Reserved)
    );
}

#[test]
fn byte_mutations_cannot_create_noncanonical_roundtrips() {
    for v in load() {
        let bytes = unhex(&v.wire);
        for i in 0..bytes.len() {
            for bit in 0..8 {
                let mut mutated = bytes.clone();
                mutated[i] ^= 1 << bit;
                rehash(&mut mutated, Checksum::VereBytes);
                if let Ok(packet) = decode(&mutated, Checksum::VereBytes, Limits::DATAGRAM) {
                    assert_eq!(
                        encode(&packet, Checksum::VereBytes, Limits::DATAGRAM).unwrap(),
                        mutated
                    );
                }
            }
        }
    }
}

/// The C-built divergence wire (vectors/pact/v0-divergence.json): its
/// header must sift (valid page, gum = C's exact-bytes mug 0xd436e) even
/// though Hoon rejects the packet at the body-gum recompute — pinning the
/// C1 finding until the spec settles the canonical fragment form.
#[test]
fn divergence_wire_header() {
    let path = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../vectors/pact/v0-divergence.json"
    );
    let vs: Vec<serde_json::Value> =
        serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
    let wire = unhex(vs[0]["wire"].as_str().unwrap());
    let head = sift_head(&wire).unwrap();
    assert_eq!(head.typ, PactType::Page);
    assert_eq!(head.gum, 0xd436e);
    assert_eq!(head.gum as u64, vs[0]["gum_on_wire"].as_u64().unwrap());
    let packet = decode(&wire, Checksum::VereBytes, Limits::DATAGRAM).unwrap();
    let Body::Page { data, .. } = packet.body else {
        panic!()
    };
    assert_eq!(data.fragment, &[0xde, 0]);
    assert_eq!(Checksum::HoonAtom.gum(&wire[8..]), 0x87ada);
    assert_eq!(
        decode(&wire, Checksum::HoonAtom, Limits::DATAGRAM),
        Err(Error::Checksum)
    );
    assert_eq!(
        encode(&packet, Checksum::VereBytes, Limits::DATAGRAM).unwrap(),
        wire
    );
}

#[test]
fn zero_terminated_trailer_needs_explicit_checksum_convention() {
    let vectors: Vec<serde_json::Value> =
        serde_json::from_str(include_str!("../../../vectors/pact/v1-checksum.json")).unwrap();
    assert_eq!(vectors.len(), 2);
    for v in vectors {
        let wire = unhex(v["wire"].as_str().unwrap());
        let (right, wrong) = if v["checksum"] == "vere" {
            (Checksum::VereBytes, Checksum::HoonAtom)
        } else {
            (Checksum::HoonAtom, Checksum::VereBytes)
        };
        assert_eq!(Checksum::VereBytes.gum(&wire[8..]), 0x188b9);
        assert_eq!(Checksum::HoonAtom.gum(&wire[8..]), 0xe0e53);
        let packet = decode(&wire, right, Limits::DATAGRAM).unwrap();
        let Body::Page { data, next, .. } = packet.body else {
            panic!()
        };
        assert_eq!(data.fragment.last(), Some(&0xde));
        assert_eq!(next.unwrap()[4..], [80, 0]);
        assert_eq!(encode(&packet, right, Limits::DATAGRAM).unwrap(), wire);
        assert_eq!(decode(&wire, wrong, Limits::DATAGRAM), Err(Error::Checksum));
    }
}

#[test]
fn corrupted_headers_rejected() {
    for v in load() {
        let wire = unhex(&v.wire);
        // Break the cookie.
        let mut bad = wire.clone();
        bad[5] ^= 0xff;
        assert!(sift_head(&bad).is_none(), "{}: cookie", v.name);
        // Set a reserved bit.
        let mut bad = wire.clone();
        bad[0] |= 0b01;
        assert!(sift_head(&bad).is_none(), "{}: reserved", v.name);
        // Truncate below the fixed header.
        assert!(
            sift_head(&wire[..HEADER_LEN - 1]).is_none(),
            "{}: short",
            v.name
        );
    }
}
