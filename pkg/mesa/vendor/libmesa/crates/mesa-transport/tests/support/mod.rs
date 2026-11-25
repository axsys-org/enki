#![allow(dead_code)]
#[path = "../../../mesa-flow/tests/support/mod.rs"]
pub mod fixture;
use fixture::*;
use mesa_core::Attachment;
use mesa_flow::{Action, Event, Node, Outcome};
use mesa_transport::{Batch, Host, PacketIo, Stream, Udp};
use std::{
    collections::VecDeque,
    net::{TcpListener, TcpStream, UdpSocket},
    time::{Duration, Instant},
};

pub fn node(i: usize, store: &Store) -> Node {
    let mut c = config();
    c.transfer.timers.read_timeout = 5000;
    c.transfer.recovery.max_attempts = 100;
    Node::new(
        c,
        view(i),
        protection(i),
        flows(i),
        store.checkpoint(),
        store.incarnation,
    )
    .unwrap()
}
pub fn host<I: PacketIo>(i: usize, store: &Store, io: I) -> Host<I> {
    let (host, batch) = Host::new(node(i, store), view(1 - i).id, Attachment(7), io).unwrap();
    assert!(batch.notices.is_empty());
    assert!(batch
        .actions
        .iter()
        .all(|a| matches!(a, Action::SetTimer(_) | Action::Send { .. })));
    host
}
pub fn commit<I: PacketIo>(host: &mut Host<I>, store: &mut Store, batch: Batch) {
    assert!(batch.notices.is_empty(), "{:?}", batch.notices);
    let mut actions: VecDeque<_> = batch.actions.into();
    for _ in 0..10000 {
        let Some(action) = actions.pop_front() else {
            return;
        };
        let more = match action {
            Action::Commit(r) => {
                store.commit(&r).unwrap();
                host.step(Event::Committed(r.token)).unwrap()
            }
            Action::Deliver { id, .. } => host
                .step(Event::Decide {
                    id,
                    outcome: Outcome::Accepted,
                })
                .unwrap(),
            Action::Stalled { .. } | Action::AnnouncementStalled { .. } => {
                panic!("unexpected stall: {action:?}")
            }
            _ => continue,
        };
        assert!(more.notices.is_empty(), "{:?}", more.notices);
        actions.extend(more.actions);
    }
    panic!("commit loop");
}
pub fn udp_pair() -> [Udp; 2] {
    let sockets = [
        UdpSocket::bind("127.0.0.1:0").unwrap(),
        UdpSocket::bind("127.0.0.1:0").unwrap(),
    ];
    let addresses = sockets.each_ref().map(|s| s.local_addr().unwrap());
    let [a, b] = sockets;
    [
        Udp::new(a, addresses[1], 1472).unwrap(),
        Udp::new(b, addresses[0], 1472).unwrap(),
    ]
}
pub fn tcp_pair() -> [Stream<TcpStream>; 2] {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let a = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
    let (b, _) = listener.accept().unwrap();
    [
        Stream::tcp(a, 1472, 65536).unwrap(),
        Stream::tcp(b, 1472, 65536).unwrap(),
    ]
}
pub fn run<I: PacketIo>(lanes: [I; 2]) -> [Store; 2] {
    let mut stores = [Store::new(0), Store::new(1)];
    let [a, b] = lanes;
    let mut hosts = [host(0, &stores[0], a), host(1, &stores[1], b)];
    let sizes = [0, 1024, 1025, 17425];
    for i in 0..2 {
        for (seq, size) in sizes.iter().enumerate() {
            let batch = hosts[i]
                .step(Event::Enqueue {
                    flow: 7,
                    sequence: seq as u64,
                    bytes: payload(*size),
                })
                .unwrap();
            commit(&mut hosts[i], &mut stores[i], batch);
        }
    }
    let start = Instant::now();
    loop {
        let now = start.elapsed().as_millis() as u64;
        assert!(now < 10000, "loopback flow deadline");
        for i in 0..2 {
            let batch = hosts[i].poll(now).unwrap();
            commit(&mut hosts[i], &mut stores[i], batch);
        }
        if stores
            .iter()
            .all(|s| s.checkpoint().flow(7).unwrap().settled.len() == 4)
        {
            break;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    for store in &stores {
        assert_eq!(store.effects.len(), 4);
        assert_eq!(
            store.order.iter().map(|id| id.sequence).collect::<Vec<_>>(),
            vec![0, 1, 2, 3]
        );
        for (id, bytes) in &store.effects {
            assert_eq!(*bytes, payload(sizes[id.sequence as usize]));
        }
        assert!(store.checkpoint().flow(7).unwrap().outbox.is_empty());
    }
    stores
}
