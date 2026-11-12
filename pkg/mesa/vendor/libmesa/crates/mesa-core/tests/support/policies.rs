use super::*;
use mesa_core::policy::{
    CongestionPolicy, RecoveryPolicy, RetryTimer, TimerPolicy, TransferPolicy,
};

fn adaptive() -> TransferPolicy {
    TransferPolicy {
        recovery: RecoveryPolicy { max_attempts: 8 },
        timers: TimerPolicy {
            read_timeout: 1000,
            retry: RetryTimer::Exponential {
                initial: 10,
                maximum: 40,
            },
        },
        congestion: CongestionPolicy::Aimd {
            initial: 4,
            maximum: 8,
        },
    }
}
fn harness(policy: TransferPolicy) -> Harness {
    Harness::with_policy(Config::default(), policy)
}
fn state(h: &Harness) -> policy::TransferState {
    h.engines[0].transfer_state(view(1).id).unwrap()
}
fn timer(h: &Harness) -> Option<Tick> {
    let Action::SetTimer(t) = h
        .trace
        .iter()
        .rev()
        .find(|(i, _)| *i == 0)
        .unwrap()
        .1
        .last()
        .unwrap()
    else {
        panic!()
    };
    *t
}
fn peek_path(f: &Frame) -> Vec<u8> {
    let Body::Peek(n) = decode(&f.bytes, Config::default().wire).unwrap().body else {
        panic!()
    };
    n.path.into()
}

#[test]
fn exponential_retry_uses_emission_time_and_exact_attempt_budget() {
    let mut p = adaptive();
    p.recovery.max_attempts = 4;
    let mut h = harness(p);
    h.read(0, 1, b"lost");
    assert_eq!(h.queue.len(), 1);
    assert_eq!(timer(&h), Some(10));
    for (before, due, next) in [(9, 10, 30), (29, 30, 70), (69, 70, 110)] {
        h.queue.clear();
        h.tick(before);
        assert!(h.queue.is_empty());
        h.tick(due);
        assert_eq!(h.queue.len(), 1);
        assert_eq!(timer(&h), Some(next));
        h.tick(due);
        assert_eq!(h.queue.len(), 1, "same-tick polling must not resend");
    }
    h.queue.clear();
    h.tick(110);
    assert_eq!(h.results, vec![(0, 1, Err(ReadFailure::RetriesExhausted))]);
    assert!(h.queue.is_empty());
    assert_eq!(timer(&h), None);
    h.idle();
}

#[test]
fn fair_peer_budget_releases_slots_on_cancel_and_completion() {
    let mut p = adaptive();
    p.congestion = CongestionPolicy::Fixed { window: 1 };
    let mut h = harness(p);
    for id in 1..=3 {
        let path = vec![id as u8];
        h.bind(1, &path, &bytes(2048));
        h.read(0, id, &path);
    }
    assert_eq!(state(&h).in_flight, 1);
    assert_eq!(state(&h).queued, 2);
    h.next(); // proof reply for read 1
    h.next(); // accepting proof must yield the next slot to read 2
    assert_eq!(peek_path(h.queue.front().unwrap()), vec![2]);
    assert_eq!(state(&h).in_flight, 1);
    h.queue.clear();
    h.step(0, Event::Cancel { id: 2 });
    assert_eq!(peek_path(h.queue.front().unwrap()), vec![3]);
    h.pump();
    assert_eq!(
        h.results,
        vec![
            (0, 2, Err(ReadFailure::Cancelled)),
            (0, 1, Ok(bytes(2048))),
            (0, 3, Ok(bytes(2048)))
        ]
    );
    assert_eq!(state(&h).in_flight, 0);
    assert_eq!(state(&h).queued, 0);
    h.idle();
}

#[test]
fn unsent_interests_cannot_consume_pages_or_gain_congestion_credit() {
    let mut p = adaptive();
    p.congestion = CongestionPolicy::Fixed { window: 1 };
    let mut h = harness(p);
    h.bind(1, b"a", b"a");
    h.bind(1, b"b", b"b");
    h.read(0, 1, b"a");
    h.read(0, 2, b"b"); // queued proof, never emitted
    let unsolicited = rewrite_peek(&h.queue[0], b"b");
    h.deliver(unsolicited);
    let page = h.queue.pop_back().unwrap();
    let before = h.engines[0].usage();
    h.deliver(page);
    assert_eq!(h.drops.last(), Some(&DropReason::NoInterest));
    assert_eq!(h.engines[0].usage(), before);
    assert_eq!(state(&h).in_flight, 1);
    h.pump();
    assert_eq!(h.results.len(), 2);
    h.idle();
}
fn rewrite_peek(f: &Frame, path: &[u8]) -> Frame {
    let Body::Peek(mut n) = decode(&f.bytes, Config::default().wire).unwrap().body else {
        panic!()
    };
    n.path = path;
    Frame {
        from: f.from,
        bytes: encode(
            &Packet {
                hop: 0,
                body: Body::Peek(n),
            },
            Config::default().wire,
        )
        .unwrap(),
    }
}

