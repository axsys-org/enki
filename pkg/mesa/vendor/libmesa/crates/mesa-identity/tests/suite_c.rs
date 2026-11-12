use mesa_identity::suite_c::{self, Error, Limits};
use mesa_noun::{atom::Atom, bits::Bits, mat};
use mesa_wire::DomainTag;
use serde_json::Value;

fn hex(s: &str) -> Vec<u8> {
    s.as_bytes()
        .as_chunks::<2>()
        .0
        .iter()
        .map(|p| u8::from_str_radix(std::str::from_utf8(p).unwrap(), 16).unwrap())
        .collect()
}
fn fixtures() -> Vec<Value> {
    serde_json::from_str::<Value>(include_str!("../../../vectors/pass/suite-c-layout.json"))
        .unwrap()["cases"]
        .as_array()
        .unwrap()
        .clone()
}
fn domain() -> DomainTag<'static> {
    DomainTag::new("gw-btc").unwrap()
}

#[test]
fn source_fixtures_keep_public_private_and_evidence_slots_distinct() {
    for fixture in fixtures() {
        let bytes = hex(fixture["pass_atom"].as_str().unwrap());
        let expected = DomainTag::new(fixture["domain"].as_str().unwrap()).unwrap();
        let pass = suite_c::parse_pass(&bytes, expected, Limits::default()).unwrap();
        assert_eq!(
            pass.genesis_key().as_slice(),
            hex(fixture["genesis"].as_str().unwrap())
        );
        assert_eq!(
            pass.live_key().as_slice(),
            hex(fixture["live"].as_str().unwrap())
        );
        assert_eq!(pass.genesis_key().as_ptr(), bytes[1..].as_ptr());
        assert_eq!(pass.live_key().as_ptr(), bytes[33..].as_ptr());
        assert_eq!(pass.claims().domain(), expected);
        assert_eq!(
            pass.claims().dat().to_atom(4096).unwrap().as_le_bytes(),
            hex(fixture["dat"].as_str().unwrap())
        );
        assert_eq!(
            pass.claims()
                .domain_tail()
                .to_atom(4096)
                .unwrap()
                .as_le_bytes(),
            hex(fixture["domain_tail"].as_str().unwrap())
        );
        assert_eq!(
            pass.claims()
                .evidence()
                .to_atom(4096)
                .unwrap()
                .as_le_bytes(),
            hex(fixture["xtr"].as_str().unwrap())
        );
        let ring_bytes = hex(fixture["ring_atom"].as_str().unwrap());
        let ring = suite_c::parse_current_ring(&ring_bytes, expected, Limits::default()).unwrap();
        assert_eq!(
            ring.live_seed().as_slice(),
            hex(fixture["live_seed"].as_str().unwrap())
        );
        assert_eq!(ring.live_seed().as_ptr(), ring_bytes[1..].as_ptr());
        assert_eq!(ring.genesis_key(), pass.genesis_key());
        assert_eq!(ring.genesis_key().as_ptr(), ring_bytes[33..].as_ptr());
        assert_eq!(
            ring.claims().dat().to_atom(4096).unwrap(),
            pass.claims().dat().to_atom(4096).unwrap()
        );
        assert_eq!(
            ring.claims().evidence().to_atom(4096).unwrap(),
            pass.claims().evidence().to_atom(4096).unwrap()
        );
    }
}

// Build an adversarial pass with an arbitrary domain cord; no cryptography.
fn with_domain(domain: &[u8]) -> Vec<u8> {
    let domain = mat::mat(&Atom::from_le_bytes(domain.to_vec()), 1024).unwrap();
    let dat = Atom::from_le_bytes(domain.bytes);
    let field = mat::mat(&dat, 2048).unwrap();
    let mut bytes = vec![0; 65];
    bytes[0] = b'c';
    bytes.extend(field.bytes);
    bytes
}

#[test]
fn formatting_public_claims_does_not_expose_ring_seed_storage() {
    let fixture = &fixtures()[0];
    let bytes = hex(fixture["ring_atom"].as_str().unwrap());
    let ring = suite_c::parse_current_ring(&bytes, domain(), Limits::default()).unwrap();
    let seed_debug = format!("{:?}", ring.live_seed());
    let claims_debug = format!("{:?}", ring.claims());
    assert!(!claims_debug.contains(&seed_debug));
    assert!(!claims_debug.contains("bytes:"));
    let view = Bits::new(b"secret", 48).unwrap().slice(32, 8).unwrap();
    let debug = format!("{view:?}");
    assert!(!debug.contains("115, 101, 99, 114, 101, 116"));
    assert!(!debug.contains("bytes:"));
}

#[test]
fn domain_commitment_must_match_canonical_selector() {
    for name in [
        b"".as_slice(),
        b"Gw-btc",
        b"gw_btc",
        b"gw--btc",
        b"-gw",
        b"gw-",
        b"9gw",
        b"gw\0btc",
        b"\xff",
    ] {
        assert!(suite_c::parse_pass(&with_domain(name), domain(), Limits::default()).is_err());
    }
    assert!(matches!(
        suite_c::parse_pass(&with_domain(&[b'a'; 64]), domain(), Limits::default()),
        Err(Error::Limit)
    ));
    let other = with_domain(b"other-pki");
    assert_eq!(
        suite_c::parse_pass(&other, domain(), Limits::default()).unwrap_err(),
        Error::DomainMismatch
    );
    let parsed = suite_c::parse_pass(
        &other,
        DomainTag::new("other-pki").unwrap(),
        Limits::default(),
    )
    .unwrap();
    assert_eq!(parsed.claims().domain().as_str(), "other-pki");
    assert!(parsed.claims().domain_tail().is_empty());
    assert!(parsed.claims().evidence().is_empty());
}

