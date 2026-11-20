mod support;
use mesa_core::{Attachment, PeerId};
use mesa_flow::*;
use support::*;

#[test]
fn signed_bidirectional_windows_deliver_in_order_and_release_soft_state() {
    let mut h = Harness::new(config());
    for i in 0..2 {
        for (seq, size) in [0, 1024, 1025, 17425].into_iter().enumerate() {
            h.send(i, seq as u64, payload(size));
        }
    }
    // Deliver the last announcement first; verified prefetch must not reorder effects.
    while let Some(f) = h.queue.pop_back() {
        h.deliver(f);
    }
    h.run(200);
    for i in 0..2 {
        h.settled(i, 4);
        assert_eq!(
            h.stores[i]
                .order
                .iter()
                .map(|id| id.sequence)
                .collect::<Vec<_>>(),
            vec![0, 1, 2, 3]
        );
        for (seq, size) in [0, 1024, 1025, 17425].into_iter().enumerate() {
            let id = &h.stores[i].order[seq];
            assert_eq!(h.stores[i].records[id], (payload(size), Outcome::Accepted));
        }
        assert_eq!(h.nodes[i].core_usage().reads, 0);
        assert_eq!(h.nodes[i].core_usage().pending_interests, 0);
        assert_eq!(h.nodes[i].core_usage().reassembly_bytes, 0);
    }
}

#[test]
fn enqueue_waits_for_commit_and_window_and_byte_budgets_apply() {
    let mut c = config();
    c.window = 1;
    c.max_outbox_bytes = 4;
    let mut h = Harness::new(c);
    h.auto = false;
    h.send(0, 0, b"four".to_vec());
    let (i, request) = h.commits.pop_front().unwrap();
    assert_eq!(i, 0);
    assert!(
        h.queue.is_empty(),
        "uncommitted outbox must not publish or announce"
    );
    assert!(h.nodes[0].checkpoint().flow(7).unwrap().outbox.is_empty());
    assert_eq!(
        h.nodes[0]
            .step(Event::Enqueue {
                flow: 7,
                sequence: 0,
                bytes: vec![]
            })
            .unwrap_err(),
        Error::Busy
    );
    let bad = CommitToken {
        incarnation: 999,
        ..request.token
    };
    assert_eq!(
        h.nodes[0].step(Event::Committed(bad)).unwrap_err(),
        Error::CommitToken
    );
    h.stores[0].commit(&request).unwrap();
    h.step(0, Event::Committed(request.token));
    assert!(!h.queue.is_empty());
    assert_eq!(
        h.nodes[0]
            .step(Event::Committed(request.token))
            .unwrap_err(),
        Error::CommitToken
    );
    assert_eq!(
        h.nodes[0]
            .step(Event::Enqueue {
                flow: 7,
                sequence: 1,
                bytes: vec![]
            })
            .unwrap_err(),
        Error::Backpressure
    );
    assert_eq!(
        h.nodes[0]
            .step(Event::Enqueue {
                flow: 7,
                sequence: 0,
                bytes: vec![]
            })
            .unwrap_err(),
        Error::Sequence
    );
    h.auto = true;
    h.run(100);
    h.settled(0, 1);
    assert_eq!(
        h.nodes[0]
            .step(Event::Enqueue {
                flow: 7,
                sequence: 1,
                bytes: vec![0; 5]
            })
            .unwrap_err(),
        Error::Backpressure
    );
    h.send(0, 1, vec![1; 4]);
    h.run(100);
    h.settled(0, 2);
}

fn pending_delivery() -> (Harness, MessageId) {
    let mut h = Harness::new(config());
    h.send(0, 0, payload(1025));
    h.auto = false;
    h.pump();
    let (i, id, bytes) = h.deliveries.pop_front().unwrap();
    assert_eq!(i, 1);
    assert_eq!(bytes, payload(1025));
    (h, id)
}

