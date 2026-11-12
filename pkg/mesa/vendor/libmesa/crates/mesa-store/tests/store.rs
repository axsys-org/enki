mod support;
use mesa_flow::{Change, CommitRequest, CommitToken, MessageId, Outcome, Receipt};
use mesa_store::{Error, Limits, Store};
use support::*;
fn enqueue(store: &Store, bytes: Vec<u8>) -> CommitRequest {
    let mut checkpoint = store.checkpoint().unwrap();
    let base_revision = checkpoint.revision;
    let sequence = checkpoint.flows[0].next_send;
    let id = MessageId {
        sender: 1,
        sender_rift: 0,
        receiver: 2,
        receiver_rift: 0,
        flow: 7,
        sequence,
    };
    checkpoint.flows[0].outbox.insert(sequence, bytes);
    checkpoint.flows[0].next_send += 1;
    checkpoint.revision += 1;
    CommitRequest {
        token: CommitToken {
            incarnation: store.incarnation(),
            revision: checkpoint.revision,
        },
        base_revision,
        checkpoint,
        change: Change::Enqueue { id },
    }
}
fn deliver(store: &Store, bytes: Vec<u8>, outcome: Outcome) -> CommitRequest {
    let mut checkpoint = store.checkpoint().unwrap();
    let base_revision = checkpoint.revision;
    let sequence = checkpoint.flows[0].next_receive();
    let id = MessageId {
        sender: 2,
        sender_rift: 0,
        receiver: 1,
        receiver_rift: 0,
        flow: 7,
        sequence,
    };
    checkpoint.flows[0].received.push(Receipt {
        digest: mesa_crypto::sha256(&bytes),
        outcome,
    });
    checkpoint.revision += 1;
    CommitRequest {
        token: CommitToken {
            incarnation: store.incarnation(),
            revision: checkpoint.revision,
        },
        base_revision,
        checkpoint,
        change: Change::Deliver { id, bytes, outcome },
    }
}
#[test]
fn explicit_creation_scope_and_authoritative_restart() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let c = initial(0);
    assert!(Store::open(&path, &c, Limits::default()).is_err());
    assert!(!path.exists());
    let mut store = Store::create(&path, &c, Limits::default()).unwrap();
    assert!(Store::create(&path, &c, Limits::default()).is_err());
    let first = store.incarnation();
    let request = enqueue(&store, vec![9; 1025]);
    store.commit(&request).unwrap();
    let read = Store::inspect(&path, Limits::default()).unwrap();
    assert_eq!(read.checkpoint, request.checkpoint);
    assert!(Store::open(&path, &initial(1), Limits::default()).is_err());
    // A failed scope check and read-only inspection must not fence the writer.
    store.commit(&enqueue(&store, vec![])).unwrap();
    let second = Store::open(&path, &c, Limits::default()).unwrap();
    assert_ne!(first, second.incarnation());
    assert_eq!(second.checkpoint().unwrap().flows[0].next_send, 2);
    assert!(matches!(
        store.commit(&enqueue(&store, vec![1])),
        Err(Error::Fenced)
    ));
    assert_eq!(
        Store::inspect(&path, Limits::default()).unwrap().checkpoint,
        second.checkpoint().unwrap()
    );
}
#[test]
fn dropped_transaction_and_failed_checkpoint_write_leave_no_inbox_effect() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let mut store = Store::create(&path, &initial(0), Limits::default()).unwrap();
    let request = deliver(&store, vec![3; 1025], Outcome::Accepted);
    drop(store.prepare(&request).unwrap());
    let old = Store::inspect(&path, Limits::default()).unwrap();
    assert!(old.deliveries.is_empty());
    assert_eq!(old.checkpoint.revision, 0);
    let sql = rusqlite::Connection::open(&path).unwrap();
    sql.execute_batch("CREATE TRIGGER fail_checkpoint BEFORE UPDATE OF checkpoint ON state BEGIN SELECT RAISE(ABORT,'injected checkpoint failure'); END;").unwrap();
    assert!(store.commit(&request).is_err());
    assert_eq!(Store::inspect(&path, Limits::default()).unwrap(), old);
    sql.execute_batch("DROP TRIGGER fail_checkpoint;").unwrap();
    assert_eq!(store.commit(&request).unwrap(), request.token);
    let s = Store::inspect(&path, Limits::default()).unwrap();
    assert_eq!(s.deliveries.len(), 1);
    assert_eq!(s.deliveries[0].bytes, vec![3; 1025]);
    assert!(matches!(store.commit(&request), Err(Error::Revision)));
}
#[test]
fn writer_claim_cannot_overtake_inflight_transaction_and_fences_old_handles() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let mut old = Store::create(&path, &initial(0), Limits::default()).unwrap();
    let request = enqueue(&old, vec![7]);
    let pending = old.prepare(&request).unwrap();
    assert!(matches!(
        Store::open(&path, &initial(0), Limits::default()),
        Err(Error::Sql(_))
    ));
    pending.commit().unwrap();
    let new = Store::open(&path, &initial(0), Limits::default()).unwrap();
    assert_eq!(new.checkpoint().unwrap(), request.checkpoint);
    let request = enqueue(&old, vec![8]);
    assert!(matches!(old.commit(&request), Err(Error::Fenced)));
}
#[test]
fn rejected_delivery_and_settlement_survive_reopen() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let mut store = Store::create(&path, &initial(0), Limits::default()).unwrap();
    store.commit(&enqueue(&store, vec![4])).unwrap();
    store
        .commit(&deliver(&store, vec![5], Outcome::Rejected(41)))
        .unwrap();
    let mut checkpoint = store.checkpoint().unwrap();
    let base_revision = checkpoint.revision;
    checkpoint.flows[0].outbox.remove(&0);
    checkpoint.flows[0].settled.insert(0, Outcome::Rejected(41));
    checkpoint.revision += 1;
    let id = MessageId {
        sender: 1,
        sender_rift: 0,
        receiver: 2,
        receiver_rift: 0,
        flow: 7,
        sequence: 0,
    };
    let r = CommitRequest {
        token: CommitToken {
            incarnation: store.incarnation(),
            revision: checkpoint.revision,
        },
        base_revision,
        checkpoint,
        change: Change::Settle {
            id,
            outcome: Outcome::Rejected(41),
        },
    };
    store.commit(&r).unwrap();
    drop(store);
    let store = Store::open(&path, &initial(0), Limits::default()).unwrap();
    assert_eq!(store.checkpoint().unwrap(), r.checkpoint);
    let s = Store::inspect(&path, Limits::default()).unwrap();
    assert_eq!(s.deliveries.len(), 1);
    assert_eq!(s.deliveries[0].outcome, Outcome::Rejected(41));
    assert!(s.deliveries.iter().all(|r| r.outcome != Outcome::Accepted));
}
#[test]
fn invalid_transitions_tokens_and_corrupt_state_never_commit() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let mut store = Store::create(&path, &initial(0), Limits::default()).unwrap();
    let valid = deliver(&store, vec![5; 10], Outcome::Accepted);
    for variant in 0..6 {
        let mut r = valid.clone();
        match variant {
            0 => r.token.incarnation += 1,
            1 => r.token.revision += 1,
            2 => r.base_revision += 1,
            3 => r.checkpoint.local.rift += 1,
            4 => r.checkpoint.flows[0].received[0].digest[0] ^= 1,
            _ => {
                if let Change::Deliver { id, .. } = &mut r.change {
                    id.sequence += 1;
                }
            }
        }
        assert!(store.commit(&r).is_err());
        assert_eq!(store.checkpoint().unwrap().revision, 0);
    }
    store.commit(&valid).unwrap();
    let sql = rusqlite::Connection::open(&path).unwrap();
    sql.execute("DELETE FROM deliveries", []).unwrap();
    assert!(matches!(
        Store::inspect(&path, Limits::default()),
        Err(Error::InvalidState)
    ));
    assert!(matches!(
        Store::open(&path, &initial(0), Limits::default()),
        Err(Error::InvalidState)
    ));
}
#[test]
fn row_byte_and_checkpoint_limits_rollback_entire_change() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let limits = Limits {
        delivery_records: 1,
        delivery_bytes: 16,
        message_bytes: 16,
        checkpoint_bytes: 1024,
    };
    let mut store = Store::create(&path, &initial(0), limits).unwrap();
    assert!(store.commit(&enqueue(&store, vec![0; 17])).is_err());
    store
        .commit(&deliver(&store, vec![0; 16], Outcome::Accepted))
        .unwrap();
    let before = Store::inspect(&path, limits).unwrap();
    assert!(matches!(
        store.commit(&deliver(&store, vec![], Outcome::Accepted)),
        Err(Error::Limit)
    ));
    assert_eq!(Store::inspect(&path, limits).unwrap(), before);
    // Schema/revision mismatches fail closed on restore.
    let sql = rusqlite::Connection::open(&path).unwrap();
    sql.execute("UPDATE state SET revision=revision+1", [])
        .unwrap();
    assert!(matches!(
        Store::open(&path, &initial(0), limits),
        Err(Error::InvalidState)
    ));
}

