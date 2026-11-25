use mesa_core::{protection::SignedPublic, *};

#[path = "support/policies.rs"]
mod policies;
use mesa_crypto::KeyPair;
use mesa_wire::{decode, encode, Auth, Body, Data, Fragment, Name, Packet};
use std::collections::{BTreeMap, VecDeque};

fn view(i: usize) -> PreconfiguredPeer {
    PreconfiguredPeer::new(
        PeerId(i as u128 + 1),
        "gw-btc",
        1,
        0,
        KeyHandle(i as u64 + 1),
    )
    .unwrap()
}
fn key(i: usize) -> KeyPair {
    KeyPair::from_seed(&[i as u8 + 1; 32])
}
fn profile(i: usize) -> SignedPublic {
    SignedPublic::new(
        view(i).key,
        key(i),
        BTreeMap::from([
            (view(1 - i).key, key(1 - i).public_key()),
            (KeyHandle(9), key(9).public_key()),
        ]),
    )
}
fn bytes(n: usize) -> Vec<u8> {
    (0..n)
        .map(|i| if i % 29 > 20 { 0 } else { (i % 251) as u8 })
        .collect()
}
#[derive(Clone)]
struct Frame {
    from: usize,
    bytes: Vec<u8>,
}
struct Harness {
    engines: [Engine<SignedPublic>; 2],
    queue: VecDeque<Frame>,
    results: Vec<(usize, ReadId, Result<Vec<u8>, ReadFailure>)>,
    drops: Vec<DropReason>,
    trace: Vec<(usize, Vec<Action>)>,
}
impl Harness {
    fn new(config: Config) -> Self {
        let policy = policy::TransferPolicy::fixed(&config);
        Self::with_policy(config, policy)
    }
    fn with_policy(config: Config, policy: policy::TransferPolicy) -> Self {
        let mut h = Self {
            engines: [
                Engine::with_policy(config.clone(), view(0), profile(0), policy).unwrap(),
                Engine::with_policy(config, view(1), profile(1), policy).unwrap(),
            ],
            queue: VecDeque::new(),
            results: vec![],
            drops: vec![],
            trace: vec![],
        };
        for i in 0..2 {
            h.step(i, Event::InstallPeer(view(1 - i)));
            h.step(
                i,
                Event::SetAttachment {
                    peer: view(1 - i).id,
                    attachment: Some(Attachment(7)),
                },
            );
        }
        h
    }
    fn step(&mut self, i: usize, event: Event<'_>) {
        let actions = self.engines[i].step(event).unwrap();
        self.trace.push((i, actions.clone()));
        for action in actions {
            match action {
                Action::Send { bytes, .. } => self.queue.push_back(Frame { from: i, bytes }),
                Action::ReadFinished { id, result } => self.results.push((i, id, result)),
                Action::Dropped(reason) => self.drops.push(reason),
                _ => {}
            }
        }
    }
    fn bind(&mut self, i: usize, path: &[u8], data: &[u8]) {
        self.step(
            i,
            Event::Bind {
                path: path.into(),
                bytes: data.into(),
                access: Access::Public,
            },
        );
    }
    fn read(&mut self, i: usize, id: ReadId, path: &[u8]) {
        self.step(
            i,
            Event::Read {
                id,
                peer: view(1 - i).id,
                path: path.into(),
            },
        );
    }
    fn deliver(&mut self, f: Frame) {
        self.step(
            1 - f.from,
            Event::Receive {
                claimed_peer: view(f.from).id,
                attachment: Attachment(7),
                bytes: &f.bytes,
            },
        );
    }
    fn next(&mut self) {
        let f = self.queue.pop_front().unwrap();
        self.deliver(f);
    }
    fn pump(&mut self) {
        for _ in 0..10000 {
            let Some(f) = self.queue.pop_front() else {
                return;
            };
            self.deliver(f);
        }
        panic!("delivery loop");
    }
    fn tick(&mut self, t: u64) {
        for i in 0..2 {
            self.step(i, Event::AdvanceTo(t));
        }
    }
    fn proof(&mut self, data: &[u8]) -> Frame {
        self.bind(1, b"value", data);
        self.read(0, 1, b"value");
        self.next();
        self.queue.pop_front().unwrap()
    }
    fn idle(&self) {
        for e in &self.engines {
            let u = e.usage();
            assert_eq!(
                (
                    u.reads,
                    u.reassembly_bytes,
                    u.pending_interests,
                    u.unverified_bytes,
                    u.unverified_fragments
                ),
                (0, 0, 0, 0, 0)
            );
        }
    }
}
fn rewrite<'a>(f: &'a Frame, change: impl FnOnce(&mut Name<'a>, &mut Data<'a>)) -> Frame {
    let Body::Page {
        mut name,
        mut data,
        next,
    } = decode(&f.bytes, Config::default().wire).unwrap().body
    else {
        panic!()
    };
    change(&mut name, &mut data);
    Frame {
        from: f.from,
        bytes: encode(
            &Packet {
                hop: 0,
                body: Body::Page { name, data, next },
            },
            Config::default().wire,
        )
        .unwrap(),
    }
}

