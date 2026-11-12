//! Availability hints are untrusted scheduling input, never receipt evidence.
use super::*;
use mesa_wire::{Auth, Body, Data, DomainTag, Fragment, Name, Packet};

pub(super) struct Attempt {
    count: u32,
    retry_at: Tick,
    stopped: bool,
}

impl Node {
    pub(super) fn emit_hint(&self, id: &MessageId, incoming: bool, out: &mut Vec<Action>) {
        self.emit_pair(id, incoming, false, out);
    }
    pub(super) fn emit_control_hint(&self, id: &MessageId, incoming: bool, out: &mut Vec<Action>) {
        self.emit_pair(id, incoming, true, out);
    }
    fn emit_pair(&self, id: &MessageId, incoming: bool, control: bool, out: &mut Vec<Action>) {
        let flow = self.flows.iter().find(|f| f.id == id.flow).unwrap();
        if !flow.peer.service_allowed {
            return;
        }
        let Some(attachment) = flow.attachment else {
            return;
        };
        let remote = &self.checkpoint.flow(id.flow).unwrap().remote;
        let (sender, receiver) = if incoming {
            (remote, &self.checkpoint.local)
        } else {
            (&self.checkpoint.local, remote)
        };
        let payload_path = id.path(if control {
            ReadKind::Release
        } else {
            ReadKind::Message
        });
        let ack_path = id.path(if control {
            ReadKind::Released
        } else {
            ReadKind::Receipt
        });
        fn name<'a>(endpoint: &'a Endpoint, path: &'a [u8]) -> Name<'a> {
            Name {
                publisher: PeerId(endpoint.name),
                domain: DomainTag::new(&endpoint.domain).unwrap(),
                rift: endpoint.rift,
                bloq: 13,
                fragment: Fragment::Auth(0),
                path,
            }
        }
        let packet = Packet {
            hop: 0,
            body: Body::Poke {
                payload: name(sender, &payload_path),
                ack: name(receiver, &ack_path),
                data: Data {
                    total_bytes: 0,
                    auth: Auth::None,
                    fragment: &[],
                },
            },
        };
        // Two canonical names fit the fixed datagram profile validated at startup.
        let bytes =
            mesa_wire::encode(&packet, self.config.core.wire).expect("validated hint profile");
        out.push(Action::Send {
            peer: flow.peer.id,
            attachment,
            bytes,
        });
    }

