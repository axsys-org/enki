use mesa_wire::legacy_v1::{
    decode, encode, Auth, Body, Checksum, Data, Error, Fragment, Limits, Name, Packet,
};

const JUMBO: Limits = Limits {
    max_packet: 131_072,
    max_path: 65_535,
    max_fragment: 100_000,
};

fn name() -> Name<'static> {
    Name {
        ship: 0,
        rift: 1,
        bloq: 13,
        fragment: Fragment::Init,
        path: b"publ/1/z",
    }
}

#[test]
fn integer_width_boundaries() {
    let boundaries = [
        0,
        1,
        255,
        256,
        65_535,
        65_536,
        0xff_ffff,
        0x100_0000,
        u32::MAX as u64,
        1 << 32,
        u64::MAX,
    ];
    for ship in [
        0,
        65_535,
        65_536,
        u32::MAX as u128,
        1 << 32,
        u64::MAX as u128,
        1 << 64,
        u128::MAX,
    ] {
        for n in boundaries {
            for fragment in [Fragment::Init, Fragment::Auth(n), Fragment::Data(n)] {
                let packet = Packet {
                    hop: 255,
                    body: Body::Page {
                        name: Name {
                            ship,
                            rift: n as u32,
                            fragment,
                            ..name()
                        },
                        data: Data {
                            total_bytes: n,
                            auth: Auth::None,
                            fragment: &[0],
                        },
                        next: None,
                    },
                };
                for checksum in [Checksum::HoonAtom, Checksum::VereBytes] {
                    let wire = encode(&packet, checksum, Limits::DATAGRAM).unwrap();
                    assert_eq!(
                        decode(&wire, checksum, Limits::DATAGRAM).unwrap(),
                        Packet { hop: 7, ..packet }
                    );
                }
            }
        }
    }
}

#[test]
fn data_length_widths_and_zero_preservation() {
    let signature = [0xab; 64];
    let mac = [0x77; 16];
    for n in [0, 1, 255, 256, 65_535, 65_536] {
        let payload = vec![0; n];
        for auth in [
            Auth::None,
            Auth::Signature(&signature),
            Auth::Mac(&mac),
            Auth::Pair(&signature),
        ] {
            let packet = Packet {
                hop: 0,
                body: Body::Page {
                    name: name(),
                    data: Data {
                        total_bytes: n as u64,
                        auth,
                        fragment: &payload,
                    },
                    next: None,
                },
            };
            let wire = encode(&packet, Checksum::VereBytes, JUMBO).unwrap();
            assert_eq!(decode(&wire, Checksum::VereBytes, JUMBO).unwrap(), packet);
            if n > 1024 {
                assert_eq!(
                    encode(&packet, Checksum::VereBytes, Limits::DATAGRAM),
                    Err(Error::Limit)
                );
                assert_eq!(
                    decode(
                        &wire,
                        Checksum::VereBytes,
                        Limits {
                            max_fragment: 1024,
                            ..JUMBO
                        }
                    ),
                    Err(Error::Limit)
                );
            }
        }
    }
}

#[test]
fn deterministic_hostile_bodies() {
    let mut seed = 0x729a_810e_u64;
    for n in 8..264 {
        for _ in 0..32 {
            let mut bytes = vec![0; n];
            for b in &mut bytes {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                *b = seed as u8;
            }
            // Supply valid framing/checksum so malformed bodies reach the parser.
            bytes[4..8].copy_from_slice(&mesa_wire::COOKIE.to_le_bytes());
            let kind = 1 + seed as u32 % 3;
            let word = ((mesa_wire::legacy_v1::VERSION as u32) << 4)
                | (kind << 7)
                | (Checksum::VereBytes.gum(&bytes[8..]) << 12);
            bytes[..4].copy_from_slice(&word.to_le_bytes());
            if let Ok(packet) = decode(&bytes, Checksum::VereBytes, Limits::DATAGRAM) {
                assert_eq!(
                    encode(&packet, Checksum::VereBytes, Limits::DATAGRAM).unwrap(),
                    bytes
                );
            }
        }
    }
}

#[test]
fn empty_and_oversize_paths_rejected() {
    for path in [vec![], vec![b'a'; 65_536]] {
        let packet = Packet {
            hop: 0,
            body: Body::Peek(Name {
                path: &path,
                ..name()
            }),
        };
        assert!(encode(&packet, Checksum::VereBytes, JUMBO).is_err());
    }
}