#[test]
fn serialization_limit_and_restore_limit_preserve_existing_state() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let mut store = Store::create(
        &path,
        &initial(0),
        Limits {
            checkpoint_bytes: 1024,
            ..Default::default()
        },
    )
    .unwrap();
    assert!(matches!(
        store.commit(&enqueue(&store, vec![255; 4096])),
        Err(Error::Limit)
    ));
    assert_eq!(store.checkpoint().unwrap().revision, 0);
    drop(store);
    let mut store = Store::open(&path, &initial(0), Limits::default()).unwrap();
    store.commit(&enqueue(&store, vec![255; 4096])).unwrap();
    assert!(matches!(
        Store::open(
            &path,
            &initial(0),
            Limits {
                checkpoint_bytes: 1024,
                ..Default::default()
            }
        ),
        Err(Error::Limit)
    ));
    // The rejected restore must not claim a writer generation.
    store.commit(&enqueue(&store, vec![1])).unwrap();
}
#[test]
fn full_width_endpoint_and_flow_ids_roundtrip_without_sql_integer_truncation() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let mut c = initial(0);
    c.local.name = u128::MAX;
    c.flows[0].remote.name = u128::MAX - 1;
    c.flows[0].flow = u64::MAX;
    let mut store = Store::create(&path, &c, Limits::default()).unwrap();
    let mut next = c.clone();
    next.revision = 1;
    next.flows[0].received.push(Receipt {
        digest: mesa_crypto::sha256(&[7]),
        outcome: Outcome::Accepted,
    });
    let id = MessageId {
        sender: u128::MAX - 1,
        sender_rift: 0,
        receiver: u128::MAX,
        receiver_rift: 0,
        flow: u64::MAX,
        sequence: 0,
    };
    let r = CommitRequest {
        token: CommitToken {
            incarnation: store.incarnation(),
            revision: 1,
        },
        base_revision: 0,
        checkpoint: next.clone(),
        change: Change::Deliver {
            id: id.clone(),
            bytes: vec![7],
            outcome: Outcome::Accepted,
        },
    };
    store.commit(&r).unwrap();
    drop(store);
    let store = Store::open(&path, &c, Limits::default()).unwrap();
    assert_eq!(store.checkpoint().unwrap(), next);
    assert_eq!(
        Store::inspect(&path, Limits::default()).unwrap().deliveries[0].id,
        id
    );
}