#[test]
fn suites_padding_and_truncated_fields_fail_closed() {
    let bytes = with_domain(b"gw-btc");
    for end in 0..bytes.len() {
        assert!(suite_c::parse_pass(&bytes[..end], domain(), Limits::default()).is_err());
    }
    for tag in [b'b', b'B', b'C', 0, 255] {
        let mut wrong = bytes.clone();
        wrong[0] = tag;
        assert_eq!(
            suite_c::parse_pass(&wrong, domain(), Limits::default()).unwrap_err(),
            Error::WrongSuite
        );
    }
    assert!(matches!(
        suite_c::parse_current_ring(&bytes, domain(), Limits::default()),
        Err(Error::WrongSuite)
    ));
    let mut padded = bytes.clone();
    padded.push(0);
    assert_eq!(
        suite_c::parse_pass(&padded, domain(), Limits::default()).unwrap_err(),
        Error::NonCanonical
    );
    let mut no_prefix = bytes[..65].to_vec();
    no_prefix.extend([0; 32]);
    no_prefix.push(1);
    assert_eq!(
        suite_c::parse_pass(&no_prefix, domain(), Limits::default()).unwrap_err(),
        Error::Limit
    );
    // mat(dat) declaring a nonzero-width zero atom cannot be canonical.
    let mut overlong = bytes[..65].to_vec();
    overlong.push(2);
    assert!(suite_c::parse_pass(&overlong, domain(), Limits::default()).is_err());
}

#[test]
fn input_data_and_evidence_limits_are_independent() {
    let fixture = &fixtures()[1];
    let bytes = hex(fixture["pass_atom"].as_str().unwrap());
    let pass = suite_c::parse_pass(&bytes, domain(), Limits::default()).unwrap();
    let limits = Limits {
        max_blob_bytes: bytes.len(),
        max_dat_bits: pass.claims().dat().len(),
        max_evidence_bits: pass.claims().evidence().len(),
    };
    assert!(suite_c::parse_pass(&bytes, domain(), limits).is_ok());
    for limited in [
        Limits {
            max_blob_bytes: bytes.len() - 1,
            ..limits
        },
        Limits {
            max_dat_bits: limits.max_dat_bits - 1,
            ..limits
        },
        Limits {
            max_evidence_bits: limits.max_evidence_bits - 1,
            ..limits
        },
    ] {
        assert_eq!(
            suite_c::parse_pass(&bytes, domain(), limited).unwrap_err(),
            Error::Limit
        );
    }
}

#[test]
fn live_key_and_evidence_changes_do_not_rewrite_committed_data() {
    let fixture = &fixtures()[1];
    let bytes = hex(fixture["pass_atom"].as_str().unwrap());
    let original = suite_c::parse_pass(&bytes, domain(), Limits::default()).unwrap();
    let mut changed = bytes.clone();
    changed[33] ^= 1;
    let evidence_start = fixture["xtr_offset"].as_u64().unwrap() as usize;
    changed[evidence_start / 8] ^= 1 << (evidence_start % 8);
    let new = suite_c::parse_pass(&changed, domain(), Limits::default()).unwrap();
    assert_ne!(original.live_key(), new.live_key());
    assert_eq!(original.genesis_key(), new.genesis_key());
    assert_eq!(
        original.claims().dat().to_atom(4096).unwrap(),
        new.claims().dat().to_atom(4096).unwrap()
    );
    assert_ne!(
        original.claims().evidence().to_atom(4096).unwrap(),
        new.claims().evidence().to_atom(4096).unwrap()
    );
    // This is a framing test, not a name-derivation or key-rotation proof.
}

#[test]
fn accepted_mutations_remain_bounded_unverified_claims() {
    for fixture in fixtures() {
        let original = hex(fixture["pass_atom"].as_str().unwrap());
        let expected = DomainTag::new(fixture["domain"].as_str().unwrap()).unwrap();
        for index in 0..original.len() {
            for bit in 0..8 {
                let mut bytes = original.clone();
                bytes[index] ^= 1 << bit;
                if let Ok(pass) = suite_c::parse_pass(&bytes, expected, Limits::default()) {
                    assert_eq!(pass.claims().domain(), expected);
                    let all = Bits::from_atom_bytes(&bytes).unwrap();
                    let data =
                        mat::rub(all.slice(520, all.len() - 520).unwrap(), 32768 * 8).unwrap();
                    assert_eq!(
                        data.atom.to_atom(32768).unwrap(),
                        pass.claims().dat().to_atom(32768).unwrap()
                    );
                    assert_eq!(
                        520 + data.consumed + pass.claims().evidence().len(),
                        all.len()
                    );
                }
            }
        }
    }
}
