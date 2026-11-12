//! Deterministic two-leg forwarding with bounded, expiring pending interests.
//! This is a configured path, not a routing or authenticated relay service.
use mesa_core::Tick;
use mesa_wire::{Body, Limits, Name, Packet, PeerId};
use std::collections::BTreeMap;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Leg {
    A,
    B,
}
impl Leg {
    fn index(self) -> usize {
        match self {
            Self::A => 0,
            Self::B => 1,
        }
    }
    fn other(self) -> Self {
        match self {
            Self::A => Self::B,
            Self::B => Self::A,
        }
    }
}
#[derive(Clone, Debug)]
pub struct Config {
    /// Publishers reachable through A and B, respectively. No discovery.
    pub peers: [PeerId; 2],
    pub max_packet: [usize; 2],
    pub wire: Limits,
    pub max_interests: usize,
    pub interest_ttl: Tick,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DropReason {
    Malformed,
    Route,
    Oversized,
    Full,
    Unsolicited,
    ClockOverflow,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Forward {
    Send { leg: Leg, bytes: Vec<u8> },
    Drop(DropReason),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    InvalidConfig,
    TimeRegression,
}
pub struct Relay {
    config: Config,
    now: Tick,
    // Canonical Peek bytes key the entire name, including domain and fragment.
    pit: BTreeMap<Vec<u8>, (Leg, Tick)>,
}
impl Relay {
    pub fn new(config: Config) -> Result<Self, Error> {
        if config.peers[0] == config.peers[1]
            || config.max_interests == 0
            || config.max_interests > 65536
            || config.interest_ttl == 0
            || config.max_packet.iter().any(|n| !(8..=65535).contains(n))
            || !(8..=65535).contains(&config.wire.max_packet)
        {
            return Err(Error::InvalidConfig);
        }
        Ok(Self {
            config,
            now: 0,
            pit: BTreeMap::new(),
        })
    }
    pub fn pending(&self) -> usize {
        self.pit.len()
    }
    pub fn next_timer(&self) -> Option<Tick> {
        self.pit.values().map(|(_, t)| *t).min()
    }
    /// Expire before receive at the same time. Duplicate interests do not extend
    /// the original lifetime; a later endpoint retry can allocate a fresh entry.
    pub fn advance_to(&mut self, now: Tick) -> Result<(), Error> {
        if now < self.now {
            return Err(Error::TimeRegression);
        }
        self.now = now;
        self.pit.retain(|_, (_, expiry)| *expiry > now);
        Ok(())
    }
    /// A lost connection discards its pending forwarding state. Endpoints repair
    /// any lost interests/pages through their unchanged retry policy.
    pub fn disconnect(&mut self, _leg: Leg) {
        self.pit.clear();
    }
    pub fn receive(&mut self, from: Leg, bytes: &[u8]) -> Forward {
        let to = from.other();
        if bytes.len() > self.config.max_packet[from.index()]
            || bytes.len() > self.config.max_packet[to.index()]
        {
            return Forward::Drop(DropReason::Oversized);
        }
        let packet = match mesa_wire::decode(bytes, self.config.wire) {
            Ok(p) => p,
            Err(_) => return Forward::Drop(DropReason::Malformed),
        };
        match packet.body {
            Body::Peek(name) => {
                if name.publisher != self.config.peers[to.index()] {
                    return Forward::Drop(DropReason::Route);
                }
                let key = key(name, self.config.wire);
                if !self.pit.contains_key(&key) {
                    if self.pit.len() == self.config.max_interests {
                        return Forward::Drop(DropReason::Full);
                    }
                    let Some(expiry) = self.now.checked_add(self.config.interest_ttl) else {
                        return Forward::Drop(DropReason::ClockOverflow);
                    };
                    self.pit.insert(key, (from, expiry));
                }
            }
            Body::Page { name, .. } => {
                if name.publisher != self.config.peers[from.index()] {
                    return Forward::Drop(DropReason::Route);
                }
                let key = key(name, self.config.wire);
                if !matches!(self.pit.get(&key), Some((requester, _)) if *requester == to) {
                    return Forward::Drop(DropReason::Unsolicited);
                }
                self.pit.remove(&key);
            }
            Body::Poke { ack, payload, data } => {
                // Flow wakeups carry no data or authority. Both named endpoints
                // must be configured on this path; endpoint code checks the rest.
                if ack.publisher == payload.publisher
                    || !self.config.peers.contains(&ack.publisher)
                    || !self.config.peers.contains(&payload.publisher)
                    || data.total_bytes != 0
                    || !data.fragment.is_empty()
                    || !matches!(data.auth, mesa_wire::Auth::None)
                {
                    return Forward::Drop(DropReason::Route);
                }
            }
        }
        Forward::Send {
            leg: to,
            bytes: bytes.to_vec(),
        }
    }
}
fn key(name: Name<'_>, limits: Limits) -> Vec<u8> {
    mesa_wire::encode(
        &Packet {
            hop: 0,
            body: Body::Peek(name),
        },
        limits,
    )
    .expect("decoded canonical name")
}