#[test]
fn delivery_callback_and_commit_proposal_never_authorize_early_ack() {
    let (mut h, id) = pending_delivery();
    h.run(40);
    assert!(h.stores[1].records.is_empty());
    assert!(h.nodes[1].checkpoint().flow(7).unwrap().received.is_empty());
    assert_eq!(h.stores[0].checkpoint().flow(7).unwrap().outbox.len(), 1);
    h.step(
        1,
        Event::Decide {
            id: id.clone(),
            outcome: Outcome::Accepted,
        },
    );
    let (i, request) = h.commits.pop_front().unwrap();
    assert_eq!(i, 1);
    h.run(40); // a proposal is still not durable
    assert!(h.stores[1].records.is_empty());
    assert_eq!(h.stores[0].checkpoint().flow(7).unwrap().outbox.len(), 1);
    assert!(!h
        .trace
        .iter()
        .flat_map(|(_, a)| a)
        .any(|a| matches!(a, Action::Settled { .. })));
    h.stores[1].commit(&request).unwrap();
    assert_eq!(h.stores[1].records.len(), 1);
    h.run(40); // storage success without callback still publishes nothing
    assert_eq!(h.stores[0].checkpoint().flow(7).unwrap().outbox.len(), 1);
    h.auto = true;
    h.step(1, Event::Committed(request.token));
    h.run(100);
    h.settled(0, 1);
}

#[test]
fn terminal_nack_is_durable_and_does_not_repeat_after_restart() {
    let (mut h, id) = pending_delivery();
    h.step(
        1,
        Event::Decide {
            id: id.clone(),
            outcome: Outcome::Rejected(23),
        },
    );
    h.auto = true;
    h.flush();
    h.run(100);
    h.settled(0, 1);
    assert_eq!(
        h.stores[0].checkpoint().flow(7).unwrap().settled[&0],
        Outcome::Rejected(23)
    );
    assert_eq!(h.stores[1].records[&id].1, Outcome::Rejected(23));
    h.restart(0);
    h.restart(1);
    h.run(100);
    assert_eq!(h.stores[1].records.len(), 1);
    h.settled(0, 1);
    assert!(h.stores[1].effects.is_empty());
}

#[test]
fn crash_matrix_covers_enqueue_delivery_and_settlement_commit_boundaries() {
    // Three transactions, each crashed before storage, after storage but before
    // callback, and after callback with all queued packets lost.
    for transaction in 0..3 {
        for boundary in 0..3 {
            let (mut h, who, request) = if transaction == 0 {
                let mut h = Harness::new(config());
                h.auto = false;
                h.send(0, 0, payload(1025));
                let (who, r) = h.commits.pop_front().unwrap();
                (h, who, r)
            } else {
                let (mut h, id) = pending_delivery();
                h.step(
                    1,
                    Event::Decide {
                        id,
                        outcome: Outcome::Accepted,
                    },
                );
                let (who, r) = h.commits.pop_front().unwrap();
                if transaction == 1 {
                    (h, who, r)
                } else {
                    h.stores[who].commit(&r).unwrap();
                    h.step(who, Event::Committed(r.token));
                    h.pump();
                    let (who, r) = h.commits.pop_front().unwrap();
                    assert!(matches!(r.change, Change::Settle { .. }));
                    (h, who, r)
                }
            };
            if boundary >= 1 {
                h.stores[who].commit(&request).unwrap();
            }
            if boundary >= 2 {
                h.step(who, Event::Committed(request.token));
            }
            h.queue.clear();
            h.auto = true;
            h.restart(who);
            assert_eq!(
                h.nodes[who]
                    .step(Event::Committed(request.token))
                    .unwrap_err(),
                Error::CommitToken
            );
            if transaction == 0 && boundary == 0 {
                h.send(0, 0, payload(1025));
            }
            h.run(250);
            h.settled(0, 1);
            assert_eq!(
                h.stores[1].records.len(),
                1,
                "transaction {transaction} boundary {boundary}"
            );
            assert_eq!(
                h.stores[1].records.values().next().unwrap(),
                &(payload(1025), Outcome::Accepted)
            );
            assert_eq!(h.nodes[0].core_usage().reads, 0);
            assert_eq!(h.nodes[1].core_usage().reads, 0);
        }
    }
}

