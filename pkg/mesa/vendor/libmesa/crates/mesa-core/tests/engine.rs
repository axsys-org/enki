mod support;

use mesa_core::{
    Access, Action, Attachment, Config, DropReason, Engine, Error, Event, KeyHandle, PeerId,
    PreconfiguredPeer, ReadFailure, Usage,
};
use mesa_wire::{decode, encode, Auth, Body, Data, DomainTag, Fragment, Name, Packet};
use support::{fixture, Envelope, FixtureProtection, Harness, A, B, LINK};

fn config() -> Config {
    Config {
        bloq: 6,
        window: 3,
        retry_after: 10,
        read_timeout: 100,
        max_attempts: 4,
        ..Config::default()
    }
}

fn bind(h: &mut Harness, path: &[u8], data: &[u8]) {
    h.step(
        B,
        Event::Bind {
            path: path.to_vec(),
            bytes: data.to_vec(),
            access: Access::Peer(A),
        },
    )
    .unwrap();
}

fn read(h: &mut Harness, id: u64, path: &[u8]) -> Vec<Action> {
    h.step(
        A,
        Event::Read {
            id,
            peer: B,
            path: path.to_vec(),
        },
    )
    .unwrap()
}

fn deliver_next(h: &mut Harness) -> Vec<Action> {
    let packet = h.queue.pop_front().unwrap();
    h.deliver(packet, true)
}

fn first_page(h: &mut Harness, data: &[u8]) -> Envelope {
    bind(h, b"value", data);
    read(h, 1, b"value");
    deliver_next(h);
    h.queue.pop_front().unwrap()
}

fn idle(usage: Usage) {
    assert_eq!(usage.reads, 0);
    assert_eq!(usage.reassembly_bytes, 0);
    assert_eq!(usage.pending_interests, 0);
    assert_eq!(usage.received_fragments, 0);
}

fn success(h: &Harness, id: u64, data: &[u8]) {
    assert_eq!(
        h.results(A),
        vec![Action::ReadFinished {
            id,
            result: Ok(data.to_vec())
        }]
    );
    idle(h.a.usage());
}

fn failure(h: &Harness, why: ReadFailure) {
    assert_eq!(
        h.results(A),
        vec![Action::ReadFinished {
            id: 1,
            result: Err(why)
        }]
    );
    idle(h.a.usage());
}

fn drop_is(actions: &[Action], reason: DropReason) {
    assert_eq!(
        actions
            .iter()
            .filter(|a| matches!(a, Action::Dropped(_)))
            .collect::<Vec<_>>(),
        vec![&Action::Dropped(reason)]
    );
    assert!(!actions
        .iter()
        .any(|a| matches!(a, Action::Send { .. } | Action::ReadFinished { .. })));
}

fn rewrite_page(e: &Envelope, f: impl FnOnce(&mut Name<'_>, &mut Data<'_>)) -> Envelope {
    let packet = decode(&e.bytes, config().wire).unwrap();
    let Body::Page {
        mut name,
        mut data,
        next,
    } = packet.body
    else {
        panic!("not a page")
    };
    f(&mut name, &mut data);
    let mut changed = e.clone();
    changed.bytes = encode(
        &Packet {
            hop: packet.hop,
            body: Body::Page { name, data, next },
        },
        config().wire,
    )
    .unwrap();
    changed
}

#[test]
fn small_read_in_both_directions() {
    let mut h = Harness::new(Config::default());
    for (publisher, reader, data) in [(B, A, b"hello\0".to_vec()), (A, B, b"reverse".to_vec())] {
        h.step(
            publisher,
            Event::Bind {
                path: b"publ/value".to_vec(),
                bytes: data.clone(),
                access: Access::Peer(reader),
            },
        )
        .unwrap();
        h.step(
            reader,
            Event::Read {
                id: 1,
                peer: publisher,
                path: b"publ/value".to_vec(),
            },
        )
        .unwrap();
        h.pump();
        assert_eq!(
            h.results(reader),
            vec![Action::ReadFinished {
                id: 1,
                result: Ok(data)
            }]
        );
    }
    assert_eq!(h.a.usage().reads, 0);
    assert_eq!(h.b.usage().reads, 0);
}

#[test]
fn fragment_boundaries_preserve_exact_bytes_including_zeros() {
    for len in [0, 1, 7, 8, 9, 16, 17, 31, 32, 80, 81, 8192] {
        let mut h = Harness::new(config());
        let data: Vec<_> = (0..len)
            .map(|i| if i % 8 >= 5 { 0 } else { (i % 251) as u8 })
            .collect();
        first_page_and_pump(&mut h, &data);
        success(&h, 1, &data);
        assert!(h.trace.iter().all(|(_, actions)| actions
            .iter()
            .filter(|a| matches!(a, Action::Send { .. }))
            .count()
            <= config().window));
    }
}

