//! Experimental v2 vectors are hand-specified bytes, not Rust encoder output.
use mesa_noun::mug::mug_bytes;
use mesa_wire::{
    decode, encode, legacy_v1, Auth, Body, Data, DomainTag, Error, Fragment, Limits, Name, Packet,
    PeerId, COOKIE, VERSION,
};

const LARGE: Limits = Limits {
    max_packet: 131_072,
    max_path: 65_535,
    max_fragment: 100_000,
};

#[derive(serde::Deserialize)]
struct Vector {
    name: String,
    len: usize,
    body: String,
    wire: String,
}
fn fixtures() -> Vec<Vector> {
    serde_json::from_str(include_str!("../../../vectors/pact/v2-basic.json")).unwrap()
}
fn unhex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}
fn rehash(wire: &mut [u8]) {
    if wire.len() >= 8 {
        let low = u32::from_le_bytes(wire[..4].try_into().unwrap()) & 0xfff;
        let word = low | ((mug_bytes(&wire[8..]) & 0xfffff) << 12);
        wire[..4].copy_from_slice(&word.to_le_bytes());
    }
}
fn name(id: u128) -> Name<'static> {
    Name {
        publisher: PeerId(id),
        domain: DomainTag::new("gw-btc").unwrap(),
        rift: 1,
        bloq: 13,
        fragment: Fragment::Init,
        path: b"a",
    }
}
fn first_name(packet: Packet<'_>) -> Name<'_> {
    match packet.body {
        Body::Peek(n) | Body::Page { name: n, .. } | Body::Poke { ack: n, .. } => n,
    }
}

#[test]
fn independent_expected_bytes_and_fields() {
    let max_domain = format!("a{}", "b".repeat(62));
    let mac = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15];
    let pair = [0xab; 64];
    let vectors = fixtures();
    assert_eq!(vectors.len(), 8);
    for v in vectors {
        let (hop, body) = match v.name.as_str() {
            "peek-zero" => (0, Body::Peek(name(0))),
            "peek-high-unknown-domain" => (
                7,
                Body::Peek(Name {
                    publisher: PeerId(1 << 127),
                    domain: DomainTag::new("new-pki").unwrap(),
                    rift: 255,
                    fragment: Fragment::Data(256),
                    path: b"z",
                    ..name(0)
                }),
            ),
            "peek-max-fields" => (
                0,
                Body::Peek(Name {
                    publisher: PeerId(u128::MAX),
                    domain: DomainTag::new(&max_domain).unwrap(),
                    rift: u32::MAX,
                    bloq: 255,
                    fragment: Fragment::Auth(u64::MAX),
                    path: b"raw",
                }),
            ),
            "page-short-signature" => (
                0,
                Body::Page {
                    name: name(1),
                    data: Data {
                        total_bytes: 3,
                        auth: Auth::Signature(&[0x11, 0x22, 0x33]),
                        fragment: &[0xaa, 0xbb, 0],
                    },
                    next: None,
                },
            ),
            "page-mac-zero-trailer" => (
                3,
                Body::Page {
                    name: name(1),
                    data: Data {
                        total_bytes: 2,
                        auth: Auth::Mac(&mac),
                        fragment: &[0x44, 0],
                    },
                    next: Some([1, 0, 0, 127, 80, 0]),
                },
            ),
            "page-empty-none" => (
                0,
                Body::Page {
                    name: name(0),
                    data: Data {
                        total_bytes: 0,
                        auth: Auth::None,
                        fragment: &[],
                    },
                    next: None,
                },
            ),
            "page-pair" => (
                0,
                Body::Page {
                    name: name(0),
                    data: Data {
                        total_bytes: 1,
                        auth: Auth::Pair(&pair),
                        fragment: &[0],
                    },
                    next: None,
                },
            ),
            "poke-distinct-domains" => (
                1,
                Body::Poke {
                    ack: Name {
                        domain: DomainTag::new("azimuth").unwrap(),
                        path: b"ack",
                        ..name(2)
                    },
                    payload: name(1),
                    data: Data {
                        total_bytes: 1,
                        auth: Auth::None,
                        fragment: &[0x42],
                    },
                },
            ),
            _ => panic!("unexpected fixture"),
        };
        let expected = Packet { hop, body };
        let wire = unhex(&v.wire);
        assert_eq!(wire.len(), v.len, "{}", v.name);
        assert_eq!(&wire[8..], unhex(&v.body), "{}", v.name);
        assert_eq!(
            decode(&wire, Limits::DATAGRAM).unwrap(),
            expected,
            "{}",
            v.name
        );
        assert_eq!(
            encode(&expected, Limits::DATAGRAM).unwrap(),
            wire,
            "{}",
            v.name
        );
    }
}