#[test]
fn signed_reads_both_directions_at_boundaries_and_protocol_maximum() {
    for n in [0, 1, 1023, 1024, 1025, 2048, 7169, 16384, 1048576] {
        let mut h = Harness::new(Config::default());
        let data = bytes(n);
        for i in 0..2 {
            h.bind(i, b"value", &data);
            h.read(i, 1, b"value");
        }
        h.pump();
        h.results.sort_by_key(|r| r.0);
        assert_eq!(h.results, vec![(0, 1, Ok(data.clone())), (1, 1, Ok(data))]);
        h.idle();
    }
}

#[test]
fn signature_spine_context_and_declared_size_corruption_do_not_allocate_or_finish() {
    let mut h = Harness::new(Config::default());
    let proof = h.proof(&bytes(5000));
    let before = h.engines[0].usage();
    let Body::Page { data, .. } = decode(&proof.bytes, Config::default().wire).unwrap().body else {
        panic!()
    };
    let mut spine = data.fragment.to_vec();
    spine[0] ^= 1;
    let Auth::Signature(sig) = data.auth else {
        panic!()
    };
    let mut sig = sig.to_vec();
    sig[0] ^= 1;
    let cases = vec![
        rewrite(&proof, |_, d| d.auth = Auth::None),
        rewrite(&proof, |_, d| d.auth = Auth::Signature(b"short")),
        rewrite(&proof, |_, d| d.auth = Auth::Signature(&sig)),
        rewrite(&proof, |_, d| d.fragment = &spine),
        rewrite(&proof, |_, d| d.total_bytes += 1),
        rewrite(&proof, |_, d| d.total_bytes = u64::MAX),
        rewrite(&proof, |n, _| n.path = b"other"),
        rewrite(&proof, |n, _| n.domain = DomainTag::new("other").unwrap()),
        rewrite(&proof, |n, _| n.rift = 1),
        rewrite(&proof, |n, _| n.bloq = 12),
        rewrite(&proof, |n, _| n.publisher = PeerId(99)),
    ];
    for f in cases {
        h.deliver(f);
        assert_eq!(h.engines[0].usage(), before);
        assert!(h.results.is_empty());
    }
    h.deliver(proof);
    h.pump();
    assert_eq!(h.results, vec![(0, 1, Ok(bytes(5000)))]);
    h.idle();
}

#[test]
fn every_signed_context_field_is_cryptographically_bound() {
    let tree = mesa_lss::Tree::build(b"hello").unwrap();
    let publisher = view(1);
    let signer = profile(1);
    let verifier = profile(0);
    let name = Name {
        publisher: publisher.id,
        domain: publisher.domain(),
        rift: 0,
        bloq: 13,
        fragment: Fragment::Auth(0),
        path: b"value",
    };
    let signature = signer.sign_root(&publisher, name, 5, &tree.root()).unwrap();
    assert!(verifier.verify_root(&publisher, name, 5, &tree.root(), &signature));
    for changed in [
        Name {
            publisher: PeerId(99),
            ..name
        },
        Name {
            domain: DomainTag::new("other").unwrap(),
            ..name
        },
        Name { rift: 1, ..name },
        Name { bloq: 12, ..name },
        Name {
            path: b"value2",
            ..name
        },
    ] {
        assert!(!verifier.verify_root(&publisher, changed, 5, &tree.root(), &signature));
    }
    assert!(!verifier.verify_root(&publisher, name, 6, &tree.root(), &signature));
    assert!(!verifier.verify_root(&publisher, name, 5, &[0; 32], &signature));
    let mut changed = publisher.clone();
    changed.life = 2;
    assert!(!verifier.verify_root(&changed, name, 5, &tree.root(), &signature));
    changed = publisher.clone();
    changed.key = KeyHandle(9);
    assert!(!verifier.verify_root(&changed, name, 5, &tree.root(), &signature));
}