fn first_page_and_pump(h: &mut Harness, data: &[u8]) {
    let page = first_page(h, data);
    h.deliver(page, true);
    h.pump();
}

#[test]
fn production_datagram_profile_fragments() {
    let mut h = Harness::new(Config::default());
    let data: Vec<_> = (0..12_345).map(|i| (i % 256) as u8).collect();
    first_page_and_pump(&mut h, &data);
    success(&h, 1, &data);
    assert!(h.trace.iter().flat_map(|(_, a)| a).all(|a| match a {
        Action::Send { bytes, .. } => bytes.len() <= 1472,
        _ => true,
    }));
}

#[test]
fn reconstructs_omitted_terminal_zeros() {
    let mut h = Harness::new(config());
    let data = b"abc\0\0\0\0\0def\0\0\0\0\0";
    let page = first_page(&mut h, data);
    h.deliver(
        rewrite_page(&page, |_, d| d.fragment = &d.fragment[..3]),
        true,
    );
    deliver_next(&mut h);
    let page = h.queue.pop_front().unwrap();
    h.deliver(
        rewrite_page(&page, |_, d| d.fragment = &d.fragment[..3]),
        true,
    );
    success(&h, 1, data);
}

#[test]
fn dropped_peek_and_page_retry_only_at_virtual_deadline() {
    for drop_page in [false, true] {
        let mut h = Harness::new(config());
        bind(&mut h, b"value", b"retry");
        let start = read(&mut h, 1, b"value");
        assert_eq!(start.last(), Some(&Action::SetTimer(Some(10))));
        if drop_page {
            deliver_next(&mut h);
        }
        h.queue.clear();
        h.advance(9);
        assert!(h.queue.is_empty());
        h.advance(10);
        assert_eq!(h.queue.len(), 1);
        h.pump();
        success(&h, 1, b"retry");
    }
}

#[test]
fn fragment_window_recovers_independent_losses_with_duplicates_and_reordering() {
    let mut h = Harness::new(config());
    let data: Vec<_> = (0..113).collect();
    let page = first_page(&mut h, &data);
    h.deliver(page.clone(), true);
    drop_is(&h.deliver(page, true), DropReason::NoInterest);
    assert_eq!(h.a.usage().pending_interests, 3);
    // Drop one interest. Deliver the other two, then drop one page.
    h.queue.pop_front().unwrap();
    deliver_next(&mut h);
    deliver_next(&mut h);
    h.queue.pop_front().unwrap();
    let last = h.queue.pop_back().unwrap();
    h.deliver(last.clone(), true);
    drop_is(&h.deliver(last, true), DropReason::NoInterest);
    // LIFO delivery exercises both page and interest reordering.
    for tick in [0, 10, 20, 30] {
        h.advance(tick);
        while let Some(e) = h.queue.pop_back() {
            h.deliver(e.clone(), true);
            h.deliver(e, true);
            assert!(h.a.usage().pending_interests <= 3);
        }
    }
    success(&h, 1, &data);
}

#[test]
fn duplicate_interest_replies_are_byte_identical() {
    let mut h = Harness::new(config());
    bind(&mut h, b"value", b"12345678abcd");
    read(&mut h, 1, b"value");
    let peek = h.queue.pop_front().unwrap();
    h.deliver(peek.clone(), true);
    h.deliver(peek, true);
    assert_eq!(h.queue[0].bytes, h.queue[1].bytes);
    h.pump();
    success(&h, 1, b"12345678abcd");
}

#[test]
fn delayed_response_after_retry_completes_once() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, b"delayed");
    h.advance(10);
    h.deliver(page.clone(), true);
    h.pump();
    h.deliver(page, true);
    h.advance(100);
    success(&h, 1, b"delayed");
}

#[test]
fn retry_exhaustion_releases_partial_reassembly_and_late_pages() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, &[9; 40]);
    h.deliver(page.clone(), true);
    assert_eq!(h.a.usage().reassembly_bytes, 40);
    for tick in [10, 20, 30, 40] {
        h.queue.clear();
        h.advance(tick);
    }
    failure(&h, ReadFailure::RetriesExhausted);
    drop_is(&h.deliver(page, true), DropReason::NoInterest);
    let end = h.step(A, Event::AdvanceTo(1000)).unwrap();
    assert_eq!(end, vec![Action::SetTimer(None)]);
    failure(&h, ReadFailure::RetriesExhausted);
}

