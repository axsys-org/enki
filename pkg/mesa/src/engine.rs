use crate::{
    obj,
    object::*,
    value::{err, nums, Result, Value as V},
};
use core::policy::*;
use mesa_core::{self as core, Action as A, Attachment, Event as E, KeyHandle, PeerId};
use std::collections::BTreeMap;
pub fn config(d: &V) -> Result<core::Config> {
    let mut c = core::Config::default();
    if !d.list()?.is_empty() {
        c.wire = crate::primitives::wire_limits(d.at(0)?)?;
        c.bloq = d.at(1)?.u()?.try_into().map_err(err)?;
        c.max_peers = d.at(2)?.size()?;
        c.max_bindings = d.at(3)?.size()?;
        c.max_binding_bytes = d.at(4)?.size()?;
        c.max_read_bytes = d.at(5)?.size()?;
        c.max_reassembly_bytes = d.at(6)?.size()?;
        c.max_unverified_bytes = d.at(7)?.size()?;
        c.max_reads = d.at(8)?.size()?;
        c.max_fragments = d.at(9)?.size()?;
        c.window = d.at(10)?.size()?;
        c.retry_after = d.at(11)?.u()?;
        c.read_timeout = d.at(12)?.u()?;
        c.max_attempts = d.at(13)?.u()?.try_into().map_err(err)?;
    }
    if c.wire.max_packet > 65535
        || c.wire.max_fragment > 65535
        || c.max_peers > 4096
        || c.max_bindings > 65536
        || c.max_binding_bytes > 64 << 20
        || c.max_read_bytes > 8 << 20
        || c.max_reassembly_bytes > 64 << 20
        || c.max_unverified_bytes > 8 << 20
        || c.max_reads > 4096
        || c.max_fragments > 65536
    {
        return Err("bridge engine limit".into());
    }
    Ok(c)
}
pub fn config_value(c: &core::Config) -> V {
    V::row([
        nums([
            c.wire.max_packet as u128,
            c.wire.max_path as u128,
            c.wire.max_fragment as u128,
        ]),
        V::num(c.bloq as u128),
        V::num(c.max_peers as u128),
        V::num(c.max_bindings as u128),
        V::num(c.max_binding_bytes as u128),
        V::num(c.max_read_bytes as u128),
        V::num(c.max_reassembly_bytes as u128),
        V::num(c.max_unverified_bytes as u128),
        V::num(c.max_reads as u128),
        V::num(c.max_fragments as u128),
        V::num(c.window as u128),
        V::num(c.retry_after as u128),
        V::num(c.read_timeout as u128),
        V::num(c.max_attempts as u128),
    ])
}
pub fn usage(u: core::Usage) -> V {
    nums([
        u.peers as u128,
        u.namespace_slots as u128,
        u.bindings as u128,
        u.binding_bytes as u128,
        u.binding_proof_bytes as u128,
        u.reads as u128,
        u.reassembly_bytes as u128,
        u.pending_interests as u128,
        u.received_fragments as u128,
        u.unverified_bytes as u128,
        u.unverified_fragments as u128,
    ])
}
pub fn peer_value(p: &core::PreconfiguredPeer) -> V {
    V::row([
        V::num(p.id.0),
        V::text(p.domain().as_str()),
        V::num(p.life as u128),
        V::num(p.rift as u128),
        V::num(p.key.0 as u128),
        V::num(p.service_allowed as u128),
    ])
}
pub fn policy(d: &V) -> Result<TransferPolicy> {
    let r = d.at(2)?;
    let c = d.at(3)?;
    Ok(TransferPolicy {
        recovery: RecoveryPolicy {
            max_attempts: d.at(0)?.u()?.try_into().map_err(err)?,
        },
        timers: TimerPolicy {
            read_timeout: d.at(1)?.u()?,
            retry: match r.at(0)?.u()? {
                0 => RetryTimer::Fixed {
                    after: r.at(1)?.u()?,
                },
                1 => RetryTimer::Exponential {
                    initial: r.at(1)?.u()?,
                    maximum: r.at(2)?.u()?,
                },
                _ => return Err("retry tag".into()),
            },
        },
        congestion: match c.at(0)?.u()? {
            0 => CongestionPolicy::Fixed {
                window: c.at(1)?.size()?,
            },
            1 => CongestionPolicy::Aimd {
                initial: c.at(1)?.size()?,
                maximum: c.at(2)?.size()?,
            },
            _ => return Err("congestion tag".into()),
        },
    })
}
pub fn policy_value(p: &TransferPolicy) -> V {
    V::row([
        V::num(p.recovery.max_attempts as u128),
        V::num(p.timers.read_timeout as u128),
        match p.timers.retry {
            RetryTimer::Fixed { after } => nums([0, after as u128]),
            RetryTimer::Exponential { initial, maximum } => {
                nums([1, initial as u128, maximum as u128])
            }
        },
        match p.congestion {
            CongestionPolicy::Fixed { window } => nums([0, window as u128]),
            CongestionPolicy::Aimd { initial, maximum } => {
                nums([1, initial as u128, maximum as u128])
            }
        },
    ])
}
pub fn actions(a: Vec<A>) -> Output {
    output(Object::Actions(Actions::Core(a.into())))
}
pub fn core_action(a: A) -> Output {
    data(match a {
        A::Send {
            peer,
            attachment,
            bytes,
        } => V::row([
            V::text("Send"),
            V::num(peer.0),
            V::num(attachment.0 as u128),
            V::bytes(bytes),
        ]),
        A::ReadFinished { id, result } => match result {
            Ok(b) => V::row([
                V::text("ReadFinished"),
                V::num(id as u128),
                V::num(0),
                V::bytes(b),
            ]),
            Err(e) => V::row([
                V::text("ReadFinished"),
                V::num(id as u128),
                V::num(1),
                V::text(err(e)),
            ]),
        },
        A::Dropped(e) => V::row([V::text("Dropped"), V::text(err(e))]),
        A::SetTimer(t) => V::row([
            V::text("SetTimer"),
            t.map_or(V::num(0), |t| nums([t as u128])),
        ]),
    })
}
pub fn event<'a>(op: &str, h: &mut Handles, d: &'a V) -> Result<E<'a>> {
    Ok(match op {
        "EngineInstallPeer" => E::InstallPeer(obj!(h, 1, Peer).clone()),
        "EngineRemoteBreach" => E::RemoteBreach(obj!(h, 1, Peer).clone()),
        "EngineSetService" => E::SetService {
            peer: PeerId(d.at(0)?.n()?),
            allowed: d.at(1)?.bool()?,
        },
        "EngineSetAttachment" => E::SetAttachment {
            peer: PeerId(d.at(0)?.n()?),
            attachment: if d.at(1)?.list()?.is_empty() {
                None
            } else {
                Some(Attachment(d.at(1)?.at(0)?.u()?))
            },
        },
        "EngineBind" => E::Bind {
            path: d.at(0)?.bar()?.to_vec(),
            bytes: d.at(1)?.bar()?.to_vec(),
            access: if d.at(2)?.list()?.is_empty() {
                core::Access::Public
            } else {
                core::Access::Peer(PeerId(d.at(2)?.at(0)?.n()?))
            },
        },
        "EngineUnbind" => E::Unbind {
            path: d.at(0)?.bar()?,
        },
        "EngineRetireBefore" => E::RetireBefore {
            prefix: d.at(0)?.bar()?.to_vec(),
            suffix: d.at(1)?.bar()?.to_vec(),
        },
        "EngineRead" => E::Read {
            id: d.at(0)?.u()?,
            peer: PeerId(d.at(1)?.n()?),
            path: d.at(2)?.bar()?.to_vec(),
        },
        "EngineCancel" => E::Cancel { id: d.at(0)?.u()? },
        "EngineAdvanceTo" => E::AdvanceTo(d.at(0)?.u()?),
        "EngineReceive" => E::Receive {
            claimed_peer: PeerId(d.at(0)?.n()?),
            attachment: Attachment(d.at(1)?.u()?),
            bytes: d.at(2)?.bar()?,
        },
        _ => return Err("unknown engine operation".into()),
    })
}
pub fn dispatch(op: &str, h: &mut Handles, d: &V) -> Result<Output> {
    Ok(match op {
        "Config" => output(Object::Config(config(d)?)),
        "ConfigFields" => data(config_value(obj!(h, 0, Config))),
        "Policy" => output(Object::Policy(policy(d)?)),
        "PolicyFixed" => output(Object::Policy(TransferPolicy::fixed(obj!(h, 0, Config)))),
        "PolicyFields" => data(policy_value(obj!(h, 0, Policy))),
        "RetryDelay" => data(V::num(
            obj!(h, 0, Policy)
                .timers
                .retry
                .delay(d.at(0)?.u()?.try_into().map_err(err)?) as u128,
        )),
        "Peer" => {
            let mut p = core::PreconfiguredPeer::new(
                PeerId(d.at(0)?.n()?),
                d.at(1)?.str()?,
                d.at(2)?.u()?.try_into().map_err(err)?,
                d.at(3)?.u()?.try_into().map_err(err)?,
                KeyHandle(d.at(4)?.u()?),
            )
            .map_err(err)?;
            p.service_allowed = d.at(5)?.bool()?;
            output(Object::Peer(p))
        }
        "PeerFields" => data(peer_value(obj!(h, 0, Peer))),
        "PeerClone" => output(Object::Peer(obj!(h, 0, Peer).clone())),
        "Protection" => {
            let local = KeyHandle(d.at(0)?.u()?);
            let keys = d.at(1)?.list()?;
            if keys.len() + 1 != h.len() {
                return Err("peer keys/handles length mismatch".into());
            }
            let mut peers = BTreeMap::new();
            for (i, k) in keys.iter().enumerate() {
                if peers
                    .insert(KeyHandle(k.u()?), obj!(h, i + 1, Public).clone())
                    .is_some()
                {
                    return Err("duplicate key handle".into());
                }
            }
            let Object::Key(key) = take_kind(h, 0, "KeyPair")? else {
                unreachable!()
            };
            output(Object::Protection(core::protection::SignedPublic::new(
                local, key, peers,
            )))
        }
        "Engine" => {
            let cfg = obj!(h, 0, Config).clone();
            let peer = obj!(h, 1, Peer).clone();
            let policy = if h.len() > 3 {
                *obj!(h, 3, Policy)
            } else {
                TransferPolicy::fixed(&cfg)
            };
            let Object::Protection(p) = take_kind(h, 2, "Protection")? else {
                unreachable!()
            };
            output(Object::Engine(
                core::Engine::with_policy(cfg, peer, p, policy).map_err(err)?,
            ))
        }
        "EngineUsage" => data(usage(obj!(h, 0, Engine).usage())),
        "EnginePacketLimit" => data(V::num(obj!(h, 0, Engine).max_packet_bytes() as u128)),
        "EngineTransferState" => data(
            obj!(h, 0, Engine)
                .transfer_state(PeerId(d.at(0)?.n()?))
                .map_or(V::num(0), |s| {
                    nums([s.window as u128, s.in_flight as u128, s.queued as u128])
                }),
        ),
        "ActionsCount" => data(V::num(match obj!(h, 0, Actions) {
            Actions::Core(a) => a.len(),
            Actions::Flow(a) => a.len(),
        } as u128)),
        "ActionsNext" => match obj!(h, 0, Actions) {
            Actions::Core(a) => a.pop_front().map_or(data(V::num(0)), core_action),
            Actions::Flow(a) => a.pop_front().map_or(data(V::num(0)), crate::flow::action),
        },
        _ => {
            let e = event(op, h, d)?;
            actions(obj!(h, 0, Engine).step(e).map_err(err)?)
        }
    })
}