#[test]
fn only_new_verified_responses_grow_window_and_expiry_batches_halve_once() {
    let mut h = harness(adaptive());
    let proof = h.proof(&bytes(16384));
    h.deliver(proof.clone()); // credit 1 of 4
    h.deliver(proof); // duplicate gets no credit
    for _ in 0..4 {
        h.next();
    }
    let pages: Vec<_> = h.queue.drain(..).collect();
    h.deliver(pages[3].clone()); // buffered but not verified
    h.deliver(pages[3].clone());
    assert_eq!(state(&h).window, 4);
    assert_eq!(state(&h).in_flight, 4);
    let bad = rewrite(&pages[0], |_, d| d.fragment = &[7; 1024]);
    h.deliver(bad);
    assert_eq!(state(&h).window, 4);
    h.deliver(pages[0].clone());
    h.deliver(pages[1].clone());
    assert_eq!(state(&h).window, 4);
    h.deliver(pages[2].clone()); // verifies 2 and buffered 3; grows to 5
    assert_eq!(state(&h).window, 5);
    h.queue.clear();
    h.tick(10); // four simultaneous expired interests -> one halving
    assert_eq!(state(&h).window, 2);
    assert_eq!(state(&h).in_flight, 2);
    assert_eq!(h.queue.len(), 2);
    h.pump();
    assert_eq!(h.results, vec![(0, 1, Ok(bytes(16384)))]);
    assert!(state(&h).window <= 8);
    h.idle();
}

#[test]
fn route_outage_disarms_retry_but_preserves_absolute_deadline() {
    let mut p = adaptive();
    p.recovery.max_attempts = 2;
    let mut h = harness(p);
    h.read(0, 1, b"lost");
    h.queue.clear();
    h.step(
        0,
        Event::SetAttachment {
            peer: view(1).id,
            attachment: None,
        },
    );
    h.tick(10);
    assert_eq!(timer(&h), Some(1000));
    for now in [20, 100, 900] {
        h.tick(now);
    }
    assert!(h.results.is_empty());
    assert!(h.queue.is_empty());
    h.step(
        0,
        Event::SetAttachment {
            peer: view(1).id,
            attachment: Some(Attachment(7)),
        },
    );
    assert_eq!(h.queue.len(), 1);
    assert_eq!(timer(&h), Some(920)); // second actual transmission
    h.tick(920);
    assert_eq!(h.results, vec![(0, 1, Err(ReadFailure::RetriesExhausted))]);
    h.idle();

    let mut h = harness(adaptive());
    h.read(0, 1, b"lost");
    h.queue.clear();
    h.tick(999); // one expiry batch, no catch-up burst
    assert_eq!(h.queue.len(), 1);
    assert_eq!(timer(&h), Some(1000));
    h.queue.clear();
    h.tick(1000);
    assert_eq!(h.results, vec![(0, 1, Err(ReadFailure::Timeout))]);
    assert!(h.queue.is_empty());
    h.idle();
}

#[test]
fn queued_reads_expire_without_attempts_and_block_breach_release_all_slots() {
    for mode in 0..3 {
        let mut p = adaptive();
        p.congestion = CongestionPolicy::Fixed { window: 1 };
        p.timers.read_timeout = 5;
        let mut h = harness(p);
        h.read(0, 1, b"a");
        h.read(0, 2, b"b");
        assert_eq!(state(&h).queued, 1);
        let reason = match mode {
            0 => {
                h.tick(5);
                ReadFailure::Timeout
            }
            1 => {
                h.step(
                    0,
                    Event::SetService {
                        peer: view(1).id,
                        allowed: false,
                    },
                );
                ReadFailure::Blocked
            }
            _ => {
                let mut v = view(1);
                v.rift += 1;
                h.step(0, Event::RemoteBreach(v));
                ReadFailure::PeerReset
            }
        };
        assert_eq!(h.results, vec![(0, 1, Err(reason)), (0, 2, Err(reason))]);
        assert_eq!(state(&h).in_flight, 0);
        assert_eq!(state(&h).queued, 0);
        assert_eq!(timer(&h), None);
        h.idle();
    }
}