#[test]
fn absolute_timeout_is_not_extended_by_progress_or_large_clock_jumps() {
    let cfg = Config {
        read_timeout: 15,
        ..config()
    };
    let mut h = Harness::new(cfg);
    let page = first_page(&mut h, &[9; 40]);
    h.advance(14);
    h.deliver(page.clone(), true);
    h.queue.clear();
    h.advance(15);
    failure(&h, ReadFailure::Timeout);
    drop_is(&h.deliver(page, true), DropReason::NoInterest);
    assert!(h.queue.is_empty());

    let mut h = Harness::new(config());
    read(&mut h, 1, b"missing");
    h.queue.clear();
    h.advance(1000);
    failure(&h, ReadFailure::Timeout);
    assert!(h.queue.is_empty());
}

#[test]
fn cancellation_before_and_after_first_fragment_reclaims_every_read_resource() {
    for partial in [false, true] {
        let mut h = Harness::new(config());
        let page = first_page(&mut h, &[7; 40]);
        if partial {
            h.deliver(page.clone(), true);
        }
        h.step(A, Event::Cancel { id: 1 }).unwrap();
        failure(&h, ReadFailure::Cancelled);
        h.deliver(page, true);
        h.pump();
        h.advance(1000);
        failure(&h, ReadFailure::Cancelled);
        assert_eq!(h.step(A, Event::Cancel { id: 1 }), Err(Error::UnknownRead));
        // Capacity becomes available for a fresh read.
        read(&mut h, 2, b"value");
        h.pump();
        assert_eq!(
            h.results(A).last(),
            Some(&Action::ReadFinished {
                id: 2,
                result: Ok(vec![7; 40])
            })
        );
        idle(h.a.usage());
    }
}

#[test]
fn malformed_and_failed_protection_packets_do_not_poison_read_state() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, &[7; 20]);
    let initial = h.a.usage();
    for len in 0..page.bytes.len() {
        let mut truncated = page.clone();
        truncated.bytes.truncate(len);
        drop_is(&h.deliver(truncated, true), DropReason::Malformed);
        assert_eq!(h.a.usage(), initial);
    }
    let mut corrupt = page.clone();
    *corrupt.bytes.last_mut().unwrap() ^= 1;
    drop_is(&h.deliver(corrupt, true), DropReason::Malformed);
    drop_is(&h.deliver(page.clone(), false), DropReason::Protection);
    assert_eq!(h.a.usage(), initial);
    h.deliver(page, true);
    h.pump();
    success(&h, 1, &[7; 20]);
}

#[test]
fn source_attachment_and_publisher_are_not_authentication() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, b"secret");
    drop_is(&h.deliver(page.clone(), false), DropReason::Protection);
    let mut spoofed = page.clone();
    spoofed.from = PeerId(999);
    drop_is(&h.deliver(spoofed, true), DropReason::UnknownPeer);
    let mut wrong_route = page.clone();
    wrong_route.attachment = Attachment(8);
    drop_is(&h.deliver(wrong_route, true), DropReason::Attachment);
    let mut wrong_key = page.clone();
    wrong_key.sender.key = KeyHandle(999);
    drop_is(&h.deliver(wrong_key, true), DropReason::Protection);
    h.deliver(page, true);
    success(&h, 1, b"secret");
}

#[test]
fn full_name_and_requested_fragment_must_match() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, b"payload");
    for (changed, reason) in [
        (
            rewrite_page(&page, |n, _| n.publisher = A),
            DropReason::Name,
        ),
        (
            rewrite_page(&page, |n, _| n.domain = DomainTag::new("other").unwrap()),
            DropReason::Name,
        ),
        (rewrite_page(&page, |n, _| n.rift = 1), DropReason::Name),
        (rewrite_page(&page, |n, _| n.bloq = 7), DropReason::Name),
        (
            rewrite_page(&page, |n, _| n.path = b"different"),
            DropReason::NoInterest,
        ),
        (
            rewrite_page(&page, |n, _| n.fragment = Fragment::Data(1)),
            DropReason::NoInterest,
        ),
        (
            rewrite_page(&page, |n, _| n.fragment = Fragment::Data(u64::MAX)),
            DropReason::NoInterest,
        ),
        (
            rewrite_page(&page, |n, _| n.fragment = Fragment::Init),
            DropReason::NoInterest,
        ),
        (
            rewrite_page(&page, |n, _| n.fragment = Fragment::Auth(0)),
            DropReason::NoInterest,
        ),
    ] {
        drop_is(&h.deliver(changed, true), reason);
        assert_eq!(h.a.usage().reassembly_bytes, 0);
    }
    h.deliver(page, true);
    success(&h, 1, b"payload");
}

