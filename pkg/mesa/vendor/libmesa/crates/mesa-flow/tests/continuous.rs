mod support;
use mesa_core::{Attachment, DropReason};
use mesa_flow::*;
use support::*;

fn config_batch(batch: u64) -> Config {
    let mut c = config();
    c.max_messages = batch;
    c.window = c.window.min(batch as usize);
    c
}
fn control(frame: &Frame) -> bool {
    let packet = mesa_wire::decode(&frame.bytes, config().core.wire).unwrap();
    let path = match packet.body {
        mesa_wire::Body::Poke { payload, .. } => payload.path,
        mesa_wire::Body::Peek(name) | mesa_wire::Body::Page { name, .. } => name.path,
    };
    path.windows(9).any(|w| w == b"/release/") || path.windows(10).any(|w| w == b"/released/")
}
fn bounds(h: &Harness) {
    for node in &h.nodes {
        let f = node.checkpoint().flow(7).unwrap();
        assert!(f.valid(h.config.max_messages));
        assert_eq!(
            Some(node.checkpoint().revision),
            node.checkpoint().expected_revision()
        );
        assert!(node.usage().retained_receipts <= 2 * h.config.max_messages as usize);
        assert!(node.core_usage().namespace_slots <= 2 * h.config.max_messages as usize + 6);
        assert!(node.usage().outbox_messages <= h.config.window);
        assert!(node.usage().buffered_messages <= h.config.window);
        assert!(node.core_usage().reads <= h.config.window * 2);
        assert!(
            serde_json::to_vec(node.checkpoint()).unwrap().len()
                <= 4 * h.config.max_outbox_bytes + h.config.max_messages as usize * 512 + 4096
        );
    }
}

#[test]
fn lost_release_ack_backpressures_until_resume_and_rejects_replay_after_restart() {
    let mut c = config_batch(2);
    c.transfer.recovery.max_attempts = 2;
    let mut h = Harness::new(c);
    h.send(0, 0, payload(1025));
    h.send(0, 1, payload(2049));
    let mut replay = vec![];
    // Deliver the release proof but lose every receiver control packet. The
    // receiver commits retirement; the sender keeps both outcome records.
    for now in 0..200 {
        h.tick(now);
        while let Some(f) = h.queue.pop_front() {
            replay.push(f.clone());
            if f.from == 1
                && control(&f)
                && !matches!(
                    mesa_wire::decode(&f.bytes, config().core.wire)
                        .unwrap()
                        .body,
                    mesa_wire::Body::Peek(_)
                )
            {
                continue;
            }
            h.deliver(f);
        }
    }
    assert_eq!(h.nodes[1].checkpoint().flow(7).unwrap().receive_floor, 2);
    assert_eq!(h.nodes[0].checkpoint().flow(7).unwrap().send_floor, 0);
    assert_eq!(h.nodes[0].checkpoint().flow(7).unwrap().settled.len(), 2);
    assert_eq!(
        h.nodes[0]
            .step(Event::Enqueue {
                flow: 7,
                sequence: 2,
                bytes: vec![]
            })
            .unwrap_err(),
        Error::Backpressure
    );
    assert!(h.trace.iter().flat_map(|(_, a)| a).any(|a| matches!(
        a,
        Action::AnnouncementStalled {
            kind: ReadKind::Release,
            ..
        }
    )));
    h.tick(1000);
    assert!(h.queue.is_empty());
    h.restart(1);
    h.step(0, Event::Resume { flow: 7 });
    h.run(200);
    assert_eq!(h.nodes[0].checkpoint().flow(7).unwrap().send_floor, 2);
    h.restart(0);
    h.restart(1);
    let before = [h.stores[0].disk.clone(), h.stores[1].disk.clone()];
    for f in replay {
        h.deliver(f);
    }
    h.run(100);
    assert_eq!([h.stores[0].disk.clone(), h.stores[1].disk.clone()], before);
    assert_eq!(h.stores[1].records.len(), 2);
    h.send(0, 2, b"next batch".to_vec());
    h.run(200);
    assert_eq!(h.stores[1].records.len(), 3);
    bounds(&h);
}