#[test]
fn stale_completion_cannot_commit_a_new_proposal_after_restart() {
    let mut h = Harness::new(config());
    h.auto = false;
    h.send(0, 0, b"old".to_vec());
    let (_, old) = h.commits.pop_front().unwrap();
    h.restart(0);
    h.send(0, 0, b"new".to_vec());
    let (_, new) = h.commits.pop_front().unwrap();
    assert_eq!(old.token.revision, new.token.revision);
    assert_ne!(old.token.incarnation, new.token.incarnation);
    let before = h.stores[0].disk.clone();
    assert_eq!(h.stores[0].commit(&old), Err("fenced writer"));
    assert_eq!(h.stores[0].disk, before);
    assert_eq!(
        h.nodes[0].step(Event::Committed(old.token)).unwrap_err(),
        Error::CommitToken
    );
    assert!(h.queue.is_empty());
    h.stores[0].commit(&new).unwrap();
    assert_eq!(h.stores[0].commit(&new), Err("revision conflict"));
    h.step(0, Event::Committed(new.token));
    h.auto = true;
    h.run(100);
    h.settled(0, 1);
    assert_eq!(h.stores[1].records.values().next().unwrap().0, b"new");
}

#[test]
fn bounded_recovery_stalls_without_dropping_outbox_and_resume_repairs() {
    let mut c = config();
    c.transfer.recovery.max_attempts = 2;
    let mut h = Harness::new(c);
    h.send(0, 0, payload(1025));
    h.queue.clear();
    for now in [5, 15, 50] {
        h.tick(now);
        h.queue.clear();
    }
    assert_eq!(h.stores[0].checkpoint().flow(7).unwrap().outbox.len(), 1);
    assert_eq!(
        h.trace
            .iter()
            .flat_map(|(_, a)| a)
            .filter(|a| matches!(a, Action::AnnouncementStalled { .. }))
            .count(),
        1
    );
    h.tick(100);
    assert!(h.queue.is_empty());
    h.step(0, Event::Resume { flow: 7 });
    h.run(100);
    h.settled(0, 1);
}

#[test]
fn seeded_faults_and_restarts_replay_actions_and_commit_each_effect_once() {
    for initial in 1..=8u64 {
        let run = || {
            let mut seed = initial;
            let mut rng = || {
                seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
                seed
            };
            let mut h = Harness::new(config());
            for i in 0..2 {
                for seq in 0..4 {
                    h.send(i, seq, payload(1025 + seq as usize * 4096));
                }
            }
            let mut delayed = vec![];
            for now in 0..=500 {
                h.tick(now);
                if now == 40 {
                    h.restart(0);
                }
                while let Some(f) = h.queue.pop_front() {
                    if now < 70 && rng() % 5 == 0 {
                        continue;
                    }
                    let due = now + rng() % 8;
                    if now < 70 && rng() % 4 == 0 {
                        delayed.push((due + 3, f.clone()));
                    }
                    if now < 70 && rng() % 7 == 0 {
                        let mut corrupt = f.clone();
                        *corrupt.bytes.last_mut().unwrap() ^= 1;
                        delayed.push((due, corrupt));
                    }
                    delayed.push((due, f));
                }
                while let Some(index) = delayed.iter().rposition(|(due, _)| *due <= now) {
                    let (_, f) = delayed.swap_remove(index);
                    h.deliver(f);
                }
                for i in 0..2 {
                    assert!(h.nodes[i].usage().buffered_messages <= 4);
                    assert!(h.nodes[i].usage().buffered_message_bytes <= 4 * 65536);
                    assert!(h.nodes[i].usage().outbox_messages <= 4);
                    assert!(h.nodes[i].core_usage().reads <= 8);
                    assert!(h.nodes[i].core_usage().pending_interests <= 32);
                }
                if h.stores
                    .iter()
                    .all(|s| s.checkpoint().flow(7).unwrap().settled.len() == 4)
                {
                    break;
                }
            }
            for i in 0..2 {
                h.settled(i, 4);
                assert_eq!(h.stores[i].records.len(), 4);
                assert_eq!(
                    h.stores[i]
                        .order
                        .iter()
                        .map(|id| id.sequence)
                        .collect::<Vec<_>>(),
                    vec![0, 1, 2, 3]
                );
            }
            h.trace
        };
        assert_eq!(run(), run(), "seed {initial}");
    }
}