#[test]
fn invalid_lengths_auth_and_inconsistent_total_are_dropped() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, &[5; 20]);
    drop_is(
        &h.deliver(rewrite_page(&page, |_, d| d.total_bytes = 2), true),
        DropReason::Fragment,
    );
    drop_is(
        &h.deliver(rewrite_page(&page, |_, d| d.total_bytes = 0), true),
        DropReason::Fragment,
    );
    drop_is(
        &h.deliver(
            rewrite_page(&page, |_, d| d.auth = Auth::Signature(b"fake")),
            true,
        ),
        DropReason::Unsupported,
    );
    h.deliver(page, true);
    deliver_next(&mut h);
    deliver_next(&mut h);
    let page = h.queue.pop_front().unwrap();
    let before = h.a.usage();
    drop_is(
        &h.deliver(rewrite_page(&page, |_, d| d.total_bytes = 21), true),
        DropReason::Fragment,
    );
    assert_eq!(h.a.usage(), before);
    h.deliver(page, true);
    h.pump();
    success(&h, 1, &[5; 20]);
}

#[test]
fn oversized_totals_fail_without_allocating() {
    for total in [1024 * 1024 + 1, u64::MAX, 8193] {
        let mut h = Harness::new(config());
        let page = first_page(&mut h, b"small");
        h.deliver(rewrite_page(&page, |_, d| d.total_bytes = total), true);
        failure(&h, ReadFailure::ResourceLimit);
    }
}

#[test]
fn aggregate_reassembly_budget_and_request_limit_are_reusable() {
    let cfg = Config {
        max_reads: 2,
        max_reassembly_bytes: 24,
        ..config()
    };
    let mut h = Harness::new(cfg);
    bind(&mut h, b"one", &[1; 16]);
    bind(&mut h, b"two", &[2; 16]);
    read(&mut h, 1, b"one");
    read(&mut h, 2, b"two");
    assert_eq!(
        h.step(
            A,
            Event::Read {
                id: 3,
                peer: B,
                path: b"three".to_vec()
            }
        ),
        Err(Error::ResourceLimit)
    );
    deliver_next(&mut h);
    deliver_next(&mut h);
    deliver_next(&mut h);
    assert_eq!(h.a.usage().reassembly_bytes, 16);
    deliver_next(&mut h);
    assert_eq!(
        h.results(A),
        vec![Action::ReadFinished {
            id: 2,
            result: Err(ReadFailure::ResourceLimit)
        }]
    );
    h.pump();
    idle(h.a.usage());
    read(&mut h, 3, b"two");
    h.pump();
    assert_eq!(
        h.results(A).last(),
        Some(&Action::ReadFinished {
            id: 3,
            result: Ok(vec![2; 16])
        })
    );
}

#[test]
fn binding_budget_immutability_and_unbind() {
    let cfg = Config {
        max_bindings: 2,
        max_binding_bytes: 8,
        ..config()
    };
    let mut h = Harness::new(cfg);
    bind(&mut h, b"one", b"12345678");
    let put = |path: &[u8], bytes: &[u8]| Event::Bind {
        path: path.to_vec(),
        bytes: bytes.to_vec(),
        access: Access::Public,
    };
    assert_eq!(h.step(B, put(b"two", b"x")), Err(Error::ResourceLimit));
    assert_eq!(h.step(B, put(b"one", b"different")), Err(Error::NameInUse));
    h.step(B, Event::Unbind { path: b"one" }).unwrap();
    assert_eq!(h.b.usage().binding_bytes, 0);
    assert_eq!(h.b.usage().bindings, 0);
    assert_eq!(h.b.usage().namespace_slots, 1);
    assert_eq!(h.step(B, put(b"one", b"new")), Err(Error::RetiredName));
    assert_eq!(
        h.step(B, Event::Unbind { path: b"one" }),
        Err(Error::Unbound)
    );
    bind(&mut h, b"two", b"12345678");
    h.step(B, Event::Unbind { path: b"two" }).unwrap();
    assert_eq!(h.step(B, put(b"three", b"")), Err(Error::ResourceLimit));
    read(&mut h, 1, b"one");
    h.pump();
    h.advance(100);
    failure(&h, ReadFailure::Timeout);
}

#[test]
fn unbind_during_read_releases_server_bytes_and_reader_eventually_terminates() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, &[7; 20]);
    h.deliver(page, true);
    h.step(B, Event::Unbind { path: b"value" }).unwrap();
    assert_eq!(h.b.usage().binding_bytes, 0);
    h.pump();
    for tick in [10, 20, 30, 40] {
        h.advance(tick);
        h.pump();
    }
    failure(&h, ReadFailure::RetriesExhausted);
}

#[test]
fn namespace_access_and_failed_peek_protection_never_send_data() {
    let mut h = Harness::new(config());
    h.step(
        B,
        Event::Bind {
            path: b"private".to_vec(),
            bytes: b"secret".to_vec(),
            access: Access::Peer(PeerId(3)),
        },
    )
    .unwrap();
    read(&mut h, 1, b"private");
    let peek = h.queue.pop_front().unwrap();
    drop_is(&h.deliver(peek.clone(), false), DropReason::Protection);
    drop_is(&h.deliver(peek, true), DropReason::Unavailable);
    read(&mut h, 2, b"missing");
    drop_is(&deliver_next(&mut h), DropReason::Unavailable);
    assert!(h.queue.is_empty());
    h.advance(100);
    assert_eq!(h.results(A).len(), 2);
    idle(h.a.usage());
}