#[test]
fn persistent_generation_exhaustion_refuses_reuse() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    Store::create(&path, &initial(0), Limits::default()).unwrap();
    let sql = rusqlite::Connection::open(&path).unwrap();
    sql.execute("UPDATE state SET generation=9223372036854775807", [])
        .unwrap();
    assert!(matches!(
        Store::open(&path, &initial(0), Limits::default()),
        Err(Error::Limit)
    ));
    assert_eq!(
        sql.query_row("SELECT generation FROM state", [], |r| r.get::<_, i64>(0))
            .unwrap(),
        i64::MAX
    );
    assert_eq!(
        Store::inspect(&path, Limits::default()).unwrap().checkpoint,
        initial(0)
    );
}

fn settle(store: &Store) -> CommitRequest {
    let mut checkpoint = store.checkpoint().unwrap();
    let base_revision = checkpoint.revision;
    let flow = &mut checkpoint.flows[0];
    let sequence = *flow.outbox.keys().next().unwrap();
    flow.outbox.remove(&sequence);
    let outcome = Outcome::Rejected(17);
    flow.settled.insert(sequence, outcome);
    checkpoint.revision += 1;
    CommitRequest {
        token: CommitToken {
            incarnation: store.incarnation(),
            revision: checkpoint.revision,
        },
        base_revision,
        checkpoint,
        change: Change::Settle {
            id: MessageId {
                sender: 1,
                sender_rift: 0,
                receiver: 2,
                receiver_rift: 0,
                flow: 7,
                sequence,
            },
            outcome,
        },
    }
}
fn retire(store: &Store, incoming: bool) -> CommitRequest {
    let mut checkpoint = store.checkpoint().unwrap();
    let base_revision = checkpoint.revision;
    let batch = checkpoint.batch_size;
    let flow = &mut checkpoint.flows[0];
    let sequence = if incoming {
        flow.receive_floor += batch;
        flow.received.clear();
        flow.receive_floor
    } else {
        flow.send_floor += batch;
        flow.settled.clear();
        flow.send_floor
    };
    let id = MessageId {
        sender: if incoming { 2 } else { 1 },
        sender_rift: 0,
        receiver: if incoming { 1 } else { 2 },
        receiver_rift: 0,
        flow: 7,
        sequence,
    };
    checkpoint.revision += 1;
    CommitRequest {
        token: CommitToken {
            incarnation: store.incarnation(),
            revision: checkpoint.revision,
        },
        base_revision,
        checkpoint,
        change: if incoming {
            Change::RetireReceived { id }
        } else {
            Change::RetireSent { id }
        },
    }
}