#[test]
fn fixed_width_including_small_names_and_zero_high_bytes() {
    let mut sizes = std::collections::HashSet::new();
    for id in [
        0,
        1,
        255,
        256,
        65_535,
        65_536,
        u64::MAX as u128,
        1 << 64,
        1 << 127,
        u128::MAX,
    ] {
        let expected = Packet {
            hop: 0,
            body: Body::Peek(name(id)),
        };
        let wire = encode(&expected, Limits::DATAGRAM).unwrap();
        assert_eq!(wire[8] & 3, 0);
        assert_eq!(&wire[16..32], &id.to_le_bytes());
        assert_eq!(wire[32], 1); // rift follows all sixteen name bytes
        sizes.insert(wire.len());
        assert_eq!(decode(&wire, Limits::DATAGRAM).unwrap(), expected);
    }
    assert_eq!(sizes.len(), 1);
}

#[test]
fn domain_grammar_has_no_normalization_or_default() {
    for value in ["a", "gw-btc", "azimuth", "a0", "a-0-b2", &"a".repeat(63)] {
        assert_eq!(DomainTag::new(value).unwrap().as_str(), value);
    }
    for value in [
        "",
        "0abc",
        "Gw-btc",
        "-a",
        "a-",
        "a--b",
        "a_b",
        "a.b",
        "a b",
        "a\0b",
        "é",
        &"a".repeat(64),
    ] {
        assert_eq!(
            DomainTag::new(value),
            Err(Error::InvalidDomain),
            "{value:?}"
        );
    }
    let original = unhex(&fixtures()[0].wire);
    for domain in [
        b"".as_slice(),
        b"GW-btc",
        b"a--b",
        b"-a",
        b"a-",
        b"a_b",
        b"\xff",
        b"a\0b",
        &[b'a'; 64],
    ] {
        let mut wire = original.clone();
        wire[9] = domain.len() as u8;
        wire.splice(10..16, domain.iter().copied());
        rehash(&mut wire);
        assert_eq!(decode(&wire, Limits::DATAGRAM), Err(Error::InvalidDomain));
    }
    let mut truncated = original;
    truncated[9] = 63;
    rehash(&mut truncated);
    assert_eq!(decode(&truncated, Limits::DATAGRAM), Err(Error::Truncated));
}

#[test]
fn identity_equality_is_separate_from_full_name_context() {
    let a = name(42);
    let b = Name {
        domain: DomainTag::new("other").unwrap(),
        ..a
    };
    assert_eq!(a.publisher, b.publisher);
    assert_ne!(a, b);
    let mut peers = std::collections::HashSet::new();
    peers.insert(a.publisher);
    peers.insert(b.publisher);
    assert_eq!(peers.len(), 1);
    assert_ne!(
        encode(
            &Packet {
                hop: 0,
                body: Body::Peek(a)
            },
            Limits::DATAGRAM
        )
        .unwrap(),
        encode(
            &Packet {
                hop: 0,
                body: Body::Peek(b)
            },
            Limits::DATAGRAM
        )
        .unwrap()
    );
    // These types represent claims only; no first-claim-wins registry is installed.
}

#[test]
fn v1_and_v2_never_fall_back_to_each_other() {
    let old: Vec<serde_json::Value> =
        serde_json::from_str(include_str!("../../../vectors/pact/v0-basic.json")).unwrap();
    for v in old {
        assert_eq!(
            decode(&unhex(v["wire"].as_str().unwrap()), Limits::DATAGRAM),
            Err(Error::Header)
        );
    }
    for v in fixtures() {
        let wire = unhex(&v.wire);
        assert_eq!(
            legacy_v1::decode(
                &wire,
                legacy_v1::Checksum::VereBytes,
                legacy_v1::Limits::DATAGRAM
            ),
            Err(legacy_v1::Error::Header)
        );
        for version in [0, 1, 3, 4, 5, 6, 7] {
            let mut wrong = wire.clone();
            wrong[0] = (wrong[0] & !0x70) | (version << 4);
            assert_eq!(decode(&wrong, Limits::DATAGRAM), Err(Error::Header));
        }
    }
}

#[test]
fn exact_body_checksum_includes_zero_terminated_trailer() {
    let v = fixtures()
        .into_iter()
        .find(|v| v.name == "page-mac-zero-trailer")
        .unwrap();
    let mut wire = unhex(&v.wire);
    let exact = mug_bytes(&wire[8..]) & 0xfffff;
    let atom = legacy_v1::Checksum::HoonAtom.gum(&wire[8..]);
    assert_ne!(exact, atom);
    let low = u32::from_le_bytes(wire[..4].try_into().unwrap()) & 0xfff;
    wire[..4].copy_from_slice(&(low | (atom << 12)).to_le_bytes());
    assert_eq!(decode(&wire, Limits::DATAGRAM), Err(Error::Checksum));
}