#[test]
fn block_cancels_pending_reads_and_unblock_allows_new_work() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, &[1; 30]);
    h.deliver(page.clone(), true);
    h.step(
        A,
        Event::SetService {
            peer: B,
            allowed: false,
        },
    )
    .unwrap();
    failure(&h, ReadFailure::Blocked);
    drop_is(&h.deliver(page, true), DropReason::Blocked);
    assert_eq!(
        h.step(
            A,
            Event::Read {
                id: 2,
                peer: B,
                path: b"value".to_vec()
            }
        ),
        Err(Error::Blocked)
    );
    h.step(
        B,
        Event::SetService {
            peer: A,
            allowed: false,
        },
    )
    .unwrap();
    while !h.queue.is_empty() {
        drop_is(&deliver_next(&mut h), DropReason::Blocked);
    }
    h.step(
        A,
        Event::SetService {
            peer: B,
            allowed: true,
        },
    )
    .unwrap();
    h.step(
        B,
        Event::SetService {
            peer: A,
            allowed: true,
        },
    )
    .unwrap();
    read(&mut h, 2, b"value");
    h.pump();
    assert_eq!(
        h.results(A).last(),
        Some(&Action::ReadFinished {
            id: 2,
            result: Ok(vec![1; 30])
        })
    );
}

#[test]
fn peer_updates_reject_conflicts_and_coordinate_regressions() {
    let mut h = Harness::new(config());
    let before = h.a.usage();
    assert_eq!(
        h.step(
            A,
            Event::InstallPeer(PreconfiguredPeer::new(B, "other", 1, 0, KeyHandle(102)).unwrap())
        ),
        Err(Error::DomainConflict)
    );
    let mut view = fixture(B);
    view.life = 0;
    assert_eq!(
        h.step(A, Event::InstallPeer(view)),
        Err(Error::CoordinateRegression)
    );
    let mut view = fixture(B);
    view.key = KeyHandle(999);
    assert_eq!(h.step(A, Event::InstallPeer(view)), Err(Error::KeyConflict));
    let mut view = fixture(B);
    view.rift = 1;
    assert_eq!(
        h.step(A, Event::InstallPeer(view.clone())),
        Err(Error::BreachRequired)
    );
    assert_eq!(
        h.step(A, Event::RemoteBreach(fixture(B))),
        Err(Error::InvalidBreach)
    );
    h.step(A, Event::RemoteBreach(view)).unwrap();
    assert_eq!(
        h.step(A, Event::InstallPeer(fixture(B))),
        Err(Error::CoordinateRegression)
    );
    assert_eq!(
        h.step(A, Event::InstallPeer(fixture(A))),
        Err(Error::LocalPeer)
    );
    assert_eq!(h.a.usage(), before);
}

#[test]
fn same_rift_key_change_preserves_partial_read_but_uses_current_key() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, &[1; 20]);
    h.deliver(page, true);
    deliver_next(&mut h);
    deliver_next(&mut h);
    let before = h.a.usage();
    let mut new_view = fixture(B);
    new_view.life = 2;
    new_view.key = KeyHandle(200);
    h.step(A, Event::InstallPeer(new_view.clone())).unwrap();
    assert_eq!(h.a.usage(), before);
    let old = h.queue.pop_front().unwrap();
    drop_is(&h.deliver(old.clone(), true), DropReason::Protection);
    let mut current = old;
    current.sender = new_view.clone();
    h.deliver(current, true);
    while let Some(mut e) = h.queue.pop_front() {
        e.sender = new_view.clone();
        h.deliver(e, true);
    }
    success(&h, 1, &[1; 20]);
}

#[test]
fn breach_invalidates_old_epoch_work_and_new_interests_use_new_rift() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, &[1; 20]);
    h.deliver(page.clone(), true);
    let mut new_view = fixture(B);
    new_view.rift = 1;
    h.step(A, Event::RemoteBreach(new_view.clone())).unwrap();
    failure(&h, ReadFailure::PeerReset);
    // Grant a current-key verdict to prove the separate name/rift gate.
    let mut stale = page;
    stale.sender = new_view;
    drop_is(&h.deliver(stale, true), DropReason::Name);
    h.queue.clear();
    read(&mut h, 2, b"value");
    let peek = h.queue.pop_front().unwrap();
    let Body::Peek(name) = decode(&peek.bytes, config().wire).unwrap().body else {
        panic!()
    };
    assert_eq!(name.rift, 1);
    drop_is(&h.deliver(peek, true), DropReason::Name);
    h.advance(100);
    idle(h.a.usage());
}

