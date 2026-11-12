use mesa_core::{PeerId, PreconfiguredPeer};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

pub type FlowId = u64;
pub type Sequence = u64;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Endpoint {
    pub name: u128,
    pub domain: String,
    pub rift: u32,
}
impl From<&PreconfiguredPeer> for Endpoint {
    fn from(peer: &PreconfiguredPeer) -> Self {
        Self {
            name: peer.id.0,
            domain: peer.domain().as_str().into(),
            rift: peer.rift,
        }
    }
}

/// Stable across process restarts and same-rift key updates. Both endpoint
/// epochs distinguish old deliveries from a new incarnation of either peer.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub struct MessageId {
    pub sender: u128,
    pub sender_rift: u32,
    pub receiver: u128,
    pub receiver_rift: u32,
    pub flow: FlowId,
    pub sequence: Sequence,
}
impl MessageId {
    pub fn path(&self, kind: ReadKind) -> Vec<u8> {
        let role = match kind {
            ReadKind::Message => "msg",
            ReadKind::Receipt => "receipt",
            ReadKind::Release => "release",
            ReadKind::Released => "released",
        };
        format!(
            "flow/2/{:032x}/{:08x}/{:032x}/{:08x}/{:016x}/{role}/{:016x}",
            self.sender,
            self.sender_rift,
            self.receiver,
            self.receiver_rift,
            self.flow,
            self.sequence
        )
        .into_bytes()
    }
}

/// A rejection is a durable, terminal application disposition, not a request
/// to repair missing fragments. Reads supply fragment recovery independently.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Outcome {
    Accepted,
    Rejected(u16),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Receipt {
    pub digest: [u8; 32],
    pub outcome: Outcome,
}
const RECEIPT_LABEL: &[u8] = b"libmesa/flow-receipt/1\0";
pub(crate) const RECEIPT_BYTES: usize = RECEIPT_LABEL.len() + 35;
impl Receipt {
    pub(crate) fn encode(&self) -> Vec<u8> {
        let mut bytes = RECEIPT_LABEL.to_vec();
        bytes.extend_from_slice(&self.digest);
        let (tag, code) = match self.outcome {
            Outcome::Accepted => (0, 0u16),
            Outcome::Rejected(code) => (1, code),
        };
        bytes.push(tag);
        bytes.extend_from_slice(&code.to_le_bytes());
        bytes
    }
    pub(crate) fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() != RECEIPT_BYTES || !bytes.starts_with(RECEIPT_LABEL) {
            return None;
        }
        let start = RECEIPT_LABEL.len();
        let digest = bytes[start..start + 32].try_into().ok()?;
        let code = u16::from_le_bytes(bytes[start + 33..].try_into().ok()?);
        let outcome = match (bytes[start + 32], code) {
            (0, 0) => Outcome::Accepted,
            (1, code) => Outcome::Rejected(code),
            _ => return None,
        };
        Some(Self { digest, outcome })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum ReadKind {
    Message,
    Receipt,
    Release,
    Released,
}

/// Host-owned durable state. Store a proposed checkpoint atomically with its
/// Change, using base_revision as a compare-and-swap guard. Loading an older
/// checkpoint is rollback, not crash recovery, and is outside this contract.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Checkpoint {
    pub version: u32,
    pub revision: u64,
    /// Pinned reclamation batch size, shared by both endpoints.
    pub batch_size: u64,
    pub local: Endpoint,
    pub flows: Vec<FlowState>,
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct FlowState {
    pub flow: FlowId,
    pub remote: Endpoint,
    pub next_send: Sequence,
    pub send_floor: Sequence,
    pub receive_floor: Sequence,
    pub outbox: BTreeMap<Sequence, Vec<u8>>,
    pub settled: BTreeMap<Sequence, Outcome>,
    /// Receipts for [receive_floor, next_receive()). The floor permanently
    /// rejects earlier deliveries even after their receipts are reclaimed.
    pub received: Vec<Receipt>,
}
impl FlowState {
    pub fn next_receive(&self) -> Sequence {
        self.receive_floor
            .saturating_add(self.received.len() as u64)
    }
    pub fn batch_settled(&self, batch: u64) -> bool {
        self.send_floor.checked_add(batch) == Some(self.next_send)
            && self.outbox.is_empty()
            && self.settled.len() as u64 == batch
    }
    /// Structural bounds independent of a host's payload quotas.
    pub fn valid(&self, batch: u64) -> bool {
        batch > 0
            && batch <= 1024
            && self.send_floor.is_multiple_of(batch)
            && self.receive_floor.is_multiple_of(batch)
            && self
                .next_send
                .checked_sub(self.send_floor)
                .is_some_and(|n| n <= batch && n == (self.outbox.len() + self.settled.len()) as u64)
            && self.received.len() as u64 <= batch
            && self
                .receive_floor
                .checked_add(self.received.len() as u64)
                .is_some()
            && self.outbox.keys().all(|s| {
                *s >= self.send_floor && *s < self.next_send && !self.settled.contains_key(s)
            })
            && self
                .settled
                .keys()
                .all(|s| *s >= self.send_floor && *s < self.next_send)
    }
}
impl Checkpoint {
    pub fn expected_revision(&self) -> Option<u64> {
        if !(1..=1024).contains(&self.batch_size) {
            return None;
        }
        self.flows.iter().try_fold(0u64, |sum, f| {
            [
                f.next_send,
                f.send_floor,
                f.settled.len() as u64,
                f.receive_floor,
                f.received.len() as u64,
                f.send_floor / self.batch_size,
                f.receive_floor / self.batch_size,
            ]
            .into_iter()
            .try_fold(sum, u64::checked_add)
        })
    }