#[test]
fn policy_validation_and_timer_overflow_leave_no_partial_read() {
    let p = adaptive();
    let mut invalid = vec![];
    let mut bad = p;
    bad.recovery.max_attempts = 0;
    invalid.push(bad);
    let mut bad = p;
    bad.timers.read_timeout = 0;
    invalid.push(bad);
    let mut bad = p;
    bad.timers.retry = RetryTimer::Fixed { after: 0 };
    invalid.push(bad);
    let mut bad = p;
    bad.timers.retry = RetryTimer::Exponential {
        initial: 2,
        maximum: 1,
    };
    invalid.push(bad);
    for c in [
        CongestionPolicy::Fixed { window: 0 },
        CongestionPolicy::Fixed { window: 129 },
        CongestionPolicy::Aimd {
            initial: 0,
            maximum: 4,
        },
        CongestionPolicy::Aimd {
            initial: 5,
            maximum: 4,
        },
    ] {
        let mut bad = p;
        bad.congestion = c;
        invalid.push(bad);
    }
    for bad in invalid {
        assert!(matches!(
            Engine::with_policy(Config::default(), view(0), profile(0), bad),
            Err(Error::InvalidConfig)
        ));
    }
    assert_eq!(p.timers.retry.delay(u32::MAX), 40);
    let mut h = harness(p);
    h.tick(u64::MAX - 1);
    assert_eq!(
        h.engines[0]
            .step(Event::Read {
                id: 1,
                peer: view(1).id,
                path: b"x".into()
            })
            .unwrap_err(),
        Error::ClockOverflow
    );
    h.idle();

    let mut p = p;
    p.timers.read_timeout = 1;
    let mut h = harness(p);
    h.tick(u64::MAX - 1);
    h.read(0, 1, b"last tick");
    assert_eq!(timer(&h), Some(u64::MAX));
    h.tick(u64::MAX);
    assert_eq!(h.results, vec![(0, 1, Err(ReadFailure::Timeout))]);
    h.idle();
    let config = Config {
        max_reads: usize::MAX,
        ..Config::default()
    };
    assert!(matches!(
        Engine::with_policy(config, view(0), profile(0), p),
        Err(Error::InvalidConfig)
    ));
}

#[test]
fn peer_windows_are_independent_and_only_breach_resets_controller() {
    let mut h = harness(adaptive());
    h.read(0, 1, b"a");
    h.tick(5);
    let other = PreconfiguredPeer::new(PeerId(3), "gw-btc", 1, 0, KeyHandle(9)).unwrap();
    h.step(0, Event::InstallPeer(other.clone()));
    h.step(
        0,
        Event::SetAttachment {
            peer: other.id,
            attachment: Some(Attachment(8)),
        },
    );
    h.step(
        0,
        Event::Read {
            id: 2,
            peer: other.id,
            path: b"b".into(),
        },
    );
    h.tick(10);
    assert_eq!(state(&h).window, 2);
    let other_before = h.engines[0].transfer_state(other.id).unwrap();
    assert_eq!(other_before.window, 4);
    assert_eq!(other_before.in_flight, 1);
    let mut updated = view(1);
    updated.life += 1;
    updated.key = KeyHandle(9);
    h.step(0, Event::InstallPeer(updated.clone()));
    assert_eq!(state(&h).window, 2);
    h.step(
        0,
        Event::SetAttachment {
            peer: updated.id,
            attachment: Some(Attachment(99)),
        },
    );
    assert_eq!(state(&h).window, 2);
    updated.rift += 1;
    h.step(0, Event::RemoteBreach(updated));
    assert_eq!(state(&h).window, 4);
    assert_eq!(state(&h).in_flight, 0);
    assert_eq!(h.engines[0].transfer_state(other.id).unwrap(), other_before);
    h.step(0, Event::Cancel { id: 2 });
    h.idle();
}

#[test]
fn adaptive_fault_schedules_replay_exact_actions_with_multiple_reads() {
    for seed in 1..=8 {
        let run = || {
            let mut seed: u64 = seed;
            let mut rng = || {
                seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
                seed
            };
            let mut h = harness(adaptive());
            for i in 0..2 {
                for id in 1..=3 {
                    h.bind(i, &[id as u8], &bytes(17425));
                    h.read(i, id, &[id as u8]);
                }
            }
            let mut delayed = vec![];
            for now in 0..=1000 {
                h.tick(now);
                while let Some(f) = h.queue.pop_front() {
                    if now < 80 && rng() % 5 == 0 {
                        continue;
                    }
                    let due = now + rng() % 8;
                    if now < 80 && rng() % 4 == 0 {
                        delayed.push((due + 3, f.clone()));
                    }
                    if now < 80 && rng() % 7 == 0 {
                        let mut bad = f.clone();
                        *bad.bytes.last_mut().unwrap() ^= 1;
                        delayed.push((due, bad));
                    }
                    delayed.push((due, f));
                }
                while let Some(i) = delayed.iter().rposition(|(due, _)| *due <= now) {
                    let (_, f) = delayed.swap_remove(i);
                    h.deliver(f);
                }
                for (i, e) in h.engines.iter().enumerate() {
                    let s = e.transfer_state(view(1 - i).id).unwrap();
                    assert!((1..=8).contains(&s.window));
                    // A decrease may leave older flights above the new window.
                    assert!(s.in_flight <= 8);
                    assert!(e.usage().pending_interests <= 12);
                }
                if h.results.len() == 6 {
                    break;
                }
            }
            h.results.sort_by_key(|r| (r.0, r.1));
            let expected: Vec<_> = (0..2)
                .flat_map(|i| (1..=3).map(move |id| (i, id, Ok(bytes(17425)))))
                .collect();
            assert_eq!(h.results, expected, "seed {seed}");
            h.idle();
            h.trace
        };
        assert_eq!(run(), run());
    }
}