#[test]
fn attachments_are_updated_independently_and_pending_reads_survive() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, b"route");
    h.step(
        A,
        Event::SetAttachment {
            peer: B,
            attachment: None,
        },
    )
    .unwrap();
    drop_is(&h.deliver(page, true), DropReason::Attachment);
    h.advance(10);
    assert!(h.queue.is_empty());
    let new_link = Attachment(8);
    h.step(
        A,
        Event::SetAttachment {
            peer: B,
            attachment: Some(new_link),
        },
    )
    .unwrap();
    h.step(
        B,
        Event::SetAttachment {
            peer: A,
            attachment: Some(new_link),
        },
    )
    .unwrap();
    h.advance(20);
    assert_eq!(h.queue.front().unwrap().attachment, new_link);
    h.pump();
    success(&h, 1, b"route");
}

#[test]
fn host_errors_leave_existing_work_unchanged() {
    let mut h = Harness::new(config());
    read(&mut h, 1, b"value");
    let before = h.a.usage();
    for (event, error) in [
        (
            Event::Read {
                id: 1,
                peer: B,
                path: b"else".to_vec(),
            },
            Error::ReadIdInUse,
        ),
        (
            Event::Read {
                id: 2,
                peer: B,
                path: b"value".to_vec(),
            },
            Error::AlreadyPending,
        ),
        (
            Event::Read {
                id: 2,
                peer: B,
                path: vec![],
            },
            Error::InvalidPath,
        ),
        (
            Event::Read {
                id: 2,
                peer: B,
                path: vec![0; 257],
            },
            Error::InvalidPath,
        ),
        (
            Event::Read {
                id: 2,
                peer: PeerId(99),
                path: b"value".to_vec(),
            },
            Error::UnknownPeer,
        ),
        (
            Event::SetService {
                peer: PeerId(99),
                allowed: false,
            },
            Error::UnknownPeer,
        ),
        (
            Event::SetAttachment {
                peer: PeerId(99),
                attachment: Some(LINK),
            },
            Error::UnknownPeer,
        ),
    ] {
        assert_eq!(h.step(A, event), Err(error));
        assert_eq!(h.a.usage(), before);
    }
    h.advance(1);
    assert_eq!(h.step(A, Event::AdvanceTo(0)), Err(Error::ClockRegression));
    h.step(
        A,
        Event::SetAttachment {
            peer: B,
            attachment: None,
        },
    )
    .unwrap();
    assert_eq!(
        h.step(
            A,
            Event::Read {
                id: 2,
                peer: B,
                path: b"other".to_vec()
            }
        ),
        Err(Error::NoRoute)
    );
    h.advance(100);
    failure(&h, ReadFailure::Timeout);
}

#[test]
fn invalid_config_and_time_overflow_fail_explicitly() {
    for cfg in [
        Config {
            bloq: 2,
            ..config()
        },
        Config {
            bloq: 255,
            ..config()
        },
        Config {
            window: 0,
            ..config()
        },
        Config {
            retry_after: 0,
            ..config()
        },
        Config {
            max_attempts: 0,
            ..config()
        },
        Config {
            read_timeout: 0,
            ..config()
        },
        Config {
            max_peers: 0,
            ..config()
        },
        Config {
            max_reads: 0,
            ..config()
        },
        Config {
            max_fragments: 1,
            window: 2,
            ..config()
        },
        Config {
            wire: mesa_wire::Limits {
                max_packet: 10,
                ..config().wire
            },
            ..config()
        },
    ] {
        assert!(matches!(
            Engine::new(cfg, fixture(A), FixtureProtection::default()),
            Err(Error::InvalidConfig)
        ));
    }
    assert_eq!(
        PreconfiguredPeer::new(B, "INVALID", 1, 0, KeyHandle(1)),
        Err(Error::InvalidDomain)
    );
    let mut h = Harness::new(config());
    h.advance(u64::MAX - 1);
    assert_eq!(
        h.step(
            A,
            Event::Read {
                id: 1,
                peer: B,
                path: b"value".to_vec()
            }
        ),
        Err(Error::ClockOverflow)
    );
    idle(h.a.usage());
}

#[test]
fn peer_table_is_bounded_and_existing_views_can_be_updated() {
    let mut h = Harness::new(Config {
        max_peers: 1,
        ..config()
    });
    assert_eq!(
        h.step(A, Event::InstallPeer(fixture(PeerId(3)))),
        Err(Error::ResourceLimit)
    );
    h.step(A, Event::InstallPeer(fixture(B))).unwrap();
    assert_eq!(h.a.usage().peers, 1);
}