#[test]
fn checkpoint_validation_and_epoch_invalidation_preserve_durable_evidence() {
    let mut h = Harness::new(config());
    h.send(0, 0, b"pending".to_vec());
    let checkpoint = h.stores[0].checkpoint();
    let mut newer = flows(0);
    newer[0].peer.rift += 1;
    assert!(matches!(
        Node::new(
            config(),
            view(0),
            protection(0),
            newer,
            checkpoint.clone(),
            9
        ),
        Err(Error::EpochMismatch)
    ));
    let mut local = view(0);
    local.rift += 1;
    assert!(matches!(
        Node::new(
            config(),
            local,
            protection(0),
            flows(0),
            checkpoint.clone(),
            9
        ),
        Err(Error::EpochMismatch)
    ));
    let mut invalid = vec![];
    let mut bad = checkpoint.clone();
    bad.version = 9;
    invalid.push(bad);
    let mut bad = checkpoint.clone();
    bad.flows[0].next_send = 0;
    invalid.push(bad);
    let mut bad = checkpoint.clone();
    bad.flows[0].settled.insert(0, Outcome::Accepted);
    invalid.push(bad);
    let mut bad = checkpoint.clone();
    bad.flows[0].outbox.insert(9, vec![]);
    invalid.push(bad);
    let mut bad = checkpoint.clone();
    bad.flows[0].outbox.insert(0, vec![0; 65537]);
    invalid.push(bad);
    let mut bad = checkpoint.clone();
    bad.revision = u64::MAX;
    invalid.push(bad);
    for bad in invalid {
        assert!(matches!(
            Node::new(config(), view(0), protection(0), flows(0), bad, 9),
            Err(Error::InvalidCheckpoint)
        ));
    }
    h.step(
        0,
        Event::InvalidateEpoch {
            peer: PeerId(2),
            new_rift: 1,
        },
    );
    assert!(h.nodes[0].is_halted());
    assert_eq!(
        h.nodes[0].step(Event::AdvanceTo(100)).unwrap_err(),
        Error::Halted
    );
    assert_eq!(h.nodes[0].checkpoint(), &checkpoint);
    assert_eq!(h.stores[0].checkpoint(), checkpoint);
}

#[test]
fn service_and_route_changes_preserve_outbox_and_resume_delivery() {
    let mut h = Harness::new(config());
    h.send(0, 0, payload(1025));
    h.queue.clear();
    h.step(
        0,
        Event::SetAttachment {
            peer: PeerId(2),
            attachment: None,
        },
    );
    h.run(100);
    assert!(h.queue.is_empty());
    assert_eq!(h.stores[0].checkpoint().flow(7).unwrap().outbox.len(), 1);
    h.step(
        0,
        Event::SetAttachment {
            peer: PeerId(2),
            attachment: Some(Attachment(7)),
        },
    );
    h.step(
        1,
        Event::SetService {
            peer: PeerId(1),
            allowed: false,
        },
    );
    h.run(50);
    assert!(h.stores[1].records.is_empty());
    h.step(
        1,
        Event::SetService {
            peer: PeerId(1),
            allowed: true,
        },
    );
    h.run(200);
    h.settled(0, 1);
}

