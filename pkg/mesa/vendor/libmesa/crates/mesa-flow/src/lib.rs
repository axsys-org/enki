//! Bounded durable message flows over the signed-public named-read engine.
//! The host commits checkpoints and application effects; this crate owns no I/O.
//! See spec/13-continuous-flows.md for the transaction and restart contract.
mod hints;
mod retirement;
mod state;
use mesa_core::{
    policy::TransferPolicy, protection::SignedPublic, Access, Action as CoreAction, Attachment,
    Engine, Event as CoreEvent, PeerId, PreconfiguredPeer, ReadFailure, Tick,
};
pub use state::*;
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Debug)]
pub struct FlowConfig {
    pub id: FlowId,
    pub peer: PreconfiguredPeer,
    pub attachment: Option<Attachment>,
}
#[derive(Clone, Debug)]
pub struct Config {
    pub core: mesa_core::Config,
    pub transfer: TransferPolicy,
    pub max_flows: usize,
    /// Retained messages per reclamation batch; must match checkpoint.batch_size.
    pub max_messages: u64,
    /// Per-flow outstanding outbox entries and receive prefetch slots.
    pub window: usize,
    pub max_message_bytes: usize,
    pub max_outbox_bytes: usize,
}
impl Default for Config {
    fn default() -> Self {
        let core = mesa_core::Config {
            max_bindings: 1024,
            max_read_bytes: 65536,
            ..Default::default()
        };
        Self {
            transfer: TransferPolicy::fixed(&core),
            core,
            max_flows: 4,
            max_messages: 64,
            window: 4,
            max_message_bytes: 65536,
            max_outbox_bytes: 1024 * 1024,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    InvalidConfig,
    InvalidCheckpoint,
    EpochMismatch,
    UnknownFlow,
    Busy,
    Backpressure,
    Sequence,
    Limit,
    CommitToken,
    DeliveryId,
    Halted,
    Core(mesa_core::Error),
}
impl From<mesa_core::Error> for Error {
    fn from(e: mesa_core::Error) -> Self {
        Self::Core(e)
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for Error {}

#[derive(Debug)]
pub enum Event<'a> {
    /// Explicit sequence makes a retried enqueue unambiguous after restart.
    Enqueue {
        flow: FlowId,
        sequence: Sequence,
        bytes: Vec<u8>,
    },
    Decide {
        id: MessageId,
        outcome: Outcome,
    },
    Committed(CommitToken),
    Receive {
        claimed_peer: PeerId,
        attachment: Attachment,
        bytes: &'a [u8],
    },
    AdvanceTo(Tick),
    /// Restart failed named reads. Each read has the configured finite budget.
    Resume {
        flow: FlowId,
    },
    SetAttachment {
        peer: PeerId,
        attachment: Option<Attachment>,
    },
    SetService {
        peer: PeerId,
        allowed: bool,
    },
    /// Same-rift current-key update. Epoch changes use InvalidateEpoch.
    UpdatePeer(PreconfiguredPeer),
    /// Halt this node without deleting durable state. The host must resolve any
    /// in-flight storage transaction and reconcile the old epoch before restart.
    InvalidateEpoch {
        peer: PeerId,
        new_rift: u32,
    },
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Failure {
    Read(ReadFailure),
    Start(mesa_core::Error),
    InvalidReceipt,
    InvalidWatermark,
    MessageTooLarge,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Action {
    Send {
        peer: PeerId,
        attachment: Attachment,
        bytes: Vec<u8>,
    },
    Commit(CommitRequest),
    /// Proposal only. Applying an effect here without the commit transaction
    /// forfeits crash-safe deduplication.
    Deliver {
        id: MessageId,
        bytes: Vec<u8>,
    },
    Enqueued {
        id: MessageId,
    },
    CommittedDelivery {
        id: MessageId,
        outcome: Outcome,
    },
    Settled {
        id: MessageId,
        outcome: Outcome,
    },
    Stalled {
        flow: FlowId,
        sequence: Sequence,
        kind: ReadKind,
        failure: Failure,
    },
    Dropped(mesa_core::DropReason),
    AnnouncementStalled {
        id: MessageId,
        kind: ReadKind,
    },
    EpochInvalidated {
        peer: PeerId,
        new_rift: u32,
    },
    SetTimer(Option<Tick>),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
struct Work {
    flow: FlowId,
    sequence: Sequence,
    kind: ReadKind,
}

/// Payload counts exclude allocator overhead and host-owned Action copies.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Usage {
    pub outbox_messages: usize,
    pub outbox_bytes: usize,
    pub buffered_messages: usize,
    pub buffered_message_bytes: usize,
    pub pending_commit_bytes: usize,
    pub retained_receipts: usize,
    pub active_reads: usize,
    pub stalled_reads: usize,
    pub pending_commit: bool,
    pub awaiting_decision: bool,
}

pub struct Node {
    config: Config,
    flows: Vec<FlowConfig>,
    core: Engine<SignedPublic>,
    checkpoint: Checkpoint,
    incarnation: u128,
    pending: Option<CommitRequest>,
    delivery: Option<MessageId>,
    messages: BTreeMap<(FlowId, Sequence), Vec<u8>>,
    receipts: BTreeMap<(FlowId, Sequence), Outcome>,
    jobs: BTreeMap<u64, Work>,
    stalled: BTreeSet<Work>,
    advertised: BTreeSet<Work>,
    hints: BTreeMap<Work, hints::Attempt>,
    controls: BTreeSet<Work>,
    now: Tick,
    next_read: u64,
    timer: Option<Tick>,
    halted: bool,
}
impl Node {
    pub fn new(
        config: Config,
        local: PreconfiguredPeer,
        protection: SignedPublic,
        mut flows: Vec<FlowConfig>,
        checkpoint: Checkpoint,
        incarnation: u128,
    ) -> Result<Self, Error> {
        flows.sort_by_key(|f| f.id);
        Self::validate(&config, &local, &flows, &checkpoint, incarnation)?;
        let core = Engine::with_policy(config.core.clone(), local, protection, config.transfer)?;
        let mut node = Self {
            config,
            flows,
            core,
            checkpoint,
            incarnation,
            pending: None,
            delivery: None,
            messages: BTreeMap::new(),
            receipts: BTreeMap::new(),
            jobs: BTreeMap::new(),
            stalled: BTreeSet::new(),
            advertised: BTreeSet::new(),
            hints: BTreeMap::new(),
            controls: BTreeSet::new(),
            now: 0,
            next_read: 0,
            timer: None,
            halted: false,
        };
        let peers: Vec<_> = node
            .flows
            .iter()
            .map(|f| (f.peer.clone(), f.attachment))
            .collect();
        for (peer, attachment) in peers {
            node.core.step(CoreEvent::InstallPeer(peer.clone()))?;
            node.core.step(CoreEvent::SetAttachment {
                peer: peer.id,
                attachment,
            })?;
        }
        for flow in node.checkpoint.flows.clone() {
            if flow.send_floor > 0 {
                node.retire_names(flow.flow, false, &mut vec![])?;
            }
            if flow.receive_floor > 0 {
                node.retire_names(flow.flow, true, &mut vec![])?;
            }
        }
        // Restore only committed objects. Receipts survive the loss of all
        // soft read state and can repair a sender's lost acknowledgment.
        let mut bindings = vec![];
        for flow in &node.checkpoint.flows {
            for (&seq, bytes) in &flow.outbox {
                bindings.push((
                    node.checkpoint
                        .message_id(flow.flow, seq, false)
                        .path(ReadKind::Message),
                    bytes.clone(),
                ));
            }
            for (seq, receipt) in flow.received.iter().enumerate() {
                bindings.push((
                    node.checkpoint
                        .message_id(flow.flow, flow.receive_floor + seq as u64, true)
                        .path(ReadKind::Receipt),
                    receipt.encode(),
                ));
            }
        }
        for (path, bytes) in bindings {
            node.core.step(CoreEvent::Bind {
                path,
                bytes,
                access: Access::Public,
            })?;
        }
        for flow in node.checkpoint.flows.clone() {
            if flow.batch_settled(node.config.max_messages) {
                node.bind_control(flow.flow, flow.next_send, ReadKind::Release, &mut vec![])?;
            }
            if flow.receive_floor > 0 {
                node.bind_control(
                    flow.flow,
                    flow.receive_floor,
                    ReadKind::Released,
                    &mut vec![],
                )?;
            }
        }
        Ok(node)
    }

    fn validate(
        c: &Config,
        local: &PreconfiguredPeer,
        flows: &[FlowConfig],
        s: &Checkpoint,
        boot: u128,
    ) -> Result<(), Error> {
        // Batch bounds limit retained history independently of sequence numbers.
        if boot == 0
            || c.max_flows == 0
            || c.max_flows > 16
            || flows.is_empty()
            || flows.len() > c.max_flows
            || c.max_messages == 0
            || c.max_messages > 1024
            || c.window == 0
            || c.window as u64 > c.max_messages
            || c.max_message_bytes == 0
            || c.max_message_bytes > c.core.max_read_bytes
            || c.core.max_read_bytes < state::RECEIPT_BYTES
            || c.core.wire.max_path < 133
        {
            return Err(Error::InvalidConfig);
        }
        let objects = flows.len() * c.max_messages as usize;
        let reads = flows.len() * c.window * 2;
        let binding_bytes = c
            .max_outbox_bytes
            .checked_add(
                objects * state::RECEIPT_BYTES + flows.len() * 2 * retirement::MARKER.len(),
            )
            .ok_or(Error::InvalidConfig)?;
        let reassembly = reads
            .checked_mul(c.core.max_read_bytes)
            .ok_or(Error::InvalidConfig)?;
        if c.core.max_bindings < objects * 2 + flows.len() * 6
            || c.core.max_reads < reads
            || c.core.max_binding_bytes < binding_bytes
            || c.core.max_reassembly_bytes < reassembly
        {
            return Err(Error::InvalidConfig);
        }
        let mut peers = BTreeMap::new();
        for (index, flow) in flows.iter().enumerate() {
            if flow.peer.id == local.id || index > 0 && flows[index - 1].id == flow.id {
                return Err(Error::InvalidConfig);
            }
            if let Some((view, route)) = peers.insert(flow.peer.id, (&flow.peer, flow.attachment)) {
                if view != &flow.peer || route != flow.attachment {
                    return Err(Error::InvalidConfig);
                }
            }
        }
        if s.version != 2
            || s.batch_size != c.max_messages
            || s.revision == u64::MAX
            || s.flows.len() != flows.len()
        {
            return Err(Error::InvalidCheckpoint);
        }
        if s.local != Endpoint::from(local) {
            return Err(Error::EpochMismatch);
        }
        let mut total = 0usize;
        for (stored, configured) in s.flows.iter().zip(flows) {
            if stored.flow != configured.id {
                return Err(Error::InvalidCheckpoint);
            }
            if stored.remote != Endpoint::from(&configured.peer) {
                return Err(Error::EpochMismatch);
            }
            if !stored.valid(c.max_messages) || stored.outbox.len() > c.window {
                return Err(Error::InvalidCheckpoint);
            }
            for (&seq, bytes) in &stored.outbox {
                if seq >= stored.next_send
                    || stored.settled.contains_key(&seq)
                    || bytes.len() > c.max_message_bytes
                {
                    return Err(Error::InvalidCheckpoint);
                }
                total = total
                    .checked_add(bytes.len())
                    .ok_or(Error::InvalidCheckpoint)?;
            }
            if stored.settled.keys().any(|seq| *seq >= stored.next_send) {
                return Err(Error::InvalidCheckpoint);
            }
        }
        if total > c.max_outbox_bytes || Some(s.revision) != s.expected_revision() {
            return Err(Error::InvalidCheckpoint);
        }
        Ok(())
    }

    pub fn usage(&self) -> Usage {
        let outbox_bytes = |s: &Checkpoint| {
            s.flows
                .iter()
                .flat_map(|f| f.outbox.values())
                .map(Vec::len)
                .sum::<usize>()
        };
        Usage {
            outbox_messages: self.checkpoint.flows.iter().map(|f| f.outbox.len()).sum(),
            outbox_bytes: outbox_bytes(&self.checkpoint),
            buffered_messages: self.messages.len(),
            buffered_message_bytes: self.messages.values().map(Vec::len).sum(),
            pending_commit_bytes: self.pending.as_ref().map_or(0, |p| {
                outbox_bytes(&p.checkpoint)
                    + match &p.change {
                        Change::Deliver { bytes, .. } => bytes.len(),
                        _ => 0,
                    }
            }),
            retained_receipts: self
                .checkpoint
                .flows
                .iter()
                .map(|f| f.received.len() + f.settled.len())
                .sum(),
            active_reads: self.jobs.len(),
            stalled_reads: self.stalled.len(),
            pending_commit: self.pending.is_some(),
            awaiting_decision: self.delivery.is_some(),
        }
    }
    /// Maximum pact size, independent of the local attachment framing.
    pub fn max_packet_bytes(&self) -> usize {
        self.core.max_packet_bytes()
    }
    pub fn checkpoint(&self) -> &Checkpoint {
        &self.checkpoint
    }
    pub fn core_usage(&self) -> mesa_core::Usage {
        self.core.usage()
    }
    pub fn is_halted(&self) -> bool {
        self.halted
    }

    pub fn step(&mut self, event: Event<'_>) -> Result<Vec<Action>, Error> {
        if self.halted {
            return Err(Error::Halted);
        }
        let mut out = vec![];
        match event {
            Event::Enqueue {
                flow,
                sequence,
                bytes,
            } => {
                let state = self.checkpoint.flow(flow).ok_or(Error::UnknownFlow)?;
                if self.pending.is_some() {
                    return Err(Error::Busy);
                }
                if sequence != state.next_send {
                    return Err(Error::Sequence);
                }
                if sequence == u64::MAX || bytes.len() > self.config.max_message_bytes {
                    return Err(Error::Limit);
                }
                let total: usize = self
                    .checkpoint
                    .flows
                    .iter()
                    .flat_map(|f| f.outbox.values())
                    .map(Vec::len)
                    .sum();
                if sequence - state.send_floor >= self.config.max_messages
                    || state.outbox.len() >= self.config.window
                    || bytes.len() > self.config.max_outbox_bytes.saturating_sub(total)
                {
                    return Err(Error::Backpressure);
                }
                let id = self.checkpoint.message_id(flow, sequence, false);
                let mut next = self.checkpoint.clone();
                let state = next.flow_mut(flow);
                state.next_send += 1;
                state.outbox.insert(sequence, bytes);
                self.propose(next, Change::Enqueue { id }, &mut out)?;
            }
            Event::Decide { id, outcome } => {
                if self.pending.is_some() {
                    return Err(Error::Busy);
                }
                if self.delivery.as_ref() != Some(&id) {
                    return Err(Error::DeliveryId);
                }
                let bytes = self.messages[&(id.flow, id.sequence)].clone();
                let mut next = self.checkpoint.clone();
                next.flow_mut(id.flow).received.push(Receipt {
                    digest: mesa_crypto::sha256(&bytes),
                    outcome,
                });
                self.propose(next, Change::Deliver { id, bytes, outcome }, &mut out)?;
            }
            Event::Committed(token) => self.committed(token, &mut out)?,
            Event::Receive {
                claimed_peer,
                attachment,
                bytes,
            } => {
                if let Ok(mesa_wire::Packet {
                    body: mesa_wire::Body::Poke { ack, payload, data },
                    ..
                }) = mesa_wire::decode(bytes, self.config.core.wire)
                {
                    self.receive_hint(claimed_peer, attachment, ack, payload, data, &mut out);
                } else {
                    self.core_step(
                        CoreEvent::Receive {
                            claimed_peer,
                            attachment,
                            bytes,
                        },
                        &mut out,
                    )?;
                }
            }
            Event::AdvanceTo(now) => {
                self.core_step(CoreEvent::AdvanceTo(now), &mut out)?;
                self.now = now;
            }
            Event::Resume { flow } => {
                if self.checkpoint.flow(flow).is_none() {
                    return Err(Error::UnknownFlow);
                }
                self.stalled.retain(|w| w.flow != flow);
                self.hints.retain(|w, _| w.flow != flow);
            }
            Event::SetAttachment { peer, attachment } => {
                self.core_step(CoreEvent::SetAttachment { peer, attachment }, &mut out)?;
                for f in &mut self.flows {
                    if f.peer.id == peer {
                        f.attachment = attachment;
                    }
                }
                if attachment.is_some() {
                    self.resume_peer(peer);
                }
            }
            Event::SetService { peer, allowed } => {
                self.core_step(CoreEvent::SetService { peer, allowed }, &mut out)?;
                for f in &mut self.flows {
                    if f.peer.id == peer {
                        f.peer.service_allowed = allowed;
                    }
                }
                if allowed {
                    self.resume_peer(peer);
                }
            }
            Event::UpdatePeer(view) => {
                let old = self
                    .flows
                    .iter()
                    .find(|f| f.peer.id == view.id)
                    .ok_or(Error::UnknownFlow)?;
                if Endpoint::from(&old.peer) != Endpoint::from(&view) {
                    return Err(Error::EpochMismatch);
                }
                self.core_step(CoreEvent::InstallPeer(view.clone()), &mut out)?;
                for f in &mut self.flows {
                    if f.peer.id == view.id {
                        f.peer = view.clone();
                    }
                }
            }
            Event::InvalidateEpoch { peer, new_rift } => {
                let old = self
                    .flows
                    .iter()
                    .find(|f| f.peer.id == peer)
                    .ok_or(Error::UnknownFlow)?;
                if new_rift <= old.peer.rift {
                    return Err(Error::EpochMismatch);
                }
                self.halted = true;
                return Ok(vec![
                    Action::EpochInvalidated { peer, new_rift },
                    Action::SetTimer(None),
                ]);
            }
        }
        if let Err(e) = self.drive(&mut out) {
            self.halted = true;
            return Err(e);
        }
        out.push(Action::SetTimer(self.next_timer()));
        Ok(out)
    }

    fn resume_peer(&mut self, peer: PeerId) {
        self.stalled
            .retain(|w| self.checkpoint.peer(w.flow) != peer);
        self.hints
            .retain(|w, _| self.checkpoint.peer(w.flow) != peer);
    }
    fn propose(
        &mut self,
        mut checkpoint: Checkpoint,
        change: Change,
        out: &mut Vec<Action>,
    ) -> Result<(), Error> {
        let revision = self
            .checkpoint
            .revision
            .checked_add(1)
            .filter(|r| *r < u64::MAX)
            .ok_or(Error::Limit)?;
        checkpoint.revision = revision;
        let request = CommitRequest {
            token: CommitToken {
                incarnation: self.incarnation,
                revision,
            },
            base_revision: self.checkpoint.revision,
            checkpoint,
            change,
        };
        self.pending = Some(request.clone());
        out.push(Action::Commit(request));
        Ok(())
    }
    fn committed(&mut self, token: CommitToken, out: &mut Vec<Action>) -> Result<(), Error> {
        if self.pending.as_ref().map(|r| r.token) != Some(token) {
            return Err(Error::CommitToken);
        }
        let request = self.pending.take().unwrap();
        self.checkpoint = request.checkpoint;
        let result = match request.change {
            Change::RetireReceived { id } | Change::RetireSent { id } => {
                self.commit_retirement(id, out)
            }
            Change::Enqueue { id } => {
                let bytes = self.checkpoint.flow(id.flow).unwrap().outbox[&id.sequence].clone();
                self.core_step(
                    CoreEvent::Bind {
                        path: id.path(ReadKind::Message),
                        bytes,
                        access: Access::Public,
                    },
                    out,
                )
                .map(|()| out.push(Action::Enqueued { id }))
            }
            Change::Deliver { id, outcome, .. } => {
                let receipt = &self.checkpoint.flow(id.flow).unwrap().received
                    [(id.sequence - self.checkpoint.flow(id.flow).unwrap().receive_floor) as usize];
                let bytes = receipt.encode();
                self.messages.remove(&(id.flow, id.sequence));
                self.advertised.remove(&Work {
                    flow: id.flow,
                    sequence: id.sequence,
                    kind: ReadKind::Message,
                });
                self.delivery = None;
                self.core_step(
                    CoreEvent::Bind {
                        path: id.path(ReadKind::Receipt),
                        bytes,
                        access: Access::Public,
                    },
                    out,
                )
                .map(|()| {
                    self.emit_hint(&id, true, out);
                    out.push(Action::CommittedDelivery { id, outcome });
                })
            }
            Change::Settle { id, outcome } => {
                self.receipts.remove(&(id.flow, id.sequence));
                self.advertised.remove(&Work {
                    flow: id.flow,
                    sequence: id.sequence,
                    kind: ReadKind::Receipt,
                });
                self.hints.remove(&Work {
                    flow: id.flow,
                    sequence: id.sequence,
                    kind: ReadKind::Message,
                });
                self.core_step(
                    CoreEvent::Unbind {
                        path: &id.path(ReadKind::Message),
                    },
                    out,
                )
                .and_then(|()| {
                    let state = self.checkpoint.flow(id.flow).unwrap();
                    if state.batch_settled(self.config.max_messages) {
                        self.bind_control(id.flow, state.next_send, ReadKind::Release, out)?;
                    }
                    out.push(Action::Settled { id, outcome });
                    Ok(())
                })
            }
        };
        if result.is_err() {
            self.halted = true;
        }
        result
    }
    fn core_step(&mut self, event: CoreEvent<'_>, out: &mut Vec<Action>) -> Result<(), Error> {
        for action in self.core.step(event)? {
            match action {
                CoreAction::Send {
                    peer,
                    attachment,
                    bytes,
                } => out.push(Action::Send {
                    peer,
                    attachment,
                    bytes,
                }),
                CoreAction::SetTimer(timer) => self.timer = timer,
                CoreAction::Dropped(reason) => out.push(Action::Dropped(reason)),
                CoreAction::ReadFinished { id, result } => {
                    let work = self.jobs.remove(&id).expect("private core read IDs");
                    match result {
                        Err(reason) => self.stall(work, Failure::Read(reason), out),
                        Ok(bytes) => match work.kind {
                            ReadKind::Message => {
                                if bytes.len() > self.config.max_message_bytes {
                                    self.stall(work, Failure::MessageTooLarge, out);
                                } else {
                                    self.messages.insert((work.flow, work.sequence), bytes);
                                }
                            }
                            ReadKind::Release | ReadKind::Released => {
                                if bytes == retirement::MARKER {
                                    self.controls.insert(work);
                                } else {
                                    self.stall(work, Failure::InvalidWatermark, out);
                                }
                            }
                            ReadKind::Receipt => {
                                let expected = mesa_crypto::sha256(
                                    &self.checkpoint.flow(work.flow).unwrap().outbox
                                        [&work.sequence],
                                );
                                match Receipt::decode(&bytes).filter(|r| r.digest == expected) {
                                    Some(receipt) => {
                                        self.receipts
                                            .insert((work.flow, work.sequence), receipt.outcome);
                                    }
                                    None => self.stall(work, Failure::InvalidReceipt, out),
                                }
                            }
                        },
                    }
                }
            }
        }
        Ok(())
    }
    fn stall(&mut self, work: Work, failure: Failure, out: &mut Vec<Action>) {
        self.stalled.insert(work);
        out.push(Action::Stalled {
            flow: work.flow,
            sequence: work.sequence,
            kind: work.kind,
            failure,
        });
    }
    fn drive(&mut self, out: &mut Vec<Action>) -> Result<(), Error> {
        if self.pending.is_none() && self.delivery.is_none() {
            if let Some(work) = self.controls.iter().next().copied() {
                let incoming = work.kind == ReadKind::Release;
                let id = self
                    .checkpoint
                    .message_id(work.flow, work.sequence, incoming);
                let mut next = self.checkpoint.clone();
                let state = next.flow_mut(work.flow);
                let change = if incoming {
                    state.receive_floor = work.sequence;
                    state.received.clear();
                    Change::RetireReceived { id }
                } else {
                    state.send_floor = work.sequence;
                    state.settled.clear();
                    Change::RetireSent { id }
                };
                self.propose(next, change, out)?;
            } else if let Some((&(flow, sequence), &outcome)) = self.receipts.iter().next() {
                let id = self.checkpoint.message_id(flow, sequence, false);
                let mut next = self.checkpoint.clone();
                let state = next.flow_mut(flow);
                state.outbox.remove(&sequence);
                state.settled.insert(sequence, outcome);
                self.propose(next, Change::Settle { id, outcome }, out)?;
            } else if self.delivery.is_none() {
                for flow in &self.flows {
                    if !flow.peer.service_allowed {
                        continue;
                    }
                    let sequence = self.checkpoint.flow(flow.id).unwrap().next_receive();
                    if let Some(bytes) = self.messages.get(&(flow.id, sequence)) {
                        let id = self.checkpoint.message_id(flow.id, sequence, true);
                        self.delivery = Some(id.clone());
                        out.push(Action::Deliver {
                            id,
                            bytes: bytes.clone(),
                        });
                        break;
                    }
                }
            }
        }
        let mut wanted = vec![];
        for flow in &self.flows {
            if !flow.peer.service_allowed {
                continue;
            }
            let state = self.checkpoint.flow(flow.id).unwrap();
            let start = state.next_receive();
            if state.received.len() as u64 == self.config.max_messages {
                wanted.push(Work {
                    flow: flow.id,
                    sequence: start,
                    kind: ReadKind::Release,
                });
            }
            if state.batch_settled(self.config.max_messages) {
                wanted.push(Work {
                    flow: flow.id,
                    sequence: state.next_send,
                    kind: ReadKind::Released,
                });
            }
            for sequence in start
                ..start
                    .saturating_add(self.config.window as u64)
                    .min(state.receive_floor.saturating_add(self.config.max_messages))
            {
                if !self.messages.contains_key(&(flow.id, sequence)) {
                    wanted.push(Work {
                        flow: flow.id,
                        sequence,
                        kind: ReadKind::Message,
                    });
                }
            }
            for sequence in state.outbox.keys() {
                if !self.receipts.contains_key(&(flow.id, *sequence)) {
                    wanted.push(Work {
                        flow: flow.id,
                        sequence: *sequence,
                        kind: ReadKind::Receipt,
                    });
                }
            }
        }
        for work in wanted {
            if self.controls.contains(&work)
                || !self.advertised.contains(&work)
                || self.stalled.contains(&work)
                || self.jobs.values().any(|w| *w == work)
            {
                continue;
            }
            let id = self.next_read;
            self.next_read = self.next_read.checked_add(1).ok_or(Error::Limit)?;
            let message = self.checkpoint.message_id(
                work.flow,
                work.sequence,
                matches!(work.kind, ReadKind::Message | ReadKind::Release),
            );
            self.jobs.insert(id, work);
            if let Err(error) = self.core_step(
                CoreEvent::Read {
                    id,
                    peer: self.checkpoint.peer(work.flow),
                    path: message.path(work.kind),
                },
                out,
            ) {
                self.jobs.remove(&id);
                match error {
                    Error::Core(error) => self.stall(work, Failure::Start(error), out),
                    other => return Err(other),
                }
            }
        }
        self.announce(out);
        Ok(())
    }
}