#[test]
fn replayed_fault_schedules_produce_identical_actions_and_exact_bidirectional_reads() {
    fn run(mut seed: u64) -> Vec<(PeerId, Vec<Action>)> {
        let mut next = || {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
            seed
        };
        let mut h = Harness::new(Config {
            max_attempts: 8,
            read_timeout: 300,
            ..config()
        });
        let forward: Vec<_> = (0..137).map(|i| (i % 111) as u8).collect();
        let reverse: Vec<_> = (0..99).map(|i| (i % 47) as u8).collect();
        bind(&mut h, b"forward", &forward);
        h.step(
            A,
            Event::Bind {
                path: b"reverse".to_vec(),
                bytes: reverse.clone(),
                access: Access::Peer(B),
            },
        )
        .unwrap();
        read(&mut h, 1, b"forward");
        h.step(
            B,
            Event::Read {
                id: 2,
                peer: A,
                path: b"reverse".to_vec(),
            },
        )
        .unwrap();
        let mut scheduled: Vec<(u64, Envelope)> = Vec::new();
        for now in 0..=300 {
            h.advance(now);
            let mut budget = 1000;
            loop {
                while let Some(e) = h.queue.pop_front() {
                    if now < 30 && next() % 5 == 0 {
                        continue;
                    }
                    let due = now + next() % 8;
                    if now < 30 && next() % 3 == 0 {
                        scheduled.push((due + 3, e.clone()));
                    }
                    if now < 30 && next() % 7 == 0 {
                        let mut malformed = e.clone();
                        *malformed.bytes.last_mut().unwrap() ^= 1;
                        scheduled.push((due, malformed));
                    }
                    scheduled.push((due, e));
                }
                // Random selection among currently due packets changes delivery
                // order; resulting sends, drops, and completions enter the trace.
                let due: Vec<_> = scheduled
                    .iter()
                    .enumerate()
                    .filter(|(_, (at, _))| *at <= now)
                    .map(|(i, _)| i)
                    .collect();
                if due.is_empty() {
                    break;
                }
                assert!(budget > 0);
                budget -= 1;
                let i = due[(next() % due.len() as u64) as usize];
                let (_, e) = scheduled.swap_remove(i);
                h.deliver(e, true);
                for usage in [h.a.usage(), h.b.usage()] {
                    assert!(usage.pending_interests <= 3);
                    assert!(usage.reassembly_bytes <= 137);
                }
            }
            if scheduled.is_empty()
                && h.queue.is_empty()
                && h.a.usage().reads == 0
                && h.b.usage().reads == 0
            {
                break;
            }
        }
        success(&h, 1, &forward);
        assert_eq!(
            h.results(B),
            vec![Action::ReadFinished {
                id: 2,
                result: Ok(reverse)
            }]
        );
        idle(h.b.usage());
        h.trace
    }
    for seed in 1..=32 {
        assert_eq!(run(seed), run(seed));
    }
}

#[test]
fn serving_checks_full_local_name_and_fragment_range() {
    let mut h = Harness::new(config());
    bind(&mut h, b"value", &[3; 20]);
    read(&mut h, 1, b"value");
    let peek = h.queue.pop_front().unwrap();
    let Body::Peek(name) = decode(&peek.bytes, config().wire).unwrap().body else {
        panic!()
    };
    for (changed, reason) in [
        (
            Name {
                publisher: A,
                ..name
            },
            DropReason::Name,
        ),
        (
            Name {
                domain: DomainTag::new("other").unwrap(),
                ..name
            },
            DropReason::Name,
        ),
        (Name { rift: 1, ..name }, DropReason::Name),
        (Name { bloq: 5, ..name }, DropReason::Name),
        (
            Name {
                fragment: Fragment::Data(u64::MAX),
                ..name
            },
            DropReason::Fragment,
        ),
        (
            Name {
                fragment: Fragment::Data(3),
                ..name
            },
            DropReason::Fragment,
        ),
        (
            Name {
                fragment: Fragment::Auth(0),
                ..name
            },
            DropReason::Unsupported,
        ),
        (
            Name {
                fragment: Fragment::Init,
                ..name
            },
            DropReason::Unsupported,
        ),
    ] {
        let mut e = peek.clone();
        e.bytes = encode(
            &Packet {
                hop: 0,
                body: Body::Peek(changed),
            },
            config().wire,
        )
        .unwrap();
        drop_is(&h.deliver(e, true), reason);
    }
    assert!(h.queue.is_empty());
    h.deliver(peek, true);
    h.pump();
    success(&h, 1, &[3; 20]);
}

