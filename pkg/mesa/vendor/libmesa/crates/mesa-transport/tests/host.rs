mod support;
use mesa_core::Attachment;
use mesa_flow::{Action, Event};
use mesa_transport::{Capabilities, Framing, Host, Notice, PacketIo, Received, Udp};
use std::{cell::RefCell, collections::VecDeque, io, net::UdpSocket, rc::Rc};
use support::{fixture::*, *};
#[derive(Default)]
struct Network {
    q: [VecDeque<Vec<u8>>; 2],
    send_error: bool,
    pressure: bool,
    fail_after_receipt: bool,
    next_error: bool,
    reads: usize,
    discard: bool,
}
struct Io {
    shared: Rc<RefCell<Network>>,
    side: usize,
}
impl PacketIo for Io {
    fn capabilities(&self) -> Capabilities {
        Capabilities {
            max_packet: 1472,
            reliable: false,
            ordered: false,
            framing: Framing::Datagram,
        }
    }
    fn send(&mut self, bytes: &[u8]) -> io::Result<bool> {
        let mut s = self.shared.borrow_mut();
        if s.send_error {
            return Err(io::ErrorKind::BrokenPipe.into());
        }
        if s.pressure {
            return Ok(false);
        }
        s.q[1 - self.side].push_back(bytes.to_vec());
        Ok(true)
    }
    fn receive(&mut self) -> io::Result<Received> {
        let mut s = self.shared.borrow_mut();
        s.reads += 1;
        if self.side == 0 && s.next_error {
            return Err(io::ErrorKind::ConnectionReset.into());
        }
        if s.discard {
            return Ok(Received::Discarded);
        }
        let Some(p) = s.q[self.side].pop_front() else {
            return Ok(Received::Idle);
        };
        if self.side == 0 && s.fail_after_receipt {
            if let mesa_wire::Body::Page { name, .. } =
                mesa_wire::decode(&p, config().core.wire).unwrap().body
            {
                if name.path.windows(7).any(|b| b == b"receipt")
                    && name.fragment == mesa_wire::Fragment::Data(0)
                {
                    s.next_error = true;
                }
            }
        }
        Ok(Received::Packet(p))
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
#[test]
fn receive_failure_keeps_settlement_commit_in_same_batch() {
    let shared = Rc::new(RefCell::new(Network {
        fail_after_receipt: true,
        ..Default::default()
    }));
    let mut stores = [Store::new(0), Store::new(1)];
    let mut hosts = std::array::from_fn::<_, 2, _>(|i| {
        host(
            i,
            &stores[i],
            Io {
                shared: shared.clone(),
                side: i,
            },
        )
    });
    let batch = hosts[0]
        .step(Event::Enqueue {
            flow: 7,
            sequence: 0,
            bytes: payload(1025),
        })
        .unwrap();
    commit(&mut hosts[0], &mut stores[0], batch);
    let mut observed = false;
    for now in 0..100 {
        for i in 0..2 {
            let mut batch = hosts[i].poll(now).unwrap();
            if !batch.notices.is_empty() {
                assert_eq!(i, 0);
                assert_eq!(
                    batch.notices,
                    vec![Notice::Disconnected(io::ErrorKind::ConnectionReset)]
                );
                assert!(batch.actions.iter().any(|a| matches!(a, Action::Commit(_))));
                assert!(!hosts[i].is_connected());
                observed = true;
                batch.notices.clear();
            }
            commit(&mut hosts[i], &mut stores[i], batch);
        }
        if observed {
            break;
        }
    }
    assert!(observed);
    assert_eq!(stores[0].checkpoint().flow(7).unwrap().settled.len(), 1);
    assert_eq!(stores[1].effects.len(), 1);
}
#[test]
fn send_failure_preserves_commit_notifications_and_reconnect_reannounces() {
    let shared = Rc::new(RefCell::new(Network {
        send_error: true,
        ..Default::default()
    }));
    let mut store = Store::new(0);
    let mut h = host(
        0,
        &store,
        Io {
            shared: shared.clone(),
            side: 0,
        },
    );
    let batch = h
        .step(Event::Enqueue {
            flow: 7,
            sequence: 0,
            bytes: payload(42),
        })
        .unwrap();
    let request = batch
        .actions
        .into_iter()
        .find_map(|a| {
            if let Action::Commit(r) = a {
                Some(r)
            } else {
                None
            }
        })
        .unwrap();
    assert!(shared.borrow().q[1].is_empty());
    assert!(h.node().checkpoint().flow(7).unwrap().outbox.is_empty());
    store.commit(&request).unwrap();
    let batch = h.step(Event::Committed(request.token)).unwrap();
    assert!(batch
        .actions
        .iter()
        .any(|a| matches!(a, Action::Enqueued { .. })));
    assert_eq!(
        batch.notices,
        vec![
            Notice::SendDropped,
            Notice::Disconnected(io::ErrorKind::BrokenPipe)
        ]
    );
    assert!(!h.is_connected());
    assert_eq!(h.node().usage().outbox_messages, 1);
    shared.borrow_mut().send_error = false;
    let batch = h
        .reconnect(Io {
            shared: shared.clone(),
            side: 0,
        })
        .unwrap();
    assert!(batch.notices.is_empty());
    assert!(h.is_connected());
    assert!(!shared.borrow().q[1].is_empty());
    assert_eq!(h.node().checkpoint(), &store.checkpoint());
}
#[test]
fn pressure_is_loss_and_poll_budget_bounds_discarded_input() {
    let shared = Rc::new(RefCell::new(Network {
        pressure: true,
        discard: true,
        ..Default::default()
    }));
    let mut store = Store::new(0);
    let mut h = host(
        0,
        &store,
        Io {
            shared: shared.clone(),
            side: 0,
        },
    );
    let batch = h
        .step(Event::Enqueue {
            flow: 7,
            sequence: 0,
            bytes: payload(1),
        })
        .unwrap();
    let request = batch
        .actions
        .into_iter()
        .find_map(|a| {
            if let Action::Commit(r) = a {
                Some(r)
            } else {
                None
            }
        })
        .unwrap();
    store.commit(&request).unwrap();
    let batch = h.step(Event::Committed(request.token)).unwrap();
    assert_eq!(batch.notices, vec![Notice::SendDropped]);
    assert!(h.is_connected());
    let batch = h.poll(1).unwrap();
    assert_eq!(shared.borrow().reads, 64);
    assert_eq!(batch.notices.len(), 64);
    assert!(h
        .step(Event::SetAttachment {
            peer: view(1).id,
            attachment: None
        })
        .is_err());
    assert_eq!(h.node().usage().outbox_messages, 1);
    assert!(Host::new(
        node(0, &store),
        view(0).id,
        Attachment(7),
        Io { shared, side: 0 }
    )
    .is_err());
}
#[test]
fn udp_rejects_unknown_source_and_oversized_datagrams() {
    let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
    let target = socket.local_addr().unwrap();
    let known = UdpSocket::bind("127.0.0.1:0").unwrap();
    let unknown = UdpSocket::bind("127.0.0.1:0").unwrap();
    let mut io = Udp::new(socket, known.local_addr().unwrap(), 1472).unwrap();
    unknown.send_to(&[1; 16], target).unwrap();
    known.send_to(&[2; 1473], target).unwrap();
    known.send_to(&[3; 16], target).unwrap();
    let mut received = vec![];
    for _ in 0..100 {
        match io.receive().unwrap() {
            Received::Idle => std::thread::sleep(std::time::Duration::from_millis(1)),
            p => received.push(p),
        }
        if received.len() == 3 {
            break;
        }
    }
    assert_eq!(
        received,
        vec![
            Received::Discarded,
            Received::Discarded,
            Received::Packet(vec![3; 16])
        ]
    );
}

#[test]
fn permanent_queue_pressure_exhausts_budget_without_retiring_outbox() {
    let shared = Rc::new(RefCell::new(Network {
        pressure: true,
        ..Default::default()
    }));
    let mut store = Store::new(0);
    let mut c = config();
    c.transfer.recovery.max_attempts = 3;
    let node = mesa_flow::Node::new(
        c,
        view(0),
        protection(0),
        flows(0),
        store.checkpoint(),
        store.incarnation,
    )
    .unwrap();
    let (mut host, _) = Host::new(node, view(1).id, Attachment(7), Io { shared, side: 0 }).unwrap();
    let batch = host
        .step(Event::Enqueue {
            flow: 7,
            sequence: 0,
            bytes: payload(10),
        })
        .unwrap();
    let request = batch
        .actions
        .into_iter()
        .find_map(|a| {
            if let Action::Commit(r) = a {
                Some(r)
            } else {
                None
            }
        })
        .unwrap();
    store.commit(&request).unwrap();
    let batch = host.step(Event::Committed(request.token)).unwrap();
    let mut sends = batch
        .notices
        .iter()
        .filter(|n| **n == Notice::SendDropped)
        .count();
    let mut stalls = 0;
    for now in 1..1000 {
        let batch = host.poll(now).unwrap();
        sends += batch
            .notices
            .iter()
            .filter(|n| **n == Notice::SendDropped)
            .count();
        stalls += batch
            .actions
            .iter()
            .filter(|a| matches!(a, Action::AnnouncementStalled { .. }))
            .count();
    }
    assert_eq!(sends, 3);
    assert_eq!(stalls, 1);
    assert_eq!(host.node().usage().outbox_messages, 1);
    assert!(store.effects.is_empty());
    assert!(host.poll(2000).unwrap().notices.is_empty());
    assert_eq!(
        host.step(Event::Resume { flow: 7 }).unwrap().notices,
        vec![Notice::SendDropped]
    );
}
