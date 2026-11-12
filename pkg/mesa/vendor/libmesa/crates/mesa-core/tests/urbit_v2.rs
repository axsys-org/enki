//! Replay bytes emitted by the Hoon Gall endpoint on an actual fake ship.
//! This regression does not itself start Urbit; interop/boot.py does that.
use mesa_core::{protection::SignedPublic, *};
use mesa_crypto::KeyPair;
use std::collections::BTreeMap;

fn engine(wrong_key: bool) -> Engine<SignedPublic> {
    let local = PreconfiguredPeer::new(PeerId(1), "gw-btc", 1, 0, KeyHandle(1)).unwrap();
    let remote = PreconfiguredPeer::new(PeerId(2), "gw-btc", 1, 0, KeyHandle(2)).unwrap();
    let protection = SignedPublic::new(
        local.key,
        KeyPair::from_seed(&[1; 32]),
        BTreeMap::from([(
            remote.key,
            KeyPair::from_seed(&[if wrong_key { 3 } else { 2 }; 32]).public_key(),
        )]),
    );
    let mut e = Engine::new(Config::default(), local, protection).unwrap();
    e.step(Event::InstallPeer(remote)).unwrap();
    e.step(Event::SetAttachment {
        peer: PeerId(2),
        attachment: Some(Attachment(1)),
    })
    .unwrap();
    e.step(Event::Read {
        id: 1,
        peer: PeerId(2),
        path: b"example/value".to_vec(),
    })
    .unwrap();
    e
}

fn replay(e: &mut Engine<SignedPublic>, fixture: &str) -> (usize, Vec<Action>) {
    let fixture: serde_json::Value = serde_json::from_str(fixture).unwrap();
    let mut actions = vec![];
    for value in fixture["pages"].as_array().unwrap() {
        let text = value.as_str().unwrap();
        let bytes: Vec<_> = (0..text.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap())
            .collect();
        actions.extend(
            e.step(Event::Receive {
                claimed_peer: PeerId(2),
                attachment: Attachment(1),
                bytes: &bytes,
            })
            .unwrap(),
        );
    }
    (fixture["length"].as_u64().unwrap() as usize, actions)
}

const FIXTURES: [&str; 3] = [
    include_str!("../../../vectors/urbit-v2/0.json"),
    include_str!("../../../vectors/urbit-v2/1.json"),
    include_str!("../../../vectors/urbit-v2/17425.json"),
];

#[test]
fn captured_hoon_pages_authenticate_and_reassemble_exact_bytes() {
    for fixture in FIXTURES {
        let mut e = engine(false);
        let (length, actions) = replay(&mut e, fixture);
        let expected: Vec<u8> = (0..length)
            .map(|i| {
                if i % 29 >= 24 {
                    0
                } else {
                    ((i + 2) % 251) as u8
                }
            })
            .collect();
        let completions: Vec<_> = actions
            .iter()
            .filter(|a| matches!(a, Action::ReadFinished { .. }))
            .collect();
        assert_eq!(completions.len(), 1);
        assert_eq!(
            completions[0],
            &Action::ReadFinished {
                id: 1,
                result: Ok(expected)
            }
        );
        assert_eq!(e.usage().reads, 0);
        assert_eq!(e.usage().pending_interests, 0);
        assert_eq!(e.usage().reassembly_bytes, 0);
    }
}

#[test]
fn captured_hoon_pages_do_not_authenticate_under_another_configured_key() {
    for fixture in FIXTURES {
        let mut e = engine(true);
        let (_, actions) = replay(&mut e, fixture);
        assert!(!actions
            .iter()
            .any(|a| matches!(a, Action::ReadFinished { .. })));
        assert_eq!(e.usage().reassembly_bytes, 0);
        let actions = e.step(Event::AdvanceTo(1_000_000)).unwrap();
        assert!(actions.contains(&Action::ReadFinished {
            id: 1,
            result: Err(ReadFailure::Timeout)
        }));
        assert_eq!(e.usage().reads, 0);
        assert_eq!(e.usage().pending_interests, 0);
    }
}