#[test]
fn small_init_peeks_get_one_page_and_pokes_or_trailers_are_unsupported() {
    let mut h = Harness::new(config());
    let page = first_page(&mut h, b"hello");
    let packet = decode(&page.bytes, config().wire).unwrap();
    let Body::Page { name, data, .. } = packet.body else {
        panic!()
    };
    let mut e = page.clone();
    e.bytes = encode(
        &Packet {
            hop: 0,
            body: Body::Page {
                name,
                data,
                next: Some([0; 6]),
            },
        },
        config().wire,
    )
    .unwrap();
    drop_is(&h.deliver(e, true), DropReason::Unsupported);
    let mut e = page.clone();
    e.bytes = encode(
        &Packet {
            hop: 0,
            body: Body::Poke {
                ack: name,
                payload: name,
                data,
            },
        },
        config().wire,
    )
    .unwrap();
    drop_is(&h.deliver(e, true), DropReason::Unsupported);
    let init_name = Name {
        fragment: Fragment::Init,
        ..name
    };
    let peek = Envelope {
        from: A,
        to: B,
        attachment: LINK,
        sender: fixture(A),
        bytes: encode(
            &Packet {
                hop: 0,
                body: Body::Peek(init_name),
            },
            config().wire,
        )
        .unwrap(),
    };
    h.deliver(peek, true);
    assert_eq!(h.queue.len(), 1);
    let response = h.queue.pop_front().unwrap();
    let Body::Page {
        name: received_name,
        data: received_data,
        ..
    } = decode(&response.bytes, config().wire).unwrap().body
    else {
        panic!()
    };
    assert_eq!(received_name, init_name);
    assert_eq!(received_data.fragment, b"hello");
    // The reader requested Data(0), so even equivalent Init data cannot match.
    drop_is(&h.deliver(response, true), DropReason::NoInterest);
    h.deliver(page, true);
    success(&h, 1, b"hello");
}

#[test]
fn missing_route_waits_until_absolute_deadline_without_spending_unsent_attempts() {
    let mut h = Harness::new(config());
    bind(&mut h, b"value", b"route");
    read(&mut h, 1, b"value");
    h.queue.clear();
    h.step(
        A,
        Event::SetAttachment {
            peer: B,
            attachment: None,
        },
    )
    .unwrap();
    for tick in [10, 20, 30, 40, 50] {
        h.advance(tick);
    }
    assert!(h.results(A).is_empty());
    assert!(h.queue.is_empty());
    h.step(
        A,
        Event::SetAttachment {
            peer: B,
            attachment: Some(LINK),
        },
    )
    .unwrap();
    h.advance(60);
    h.pump();
    success(&h, 1, b"route");
}

#[test]
fn namespace_fences_reclaim_live_objects_and_tombstones_without_rebinding() {
    let mut c = config();
    c.max_bindings = 3;
    let mut h = Harness::new(c);
    bind(&mut h, b"log/00", b"first");
    bind(&mut h, b"log/01", b"second");
    bind(&mut h, b"other", b"untouched");
    h.step(B, Event::Unbind { path: b"log/00" }).unwrap();
    h.step(
        B,
        Event::RetireBefore {
            prefix: b"log/".to_vec(),
            suffix: b"02".to_vec(),
        },
    )
    .unwrap();
    assert_eq!(h.b.usage().namespace_slots, 2);
    assert_eq!(h.b.usage().bindings, 1);
    assert_eq!(h.b.usage().binding_bytes, 9);
    for path in [b"log/00", b"log/01"] {
        assert_eq!(
            h.step(
                B,
                Event::Bind {
                    path: path.to_vec(),
                    bytes: vec![],
                    access: Access::Public
                }
            ),
            Err(Error::RetiredName)
        );
    }
    bind(&mut h, b"log/02", b"next");
    assert_eq!(
        h.step(
            B,
            Event::RetireBefore {
                prefix: b"log/".to_vec(),
                suffix: b"01".to_vec()
            }
        ),
        Err(Error::RetiredName)
    );
    assert_eq!(
        h.step(
            B,
            Event::RetireBefore {
                prefix: b"new/".to_vec(),
                suffix: b"01".to_vec()
            }
        ),
        Err(Error::ResourceLimit)
    );
    assert_eq!(h.b.usage().namespace_slots, 3);
    h.step(
        B,
        Event::RetireBefore {
            prefix: b"log/".to_vec(),
            suffix: b"03".to_vec(),
        },
    )
    .unwrap();
    assert_eq!(h.b.usage().namespace_slots, 2);
    // A restored fence rejects even names never bound in this process.
    let mut restored = Harness::new(config());
    restored
        .step(
            B,
            Event::RetireBefore {
                prefix: b"log/".to_vec(),
                suffix: b"03".to_vec(),
            },
        )
        .unwrap();
    assert_eq!(
        restored.step(
            B,
            Event::Bind {
                path: b"log/02".to_vec(),
                bytes: vec![],
                access: Access::Public
            }
        ),
        Err(Error::RetiredName)
    );
}