#[test]
fn signatures_are_bounded_length_delimited_and_borrowed() {
    for n in [1, 64, 65, 255, 256, 65_535] {
        let signature = vec![0x5a; n];
        let packet = Packet {
            hop: 0,
            body: Body::Page {
                name: name(1),
                data: Data {
                    total_bytes: 0,
                    auth: Auth::Signature(&signature),
                    fragment: &[],
                },
                next: None,
            },
        };
        let wire = encode(&packet, LARGE).unwrap();
        let decoded = decode(&wire, LARGE).unwrap();
        assert_eq!(decoded, packet);
        let Body::Page { data, name, .. } = decoded.body else {
            panic!()
        };
        let Auth::Signature(sig) = data.auth else {
            panic!()
        };
        assert_eq!(sig.as_ptr(), wire[41..].as_ptr());
        assert_eq!(name.domain.as_str().as_ptr(), wire[10..].as_ptr());
        if wire.len() > Limits::DATAGRAM.max_packet {
            assert_eq!(decode(&wire, Limits::DATAGRAM), Err(Error::Limit));
            assert_eq!(encode(&packet, Limits::DATAGRAM), Err(Error::Limit));
        }
    }
    for n in [0, 65_536] {
        let signature = vec![0; n];
        let packet = Packet {
            hop: 0,
            body: Body::Page {
                name: name(1),
                data: Data {
                    total_bytes: 0,
                    auth: Auth::Signature(&signature),
                    fragment: &[],
                },
                next: None,
            },
        };
        assert_eq!(encode(&packet, LARGE), Err(Error::InvalidSignatureLength));
    }
    let v = fixtures()
        .into_iter()
        .find(|v| v.name == "page-short-signature")
        .unwrap();
    for (length, error) in [
        (0u16, Error::InvalidSignatureLength),
        (65_535, Error::Truncated),
    ] {
        let mut wire = unhex(&v.wire);
        wire[39..41].copy_from_slice(&length.to_le_bytes());
        rehash(&mut wire);
        assert_eq!(decode(&wire, Limits::DATAGRAM), Err(error));
    }
}

#[test]
fn all_fixture_truncations_and_trailing_bytes_are_rejected() {
    for v in fixtures() {
        let wire = unhex(&v.wire);
        for n in 0..wire.len() {
            let mut short = wire[..n].to_vec();
            rehash(&mut short);
            assert!(decode(&short, LARGE).is_err(), "{} at {n}", v.name);
        }
        let mut trailing = wire;
        trailing.push(0);
        rehash(&mut trailing);
        assert_eq!(decode(&trailing, LARGE), Err(Error::TrailingBytes));
    }
}

#[test]
fn reserved_bits_and_resource_limits() {
    let original = unhex(&fixtures()[0].wire);
    for flag in [1, 2, 0x20, 0x40, 0x80] {
        let mut wire = original.clone();
        wire[8] |= flag;
        // checksum failure is earlier than the structural failure
        assert_eq!(decode(&wire, LARGE), Err(Error::Checksum));
        rehash(&mut wire);
        assert_eq!(decode(&wire, LARGE), Err(Error::Reserved));
    }
    let packet = decode(&original, LARGE).unwrap();
    for limits in [
        Limits {
            max_packet: original.len() - 1,
            ..LARGE
        },
        Limits {
            max_path: 0,
            ..LARGE
        },
    ] {
        assert_eq!(decode(&original, limits), Err(Error::Limit));
        assert_eq!(encode(&packet, limits), Err(Error::Limit));
    }
    // Same value encoded with an over-wide rift remains noncanonical.
    let mut wide = original;
    wide[8] |= 4;
    wide.insert(33, 0);
    rehash(&mut wide);
    assert_eq!(decode(&wide, LARGE), Err(Error::NonCanonical));
}

#[test]
fn successful_mutations_and_hostile_bodies_have_canonical_roundtrips() {
    for v in fixtures() {
        let wire = unhex(&v.wire);
        for i in 0..wire.len() {
            for bit in 0..8 {
                let mut mutated = wire.clone();
                mutated[i] ^= 1 << bit;
                rehash(&mut mutated);
                if let Ok(packet) = decode(&mutated, LARGE) {
                    assert_eq!(encode(&packet, LARGE).unwrap(), mutated);
                }
            }
        }
    }
    let mut seed = 0x729a_810eu64;
    for n in 8..264 {
        for _ in 0..32 {
            let mut bytes = vec![0; n];
            for b in &mut bytes {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                *b = seed as u8;
            }
            bytes[4..8].copy_from_slice(&COOKIE.to_le_bytes());
            let low = ((VERSION as u32) << 4) | ((1 + seed as u32 % 3) << 7);
            bytes[..4].copy_from_slice(&low.to_le_bytes());
            rehash(&mut bytes);
            if let Ok(packet) = decode(&bytes, LARGE) {
                assert_eq!(encode(&packet, LARGE).unwrap(), bytes);
                assert!(!first_name(packet).domain.as_str().is_empty());
            }
        }
    }
}