    pub fn empty(local: &PreconfiguredPeer, flows: &[crate::FlowConfig]) -> Self {
        Self::empty_with_batch(local, flows, 64)
    }
    pub fn empty_with_batch(
        local: &PreconfiguredPeer,
        flows: &[crate::FlowConfig],
        batch_size: u64,
    ) -> Self {
        let mut states: Vec<_> = flows
            .iter()
            .map(|f| FlowState {
                flow: f.id,
                remote: (&f.peer).into(),
                next_send: 0,
                send_floor: 0,
                receive_floor: 0,
                outbox: BTreeMap::new(),
                settled: BTreeMap::new(),
                received: vec![],
            })
            .collect();
        states.sort_by_key(|s| s.flow);
        Self {
            version: 2,
            batch_size,
            revision: 0,
            local: local.into(),
            flows: states,
        }
    }
    pub fn flow(&self, id: FlowId) -> Option<&FlowState> {
        self.flows.iter().find(|f| f.flow == id)
    }
    pub(crate) fn flow_mut(&mut self, id: FlowId) -> &mut FlowState {
        self.flows
            .iter_mut()
            .find(|f| f.flow == id)
            .expect("validated flow")
    }
    pub(crate) fn message_id(&self, flow: FlowId, sequence: Sequence, incoming: bool) -> MessageId {
        let remote = &self.flow(flow).expect("validated flow").remote;
        let (sender, receiver) = if incoming {
            (remote, &self.local)
        } else {
            (&self.local, remote)
        };
        MessageId {
            sender: sender.name,
            sender_rift: sender.rift,
            receiver: receiver.name,
            receiver_rift: receiver.rift,
            flow,
            sequence,
        }
    }
    pub(crate) fn peer(&self, flow: FlowId) -> PeerId {
        PeerId(self.flow(flow).expect("validated flow").remote.name)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CommitToken {
    /// Unique nonzero host-supplied process incarnation. Never reuse on restart.
    pub incarnation: u128,
    pub revision: u64,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Change {
    /// Authenticated sender watermark permits receipt retirement.
    RetireReceived {
        id: MessageId,
    },
    /// Authenticated receiver acknowledgment permits settlement retirement.
    RetireSent {
        id: MessageId,
    },
    Enqueue {
        id: MessageId,
    },
    /// Apply accepted application effects (or record rejection) in the same
    /// host transaction as the checkpoint and stable delivery identifier.
    Deliver {
        id: MessageId,
        bytes: Vec<u8>,
        outcome: Outcome,
    },
    Settle {
        id: MessageId,
        outcome: Outcome,
    },
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CommitRequest {
    pub token: CommitToken,
    pub base_revision: u64,
    pub checkpoint: Checkpoint,
    pub change: Change,
}
