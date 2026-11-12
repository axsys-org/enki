use crate::{
    obj,
    object::*,
    value::{err, nums, Result, Value as V},
};
use mesa_core::{Attachment, PeerId};
use mesa_transport::{self as transport, PacketIo};
use std::{
    collections::BTreeMap,
    net::{SocketAddr, TcpListener, TcpStream, UdpSocket},
    time::Duration,
};
fn addr(v: &V) -> Result<SocketAddr> {
    v.str()?.parse().map_err(err)
}
fn batch(b: transport::Batch) -> Output {
    (
        V::row(b.notices.iter().map(|n| V::text(err(n)))),
        vec![Object::Actions(Actions::Flow(b.actions.into()))],
    )
}
fn leg(v: &V) -> Result<transport::relay::Leg> {
    match v.u()? {
        0 => Ok(transport::relay::Leg::A),
        1 => Ok(transport::relay::Leg::B),
        _ => Err("relay leg".into()),
    }
}
pub fn dispatch(op: &str, h: &mut Handles, d: &V) -> Result<Output> {
    Ok(match op {
        "UdpBind" => {
            let s = UdpSocket::bind(addr(d.at(0)?)?).map_err(err)?;
            s.set_nonblocking(true).map_err(err)?;
            output(Object::Socket(s))
        }
        "UdpAddress" => data(V::text(
            obj!(h, 0, Socket).local_addr().map_err(err)?.to_string(),
        )),
        "TcpConnect" => {
            let ms = d.at(1)?.u()?;
            if ms == 0 || ms > 60000 {
                return Err("connect timeout must be 1..60000 ms".into());
            }
            let s = TcpStream::connect_timeout(&addr(d.at(0)?)?, Duration::from_millis(ms))
                .map_err(err)?;
            s.set_nonblocking(true).map_err(err)?;
            output(Object::Tcp(s))
        }
        "TcpListen" => {
            let s = TcpListener::bind(addr(d.at(0)?)?).map_err(err)?;
            s.set_nonblocking(true).map_err(err)?;
            output(Object::Listener(s))
        }
        "TcpListenAddress" => data(V::text(
            obj!(h, 0, Listener).local_addr().map_err(err)?.to_string(),
        )),
        "TcpAccept" => match obj!(h, 0, Listener).accept() {
            Ok((s, a)) => {
                s.set_nonblocking(true).map_err(err)?;
                (V::text(a.to_string()), vec![Object::Tcp(s)])
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => data(V::num(0)),
            Err(e) => return Err(err(e)),
        },
        "UdpLane" => {
            let next = addr(d.at(0)?)?;
            let max = d.at(1)?.size()?;
            let Object::Socket(s) = take_kind(h, 0, "UdpSocket")? else {
                unreachable!()
            };
            output(Object::Lane(Lane::Udp(
                transport::Udp::new(s, next, max).map_err(err)?,
            )))
        }
        "TcpLane" => {
            let max = d.at(0)?.size()?;
            let queue = d.at(1)?.size()?;
            let Object::Tcp(s) = take_kind(h, 0, "TcpSocket")? else {
                unreachable!()
            };
            output(Object::Lane(Lane::Tcp(
                transport::Stream::tcp(s, max, queue).map_err(err)?,
            )))
        }
        "LaneCapabilities" => {
            let c = obj!(h, 0, Lane).capabilities();
            data(nums([
                c.max_packet as u128,
                c.reliable as u128,
                c.ordered as u128,
                match c.framing {
                    transport::Framing::Datagram => 0,
                    transport::Framing::LengthPrefixed => 1,
                },
            ]))
        }
        "LaneSend" => data(V::num(
            obj!(h, 0, Lane).send(d.at(0)?.bar()?).map_err(err)? as u128
        )),
        "LaneReceive" => data(match obj!(h, 0, Lane).receive().map_err(err)? {
            transport::Received::Idle => nums([0]),
            transport::Received::Discarded => nums([1]),
            transport::Received::Packet(b) => V::row([V::num(2), V::bytes(b)]),
        }),
        "LaneFlush" => {
            obj!(h, 0, Lane).flush().map_err(err)?;
            data(V::num(0))
        }
        "LaneBuffers" => {
            let Lane::Tcp(s) = obj!(h, 0, Lane) else {
                return Err("expected TCP lane".into());
            };
            data(nums([s.queued_bytes() as u128, s.buffered_bytes() as u128]))
        }
        "LaneAddress" => {
            let Lane::Udp(s) = obj!(h, 0, Lane) else {
                return Err("expected UDP lane".into());
            };
            data(V::text(s.local_addr().map_err(err)?.to_string()))
        }
        "Host" => {
            let peer = PeerId(d.at(0)?.n()?);
            let attachment = Attachment(d.at(1)?.u()?);
            if get(h, 1)?.kind() != "Lane" {
                return Err("expected Lane".into());
            }
            let Object::Node(node) = take_kind(h, 0, "Node")? else {
                unreachable!()
            };
            let Object::Lane(lane) = take_kind(h, 1, "Lane")? else {
                unreachable!()
            };
            let (host, b) = transport::Host::new(node, peer, attachment, lane).map_err(err)?;
            let (v, mut o) = batch(b);
            o.insert(0, Object::Host(host));
            (v, o)
        }
        "HostConnected" => data(V::num(obj!(h, 0, Host).is_connected() as u128)),
        "HostCheckpoint" => output(Object::Checkpoint(
            obj!(h, 0, Host).node().checkpoint().clone(),
        )),
        "HostNodeUsage" => {
            let u = obj!(h, 0, Host).node().usage();
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
        "HostCoreUsage" => data(crate::engine::usage(obj!(h, 0, Host).node().core_usage())),
        "HostHalted" => data(V::num(obj!(h, 0, Host).node().is_halted() as u128)),
        "HostPacketLimit" => data(V::num(obj!(h, 0, Host).node().max_packet_bytes() as u128)),
        "HostPoll" => batch(obj!(h, 0, Host).poll(d.at(0)?.u()?).map_err(err)?),
        "HostDisconnect" => batch(obj!(h, 0, Host).disconnect().map_err(err)?),
        "HostReconnect" => {
            obj!(h, 0, Host);
            let Object::Lane(lane) = take_kind(h, 1, "Lane")? else {
                unreachable!()
            };
            batch(obj!(h, 0, Host).reconnect(lane).map_err(err)?)
        }
        "UdpHost" => {
            let mut routes = BTreeMap::new();
            for r in d.list()? {
                if routes
                    .insert(
                        PeerId(r.at(0)?.n()?),
                        (Attachment(r.at(1)?.u()?), addr(r.at(2)?)?),
                    )
                    .is_some()
                {
                    return Err("duplicate route".into());
                }
            }
            if get(h, 1)?.kind() != "UdpSocket" {
                return Err("expected UdpSocket".into());
            }
            let Object::Engine(e) = take_kind(h, 0, "Engine")? else {
                unreachable!()
            };
            let Object::Socket(s) = take_kind(h, 1, "UdpSocket")? else {
                unreachable!()
            };
            output(Object::UdpHost(
                mesa_udp::UdpHost::new(e, s, routes).map_err(err)?,
            ))
        }
        "UdpHostAddress" => data(V::text(
            obj!(h, 0, UdpHost).local_addr().map_err(err)?.to_string(),
        )),
        "UdpHostUsage" => data(crate::engine::usage(obj!(h, 0, UdpHost).engine().usage())),
        "UdpHostDiscarded" => data(V::num(obj!(h, 0, UdpHost).discarded_datagrams as u128)),
        "UdpHostPacketLimit" => data(V::num(
            obj!(h, 0, UdpHost).engine().max_packet_bytes() as u128
        )),
        "UdpHostTransferState" => data(
            obj!(h, 0, UdpHost)
                .engine()
                .transfer_state(PeerId(d.at(0)?.n()?))
                .map_or(V::num(0), |s| {
                    nums([s.window as u128, s.in_flight as u128, s.queued as u128])
                }),
        ),
        "UdpHostPoll" => {
            crate::engine::actions(obj!(h, 0, UdpHost).poll(d.at(0)?.u()?).map_err(err)?)
        }
        "UdpHostRead" => crate::engine::actions(
            obj!(h, 0, UdpHost)
                .step(mesa_core::Event::Read {
                    id: d.at(0)?.u()?,
                    peer: PeerId(d.at(1)?.n()?),
                    path: d.at(2)?.bar()?.to_vec(),
                })
                .map_err(err)?,
        ),
        "UdpHostBind" => crate::engine::actions(
            obj!(h, 0, UdpHost)
                .step(mesa_core::Event::Bind {
                    path: d.at(0)?.bar()?.to_vec(),
                    bytes: d.at(1)?.bar()?.to_vec(),
                    access: mesa_core::Access::Public,
                })
                .map_err(err)?,
        ),
        "Relay" => output(Object::Relay(
            transport::relay::Relay::new(transport::relay::Config {
                peers: [PeerId(d.at(0)?.n()?), PeerId(d.at(1)?.n()?)],
                max_packet: [d.at(2)?.size()?, d.at(3)?.size()?],
                wire: crate::primitives::wire_limits(d.at(4)?)?,
                max_interests: d.at(5)?.size()?,
                interest_ttl: d.at(6)?.u()?,
            })
            .map_err(err)?,
        )),
        "RelayPending" => data(V::num(obj!(h, 0, Relay).pending() as u128)),
        "RelayTimer" => data(
            obj!(h, 0, Relay)
                .next_timer()
                .map_or(V::num(0), |t| nums([t as u128])),
        ),
        "RelayAdvanceTo" => {
            obj!(h, 0, Relay).advance_to(d.at(0)?.u()?).map_err(err)?;
            data(V::num(0))
        }
        "RelayDisconnect" => {
            obj!(h, 0, Relay).disconnect(leg(d.at(0)?)?);
            data(V::num(0))
        }
        "RelayReceive" => data(
            match obj!(h, 0, Relay).receive(leg(d.at(0)?)?, d.at(1)?.bar()?) {
                transport::relay::Forward::Drop(e) => V::row([V::num(0), V::text(err(e))]),
                transport::relay::Forward::Send { leg, bytes } => V::row([
                    V::num(1),
                    V::num(match leg {
                        transport::relay::Leg::A => 0,
                        transport::relay::Leg::B => 1,
                    }),
                    V::bytes(bytes),
                ]),
            },
        ),
        _ if op.starts_with("Host") => {
            let e = crate::flow::event(&op[4..], h, d)?;
            batch(obj!(h, 0, Host).step(e).map_err(err)?)
        }
        _ if op.starts_with("UdpHost") => {
            let event = crate::engine::event(&format!("Engine{}", &op[7..]), h, d)?;
            crate::engine::actions(obj!(h, 0, UdpHost).step(event).map_err(err)?)
        }
        _ => return Err("unknown transport operation".into()),
    })
}
