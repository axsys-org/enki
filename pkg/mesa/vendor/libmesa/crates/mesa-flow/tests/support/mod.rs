//! Fake atomic storage and real signed two-peer network. Public test keys only.
#![allow(dead_code)]
use mesa_core::{
    policy::{CongestionPolicy, RetryTimer},
    protection::SignedPublic,
    Attachment, KeyHandle, PeerId, PreconfiguredPeer,
};
use mesa_crypto::KeyPair;
use mesa_flow::*;
use std::collections::{BTreeMap, VecDeque};

pub fn view(i: usize) -> PreconfiguredPeer {
    PreconfiguredPeer::new(
        PeerId(i as u128 + 1),
        "gw-btc",
        1,
        0,
        KeyHandle(i as u64 + 1),
    )
    .unwrap()
}
pub fn flows(i: usize) -> Vec<FlowConfig> {
    vec![FlowConfig {
        id: 7,
        peer: view(1 - i),
        attachment: Some(Attachment(7)),
    }]
}
pub fn protection(i: usize) -> SignedPublic {
    SignedPublic::new(
        view(i).key,
        KeyPair::from_seed(&[i as u8 + 1; 32]),
        BTreeMap::from([
            (
                view(1 - i).key,
                KeyPair::from_seed(&[(1 - i) as u8 + 1; 32]).public_key(),
            ),
            (KeyHandle(9), KeyPair::from_seed(&[9; 32]).public_key()),
        ]),
    )
}
pub fn config() -> Config {
    let mut c = Config::default();
    c.transfer.timers.read_timeout = 500;
    c.transfer.timers.retry = RetryTimer::Exponential {
        initial: 5,
        maximum: 40,
    };
    c.transfer.recovery.max_attempts = 20;
    c.transfer.congestion = CongestionPolicy::Aimd {
        initial: 2,
        maximum: 8,
    };
    c
}
pub fn payload(n: usize) -> Vec<u8> {
    (0..n).map(|i| (i % 251) as u8).collect()
}

