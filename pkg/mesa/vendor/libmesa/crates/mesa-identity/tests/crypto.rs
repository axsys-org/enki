use mesa_identity::suite_c::{
    self,
    crypto::{BoundPass, Error},
    Limits,
};
use mesa_noun::{bits::Bits, mat};
use mesa_wire::{DomainTag, PeerId};
use serde_json::Value;
fn hex(value: &Value) -> Vec<u8> {
    value
        .as_str()
        .unwrap()
        .as_bytes()
        .as_chunks::<2>()
        .0
        .iter()
        .map(|p| u8::from_str_radix(std::str::from_utf8(p).unwrap(), 16).unwrap())
        .collect()
}
fn fixtures() -> Vec<Value> {
    serde_json::from_str::<Value>(include_str!("../../../vectors/crypto/suite-c.json")).unwrap()
        ["vectors"]
        .as_array()
        .unwrap()
        .clone()
}
fn domain() -> DomainTag<'static> {
    DomainTag::new("gw-btc").unwrap()
}
fn bound(bytes: &[u8]) -> BoundPass<'_> {
    BoundPass::new(suite_c::parse_pass(bytes, domain(), Limits::default()).unwrap()).unwrap()
}
#[test]
fn pinned_kernel_name_derivation_and_exact_ring_pass_correspondence() {
    let rows = fixtures();
    // This independently executed Hoon case catches fixed-width hashing of nam.
    assert!(rows.iter().any(|row| hex(&row["name_key"])[31] == 0));
    for row in &rows {
        let bytes = hex(&row["pass"]);
        let pass = bound(&bytes);
        let name = PeerId(u128::from_le_bytes(
            hex(&row["name128"]).try_into().unwrap(),
        ));
        assert_eq!(pass.name(), name);
        pass.require_name(name).unwrap();
        assert_eq!(
            pass.require_name(PeerId(name.0 ^ 1)),
            Err(Error::NameMismatch)
        );
        assert_eq!(pass.name_key().as_bytes().as_slice(), hex(&row["name_key"]));
        assert_eq!(pass.live_key().as_bytes().as_slice(), hex(&row["live"]));
        let ring_bytes = hex(&row["ring"]);
        let ring = suite_c::parse_current_ring(&ring_bytes, domain(), Limits::default()).unwrap();
        pass.check_ring(&ring).unwrap();
        for other in rows.iter().filter(|other| other["seed"] != row["seed"]) {
            let bytes = hex(&other["ring"]);
            let ring = suite_c::parse_current_ring(&bytes, domain(), Limits::default()).unwrap();
            assert_eq!(pass.check_ring(&ring), Err(Error::RingMismatch));
            let other_name = bound(&hex(&other["pass"])).name();
            if row["genesis"] == other["genesis"] {
                assert_eq!(pass.name(), other_name);
            } else {
                assert_ne!(pass.name(), other_name);
            }
        }
    }
}
#[test]
fn evidence_refresh_preserves_name_but_requires_exact_ring_evidence() {
    let row = &fixtures()[0];
    let bytes = hex(&row["pass"]);
    let original = bound(&bytes);
    let bits = Bits::from_atom_bytes(&bytes).unwrap();
    let field = mat::rub(bits.slice(520, bits.len() - 520).unwrap(), 32768 * 8).unwrap();
    let start = 520 + field.consumed;
    let mut changed = bytes.clone();
    changed[start / 8] ^= 1 << (start % 8);
    let new = bound(&changed);
    assert_eq!(original.name(), new.name());
    let mut ring_bytes = hex(&row["ring"]);
    let ring = suite_c::parse_current_ring(&ring_bytes, domain(), Limits::default()).unwrap();
    assert_eq!(new.check_ring(&ring), Err(Error::RingMismatch));
    ring_bytes[start / 8] ^= 1 << (start % 8);
    let refreshed = suite_c::parse_current_ring(&ring_bytes, domain(), Limits::default()).unwrap();
    new.check_ring(&refreshed).unwrap();
}
#[test]
fn genesis_and_domain_data_are_bound_while_live_key_is_not_immutable_identity() {
    let row = &fixtures()[0];
    let bytes = hex(&row["pass"]);
    let original = bound(&bytes);
    let mut changed = bytes.clone();
    changed[1..33].copy_from_slice(&hex(&row["peer_public"]));
    assert_ne!(original.name(), bound(&changed).name());
    changed = bytes.clone();
    changed[33..65].copy_from_slice(&hex(&row["peer_public"]));
    assert_eq!(original.name(), bound(&changed).name());
    // Change a low bit in the domain-specific tail, leaving mat(domain) intact.
    // Locate dat's bit offset from its encoded size and decoded length.
    let all = Bits::from_atom_bytes(&bytes).unwrap();
    let field = mat::rub(all.slice(520, all.len() - 520).unwrap(), 32768 * 8).unwrap();
    let prefix = mat::rub(field.atom, 63 * 8).unwrap();
    let tail_start = 520 + field.consumed - field.atom.len() + prefix.consumed;
    changed = bytes.clone();
    changed[tail_start / 8] ^= 1 << (tail_start % 8);
    assert_ne!(original.name(), bound(&changed).name());
    for slot in [1..33, 33..65] {
        changed = bytes.clone();
        changed[slot].fill(0);
        let parsed = suite_c::parse_pass(&changed, domain(), Limits::default()).unwrap();
        assert_eq!(BoundPass::new(parsed).unwrap_err(), Error::InvalidKey);
    }
}