#[test]
fn crash_before_delivery_decision_reissues_same_id_without_repeating_effect() {
    let (mut h, id) = pending_delivery();
    assert!(h.stores[1].effects.is_empty());
    h.queue.clear();
    h.restart(1);
    h.run(20);
    let (_, again, bytes) = h.deliveries.pop_front().unwrap();
    assert_eq!(again, id);
    assert_eq!(bytes, payload(1025));
    h.step(
        1,
        Event::Decide {
            id: again,
            outcome: Outcome::Accepted,
        },
    );
    h.auto = true;
    h.flush();
    h.run(100);
    assert_eq!(h.stores[1].effects.len(), 1);
    h.settled(0, 1);
}

fn oracle(
    i: usize,
    path: Vec<u8>,
    bytes: Vec<u8>,
) -> mesa_core::Engine<mesa_core::protection::SignedPublic> {
    let mut e = mesa_core::Engine::new(config().core, view(i), protection(i)).unwrap();
    e.step(mesa_core::Event::InstallPeer(view(1 - i))).unwrap();
    e.step(mesa_core::Event::SetAttachment {
        peer: view(1 - i).id,
        attachment: Some(Attachment(7)),
    })
    .unwrap();
    e.step(mesa_core::Event::Bind {
        path,
        bytes,
        access: mesa_core::Access::Public,
    })
    .unwrap();
    e
}
fn pump_oracle(
    h: &mut Harness,
    server: usize,
    e: &mut mesa_core::Engine<mesa_core::protection::SignedPublic>,
) {
    for _ in 0..100 {
        let Some(f) = h.queue.pop_front() else { return };
        if f.from == server {
            continue;
        }
        for action in e
            .step(mesa_core::Event::Receive {
                claimed_peer: view(f.from).id,
                attachment: Attachment(7),
                bytes: &f.bytes,
            })
            .unwrap()
        {
            if let mesa_core::Action::Send { bytes, .. } = action {
                h.deliver(Frame {
                    from: server,
                    bytes,
                });
            }
        }
    }
    panic!("oracle loop");
}

#[test]
fn signed_but_invalid_receipts_never_retire_outbox() {
    let mut valid = b"libmesa/flow-receipt/1\0".to_vec();
    valid.extend_from_slice(&mesa_crypto::sha256(b"value"));
    valid.extend_from_slice(&[0, 0, 0]);
    let mut variants = vec![vec![]];
    let mut wrong_hash = valid.clone();
    wrong_hash[b"libmesa/flow-receipt/1\0".len()] ^= 1;
    variants.push(wrong_hash);
    let mut wrong_tag = valid.clone();
    let n = wrong_tag.len();
    wrong_tag[n - 3] = 2;
    variants.push(wrong_tag);
    let mut wrong_code = valid.clone();
    let n = wrong_code.len();
    wrong_code[n - 1] = 1;
    variants.push(wrong_code);
    let mut extra = valid;
    extra.push(0);
    variants.push(extra);
    for bytes in variants {
        let mut h = Harness::new(config());
        h.send(0, 0, b"value".to_vec());
        let hint = h.queue.pop_front().unwrap();
        h.queue.clear();
        // A receipt-availability hint is not proof that a receipt exists.
        h.deliver(Frame {
            from: 1,
            bytes: hint.bytes,
        });
        assert_eq!(h.stores[0].checkpoint().flow(7).unwrap().outbox.len(), 1);
        let id = MessageId {
            sender: 1,
            sender_rift: 0,
            receiver: 2,
            receiver_rift: 0,
            flow: 7,
            sequence: 0,
        };
        let mut server = oracle(1, id.path(ReadKind::Receipt), bytes);
        pump_oracle(&mut h, 1, &mut server);
        assert!(h.trace.iter().flat_map(|(_, a)| a).any(|a| matches!(
            a,
            Action::Stalled {
                failure: Failure::InvalidReceipt,
                ..
            }
        )));
        assert_eq!(h.stores[0].checkpoint().flow(7).unwrap().outbox.len(), 1);
        assert!(h.stores[0].checkpoint().flow(7).unwrap().settled.is_empty());
    }
}

