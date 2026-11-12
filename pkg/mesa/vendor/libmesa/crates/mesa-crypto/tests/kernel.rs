use curve25519_dalek::constants::{ED25519_BASEPOINT_POINT, EIGHT_TORSION};
use mesa_crypto::{Error, KeyPair, PublicKey};
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
#[test]
fn pinned_hoon_public_signing_and_bidirectional_agreement() {
    let mut peer_seed = [0; 32];
    peer_seed[0] = 3;
    let peer = KeyPair::from_seed(&peer_seed);
    for row in fixtures() {
        let key = KeyPair::from_seed(&hex(&row["seed"]).try_into().unwrap());
        let public = key.public_key();
        assert_eq!(public.as_bytes().as_slice(), hex(&row["live"]));
        assert_eq!(
            peer.public_key().as_bytes().as_slice(),
            hex(&row["peer_public"])
        );
        let mut message = hex(&row["message"]);
        let signature: [u8; 64] = hex(&row["signature"]).try_into().unwrap();
        assert_eq!(key.sign(&message), signature);
        public.verify(&message, &signature).unwrap();
        // Octets retain high zeros; atom messages do not. Test both Hoon arms.
        message.push(0);
        let octets_signature: [u8; 64] = hex(&row["octets_signature"]).try_into().unwrap();
        assert_eq!(key.sign(&message), octets_signature);
        public.verify(&message, &octets_signature).unwrap();
        assert!(public.verify(&message, &signature).is_err());
        let shared = key.agree(&peer.public_key()).unwrap();
        assert_eq!(shared.as_bytes().as_slice(), hex(&row["shared"]));
        let reverse = peer.agree(&public).unwrap();
        assert_eq!(reverse.as_bytes().as_slice(), hex(&row["reverse_shared"]));
        assert_eq!(shared.as_bytes(), reverse.as_bytes());
    }
}
#[test]
fn rotation_changes_live_signing_and_agreement_and_rejects_cross_life_signatures() {
    let rows = fixtures();
    let key = |i: usize| KeyPair::from_seed(&hex(&rows[i]["seed"]).try_into().unwrap());
    let (old, new) = (key(0), key(1));
    let message = hex(&rows[0]["message"]);
    let (old_sig, new_sig) = (old.sign(&message), new.sign(&message));
    assert_ne!(old.public_key().as_bytes(), new.public_key().as_bytes());
    old.public_key().verify(&message, &old_sig).unwrap();
    new.public_key().verify(&message, &new_sig).unwrap();
    assert!(new.public_key().verify(&message, &old_sig).is_err());
    assert!(old.public_key().verify(&message, &new_sig).is_err());
    let peer = PublicKey::from_bytes(hex(&rows[0]["peer_public"]).try_into().unwrap()).unwrap();
    assert_ne!(
        old.agree(&peer).unwrap().as_bytes(),
        new.agree(&peer).unwrap().as_bytes()
    );
}
#[test]
fn reject_weak_noncanonical_and_mixed_torsion_keys() {
    let mut identity = [0; 32];
    identity[0] = 1;
    let mut noncanonical_identity = [255; 32];
    noncanonical_identity[0] = 0xee;
    noncanonical_identity[31] = 0x7f;
    let mixed = (ED25519_BASEPOINT_POINT + EIGHT_TORSION[1])
        .compress()
        .to_bytes();
    for encoded in [identity, [0; 32], noncanonical_identity, mixed] {
        assert_eq!(
            PublicKey::from_bytes(encoded).unwrap_err(),
            Error::InvalidPoint
        );
    }
}
#[test]
fn reject_signature_malleability_weak_r_and_changed_messages() {
    let key = KeyPair::from_seed(&[7; 32]);
    let public = key.public_key();
    let message = b"libmesa fixture, not a network transcript";
    let signature = key.sign(message);
    assert!(public.verify(b"other message", &signature).is_err());
    let mut noncanonical = signature;
    // S+L preserves the group equation but is a noncanonical signature scalar.
    let order = [
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde,
        0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
    ];
    let mut carry = 0u16;
    for (s, l) in noncanonical[32..].iter_mut().zip(order) {
        let sum = *s as u16 + l as u16 + carry;
        *s = sum as u8;
        carry = sum >> 8;
    }
    assert!(public.verify(message, &noncanonical).is_err());
    let mut weak_r = signature;
    weak_r[..32].fill(0);
    weak_r[0] = 1;
    assert!(public.verify(message, &weak_r).is_err());
}
