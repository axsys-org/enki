mod support;
use mesa_core::{Attachment, PeerId};
use mesa_flow::{Action, Event};
use mesa_transport::{
    relay::{Config, DropReason, Forward, Leg, Relay},
    Batch, Capabilities, Framing, Host, PacketIo, Received, Stream,
};
use mesa_wire::{Auth, Body, Data, DomainTag, Fragment, Name, Packet};
use std::{
    cell::RefCell,
    collections::{BTreeMap, VecDeque},
    io::{self, Read, Write},
    rc::Rc,
};
use support::{fixture::*, *};
fn relay(cap: usize, ttl: u64) -> Relay {
    Relay::new(Config {
        peers: [PeerId(1), PeerId(2)],
        max_packet: [1472; 2],
        wire: mesa_core::Config::default().wire,
        max_interests: cap,
        interest_ttl: ttl,
    })
    .unwrap()
}
fn name(path: &[u8]) -> Name<'_> {
    Name {
        publisher: PeerId(2),
        domain: DomainTag::new("gw-btc").unwrap(),
        rift: 0,
        bloq: 13,
        fragment: Fragment::Auth(0),
        path,
    }
}
fn encode(body: Body<'_>) -> Vec<u8> {
    mesa_wire::encode(&Packet { hop: 0, body }, mesa_core::Config::default().wire).unwrap()
}
fn page(n: Name<'_>) -> Vec<u8> {
    encode(Body::Page {
        name: n,
        data: Data {
            total_bytes: 1,
            auth: Auth::None,
            fragment: &[1],
        },
        next: None,
    })
}
#[test]
fn pit_full_name_direction_capacity_expiration_and_disconnect() {
    let mut r = relay(1, 10);
    let n = name(b"one");
    let peek = encode(Body::Peek(n));
    let response = page(n);
    assert_eq!(
        r.receive(Leg::B, &response),
        Forward::Drop(DropReason::Unsolicited)
    );
    assert!(matches!(
        r.receive(Leg::A, &peek),
        Forward::Send { leg: Leg::B, .. }
    ));
    assert_eq!(r.pending(), 1);
    assert_eq!(r.receive(Leg::B, &peek), Forward::Drop(DropReason::Route));
    assert_eq!(
        r.receive(Leg::A, &encode(Body::Peek(name(b"two")))),
        Forward::Drop(DropReason::Full)
    );
    for altered in [
        Name {
            domain: DomainTag::new("other").unwrap(),
            ..n
        },
        Name { rift: 1, ..n },
        Name {
            fragment: Fragment::Data(0),
            ..n
        },
        Name { bloq: 12, ..n },
        Name { path: b"two", ..n },
    ] {
        assert_eq!(
            r.receive(Leg::B, &page(altered)),
            Forward::Drop(DropReason::Unsolicited)
        );
        assert_eq!(r.pending(), 1);
    }
    r.advance_to(9).unwrap();
    r.receive(Leg::A, &peek);
    assert_eq!(r.next_timer(), Some(10));
    r.advance_to(10).unwrap();
    assert_eq!(r.pending(), 0);
    assert_eq!(
        r.receive(Leg::B, &response),
        Forward::Drop(DropReason::Unsolicited)
    );
    assert!(r.advance_to(9).is_err());
    r.receive(Leg::A, &peek);
    assert_eq!(
        r.receive(Leg::B, &response),
        Forward::Send {
            leg: Leg::A,
            bytes: response.clone()
        }
    );
    assert_eq!(r.pending(), 0);
    r.receive(Leg::A, &peek);
    r.disconnect(Leg::A);
    assert_eq!(r.pending(), 0);
    assert_eq!(
        r.receive(Leg::A, &vec![0; 1473]),
        Forward::Drop(DropReason::Oversized)
    );
    assert_eq!(
        r.receive(Leg::A, &[0; 8]),
        Forward::Drop(DropReason::Malformed)
    );
    let mut small = Relay::new(Config {
        peers: [PeerId(1), PeerId(2)],
        max_packet: [1472, 32],
        wire: mesa_core::Config::default().wire,
        max_interests: 1,
        interest_ttl: 10,
    })
    .unwrap();
    assert_eq!(
        small.receive(Leg::A, &peek),
        Forward::Drop(DropReason::Oversized)
    );
    assert_eq!(small.pending(), 0);
    r.advance_to(u64::MAX).unwrap();
    assert_eq!(
        r.receive(Leg::A, &peek),
        Forward::Drop(DropReason::ClockOverflow)
    );
    assert_eq!(r.pending(), 0);
}
#[derive(Default)]
struct Bytes {
    q: [VecDeque<u8>; 2],
    closed: bool,
}
#[derive(Clone)]
struct ByteIo {
    shared: Rc<RefCell<Bytes>>,
    side: usize,
}
impl Read for ByteIo {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        let mut s = self.shared.borrow_mut();
        if s.closed {
            return Ok(0);
        }
        let n = out.len().min(127).min(s.q[self.side].len());
        if n == 0 {
            return Err(io::ErrorKind::WouldBlock.into());
        }
        for byte in &mut out[..n] {
            *byte = s.q[self.side].pop_front().unwrap();
        }
        Ok(n)
    }
}
impl Write for ByteIo {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let mut s = self.shared.borrow_mut();
        if s.closed {
            return Err(io::ErrorKind::BrokenPipe.into());
        }
        let q = &mut s.q[1 - self.side];
        let n = bytes.len().min(113).min(8192 - q.len());
        if n == 0 {
            return Err(io::ErrorKind::WouldBlock.into());
        }
        q.extend(&bytes[..n]);
        Ok(n)
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
fn stream_pair() -> (Stream<ByteIo>, Stream<ByteIo>, Rc<RefCell<Bytes>>) {
    let shared = Rc::new(RefCell::new(Bytes::default()));
    let a = Stream::new(
        ByteIo {
            shared: shared.clone(),
            side: 0,
        },
        1472,
        65536,
    )
    .unwrap();
    let b = Stream::new(
        ByteIo {
            shared: shared.clone(),
            side: 1,
        },
        1472,
        65536,
    )
    .unwrap();
    (a, b, shared)
}
struct Datagrams {
    q: [VecDeque<Vec<u8>>; 2],
    seed: u64,
    counter: usize,
    dropped: usize,
    duplicate: usize,
    corrupt: usize,
    first_page_dropped: bool,
    trace: Vec<(usize, Vec<u8>)>,
}
#[derive(Clone)]
struct Datagram {
    shared: Rc<RefCell<Datagrams>>,
    side: usize,
}
impl PacketIo for Datagram {
    fn capabilities(&self) -> Capabilities {
        Capabilities {
            max_packet: 1472,
            reliable: false,
            ordered: false,
            framing: Framing::Datagram,
        }
    }
    fn send(&mut self, packet: &[u8]) -> io::Result<bool> {
        let mut s = self.shared.borrow_mut();
        assert!(packet.len() <= 1472);
        s.counter += 1;
        s.seed ^= s.seed << 13;
        s.seed ^= s.seed >> 7;
        s.seed ^= s.seed << 17;
        s.trace.push((self.side, packet.to_vec()));
        let parsed = mesa_wire::decode(packet, mesa_core::Config::default().wire).unwrap();
        if self.side == 0 && matches!(parsed.body, Body::Page { .. }) && !s.first_page_dropped {
            s.first_page_dropped = true;
            s.dropped += 1;
            return Ok(true);
        }
        if s.seed.is_multiple_of(7) {
            s.dropped += 1;
            return Ok(true);
        }
        let mut bytes = packet.to_vec();
        if self.side == 0 && s.corrupt == 0 {
            if let Body::Page { name, data, next } = parsed.body {
                if let Auth::Signature(signature) = data.auth {
                    let mut signature = signature.to_vec();
                    signature[0] ^= 1;
                    bytes = encode(Body::Page {
                        name,
                        data: Data {
                            auth: Auth::Signature(&signature),
                            ..data
                        },
                        next,
                    });
                    s.corrupt += 1;
                }
            }
        }
        let duplicate = s.seed.is_multiple_of(11);
        if duplicate {
            s.duplicate += 1;
        }
        let q = &mut s.q[1 - self.side];
        if q.len() >= 126 {
            return Ok(false);
        }
        q.push_back(bytes.clone());
        if duplicate {
            q.push_front(bytes);
        }
        Ok(true)
    }
    fn receive(&mut self) -> io::Result<Received> {
        let mut s = self.shared.borrow_mut();
        let reverse = s.counter.is_multiple_of(3);
        let p = if reverse {
            s.q[self.side].pop_back()
        } else {
            s.q[self.side].pop_front()
        };
        Ok(p.map_or(Received::Idle, Received::Packet))
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
enum Lane {
    Stream(Stream<ByteIo>),
    Datagram(Datagram),
}
impl PacketIo for Lane {
    fn capabilities(&self) -> Capabilities {
        match self {
            Self::Stream(s) => s.capabilities(),
            Self::Datagram(d) => d.capabilities(),
        }
    }
    fn send(&mut self, p: &[u8]) -> io::Result<bool> {
        match self {
            Self::Stream(s) => s.send(p),
            Self::Datagram(d) => d.send(p),
        }
    }
    fn receive(&mut self) -> io::Result<Received> {
        match self {
            Self::Stream(s) => s.receive(),
            Self::Datagram(d) => d.receive(),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        match self {
            Self::Stream(s) => s.flush(),
            Self::Datagram(d) => d.flush(),
        }
    }
}
fn relay_run(seed: u64) -> Vec<(usize, Vec<u8>)> {
    let (a, mut relay_stream, mut bytes) = stream_pair();
    let datagrams = Rc::new(RefCell::new(Datagrams {
        q: Default::default(),
        seed,
        counter: 0,
        dropped: 0,
        duplicate: 0,
        corrupt: 0,
        first_page_dropped: false,
        trace: vec![],
    }));
    let mut relay_udp = Datagram {
        shared: datagrams.clone(),
        side: 0,
    };
    let peer_udp = Datagram {
        shared: datagrams.clone(),
        side: 1,
    };
    let mut stores = [Store::new(0), Store::new(1)];
    let mut hosts = [
        host(0, &stores[0], Lane::Stream(a)),
        host(1, &stores[1], Lane::Datagram(peer_udp.clone())),
    ];
    let mut r = relay(32, 80);
    for i in 0..2 {
        for seq in 0..4 {
            let batch = hosts[i]
                .step(Event::Enqueue {
                    flow: 7,
                    sequence: seq,
                    bytes: payload(1025 + seq as usize * 4096),
                })
                .unwrap();
            commit(&mut hosts[i], &mut stores[i], batch);
        }
    }
    let mut peeks = BTreeMap::<Vec<u8>, usize>::new();
    let mut verified_drops = 0;
    let mut crash_after_commit = false;
    for now in 0..5000 {
        r.advance_to(now).unwrap();
        if now == 25 {
            // Lose a partial stream and every pending relay interest.
            bytes.borrow_mut().closed = true;
            let batch = hosts[0].disconnect().unwrap();
            commit(&mut hosts[0], &mut stores[0], batch);
            r.disconnect(Leg::A);
        }
        if now == 60 {
            let (a, b, new_bytes) = stream_pair();
            relay_stream = b;
            bytes = new_bytes;
            let batch = hosts[0].reconnect(Lane::Stream(a)).unwrap();
            commit(&mut hosts[0], &mut stores[0], batch);
        }
        for i in 0..2 {
            let mut batch = hosts[i].poll(now).unwrap();
            verified_drops += batch
                .actions
                .iter()
                .filter(|a| matches!(a, Action::Dropped(mesa_core::DropReason::Protection)))
                .count();
            // Crash B after the storage transaction, before its completion
            // callback. Restore from the latest checkpoint, keeping queued packets.
            if i == 1 && !crash_after_commit {
                let mut rest = vec![];
                let mut restart = false;
                for action in batch.actions {
                    if let Action::Deliver { id, .. } = action {
                        let decision = hosts[i]
                            .step(Event::Decide {
                                id,
                                outcome: mesa_flow::Outcome::Accepted,
                            })
                            .unwrap();
                        for action in decision.actions {
                            if let Action::Commit(request) = action {
                                stores[i].commit(&request).unwrap();
                                restart = true;
                            }
                        }
                    } else {
                        rest.push(action);
                    }
                }
                batch.actions = rest;
                if restart {
                    stores[i].incarnation += 2;
                    let (new_host, init) = Host::new(
                        node(i, &stores[i]),
                        view(0).id,
                        Attachment(7),
                        Lane::Datagram(peer_udp.clone()),
                    )
                    .unwrap();
                    hosts[i] = new_host;
                    commit(&mut hosts[i], &mut stores[i], init);
                    // Old process actions are intentionally lost at this crash.
                    batch = Batch::default();
                    crash_after_commit = true;
                }
            }
            commit(&mut hosts[i], &mut stores[i], batch);
        }
        if !(25..60).contains(&now) {
            relay_stream.flush().unwrap();
        }
        for leg in [Leg::A, Leg::B] {
            if leg == Leg::A && (25..60).contains(&now) {
                continue;
            }
            for _ in 0..64 {
                let incoming = if leg == Leg::A {
                    relay_stream.receive()
                } else {
                    relay_udp.receive()
                }
                .unwrap();
                let Received::Packet(packet) = incoming else {
                    break;
                };
                if matches!(
                    mesa_wire::decode(&packet, mesa_core::Config::default().wire)
                        .unwrap()
                        .body,
                    Body::Peek(_)
                ) {
                    *peeks.entry(packet.clone()).or_default() += 1;
                }
                if let Forward::Send { leg, bytes } = r.receive(leg, &packet) {
                    if leg == Leg::A && !(25..60).contains(&now) {
                        assert!(relay_stream.send(&bytes).unwrap());
                    }
                    if leg == Leg::B {
                        assert!(relay_udp.send(&bytes).unwrap());
                    }
                }
                assert!(r.pending() <= 32);
            }
        }
        if now > 60
            && stores
                .iter()
                .all(|s| s.checkpoint().flow(7).unwrap().settled.len() == 4)
        {
            break;
        }
    }
    assert!(crash_after_commit);
    for store in &stores {
        assert_eq!(store.effects.len(), 4);
        assert_eq!(
            store.order.iter().map(|id| id.sequence).collect::<Vec<_>>(),
            vec![0, 1, 2, 3]
        );
        for (id, value) in &store.effects {
            assert_eq!(*value, payload(1025 + id.sequence as usize * 4096));
        }
        assert_eq!(store.checkpoint().flow(7).unwrap().settled.len(), 4);
        assert!(store.checkpoint().flow(7).unwrap().outbox.is_empty());
    }
    assert!(
        peeks.values().any(|count| *count > 1),
        "endpoint interests retried across reliable stream"
    );
    assert!(
        verified_drops > 0,
        "seed {seed}: checksum-valid corruption rejected by endpoint"
    );
    r.advance_to(6000).unwrap();
    assert_eq!(r.pending(), 0);
    let d = datagrams.borrow();
    assert!(d.dropped > 0 && d.duplicate > 0 && d.corrupt == 1 && d.first_page_dropped);
    d.trace.clone()
}
#[test]
fn reliable_stream_to_lossy_datagrams_retries_disconnects_and_recovers_committed_delivery() {
    for seed in 1..=4 {
        assert_eq!(relay_run(seed), relay_run(seed), "seed {seed}");
    }
}
