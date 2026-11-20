use crate::{PacketIo, Received};
use mesa_core::{Attachment, PeerId, Tick};
use mesa_flow::{Action, Error, Event, Node};
use std::io;

#[derive(Debug, PartialEq, Eq)]
pub enum Notice {
    SendDropped,
    PacketDiscarded,
    Disconnected(io::ErrorKind),
    NodeError(Error),
}
/// Network failures travel alongside actions, so a failed send never hides an
/// already-issued durable transaction or delivery proposal.
#[derive(Debug, Default)]
pub struct Batch {
    pub actions: Vec<Action>,
    pub notices: Vec<Notice>,
}
/// A node with one preconfigured remote peer and one replaceable attachment.
/// Every flow must use that peer. Connect/accept and reconnect timing belong to
/// the caller. The caller must process every Batch, including constructor output.
pub struct Host<I> {
    node: Node,
    peer: PeerId,
    attachment: Attachment,
    io: Option<I>,
}
impl<I: PacketIo> Host<I> {
    pub fn new(
        node: Node,
        peer: PeerId,
        attachment: Attachment,
        io: I,
    ) -> Result<(Self, Batch), Error> {
        if node.checkpoint().flows.is_empty()
            || node
                .checkpoint()
                .flows
                .iter()
                .any(|f| f.remote.name != peer.0)
            || io.capabilities().max_packet < node.max_packet_bytes()
        {
            return Err(Error::InvalidConfig);
        }
        let mut host = Self {
            node,
            peer,
            attachment,
            io: None,
        };
        let batch = host.reconnect(io)?;
        Ok((host, batch))
    }
    pub fn node(&self) -> &Node {
        &self.node
    }
    pub fn is_connected(&self) -> bool {
        self.io.is_some()
    }
    /// Replace the whole lane, discarding old partial frames and queued sends.
    /// A new lane restores stalled reads without changing durable flow state.
    pub fn reconnect(&mut self, io: I) -> Result<Batch, Error> {
        if io.capabilities().max_packet < self.node.max_packet_bytes() {
            return Err(Error::InvalidConfig);
        }
        let actions = self.node.step(Event::SetAttachment {
            peer: self.peer,
            attachment: Some(self.attachment),
        })?;
        self.io = Some(io);
        Ok(self.dispatch(actions))
    }
    pub fn disconnect(&mut self) -> Result<Batch, Error> {
        let actions = self.node.step(Event::SetAttachment {
            peer: self.peer,
            attachment: None,
        })?;
        self.io = None;
        Ok(self.dispatch(actions))
    }
    /// Application/control events. Receive and attachment changes are owned by
    /// this adapter; use poll, disconnect, and reconnect for those operations.
    pub fn step(&mut self, event: Event<'_>) -> Result<Batch, Error> {
        if matches!(event, Event::Receive { .. } | Event::SetAttachment { .. }) {
            return Err(Error::InvalidConfig);
        }
        let actions = self.node.step(event)?;
        Ok(self.dispatch(actions))
    }
    /// Advance time before input; perform at most 64 receive operations and one
    /// bounded write flush. The caller must keep polling while connected.
    pub fn poll(&mut self, now: Tick) -> Result<Batch, Error> {
        let mut batch = self.step(Event::AdvanceTo(now))?;
        if let Some(io) = &mut self.io {
            if let Err(e) = io.flush() {
                self.failed(e, &mut batch);
            }
        }
        for _ in 0..64 {
            let Some(io) = &mut self.io else {
                break;
            };
            match io.receive() {
                Ok(Received::Idle) => break,
                Ok(Received::Discarded) => batch.notices.push(Notice::PacketDiscarded),
                Ok(Received::Packet(bytes)) => {
                    match self.node.step(Event::Receive {
                        claimed_peer: self.peer,
                        attachment: self.attachment,
                        bytes: &bytes,
                    }) {
                        Ok(actions) => {
                            let more = self.dispatch(actions);
                            batch.actions.extend(more.actions);
                            batch.notices.extend(more.notices);
                        }
                        Err(e) => {
                            batch.notices.push(Notice::NodeError(e));
                            break;
                        }
                    }
                }
                Err(e) => {
                    self.failed(e, &mut batch);
                    break;
                }
            }
        }
        Ok(batch)
    }
    fn failed(&mut self, error: io::Error, batch: &mut Batch) {
        self.io = None;
        batch.notices.push(Notice::Disconnected(error.kind()));
        match self.node.step(Event::SetAttachment {
            peer: self.peer,
            attachment: None,
        }) {
            Ok(actions) => batch.actions.extend(actions),
            Err(e) => batch.notices.push(Notice::NodeError(e)),
        }
    }
    fn dispatch(&mut self, actions: Vec<Action>) -> Batch {
        let mut batch = Batch::default();
        let mut failure = None;
        for action in actions {
            if let Action::Send {
                peer,
                attachment,
                bytes,
            } = action
            {
                let sent =
                    if peer == self.peer && attachment == self.attachment && failure.is_none() {
                        match &mut self.io {
                            Some(io) => io.send(&bytes),
                            None => Ok(false),
                        }
                    } else {
                        Ok(false)
                    };
                match sent {
                    Ok(true) => (),
                    Ok(false) => batch.notices.push(Notice::SendDropped),
                    Err(e) => {
                        failure = Some(e);
                        batch.notices.push(Notice::SendDropped);
                    }
                }
            } else {
                batch.actions.push(action);
            }
        }
        if let Some(e) = failure {
            self.failed(e, &mut batch);
        }
        batch
    }
}