#[test]
fn reclamation_preserves_application_records_and_quota_across_restart() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let mut cp = initial(0);
    cp.batch_size = 2;
    let limits = Limits {
        delivery_records: 2,
        ..Limits::default()
    };
    let mut store = Store::create(&path, &cp, limits).unwrap();
    for incoming in [false, true] {
        // A fabricated advance cannot skip the unsettled/undelivered batch.
        assert!(store.commit(&retire(&store, incoming)).is_err());
    }
    for _ in 0..2 {
        store.commit(&enqueue(&store, vec![1])).unwrap();
        store.commit(&settle(&store)).unwrap();
        store
            .commit(&deliver(&store, vec![7], Outcome::Accepted))
            .unwrap();
    }
    let full = Store::inspect(&path, limits).unwrap();
    for incoming in [false, true] {
        let r = retire(&store, incoming);
        drop(store.prepare(&r).unwrap());
        assert_eq!(store.checkpoint().unwrap().revision, r.base_revision);
        store.commit(&r).unwrap();
        assert!(store.commit(&r).is_err(), "retirement is monotonic");
    }
    let retired = Store::inspect(&path, limits).unwrap();
    assert_eq!(retired.deliveries, full.deliveries);
    assert_eq!(retired.settlements, full.settlements);
    assert!(retired.checkpoint.flows[0].received.is_empty());
    assert!(retired.checkpoint.flows[0].settled.is_empty());
    let mut wrong_batch = cp.clone();
    wrong_batch.batch_size = 4;
    assert!(matches!(
        Store::open(&path, &wrong_batch, limits),
        Err(Error::Scope)
    ));
    // Quotas are application retention limits, independent of protocol floors.
    store.commit(&enqueue(&store, vec![2])).unwrap();
    assert!(matches!(store.commit(&settle(&store)), Err(Error::Limit)));
    assert!(matches!(
        store.commit(&deliver(&store, vec![3], Outcome::Accepted)),
        Err(Error::Limit)
    ));
    let expected = Store::inspect(&path, limits).unwrap();
    let reopened = Store::open(&path, &cp, limits).unwrap();
    assert_eq!(reopened.checkpoint().unwrap(), expected.checkpoint);
    let sql = rusqlite::Connection::open(&path).unwrap();
    sql.execute(
        "DELETE FROM settlements WHERE revision=(SELECT min(revision) FROM settlements)",
        [],
    )
    .unwrap();
    assert!(
        Store::open(&path, &cp, limits).is_err(),
        "missing retired outcome history fails closed"
    );
}

#[test]
fn failed_settlement_transaction_never_leaks_an_application_outcome() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let mut store = Store::create(&path, &initial(0), Limits::default()).unwrap();
    store.commit(&enqueue(&store, vec![1])).unwrap();
    let before = Store::inspect(&path, Limits::default()).unwrap();
    let r = settle(&store);
    let sql = rusqlite::Connection::open(&path).unwrap();
    sql.execute_batch("CREATE TRIGGER fail_checkpoint BEFORE UPDATE OF checkpoint ON state BEGIN SELECT RAISE(ABORT,'injected checkpoint failure'); END;").unwrap();
    assert!(store.commit(&r).is_err());
    assert_eq!(Store::inspect(&path, Limits::default()).unwrap(), before);
    sql.execute_batch("DROP TRIGGER fail_checkpoint;").unwrap();
    store.commit(&r).unwrap();
    assert_eq!(
        Store::inspect(&path, Limits::default())
            .unwrap()
            .settlements
            .len(),
        1
    );
}

#[test]
fn legacy_profile_and_format_fail_closed_without_claiming_writer() {
    let tmp = Temp::new();
    let path = tmp.0.join("state.sqlite");
    let mut old = initial(0);
    old.version = 1;
    assert!(Store::create(&path, &old, Limits::default()).is_err());
    assert!(!path.exists());
    let mut store = Store::create(&path, &initial(0), Limits::default()).unwrap();
    let sql = rusqlite::Connection::open(&path).unwrap();
    sql.execute_batch("PRAGMA ignore_check_constraints=ON; UPDATE state SET format=1;")
        .unwrap();
    assert!(Store::open(&path, &initial(0), Limits::default()).is_err());
    sql.execute("UPDATE state SET format=2", []).unwrap();
    store.commit(&enqueue(&store, vec![])).unwrap();
}
