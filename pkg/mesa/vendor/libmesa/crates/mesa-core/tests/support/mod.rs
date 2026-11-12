//! Test-only authority/protection and virtual link. Never exported by mesa-core.
#![allow(dead_code)]

use std::{cell::RefCell, collections::VecDeque, rc::Rc};

use mesa_core::{
    Action, Attachment, Config, Engine, Error, Event, KeyHandle, PeerId, PreconfiguredPeer,
    Protection,
};

pub const A: PeerId = PeerId(1);
pub const B: PeerId = PeerId(2);
pub const LINK: Attachment = Attachment(7);

pub fn fixture(id: PeerId) -> PreconfiguredPeer {
    PreconfiguredPeer::new(id, "gw-btc", 1, 0, KeyHandle(id.0 as u64 + 100)).unwrap()
}

struct Ticket {
    local: PreconfiguredPeer,
    remote: PreconfiguredPeer,
    bytes: Vec<u8>,
}

/// One-shot, out-of-band test verdict, bound to exact bytes and current keys.
/// A wire mutation cannot request success. Harness tests may deliberately grant
/// malformed bytes a ticket to exercise semantic validation after protection.
#[derive(Clone, Default)]
pub struct FixtureProtection(Rc<RefCell<Option<Ticket>>>);

impl Protection for FixtureProtection {
    fn verify(
        &mut self,
        local: &PreconfiguredPeer,
        peer: &PreconfiguredPeer,
        packet: &[u8],
    ) -> bool {
        self.0
            .borrow_mut()
            .take()
            .is_some_and(|t| t.local == *local && t.remote == *peer && t.bytes == packet)
    }
}

#[derive(Clone, Debug)]
pub struct Envelope {
    pub from: PeerId,
    pub to: PeerId,
    pub attachment: Attachment,
    pub bytes: Vec<u8>,
    pub sender: PreconfiguredPeer,
}

pub struct Harness {
    pub a: Engine<FixtureProtection>,
    pub b: Engine<FixtureProtection>,
    a_verdict: FixtureProtection,
    b_verdict: FixtureProtection,
    pub queue: VecDeque<Envelope>,
    pub trace: Vec<(PeerId, Vec<Action>)>,
}

impl Harness {
    pub fn new(config: Config) -> Self {
        Self::with_configs(config.clone(), config)
    }

    pub fn with_configs(a_config: Config, b_config: Config) -> Self {
        let a_verdict = FixtureProtection::default();
        let b_verdict = FixtureProtection::default();
        let mut h = Self {
            a: Engine::new(a_config, fixture(A), a_verdict.clone()).unwrap(),
            b: Engine::new(b_config, fixture(B), b_verdict.clone()).unwrap(),
            a_verdict,
            b_verdict,
            queue: VecDeque::new(),
            trace: Vec::new(),
        };
        h.step(A, Event::InstallPeer(fixture(B))).unwrap();
        h.step(B, Event::InstallPeer(fixture(A))).unwrap();
        h.step(
            A,
            Event::SetAttachment {
                peer: B,
                attachment: Some(LINK),
            },
        )
        .unwrap();
        h.step(
            B,
            Event::SetAttachment {
                peer: A,
                attachment: Some(LINK),
            },
        )
        .unwrap();
        h
    }

    pub fn step(&mut self, who: PeerId, event: Event<'_>) -> Result<Vec<Action>, Error> {
        let actions = if who == A {
            self.a.step(event)?
        } else {
            assert_eq!(who, B);
            self.b.step(event)?
        };
        for action in &actions {
            if let Action::Send {
                peer,
                attachment,
                bytes,
            } = action
            {
                self.queue.push_back(Envelope {
                    from: who,
                    to: *peer,
                    attachment: *attachment,
                    bytes: bytes.clone(),
                    sender: fixture(who),
                });
            }
        }
        self.trace.push((who, actions.clone()));
        Ok(actions)
    }

    pub fn deliver(&mut self, envelope: Envelope, verified: bool) -> Vec<Action> {
        let verdict = if envelope.to == A {
            &self.a_verdict
        } else {
            &self.b_verdict
        };
        *verdict.0.borrow_mut() = verified.then(|| Ticket {
            local: fixture(envelope.to),
            remote: envelope.sender,
            bytes: envelope.bytes.clone(),
        });
        self.step(
            envelope.to,
            Event::Receive {
                claimed_peer: envelope.from,
                attachment: envelope.attachment,
                bytes: &envelope.bytes,
            },
        )
        .unwrap()
    }

    pub fn pump(&mut self) {
        let mut budget = 10_000;
        while let Some(envelope) = self.queue.pop_front() {
            assert!(budget > 0, "harness delivery budget exhausted");
            budget -= 1;
            self.deliver(envelope, true);
        }
    }

    pub fn advance(&mut self, now: u64) {
        self.step(A, Event::AdvanceTo(now)).unwrap();
        self.step(B, Event::AdvanceTo(now)).unwrap();
    }

    pub fn results(&self, who: PeerId) -> Vec<Action> {
        self.trace
            .iter()
            .filter(|(p, _)| *p == who)
            .flat_map(|(_, actions)| actions)
            .filter(|a| matches!(a, Action::ReadFinished { .. }))
            .cloned()
            .collect()
    }
}