#[test]
fn corrupt_leaves_pairs_and_buffer_poisoning_recover_through_retry() {
    let mut h = Harness::new(Config::default());
    let proof = h.proof(&bytes(16 * 1024));
    h.deliver(proof);
    for _ in 0..4 {
        h.next();
    } // all four data replies are queued
    let pages: Vec<_> = h.queue.drain(..).collect();
    // Buffer a corrupt out-of-order leaf, then verify the predecessor.
    let Body::Page { data, .. } = decode(&pages[1].bytes, Config::default().wire)
        .unwrap()
        .body
    else {
        panic!()
    };
    let mut bad = data.fragment.to_vec();
    bad[0] ^= 1;
    h.deliver(rewrite(&pages[1], |_, d| d.fragment = &bad));
    assert_eq!(h.engines[0].usage().received_fragments, 0);
    assert_eq!(h.engines[0].usage().unverified_bytes, 1024);
    h.deliver(pages[0].clone());
    assert_eq!(h.engines[0].usage().received_fragments, 1);
    assert_eq!(h.engines[0].usage().unverified_bytes, 0);
    // Index 1 carries a pair. A bad pair must not advance the verifier.
    let mut bad_pair = [0; 64];
    bad_pair[0] = 1;
    h.deliver(rewrite(&pages[1], |_, d| d.auth = Auth::Pair(&bad_pair)));
    assert_eq!(h.engines[0].usage().received_fragments, 1);
    h.deliver(pages[3].clone());
    h.deliver(pages[2].clone());
    assert!(h.results.is_empty());
    h.tick(100);
    h.pump();
    assert_eq!(h.results, vec![(0, 1, Ok(bytes(16 * 1024)))]);
    h.idle();
    assert!(
        h.drops
            .iter()
            .filter(|d| **d == DropReason::Protection)
            .count()
            >= 2
    );
}

#[test]
fn seeded_loss_duplicate_reordering_and_corruption_schedules_return_exact_bytes() {
    for mut seed in 1u64..=16 {
        let mut rng = || {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
            seed
        };
        let mut h = Harness::new(Config {
            retry_after: 10,
            read_timeout: 500,
            max_attempts: 20,
            ..Config::default()
        });
        for i in 0..2 {
            h.bind(i, b"value", &bytes(17425));
            h.read(i, 1, b"value");
        }
        let mut queue: Vec<(u64, Frame)> = vec![];
        for now in 0..=500 {
            h.tick(now);
            while let Some(f) = h.queue.pop_front() {
                if now < 60 && rng() % 5 == 0 {
                    continue;
                }
                let due = now + rng() % 8;
                if now < 60 && rng() % 4 == 0 {
                    queue.push((due + 3, f.clone()));
                }
                if now < 60 && rng() % 7 == 0 {
                    let mut corrupt = f.clone();
                    *corrupt.bytes.last_mut().unwrap() ^= 1;
                    queue.push((due, corrupt));
                }
                queue.push((due, f));
            }
            while let Some(i) = queue.iter().rposition(|(due, _)| *due <= now) {
                let (_, f) = queue.swap_remove(i);
                h.deliver(f);
            }
            for e in &h.engines {
                assert!(e.usage().unverified_bytes <= 4 * 1024);
                assert!(e.usage().pending_interests <= 4);
            }
            if h.results.len() == 2 {
                break;
            }
        }
        h.results.sort_by_key(|r| r.0);
        assert_eq!(
            h.results,
            vec![(0, 1, Ok(bytes(17425))), (1, 1, Ok(bytes(17425)))]
        );
        h.idle();
    }
}

#[test]
fn cancellation_breach_block_and_timeout_release_verified_and_unverified_state() {
    for mode in 0..4 {
        let mut h = Harness::new(Config::default());
        let proof = h.proof(&bytes(5000));
        h.deliver(proof);
        for _ in 0..4 {
            h.next();
        }
        let f = h.queue.pop_back().unwrap();
        h.deliver(f);
        assert!(h.engines[0].usage().unverified_bytes > 0);
        let (event, expected) = match mode {
            0 => (Event::Cancel { id: 1 }, ReadFailure::Cancelled),
            1 => {
                let mut v = view(1);
                v.rift = 1;
                (Event::RemoteBreach(v), ReadFailure::PeerReset)
            }
            2 => (
                Event::SetService {
                    peer: view(1).id,
                    allowed: false,
                },
                ReadFailure::Blocked,
            ),
            _ => (Event::AdvanceTo(2000), ReadFailure::Timeout),
        };
        h.step(0, event);
        h.pump();
        assert_eq!(h.results, vec![(0, 1, Err(expected))]);
        h.idle();
    }
}

#[test]
fn bounded_unverified_buffer_drops_excess_without_poisoning_read() {
    let mut h = Harness::new(Config {
        max_unverified_bytes: 1024,
        ..Config::default()
    });
    let proof = h.proof(&bytes(5000));
    h.deliver(proof);
    for _ in 0..4 {
        h.next();
    }
    let pages: Vec<_> = h.queue.drain(..).collect();
    h.deliver(pages[2].clone());
    h.deliver(pages[3].clone());
    assert_eq!(h.engines[0].usage().unverified_bytes, 1024);
    assert_eq!(h.drops.last(), Some(&DropReason::ResourceLimit));
    h.deliver(pages[0].clone());
    h.deliver(pages[1].clone());
    h.tick(100);
    h.pump();
    assert_eq!(h.results, vec![(0, 1, Ok(bytes(5000)))]);
    h.idle();
}