#[test]
fn hints_are_bounded_untrusted_input_and_inbound_payload_cap_is_enforced() {
    let mut c = config();
    c.max_message_bytes = 8;
    let mut h = Harness::new(c);
    h.send(0, 0, b"value".to_vec());
    let hint = h.queue.pop_front().unwrap();
    h.queue.clear();
    for mutation in 0..3 {
        let mut packet = mesa_wire::decode(&hint.bytes, config().core.wire).unwrap();
        let mesa_wire::Body::Poke { ack, payload, .. } = &mut packet.body else {
            panic!()
        };
        match mutation {
            0 => ack.rift += 1,
            1 => payload.publisher = PeerId(99),
            _ => ack.domain = mesa_wire::DomainTag::new("other").unwrap(),
        }
        let bytes = mesa_wire::encode(&packet, config().core.wire).unwrap();
        h.deliver(Frame { from: 0, bytes });
        assert_eq!(h.nodes[1].core_usage().reads, 0);
    }
    for _ in 0..100 {
        h.deliver(hint.clone());
    }
    assert_eq!(h.nodes[1].core_usage().reads, 1);
    assert_eq!(h.nodes[1].core_usage().reassembly_bytes, 0);
    assert!(h.stores[1].records.is_empty());
    let id = MessageId {
        sender: 1,
        sender_rift: 0,
        receiver: 2,
        receiver_rift: 0,
        flow: 7,
        sequence: 0,
    };
    let mut server = oracle(0, id.path(ReadKind::Message), vec![0; 9]);
    pump_oracle(&mut h, 0, &mut server);
    assert!(h.trace.iter().flat_map(|(_, a)| a).any(|a| matches!(
        a,
        Action::Stalled {
            failure: Failure::MessageTooLarge,
            ..
        }
    )));
    assert!(h.stores[1].records.is_empty());
    assert!(h.stores[1].effects.is_empty());
    assert_eq!(h.stores[0].checkpoint().flow(7).unwrap().outbox.len(), 1);
}

#[test]
fn reclaimed_batches_keep_sequences_monotonic_after_restart() {
    let mut c = config();
    c.max_messages = 2;
    c.window = 2;
    let mut h = Harness::new(c);
    for batch in 0..5 {
        for sequence in batch * 2..batch * 2 + 2 {
            h.send(0, sequence, vec![sequence as u8]);
        }
        h.run(100);
        assert_eq!(
            h.nodes[0].checkpoint().flow(7).unwrap().send_floor,
            batch * 2 + 2
        );
        assert_eq!(
            h.nodes[1].checkpoint().flow(7).unwrap().receive_floor,
            batch * 2 + 2
        );
        assert_eq!(h.nodes[0].usage().retained_receipts, 0);
        assert_eq!(h.nodes[1].usage().retained_receipts, 0);
        h.restart(0);
        h.restart(1);
        h.run(50);
    }
    assert_eq!(
        h.nodes[0]
            .step(Event::Enqueue {
                flow: 7,
                sequence: 0,
                bytes: vec![]
            })
            .unwrap_err(),
        Error::Sequence
    );
    assert_eq!(h.stores[1].effects.len(), 10);
    assert_eq!(h.nodes[1].core_usage().namespace_slots, 3);
    assert_eq!(h.nodes[0].core_usage().namespace_slots, 2);
}