#[test]
fn untrusted_control_hints_and_corrupted_proofs_cannot_advance_floors() {
    let mut h = Harness::new(config_batch(2));
    for seq in 0..2 {
        h.send(0, seq, payload(1025));
    }
    let mut hint = None;
    for now in 0..20 {
        h.tick(now);
        while let Some(f) = h.queue.pop_front() {
            if control(&f) {
                hint = Some(f);
                continue;
            }
            h.deliver(f);
        }
    }
    let hint = hint.expect("sender release hint");
    let before = h.stores[1].disk.clone();
    for _ in 0..20 {
        h.deliver(hint.clone());
    }
    assert_eq!(h.stores[1].disk, before, "hints are not durable evidence");
    // Alter the signature on every authentic release root while preserving
    // the name, then let the finite read and announcement budgets expire.
    for now in 20..1000 {
        h.tick(now);
        while let Some(mut f) = h.queue.pop_front() {
            if f.from == 0 && control(&f) {
                if let mesa_wire::Body::Page { name, data, .. } =
                    mesa_wire::decode(&f.bytes, config().core.wire)
                        .unwrap()
                        .body
                {
                    if name.fragment == mesa_wire::Fragment::Auth(0) {
                        let mut sig = match data.auth {
                            mesa_wire::Auth::Signature(s) => s.to_vec(),
                            _ => panic!("signed root"),
                        };
                        sig[0] ^= 1;
                        let mut packet = mesa_wire::decode(&f.bytes, config().core.wire).unwrap();
                        if let mesa_wire::Body::Page { data, .. } = &mut packet.body {
                            data.auth = mesa_wire::Auth::Signature(sig.as_slice());
                        }
                        f.bytes = mesa_wire::encode(&packet, config().core.wire).unwrap();
                    }
                }
            }
            h.deliver(f);
        }
    }
    assert_eq!(h.stores[1].disk, before);
    assert_eq!(h.nodes[0].checkpoint().flow(7).unwrap().send_floor, 0);
    assert!(h
        .trace
        .iter()
        .flat_map(|(_, a)| a)
        .any(|a| matches!(a, Action::Dropped(DropReason::Protection))));
    h.step(0, Event::Resume { flow: 7 });
    h.step(1, Event::Resume { flow: 7 });
    h.run(100);
    assert_eq!(h.nodes[0].checkpoint().flow(7).unwrap().send_floor, 2);
}

fn soak(initial: u64) -> [Vec<u8>; 2] {
    let mut seed = initial;
    let mut rng = || {
        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
        seed
    };
    let mut h = Harness::new(config_batch(8));
    let mut delayed = vec![];
    let mut replay = vec![];
    let mut trace = vec![];
    for now in 0..20000 {
        h.tick(now);
        if now == 60 {
            for i in 0..2 {
                h.step(
                    i,
                    Event::SetAttachment {
                        peer: view(1 - i).id,
                        attachment: None,
                    },
                );
            }
        }
        if now == 90 {
            for i in 0..2 {
                h.step(
                    i,
                    Event::SetAttachment {
                        peer: view(1 - i).id,
                        attachment: Some(Attachment(7)),
                    },
                );
            }
        }
        if now == 150 || now == 600 {
            h.restart((now / 150) as usize % 2);
        }
        if now % 300 == 0 {
            for i in 0..2 {
                h.step(i, Event::Resume { flow: 7 });
            }
        }
        for i in 0..2 {
            let f = h.nodes[i].checkpoint().flow(7).unwrap();
            if f.next_send < 128 && f.next_send - f.send_floor < 8 && f.outbox.len() < 4 {
                h.send(
                    i,
                    f.next_send,
                    payload([0, 1024, 1025, 4097][f.next_send as usize % 4]),
                );
            }
        }
        while let Some(f) = h.queue.pop_front() {
            if replay.len() < 80 && control(&f) {
                replay.push(f.clone());
            }
            if rng().is_multiple_of(13) {
                continue;
            }
            let due = now + rng() % 8;
            if rng().is_multiple_of(7) {
                delayed.push((due + 2, f.clone()));
            }
            delayed.push((due, f));
        }
        while let Some(index) = delayed.iter().rposition(|(due, _)| *due <= now) {
            let (_, f) = delayed.swap_remove(index);
            h.deliver(f);
        }
        bounds(&h);
        // Hash each event group so determinism checks do not retain the host's
        // full history of payload copies in memory during the soak.
        trace.extend_from_slice(&mesa_crypto::sha256(format!("{:?}", h.trace).as_bytes()));
        h.trace.clear();
        if h.nodes
            .iter()
            .all(|n| n.checkpoint().flow(7).unwrap().send_floor == 128)
        {
            break;
        }
    }
    for i in 0..2 {
        assert_eq!(h.nodes[i].checkpoint().flow(7).unwrap().send_floor, 128);
        assert_eq!(h.stores[i].records.len(), 128);
        assert_eq!(
            h.stores[i]
                .order
                .iter()
                .map(|id| id.sequence)
                .collect::<Vec<_>>(),
            (0..128).collect::<Vec<_>>()
        );
        h.restart(i);
    }
    let before = [h.stores[0].disk.clone(), h.stores[1].disk.clone()];
    for f in replay {
        h.deliver(f);
    }
    for (_, f) in delayed {
        h.deliver(f);
    }
    h.run(50);
    assert_eq!([h.stores[0].disk.clone(), h.stores[1].disk.clone()], before);
    [mesa_crypto::sha256(&trace).to_vec(), before.concat()]
}
#[test]
fn continuous_seeded_loss_duplication_reordering_disconnects_and_restarts_are_deterministic() {
    for seed in [1, 7, 91] {
        assert_eq!(soak(seed), soak(seed), "seed {seed}");
    }
}
