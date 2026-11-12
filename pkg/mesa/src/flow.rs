use crate::{
    obj,
    object::*,
    value::{err, nums, Result, Value as V},
};
use mesa_core::{Attachment, PeerId};
use mesa_flow::{self as flow, Action as A, Event as E};
pub fn id(v: &V) -> Result<flow::MessageId> {
    Ok(flow::MessageId {
        sender: v.at(0)?.n()?,
        sender_rift: v.at(1)?.u()?.try_into().map_err(err)?,
        receiver: v.at(2)?.n()?,
        receiver_rift: v.at(3)?.u()?.try_into().map_err(err)?,
        flow: v.at(4)?.u()?,
        sequence: v.at(5)?.u()?,
    })
}
pub fn id_value(i: &flow::MessageId) -> V {
    nums([
        i.sender,
        i.sender_rift as u128,
        i.receiver,
        i.receiver_rift as u128,
        i.flow as u128,
        i.sequence as u128,
    ])
}
pub fn outcome(v: &V) -> Result<flow::Outcome> {
    if v.list()?.is_empty() {
        Ok(flow::Outcome::Accepted)
    } else {
        Ok(flow::Outcome::Rejected(
            v.at(0)?.u()?.try_into().map_err(err)?,
        ))
    }
}
pub fn outcome_value(o: flow::Outcome) -> V {
    match o {
        flow::Outcome::Accepted => V::num(0),
        flow::Outcome::Rejected(n) => nums([n as u128]),
    }
}
pub fn kind(v: &V) -> Result<flow::ReadKind> {
    match v.u()? {
        0 => Ok(flow::ReadKind::Message),
        1 => Ok(flow::ReadKind::Receipt),
        2 => Ok(flow::ReadKind::Release),
        3 => Ok(flow::ReadKind::Released),
        _ => Err("read kind".into()),
    }
}
pub fn actions(a: Vec<A>) -> Output {
    output(Object::Actions(Actions::Flow(a.into())))
}
pub fn action(a: A) -> Output {
    match a {
        A::Send {
            peer,
            attachment,
            bytes,
        } => data(V::row([
            V::text("Send"),
            V::num(peer.0),
            V::num(attachment.0 as u128),
            V::bytes(bytes),
        ])),
        A::Commit(c) => (V::row([V::text("Commit")]), vec![Object::Commit(c)]),
        A::Deliver { id, bytes } => {
            data(V::row([V::text("Deliver"), id_value(&id), V::bytes(bytes)]))
        }
        A::Enqueued { id } => data(V::row([V::text("Enqueued"), id_value(&id)])),
        A::CommittedDelivery { id, outcome } => data(V::row([
            V::text("CommittedDelivery"),
            id_value(&id),
            outcome_value(outcome),
        ])),
        A::Settled { id, outcome } => data(V::row([
            V::text("Settled"),
            id_value(&id),
            outcome_value(outcome),
        ])),
        A::Stalled {
            flow,
            sequence,
            kind,
            failure,
        } => data(V::row([
            V::text("Stalled"),
            V::num(flow as u128),
            V::num(sequence as u128),
            V::text(err(kind)),
            V::text(err(failure)),
        ])),
        A::Dropped(e) => data(V::row([V::text("Dropped"), V::text(err(e))])),
        A::AnnouncementStalled { id, kind } => data(V::row([
            V::text("AnnouncementStalled"),
            id_value(&id),
            V::text(err(kind)),
        ])),
        A::EpochInvalidated { peer, new_rift } => data(V::row([
            V::text("EpochInvalidated"),
            V::num(peer.0),
            V::num(new_rift as u128),
        ])),
        A::SetTimer(t) => data(V::row([
            V::text("SetTimer"),
            t.map_or(V::num(0), |t| nums([t as u128])),
        ])),
    }
}
pub fn event<'a>(op: &str, h: &mut Handles, d: &'a V) -> Result<E<'a>> {
    Ok(match op {
        "Enqueue" => E::Enqueue {
            flow: d.at(0)?.u()?,
            sequence: d.at(1)?.u()?,
            bytes: d.at(2)?.bar()?.to_vec(),
        },
        "Decide" => E::Decide {
            id: id(d.at(0)?)?,
            outcome: outcome(d.at(1)?)?,
        },
        "Committed" => E::Committed(*obj!(h, 1, Token)),
        "Receive" => E::Receive {
            claimed_peer: PeerId(d.at(0)?.n()?),
            attachment: Attachment(d.at(1)?.u()?),
            bytes: d.at(2)?.bar()?,
        },
        "AdvanceTo" => E::AdvanceTo(d.at(0)?.u()?),
        "Resume" => E::Resume {
            flow: d.at(0)?.u()?,
        },
        "SetAttachment" => E::SetAttachment {
            peer: PeerId(d.at(0)?.n()?),
            attachment: if d.at(1)?.list()?.is_empty() {
                None
            } else {
                Some(Attachment(d.at(1)?.at(0)?.u()?))
            },
        },
        "SetService" => E::SetService {
            peer: PeerId(d.at(0)?.n()?),
            allowed: d.at(1)?.bool()?,
        },
        "UpdatePeer" => E::UpdatePeer(obj!(h, 1, Peer).clone()),
        "InvalidateEpoch" => E::InvalidateEpoch {
            peer: PeerId(d.at(0)?.n()?),
            new_rift: d.at(1)?.u()?.try_into().map_err(err)?,
        },
        _ => return Err("unknown flow event".into()),
    })
}
pub fn dispatch(op: &str, h: &mut Handles, d: &V) -> Result<Output> {
    Ok(match op {
        "FlowConfig" => {
            let core = obj!(h, 0, Config).clone();
            let transfer = *obj!(h, 1, Policy);
            let mut c = flow::Config {
                core,
                transfer,
                ..Default::default()
            };
            if !d.list()?.is_empty() {
                c.max_flows = d.at(0)?.size()?;
                c.max_messages = d.at(1)?.u()?;
                c.window = d.at(2)?.size()?;
                c.max_message_bytes = d.at(3)?.size()?;
                c.max_outbox_bytes = d.at(4)?.size()?;
            }
            if c.max_flows > 4096 || c.max_message_bytes > 8 << 20 || c.max_outbox_bytes > 64 << 20
            {
                return Err("bridge flow limit".into());
            }
            output(Object::FlowConfig(c))
        }
        "FlowConfigFields" => {
            let c = obj!(h, 0, FlowConfig);
            (
                nums([
                    c.max_flows as u128,
                    c.max_messages as u128,
                    c.window as u128,
                    c.max_message_bytes as u128,
                    c.max_outbox_bytes as u128,
                ]),
                vec![Object::Config(c.core.clone()), Object::Policy(c.transfer)],
            )
        }
        "Flows" => {
            let rows = d.list()?;
            if rows.len() != h.len() {
                return Err("flow/peer count".into());
            }
            let mut flows = Vec::new();
            for (i, r) in rows.iter().enumerate() {
                flows.push(flow::FlowConfig {
                    id: r.at(0)?.u()?,
                    peer: obj!(h, i, Peer).clone(),
                    attachment: if r.at(1)?.list()?.is_empty() {
                        None
                    } else {
                        Some(Attachment(r.at(1)?.at(0)?.u()?))
                    },
                })
            }
            output(Object::Flows(flows))
        }
        "FlowsFields" => {
            let f = obj!(h, 0, Flows);
            (
                V::row(f.iter().map(|f| {
                    V::row([
                        V::num(f.id as u128),
                        f.attachment.map_or(V::num(0), |a| nums([a.0 as u128])),
                    ])
                })),
                f.iter().map(|f| Object::Peer(f.peer.clone())).collect(),
            )
        }
        "Checkpoint" => {
            let local = obj!(h, 0, Peer).clone();
            let flows = obj!(h, 1, Flows);
            output(Object::Checkpoint(flow::Checkpoint::empty_with_batch(
                &local,
                flows,
                d.at(0)?.u()?,
            )))
        }
        "CheckpointImport" => output(Object::Checkpoint(
            serde_json::from_slice(d.at(0)?.bar()?).map_err(err)?,
        )),
        "CheckpointExport" => data(V::bytes(
            serde_json::to_vec(obj!(h, 0, Checkpoint)).map_err(err)?,
        )),
        "CheckpointClone" => output(Object::Checkpoint(obj!(h, 0, Checkpoint).clone())),
        "CheckpointFlow" => {
            let c = obj!(h, 0, Checkpoint);
            data(match c.flow(d.at(0)?.u()?) {
                None => V::num(0),
                Some(f) => V::row([
                    V::bytes(serde_json::to_vec(f).map_err(err)?),
                    V::num(f.next_receive() as u128),
                    V::num(f.batch_settled(c.batch_size) as u128),
                    V::num(f.valid(c.batch_size) as u128),
                ]),
            })
        }
        "CheckpointRevision" => {
            let c = obj!(h, 0, Checkpoint);
            data(
                c.expected_revision()
                    .map_or(V::num(0), |r| nums([r as u128])),
            )
        }
        "Node" => {
            let cfg = obj!(h, 0, FlowConfig).clone();
            let local = obj!(h, 1, Peer).clone();
            let flows = obj!(h, 3, Flows).clone();
            let checkpoint = obj!(h, 4, Checkpoint).clone();
            let inc = d.at(0)?.n()?;
            let Object::Protection(p) = take_kind(h, 2, "Protection")? else {
                unreachable!()
            };
            output(Object::Node(
                flow::Node::new(cfg, local, p, flows, checkpoint, inc).map_err(err)?,
            ))
        }
        "NodeCheckpoint" => output(Object::Checkpoint(obj!(h, 0, Node).checkpoint().clone())),
        "NodeUsage" => {
            let u = obj!(h, 0, Node).usage();
            data(nums([
                u.outbox_messages as u128,
                u.outbox_bytes as u128,
                u.buffered_messages as u128,
                u.buffered_message_bytes as u128,
                u.pending_commit_bytes as u128,
                u.retained_receipts as u128,
                u.active_reads as u128,
                u.stalled_reads as u128,
                u.pending_commit as u128,
                u.awaiting_decision as u128,
            ]))
        }
        "NodeCoreUsage" => data(crate::engine::usage(obj!(h, 0, Node).core_usage())),
        "NodeHalted" => data(V::num(obj!(h, 0, Node).is_halted() as u128)),
        "NodePacketLimit" => data(V::num(obj!(h, 0, Node).max_packet_bytes() as u128)),
        "CommitFields" => {
            let c = obj!(h, 0, Commit);
            let change = match &c.change {
                flow::Change::RetireReceived { id } => {
                    V::row([V::text("RetireReceived"), id_value(id)])
                }
                flow::Change::RetireSent { id } => V::row([V::text("RetireSent"), id_value(id)]),
                flow::Change::Enqueue { id } => V::row([V::text("Enqueue"), id_value(id)]),
                flow::Change::Deliver { id, bytes, outcome } => V::row([
                    V::text("Deliver"),
                    id_value(id),
                    V::bytes(bytes),
                    outcome_value(*outcome),
                ]),
                flow::Change::Settle { id, outcome } => {
                    V::row([V::text("Settle"), id_value(id), outcome_value(*outcome)])
                }
            };
            (
                V::row([V::num(c.base_revision as u128), change]),
                vec![
                    Object::Token(c.token),
                    Object::Checkpoint(c.checkpoint.clone()),
                ],
            )
        }
        "Token" => output(Object::Token(flow::CommitToken {
            incarnation: d.at(0)?.n()?,
            revision: d.at(1)?.u()?,
        })),
        "TokenFields" => {
            let t = obj!(h, 0, Token);
            data(nums([t.incarnation, t.revision as u128]))
        }
        "MessagePath" => data(V::bytes(id(d.at(0)?)?.path(kind(d.at(1)?)?))),
        _ => {
            let e = event(
                op.strip_prefix("Node").ok_or("unknown flow operation")?,
                h,
                d,
            )?;
            actions(obj!(h, 0, Node).step(e).map_err(err)?)
        }
    })
}