    pub(super) fn receive_hint(
        &mut self,
        peer: PeerId,
        attachment: Attachment,
        ack: Name<'_>,
        payload: Name<'_>,
        data: Data<'_>,
        out: &mut Vec<Action>,
    ) {
        let drop = |out: &mut Vec<Action>, reason| out.push(Action::Dropped(reason));
        let Some(configured) = self.flows.iter().find(|f| f.peer.id == peer) else {
            drop(out, mesa_core::DropReason::UnknownPeer);
            return;
        };
        if !configured.peer.service_allowed {
            drop(out, mesa_core::DropReason::Blocked);
            return;
        }
        if configured.attachment != Some(attachment) {
            drop(out, mesa_core::DropReason::Attachment);
            return;
        }
        if data.total_bytes != 0 || data.auth != Auth::None || !data.fragment.is_empty() {
            drop(out, mesa_core::DropReason::Unsupported);
            return;
        }
        let Some((id, control)) = parse_path(payload.path) else {
            drop(out, mesa_core::DropReason::Name);
            return;
        };
        let Some(flow) = self.checkpoint.flow(id.flow) else {
            drop(out, mesa_core::DropReason::Name);
            return;
        };
        if flow.remote.name != peer.0 {
            drop(out, mesa_core::DropReason::Name);
            return;
        }
        let incoming = id.sender == peer.0;
        if id != self.checkpoint.message_id(id.flow, id.sequence, incoming)
            || ack.path
                != id.path(if control {
                    ReadKind::Released
                } else {
                    ReadKind::Receipt
                })
        {
            drop(out, mesa_core::DropReason::Name);
            return;
        }
        let (sender, receiver) = if incoming {
            (&flow.remote, &self.checkpoint.local)
        } else {
            (&self.checkpoint.local, &flow.remote)
        };
        let matches = |name: Name<'_>, endpoint: &Endpoint| {
            name.publisher.0 == endpoint.name
                && name.domain.as_str() == endpoint.domain
                && name.rift == endpoint.rift
                && name.bloq == 13
                && name.fragment == Fragment::Auth(0)
        };
        if !matches(payload, sender) || !matches(ack, receiver) {
            drop(out, mesa_core::DropReason::Name);
            return;
        }
        let kind = if control {
            if incoming {
                if id.sequence > 0 && id.sequence == flow.receive_floor {
                    self.emit_control_hint(&id, true, out);
                    return;
                }
                if flow.received.len() as u64 != self.config.max_messages
                    || id.sequence != flow.next_receive()
                {
                    return;
                }
                ReadKind::Release
            } else {
                if !flow.batch_settled(self.config.max_messages) || id.sequence != flow.next_send {
                    return;
                }
                ReadKind::Released
            }
        } else if incoming {
            if id.sequence < flow.receive_floor {
                return;
            }
            if id.sequence < flow.next_receive() {
                // The durable prefix authorizes another availability hint. The
                // sender must still authenticate the actual receipt by reading it.
                self.emit_hint(&id, true, out);
                return;
            }
            if id.sequence
                >= flow
                    .next_receive()
                    .saturating_add(self.config.window as u64)
                || id.sequence >= flow.receive_floor.saturating_add(self.config.max_messages)
            {
                drop(out, mesa_core::DropReason::ResourceLimit);
                return;
            }
            ReadKind::Message
        } else {
            if !flow.outbox.contains_key(&id.sequence) {
                return;
            }
            ReadKind::Receipt
        };
        self.advertised.insert(Work {
            flow: id.flow,
            sequence: id.sequence,
            kind,
        });
    }

    pub(super) fn announce(&mut self, out: &mut Vec<Action>) {
        let pending: Vec<_> = self
            .checkpoint
            .flows
            .iter()
            .flat_map(|f| {
                let mut work: Vec<_> = f
                    .outbox
                    .keys()
                    .map(|s| Work {
                        flow: f.flow,
                        sequence: *s,
                        kind: ReadKind::Message,
                    })
                    .collect();
                if f.batch_settled(self.config.max_messages) {
                    work.push(Work {
                        flow: f.flow,
                        sequence: f.next_send,
                        kind: ReadKind::Release,
                    });
                }
                work
            })
            .collect();
        for work in pending {
            let Work {
                flow,
                sequence,
                kind,
            } = work;
            if kind == ReadKind::Message && self.receipts.contains_key(&(flow, sequence)) {
                continue;
            }
            let configured = self.flows.iter().find(|f| f.id == flow).unwrap();
            if !configured.peer.service_allowed || configured.attachment.is_none() {
                continue;
            }
            let attempt = self.hints.entry(work).or_insert(Attempt {
                count: 0,
                retry_at: self.now,
                stopped: false,
            });
            if attempt.stopped || attempt.retry_at > self.now {
                continue;
            }
            let next = self.now.checked_add(
                self.config
                    .transfer
                    .timers
                    .retry
                    .delay(attempt.count.saturating_add(1)),
            );
            if attempt.count >= self.config.transfer.recovery.max_attempts || next.is_none() {
                attempt.stopped = true;
                out.push(Action::AnnouncementStalled {
                    id: self.checkpoint.message_id(flow, sequence, false),
                    kind,
                });
                continue;
            }
            attempt.count += 1;
            attempt.retry_at = next.unwrap();
            self.emit_pair(
                &self.checkpoint.message_id(flow, sequence, false),
                false,
                kind == ReadKind::Release,
                out,
            );
        }
    }
    pub(super) fn next_timer(&self) -> Option<Tick> {
        self.hints
            .iter()
            .filter(|(work, attempt)| {
                let configured = self.flows.iter().find(|f| f.id == work.flow).unwrap();
                !attempt.stopped
                    && configured.peer.service_allowed
                    && configured.attachment.is_some()
                    && !(work.kind == ReadKind::Message
                        && self.receipts.contains_key(&(work.flow, work.sequence)))
            })
            .map(|(_, a)| a.retry_at)
            .chain(self.timer)
            .min()
    }
}
fn parse_path(path: &[u8]) -> Option<(MessageId, bool)> {
    if path.len() != 128 && path.len() != 132 {
        return None;
    }
    let text = std::str::from_utf8(path).ok()?;
    let parts: Vec<_> = text.split('/').collect();
    if parts.len() != 9
        || parts[0] != "flow"
        || parts[1] != "2"
        || !matches!(parts[7], "msg" | "release")
    {
        return None;
    }
    let id = MessageId {
        sender: u128::from_str_radix(parts[2], 16).ok()?,
        sender_rift: u32::from_str_radix(parts[3], 16).ok()?,
        receiver: u128::from_str_radix(parts[4], 16).ok()?,
        receiver_rift: u32::from_str_radix(parts[5], 16).ok()?,
        flow: u64::from_str_radix(parts[6], 16).ok()?,
        sequence: u64::from_str_radix(parts[8], 16).ok()?,
    };
    let control = parts[7] == "release";
    (id.path(if control {
        ReadKind::Release
    } else {
        ReadKind::Message
    }) == path)
        .then_some((id, control))
}