/// Replacing this entire value models one atomic host transaction. Neither
/// application effects nor receipt/sequence state can persist on their own.
#[derive(Clone)]
pub struct Store {
    pub incarnation: u128,
    pub disk: Vec<u8>,
    pub records: BTreeMap<MessageId, (Vec<u8>, Outcome)>,
    pub effects: BTreeMap<MessageId, Vec<u8>>,
    pub order: Vec<MessageId>,
}
impl Store {
    pub fn new(i: usize) -> Self {
        Self {
            incarnation: i as u128 + 1,
            disk: serde_json::to_vec(&Checkpoint::empty(&view(i), &flows(i))).unwrap(),
            records: BTreeMap::new(),
            effects: BTreeMap::new(),
            order: vec![],
        }
    }
    pub fn checkpoint(&self) -> Checkpoint {
        serde_json::from_slice(&self.disk).unwrap()
    }
    pub fn commit(&mut self, r: &CommitRequest) -> Result<(), &'static str> {
        if r.token.incarnation != self.incarnation {
            return Err("fenced writer");
        }
        if self.checkpoint().revision != r.base_revision {
            return Err("revision conflict");
        }
        assert_eq!(r.checkpoint.revision, r.base_revision + 1);
        let mut next = self.clone();
        next.disk = serde_json::to_vec(&r.checkpoint).unwrap();
        if let Change::Deliver { id, bytes, outcome } = &r.change {
            assert!(
                next.records
                    .insert(id.clone(), (bytes.clone(), *outcome))
                    .is_none(),
                "duplicate application effect"
            );
            if *outcome == Outcome::Accepted {
                assert!(
                    next.effects.insert(id.clone(), bytes.clone()).is_none(),
                    "duplicate application effect"
                );
            }
            next.order.push(id.clone());
        }
        *self = next;
        Ok(())
    }
}
#[derive(Clone, Debug)]
pub struct Frame {
    pub from: usize,
    pub bytes: Vec<u8>,
}
pub struct Harness {
    pub nodes: [Node; 2],
    pub stores: [Store; 2],
    pub boot: [u128; 2],
    pub config: Config,
    pub queue: VecDeque<Frame>,
    pub commits: VecDeque<(usize, CommitRequest)>,
    pub deliveries: VecDeque<(usize, MessageId, Vec<u8>)>,
    pub trace: Vec<(usize, Vec<Action>)>,
    pub auto: bool,
    pub now: u64,
}
impl Harness {
    pub fn new(c: Config) -> Self {
        let mut stores = [Store::new(0), Store::new(1)];
        for (i, store) in stores.iter_mut().enumerate() {
            store.disk = serde_json::to_vec(&Checkpoint::empty_with_batch(
                &view(i),
                &flows(i),
                c.max_messages,
            ))
            .unwrap();
        }
        let nodes = std::array::from_fn(|i| {
            Node::new(
                c.clone(),
                view(i),
                protection(i),
                flows(i),
                stores[i].checkpoint(),
                i as u128 + 1,
            )
            .unwrap()
        });
        Self {
            nodes,
            stores,
            boot: [1, 2],
            config: c,
            queue: VecDeque::new(),
            commits: VecDeque::new(),
            deliveries: VecDeque::new(),
            trace: vec![],
            auto: true,
            now: 0,
        }
    }
    pub fn record(&mut self, i: usize, actions: Vec<Action>) {
        for action in &actions {
            match action {
                Action::Send { bytes, .. } => self.queue.push_back(Frame {
                    from: i,
                    bytes: bytes.clone(),
                }),
                Action::Commit(r) => self.commits.push_back((i, r.clone())),
                Action::Deliver { id, bytes } => {
                    self.deliveries.push_back((i, id.clone(), bytes.clone()))
                }
                _ => {}
            }
        }
        self.trace.push((i, actions));
    }
    pub fn step(&mut self, i: usize, event: Event<'_>) {
        let actions = self.nodes[i].step(event).unwrap();
        self.record(i, actions);
        if self.auto {
            self.flush();
        }
    }
    pub fn flush(&mut self) {
        for _ in 0..10000 {
            if let Some((i, request)) = self.commits.pop_front() {
                self.stores[i].commit(&request).unwrap();
                let actions = self.nodes[i].step(Event::Committed(request.token)).unwrap();
                self.record(i, actions);
            } else if let Some((i, id, _)) = self.deliveries.pop_front() {
                let actions = self.nodes[i]
                    .step(Event::Decide {
                        id,
                        outcome: Outcome::Accepted,
                    })
                    .unwrap();
                self.record(i, actions);
            } else {
                return;
            }
        }
        panic!("storage loop");
    }
    pub fn send(&mut self, i: usize, sequence: u64, bytes: Vec<u8>) {
        self.step(
            i,
            Event::Enqueue {
                flow: 7,
                sequence,
                bytes,
            },
        );
    }
    pub fn deliver(&mut self, f: Frame) {
        self.step(
            1 - f.from,
            Event::Receive {
                claimed_peer: view(f.from).id,
                attachment: Attachment(7),
                bytes: &f.bytes,
            },
        );
    }
    pub fn tick(&mut self, now: u64) {
        self.now = now;
        for i in 0..2 {
            self.step(i, Event::AdvanceTo(now));
        }
    }
    pub fn pump(&mut self) {
        for _ in 0..100000 {
            let Some(f) = self.queue.pop_front() else {
                return;
            };
            self.deliver(f);
        }
        panic!("network loop");
    }
    pub fn run(&mut self, duration: u64) {
        let end = self.now + duration;
        for now in self.now..=end {
            self.tick(now);
            self.pump();
        }
    }
    pub fn restart(&mut self, i: usize) {
        self.boot[i] += 2;
        self.stores[i].incarnation = self.boot[i];
        self.nodes[i] = Node::new(
            self.config.clone(),
            view(i),
            protection(i),
            flows(i),
            self.stores[i].checkpoint(),
            self.boot[i],
        )
        .unwrap();
        self.commits.retain(|(who, _)| *who != i);
        self.deliveries.retain(|(who, _, _)| *who != i);
        self.step(i, Event::AdvanceTo(self.now));
    }
    pub fn settled(&self, i: usize, count: usize) {
        let s = self.stores[i].checkpoint();
        let f = s.flow(7).unwrap();
        assert_eq!(f.settled.len(), count);
        assert!(f.outbox.is_empty());
    }
}