#[test]
fn multiple_flows_share_atomic_revision_without_colliding_delivery_ids() {
    let mut h = Harness::new(config());
    for i in 0..2 {
        let mut configured = flows(i);
        let mut second = configured[0].clone();
        second.id = 8;
        configured.push(second);
        let checkpoint = Checkpoint::empty(&view(i), &configured);
        h.stores[i].disk = serde_json::to_vec(&checkpoint).unwrap();
        h.nodes[i] = Node::new(
            config(),
            view(i),
            protection(i),
            configured,
            checkpoint,
            i as u128 + 1,
        )
        .unwrap();
    }
    for i in 0..2 {
        for flow in [7, 8] {
            for sequence in 0..2 {
                h.step(
                    i,
                    Event::Enqueue {
                        flow,
                        sequence,
                        bytes: vec![flow as u8, sequence as u8],
                    },
                );
            }
        }
    }
    h.run(200);
    for i in 0..2 {
        for flow in [7, 8] {
            let checkpoint = h.stores[i].checkpoint();
            let state = checkpoint.flow(flow).unwrap();
            assert_eq!(state.settled.len(), 2);
            assert!(state.outbox.is_empty());
            assert_eq!(state.received.len(), 2);
            assert_eq!(
                h.stores[i]
                    .order
                    .iter()
                    .filter(|id| id.flow == flow)
                    .map(|id| id.sequence)
                    .collect::<Vec<_>>(),
                vec![0, 1]
            );
        }
        assert_eq!(h.stores[i].effects.len(), 4);
    }
}

#[test]
fn same_epoch_signing_key_update_preserves_pending_flow_identity() {
    let mut h = Harness::new(config());
    h.send(0, 0, b"across key update".to_vec());
    h.queue.clear();
    let before = h.nodes[0].checkpoint().clone();
    let mut local = view(1);
    local.life = 2;
    local.key = mesa_core::KeyHandle(9);
    let protection = mesa_core::protection::SignedPublic::new(
        local.key,
        mesa_crypto::KeyPair::from_seed(&[9; 32]),
        std::collections::BTreeMap::from([(
            view(0).key,
            mesa_crypto::KeyPair::from_seed(&[1; 32]).public_key(),
        )]),
    );
    h.boot[1] += 2;
    h.stores[1].incarnation = h.boot[1];
    h.nodes[1] = Node::new(
        config(),
        local.clone(),
        protection,
        flows(1),
        h.stores[1].checkpoint(),
        h.boot[1],
    )
    .unwrap();
    h.step(0, Event::UpdatePeer(local));
    assert_eq!(h.nodes[0].checkpoint(), &before);
    h.run(100);
    h.settled(0, 1);
    assert_eq!(h.stores[1].effects.len(), 1);
    let id = &h.stores[1].order[0];
    assert_eq!((id.sender_rift, id.receiver_rift, id.sequence), (0, 0, 0));
}

#[test]
fn signed_invalid_watermark_body_cannot_retire_receipts() {
    for bytes in [
        vec![],
        b"libmesa/flow-retirement/1\0".to_vec(),
        b"libmesa/flow-retirement/2\0extra".to_vec(),
    ] {
        let mut c = config();
        c.max_messages = 2;
        c.window = 2;
        let mut h = Harness::new(c);
        for seq in 0..2 {
            h.send(0, seq, payload(1025));
        }
        let mut release = None;
        for _ in 0..1000 {
            let Some(frame) = h.queue.pop_front() else {
                break;
            };
            if let mesa_wire::Body::Poke { payload, .. } =
                mesa_wire::decode(&frame.bytes, config().core.wire)
                    .unwrap()
                    .body
            {
                if payload.path.windows(9).any(|p| p == b"/release/") {
                    release = Some(frame);
                    break;
                }
            }
            h.deliver(frame);
        }
        let release = release.unwrap();
        let path = match mesa_wire::decode(&release.bytes, config().core.wire)
            .unwrap()
            .body
        {
            mesa_wire::Body::Poke { payload, .. } => payload.path.to_vec(),
            _ => unreachable!(),
        };
        h.queue.clear();
        h.deliver(release);
        let mut server = oracle(0, path, bytes);
        pump_oracle(&mut h, 0, &mut server);
        assert_eq!(h.nodes[1].checkpoint().flow(7).unwrap().receive_floor, 0);
        assert_eq!(h.nodes[1].checkpoint().flow(7).unwrap().received.len(), 2);
        assert!(h.trace.iter().flat_map(|(_, a)| a).any(|a| matches!(
            a,
            Action::Stalled {
                failure: Failure::InvalidWatermark,
                ..
            }
        )));
    }
}