#[test]
fn signed_resource_overflow_and_unsigned_metadata_have_distinct_effects() {
    let mut h = Harness::new(Config::default());
    let proof = h.proof(&bytes(5000));
    // Smaller receiver limit, same installed peer and outstanding request.
    let mut receiver = Engine::new(
        Config {
            max_reassembly_bytes: 1024,
            ..Config::default()
        },
        view(0),
        profile(0),
    )
    .unwrap();
    receiver.step(Event::InstallPeer(view(1))).unwrap();
    receiver
        .step(Event::SetAttachment {
            peer: view(1).id,
            attachment: Some(Attachment(7)),
        })
        .unwrap();
    receiver
        .step(Event::Read {
            id: 1,
            peer: view(1).id,
            path: b"value".into(),
        })
        .unwrap();
    h.engines[0] = receiver;
    h.deliver(proof);
    assert_eq!(h.results, vec![(0, 1, Err(ReadFailure::ResourceLimit))]);
    h.idle();
}

#[test]
fn key_updates_pin_existing_root_but_reject_old_signatures_for_new_reads() {
    let mut h = Harness::new(Config::default());
    let proof = h.proof(&bytes(5000));
    h.deliver(proof.clone());
    let before = h.engines[0].usage();
    let mut rotated = view(1);
    rotated.life = 2;
    rotated.key = KeyHandle(9);
    h.step(0, Event::InstallPeer(rotated));
    assert_eq!(h.engines[0].usage(), before);
    h.pump();
    assert_eq!(h.results, vec![(0, 1, Ok(bytes(5000)))]);
    h.read(0, 2, b"value");
    h.deliver(proof);
    assert_eq!(h.drops.last(), Some(&DropReason::Protection));
    assert_eq!(h.engines[0].usage().reassembly_bytes, 0);
    h.step(0, Event::Cancel { id: 2 });
    h.idle();
}

#[test]
fn private_bindings_and_missing_keys_fail_closed() {
    let mut h = Harness::new(Config::default());
    assert_eq!(
        h.engines[0].step(Event::Bind {
            path: b"private".into(),
            bytes: b"secret".into(),
            access: Access::Peer(view(1).id)
        }),
        Err(Error::UnsupportedAccess)
    );
    let mut unknown = view(1);
    unknown.key = KeyHandle(42);
    assert_eq!(
        h.engines[0].step(Event::InstallPeer(unknown)),
        Err(Error::MissingKey)
    );
    assert!(matches!(
        Engine::new(
            Config {
                bloq: 12,
                ..Config::default()
            },
            view(0),
            profile(0)
        ),
        Err(Error::InvalidConfig)
    ));
    let mut bad_local = view(0);
    bad_local.key = KeyHandle(42);
    assert!(matches!(
        Engine::new(Config::default(), bad_local, profile(0)),
        Err(Error::MissingKey)
    ));
}

#[test]
fn independent_python_context_and_openssl_signature_match() {
    fn unhex(s: &str) -> Vec<u8> {
        s.as_bytes()
            .as_chunks::<2>()
            .0
            .iter()
            .map(|c| u8::from_str_radix(std::str::from_utf8(c).unwrap(), 16).unwrap())
            .collect()
    }
    let v: serde_json::Value =
        serde_json::from_str(include_str!("../../../vectors/lss/signed-public.json")).unwrap();
    let local = PreconfiguredPeer::new(
        PeerId(u128::from_str_radix(v["publisher"].as_str().unwrap(), 16).unwrap()),
        v["domain"].as_str().unwrap(),
        v["life"].as_u64().unwrap() as u32,
        v["rift"].as_u64().unwrap() as u32,
        KeyHandle(7),
    )
    .unwrap();
    let path = unhex(v["path"].as_str().unwrap());
    let root: [u8; 32] = unhex(v["root"].as_str().unwrap()).try_into().unwrap();
    let key = KeyPair::from_seed(&unhex(v["seed"].as_str().unwrap()).try_into().unwrap());
    assert_eq!(
        key.public_key().as_bytes().as_slice(),
        unhex(v["public"].as_str().unwrap())
    );
    let name = Name {
        publisher: local.id,
        domain: local.domain(),
        rift: local.rift,
        bloq: 13,
        fragment: Fragment::Auth(0),
        path: &path,
    };
    let transcript = mesa_core::protection::root_context(&local, name, 17425, &root);
    assert_eq!(transcript, unhex(v["context"].as_str().unwrap()));
    assert_eq!(
        key.sign(&transcript).as_slice(),
        unhex(v["signature"].as_str().unwrap())
    );
}
