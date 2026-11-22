//! The raw host embedding of libmesa (phases 1 to 5): an engine that holds no
//! seed of the caller and takes events in batches, one UDP endpoint for all
//! peers, the authority (`mesa-jael` over `mesa-pki`) and key custody
//! (`mesa-keys`).
//!
//! One actor is the caller of an engine, its endpoint, its authority and its
//! keys. libmesa requires one serialized caller, and a handle does not move
//! between actors. A waiting actor holds only the number of the socket.
use crate::{
    object::*,
    value::{Result, Value as V, err},
};
use chacha20poly1305::{
    Key, XChaCha20Poly1305, XNonce,
    aead::{Aead, KeyInit, Payload},
};
use mesa_core::{
    self as core, Attachment, KeyHandle, PeerId, PreconfiguredPeer, credentials::SendGate,
    policy::TransferPolicy, protection::SignedPublic,
};
use mesa_jael::{self as jael, Jael};
use mesa_keys::{self as keys, operations::Purpose, operations::Reply};
use mesa_pki as pki;
use mesa_transport::{Datagram, UdpEndpoint};
use std::{
    collections::BTreeMap,
    io::{Read, Write},
    net::{SocketAddr, ToSocketAddrs, UdpSocket},
    os::{fd::AsRawFd, unix::fs::OpenOptionsExt},
    path::Path,
};
use zeroize::Zeroizing;

/// The output of one batch stays below the reply limit of the bridge. A batch
/// stops after the event that passes this number, and the caller sends the
/// rest again.
const BATCH_BYTES: usize = 8 << 20;
const BATCH_EVENTS: usize = 256;
/// The one key object of a world. A new key is a new version of it.
const BACKEND: u128 = 1;
const OBJECT: u128 = 1;

fn slots(h: &mut Handles) -> impl Iterator<Item = Option<&mut Object>> {
    h.iter_mut().map(Option::as_mut)
}
macro_rules! want {
    ($o:expr, $v:ident) => {
        match $o {
            Some(Object::$v(x)) => x,
            _ => return Err(concat!("expected ", stringify!($v)).into()),
        }
    };
}
fn endpoint_of(o: Option<&mut Object>) -> Result<Option<&mut UdpEndpoint>> {
    match o {
        None => Ok(None),
        Some(Object::Endpoint(e)) => Ok(Some(&mut e.endpoint)),
        _ => Err("expected Endpoint".into()),
    }
}
fn empty(v: &V) -> bool {
    matches!(v, V::Nat(b) if b.is_empty())
}
fn optional(t: Option<u64>) -> V {
    t.map_or(V::num(0), |t| V::row([V::num(t as u128)]))
}
fn entropy() -> Result<Zeroizing<[u8; 32]>> {
    let mut out = Zeroizing::new([0u8; 32]);
    std::fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut out[..]))
        .map_err(err)?;
    Ok(out)
}
/// The bytes are in the file, and the name is in its folder, when this
/// function returns. A stop of the process leaves the old file or the new
/// file, and not a part of one.
fn persist(path: &str, bytes: &[u8], mode: u32) -> Result<()> {
    let target = Path::new(path);
    let folder = target.parent().ok_or("path has no folder")?;
    let partial = format!("{path}.partial");
    let written = (|| {
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(mode)
            .open(&partial)?;
        f.write_all(bytes)?;
        f.sync_all()?;
        drop(f);
        std::fs::rename(&partial, target)?;
        std::fs::File::open(folder)?.sync_all()
    })();
    if written.is_err() {
        let _ = std::fs::remove_file(&partial);
    }
    written.map_err(err)
}

/// A send goes to the endpoint in the same call that made it, under the send
/// gate of the engine. Thus a packet that an old credential made is not sent
/// after a change of credential. Without an endpoint, the caller gets the
/// bytes.
fn records(
    actions: Vec<core::Action>,
    gate: &SendGate,
    mut endpoint: Option<&mut UdpEndpoint>,
) -> (V, usize) {
    let mut bytes = 0;
    let rows = actions
        .into_iter()
        .map(|a| match (a, endpoint.as_deref_mut()) {
            (
                core::Action::Send {
                    peer,
                    attachment,
                    bytes,
                },
                Some(e),
            ) => {
                let head = [V::num(peer.0), V::num(attachment.0 as u128)];
                match e.send_fenced(&gate.packet(peer, attachment, bytes.into())) {
                    Ok(sent) => V::row(
                        [V::text("Sent")]
                            .into_iter()
                            .chain(head)
                            .chain([V::num(!sent as u128)]),
                    ),
                    Err(x) => V::row(
                        [V::text("Unsent")]
                            .into_iter()
                            .chain(head)
                            .chain([V::text(err(x))]),
                    ),
                }
            }
            (a, _) => {
                bytes += match &a {
                    core::Action::Send { bytes, .. }
                    | core::Action::ReadFinished {
                        result: Ok(bytes), ..
                    } => bytes.len(),
                    _ => 0,
                };
                crate::engine::core_action(a).0
            }
        })
        .collect::<Vec<_>>();
    (V::row(rows), bytes)
}
fn outcome<E: std::fmt::Debug>(
    r: std::result::Result<Vec<core::Action>, E>,
    gate: &SendGate,
    endpoint: Option<&mut UdpEndpoint>,
) -> (V, usize) {
    match r {
        Ok(actions) => {
            let (rows, bytes) = records(actions, gate, endpoint);
            (V::row([V::num(0), rows]), bytes)
        }
        Err(e) => (V::row([V::num(1), V::text(err(e))]), 0),
    }
}

fn address(v: &V) -> Result<SocketAddr> {
    v.str()?.parse().map_err(err)
}
enum Wire {
    SetRoute(PeerId, u64, u64, Option<SocketAddr>),
    Send(PeerId, Attachment, Vec<u8>),
    Receive(usize),
}
fn wire(c: &V) -> Result<Wire> {
    Ok(match c.at(0)?.str()? {
        "SetRoute" => Wire::SetRoute(
            PeerId(c.at(1)?.n()?),
            c.at(2)?.u()?,
            c.at(3)?.u()?,
            if empty(c.at(4)?) {
                None
            } else {
                Some(address(c.at(4)?)?)
            },
        ),
        "Send" => Wire::Send(
            PeerId(c.at(1)?.n()?),
            Attachment(c.at(2)?.u()?),
            c.at(3)?.bar()?.to_vec(),
        ),
        "Receive" => Wire::Receive(c.at(1)?.size()?),
        _ => return Err("unknown endpoint command".into()),
    })
}

fn candidate(v: &V) -> Result<pki::Candidate> {
    Ok(pki::Candidate {
        peer: v.at(0)?.n()?,
        domain: v.at(1)?.str()?.into(),
        life: v.at(2)?.u()?.try_into().map_err(err)?,
        rift: v.at(3)?.u()?.try_into().map_err(err)?,
        public_key: v.at(4)?.fixed()?,
        evidence: v.at(5)?.bar()?.to_vec().into(),
    })
}
enum Rule {
    Advance(u64),
    Register(String, u64, u64),
    Ready(String, bool, bool),
    Accept(u64, pki::Candidate),
    Block(u128, bool),
    Commit(pki::Token),
}
fn rule(c: &V) -> Result<Rule> {
    Ok(match c.at(0)?.str()? {
        "Advance" => Rule::Advance(c.at(1)?.u()?),
        "Register" => Rule::Register(c.at(1)?.str()?.into(), c.at(2)?.u()?, c.at(3)?.u()?),
        "Ready" => Rule::Ready(c.at(1)?.str()?.into(), c.at(2)?.bool()?, c.at(3)?.bool()?),
        "Accept" => Rule::Accept(c.at(1)?.u()?, candidate(c.at(2)?)?),
        "Block" => Rule::Block(c.at(1)?.n()?, c.at(2)?.bool()?),
        "Commit" => Rule::Commit(pki::Token {
            incarnation: c.at(1)?.n()?,
            revision: c.at(2)?.u()?,
        }),
        _ => return Err("unknown authority command".into()),
    })
}
/// `mesa-jael` holds the accepted identity of each peer, the domains and each
/// change of key. A point is accepted the one way that it has: `resolve`
/// makes a verification request, and a verifier completes it. Until a
/// verifier for a domain exists, the caller is the verifier of each domain
/// that it registered, and the bridge completes the request in the same
/// call. Thus a key that the caller names takes the same path, with the same
/// refusals, as evidence that a peer will send.
fn decide(j: &mut Jael, r: Rule) -> std::result::Result<(), pki::Error> {
    match r {
        // The time only resets the count of requests that mesa-jael takes in
        // each second. A clock of the host that goes back must not stop the
        // caller from naming a key: the count then resets later.
        Rule::Advance(now) => {
            let _ = j.advance(now);
        }
        Rule::Register(domain, expected, generation) => {
            j.register(&domain, expected, generation)?;
        }
        Rule::Ready(domain, available, ready) => {
            let provider = j.authority().provider(&domain)?;
            j.set_status(&domain, provider, pki::ProviderStatus { available, ready })?;
        }
        Rule::Accept(expected, c) => {
            if j.authority().revision(c.peer) != expected {
                return Err(pki::Error::Stale);
            }
            let domain = c.domain.clone();
            let ticket = j
                .resolve(c)?
                .into_iter()
                .find_map(|a| match a {
                    jael::Action::Verify { ticket, .. } => Some(ticket),
                    _ => None,
                })
                .ok_or(pki::Error::Busy)?;
            j.complete(
                &domain,
                ticket,
                jael::Verdict::Accept {
                    sponsor: None,
                    fief: None,
                },
            )?;
        }
        Rule::Block(peer, blocked) => {
            j.operator_block(peer, blocked)?;
        }
        Rule::Commit(token) => {
            j.commit(token)?;
        }
    }
    Ok(())
}
fn proposed(incarnation: u128, revision: u64, base: u64, checkpoint: &pki::Buffer) -> V {
    V::row([
        V::row([V::num(incarnation), V::num(revision as u128)]),
        V::num(base as u128),
        V::bytes(checkpoint.to_vec()),
    ])
}
fn reference(v: &V) -> Result<keys::KeyRef> {
    Ok(keys::KeyRef {
        backend: BACKEND,
        object: OBJECT,
        version: v.u()?,
    })
}
fn sealer(key: &[u8; 32]) -> XChaCha20Poly1305 {
    XChaCha20Poly1305::new(Key::from_slice(key))
}

pub fn dispatch(op: &str, h: &mut Handles, d: &V) -> Result<Output> {
    if h.iter().any(Option::is_none) {
        return Err("closed or moved handle".into());
    }
    Ok(match op {
        // The engine starts closed, with a key that nothing else holds. It
        // signs and sends nothing until `KeysSync` gives it the key of the
        // world. libmesa makes its own engines the same way.
        "EngineConfigured" => {
            let mut it = slots(h);
            let config = want!(it.next().flatten(), Config).clone();
            let policy = match it.next().flatten() {
                None => TransferPolicy::fixed(&config),
                Some(Object::Policy(p)) => *p,
                _ => return Err("expected Policy".into()),
            };
            let local =
                PreconfiguredPeer::new(PeerId(d.at(0)?.n()?), d.at(1)?.str()?, 1, 0, KeyHandle(0))
                    .map_err(err)?;
            let key = mesa_crypto::KeyPair::from_seed(&*entropy()?);
            let protection = SignedPublic::new(KeyHandle(0), key, BTreeMap::new());
            let mut engine =
                core::Engine::with_policy(config, local, protection, policy).map_err(err)?;
            engine.quiesce().map_err(err)?;
            output(Object::Engine(engine))
        }
        "EngineStep" => {
            let mut it = slots(h);
            let engine = want!(it.next().flatten(), Engine);
            let mut endpoint = endpoint_of(it.next().flatten())?;
            let list = d.at(0)?.list()?;
            if list.len() > BATCH_EVENTS {
                return Err("event batch limit".into());
            }
            let parts = list
                .iter()
                .map(|e| {
                    let tag = e.at(0)?.str()?;
                    if !matches!(
                        tag,
                        "Bind"
                            | "Unbind"
                            | "RetireBefore"
                            | "Read"
                            | "Cancel"
                            | "AdvanceTo"
                            | "Receive"
                            | "SetService"
                            | "ReplaceAttachment"
                    ) {
                        return Err(format!("unknown engine event {tag}"));
                    }
                    Ok((format!("Engine{tag}"), V::Row(e.list()?[1..].to_vec())))
                })
                .collect::<Result<Vec<_>>>()?;
            // All events are read before the first one runs: a malformed
            // event must not lose the results of the events before it.
            let events = parts
                .iter()
                .map(|(name, rest)| crate::engine::event(name, &mut vec![], rest))
                .collect::<Result<Vec<_>>>()?;
            let gate = engine.send_gate();
            let mut out = Vec::new();
            let mut total = 0;
            for event in events {
                let result = engine.step(event);
                let failed = result.is_err();
                let (row, bytes) = outcome(result, &gate, endpoint.as_deref_mut());
                out.push(row);
                total += bytes;
                if failed || total > BATCH_BYTES {
                    break;
                }
            }
            data(V::row([V::row(out), optional(engine.next_timer())]))
        }
        "Endpoint" => {
            let socket = UdpSocket::bind(address(d.at(0)?)?).map_err(err)?;
            let second = socket.try_clone().map_err(err)?;
            let endpoint =
                UdpEndpoint::new(socket, d.at(1)?.size()?, d.at(2)?.size()?).map_err(err)?;
            output(Object::Endpoint(Endpoint {
                endpoint,
                socket: second,
            }))
        }
        "EndpointSocket" => {
            let e = want!(slots(h).next().flatten(), Endpoint);
            data(V::row([
                V::text(e.endpoint.local_addr().map_err(err)?.to_string()),
                V::num(e.socket.as_raw_fd() as u128),
            ]))
        }
        "EndpointStep" => {
            let e = &mut want!(slots(h).next().flatten(), Endpoint).endpoint;
            // All commands are read before the first one runs: a receive
            // takes packets from the socket, and they must reach the caller.
            let commands = d
                .at(0)?
                .list()?
                .iter()
                .map(wire)
                .collect::<Result<Vec<_>>>()?;
            if commands.len() > BATCH_EVENTS {
                return Err("command batch limit".into());
            }
            data(V::row(commands.into_iter().map(|c| match c {
                Wire::SetRoute(peer, expected, generation, hop) => {
                    match e.set_route(peer, expected, generation, hop) {
                        Ok(()) => V::row([V::num(0)]),
                        Err(x) => V::row([V::num(1), V::text(err(x))]),
                    }
                }
                Wire::Send(peer, attachment, bytes) => match e.send(peer, attachment, &bytes) {
                    Ok(sent) => V::row([V::num(0), V::num(!sent as u128)]),
                    Err(x) => V::row([V::num(1), V::text(err(x))]),
                },
                Wire::Receive(attempts) => match e.receive_batch(attempts) {
                    Err(x) => V::row([V::num(1), V::text(err(x))]),
                    Ok(got) => {
                        let mut packets = Vec::new();
                        let mut discarded = 0u128;
                        let mut failure = V::num(0);
                        for item in got {
                            match item {
                                Ok(Datagram::Packet {
                                    peer,
                                    attachment,
                                    bytes,
                                }) => packets.push(V::row([
                                    V::num(peer.0),
                                    V::num(attachment.0 as u128),
                                    V::bytes(bytes),
                                ])),
                                Ok(Datagram::Discarded) => discarded += 1,
                                Ok(Datagram::Idle) => {}
                                Err(x) => failure = V::text(err(x)),
                            }
                        }
                        V::row([V::num(0), V::row(packets), V::num(discarded), failure])
                    }
                },
            })))
        }
        // 0: the time passed. 1: the socket can be read. 2: the socket is
        // closed or in error, and the caller must not wait on it again.
        "WaitReadable" => {
            let fd: i32 = d.at(0)?.u()?.try_into().map_err(err)?;
            let wait = d.at(1)?.u()?.min(60_000) as i32;
            let mut p = libc::pollfd {
                fd,
                events: libc::POLLIN,
                revents: 0,
            };
            // SAFETY: one live pollfd; poll reads and writes only that struct.
            let ready = unsafe { libc::poll(&mut p, 1, wait) };
            data(V::num(if ready <= 0 {
                0
            } else if p.revents & libc::POLLIN != 0 {
                1
            } else {
                2
            }))
        }
        "Resolve" => {
            let port: u16 = d.at(1)?.u()?.try_into().map_err(err)?;
            let found = (d.at(0)?.str()?, port).to_socket_addrs().map_err(err)?;
            data(V::row(found.take(16).map(|a| V::text(a.to_string()))))
        }
        "Authority" => {
            let incarnation = d.at(0)?.n()?;
            let limits = match d.at(2)? {
                l if empty(l) => pki::Limits::default(),
                l => pki::Limits {
                    domains: l.at(0)?.size()?,
                    peers: l.at(1)?.size()?,
                    history: l.at(2)?.size()?,
                    evidence_bytes: l.at(3)?.size()?,
                    checkpoint_bytes: l.at(4)?.size()?,
                },
            };
            let config = jael::Config {
                authority: limits,
                // The caller names a key for each of its peers at one time.
                dispatches: 256,
                ..Default::default()
            };
            output(Object::Authority(
                match d.at(1)? {
                    c if empty(c) => Jael::new(config, incarnation),
                    c => Jael::restore(config, incarnation, &c.bar()?.to_vec().into()),
                }
                .map_err(err)?,
            ))
        }
        // A command that the authority refuses ends the batch. Each proposal
        // replaces the one before it and holds all of its changes, so the
        // caller needs only the last: `pending`.
        "AuthorityStep" => {
            let a = want!(slots(h).next().flatten(), Authority);
            let rules = d
                .at(0)?
                .list()?
                .iter()
                .map(rule)
                .collect::<Result<Vec<_>>>()?;
            if rules.len() > 64 {
                return Err("command batch limit".into());
            }
            let mut out = Vec::new();
            for r in rules {
                let result = decide(a, r);
                let failed = result.is_err();
                out.push(match result {
                    Ok(()) => V::row([V::num(0)]),
                    Err(e) => V::row([V::num(1), V::text(err(e))]),
                });
                if failed {
                    break;
                }
            }
            let pending = a.authority().pending().map_or(V::num(0), |p| {
                proposed(
                    p.token.incarnation,
                    p.token.revision,
                    p.base_revision,
                    &p.checkpoint,
                )
            });
            data(V::row([V::row(out), pending]))
        }
        "AuthorityPeer" => {
            let a = want!(slots(h).next().flatten(), Authority).authority();
            let peer = d.at(0)?.n()?;
            let point = a.peer(peer).map_or(V::num(0), |p| {
                let c = &p.point.candidate;
                V::row([
                    V::text(&c.domain),
                    V::num(c.life as u128),
                    V::num(c.rift as u128),
                    V::bytes(c.public_key),
                    V::num(p.point.provider_generation as u128),
                ])
            });
            let policy = a.policy(peer).map_or(V::num(0), |p| {
                V::row(
                    [
                        p.ordinary(),
                        p.operator_blocked,
                        p.withdrawn,
                        p.domain_active,
                        p.ready,
                        p.committed,
                        p.revalidated,
                    ]
                    .map(|b| V::num(b as u128)),
                )
            });
            data(V::row([V::num(a.revision(peer) as u128), point, policy]))
        }
        "AuthoritySync" => {
            let mut it = slots(h);
            let a = want!(it.next().flatten(), Authority).authority();
            let engine = want!(it.next().flatten(), Engine);
            let mut endpoint = endpoint_of(it.next().flatten())?;
            let peers = d
                .at(0)?
                .list()?
                .iter()
                .map(V::n)
                .collect::<Result<Vec<_>>>()?;
            if peers.len() > BATCH_EVENTS {
                return Err("peer batch limit".into());
            }
            let gate = engine.send_gate();
            let rows = peers
                .into_iter()
                .map(|p| outcome(a.sync_peer(engine, p), &gate, endpoint.as_deref_mut()).0)
                .collect::<Vec<_>>();
            data(V::row([V::row(rows), optional(engine.next_timer())]))
        }
        "Keys" => {
            let config = keys::Config {
                peer: d.at(0)?.n()?,
                domain: d.at(1)?.str()?.into(),
                incarnation: d.at(2)?.n()?,
                host_epoch: d.at(3)?.n()?,
                history: 8,
                operations: 16,
                timeout: d.at(4)?.u()?,
                checkpoint_bytes: 1 << 20,
            };
            let activation = match d.at(5)? {
                r if empty(r) => keys::Activation::new(config),
                r => keys::Activation::restore(
                    config,
                    &r.at(0)?.bar()?.to_vec().into(),
                    r.at(1)?.u()?,
                ),
            }
            .map_err(err)?;
            output(Object::Keys(Keys {
                store: keys::SoftwareStore::new(8, 8).map_err(err)?,
                activation,
            }))
        }
        // The seed goes from the file to the store of the bridge. The caller
        // gets the public key only.
        "KeysImport" => {
            let k = want!(slots(h).next().flatten(), Keys);
            let path = d.at(0)?.str()?;
            let o = d.at(1)?;
            let (seed, created) = match std::fs::read(path).map(Zeroizing::new) {
                Ok(held) => {
                    let seed: [u8; 32] = held[..]
                        .try_into()
                        .map_err(|_| "the key file does not hold 32 bytes")?;
                    (Zeroizing::new(seed), false)
                }
                Err(e) if e.kind() == std::io::ErrorKind::NotFound && o.at(3)?.bool()? => {
                    let fresh = entropy()?;
                    persist(path, &fresh[..], 0o600)?;
                    (fresh, true)
                }
                Err(e) => return Err(err(e)),
            };
            let public = *mesa_crypto::KeyPair::from_seed(&seed)
                .public_key()
                .as_bytes();
            let config = k.activation.config();
            let point = pki::Candidate {
                peer: config.peer,
                domain: config.domain.clone(),
                life: o.at(1)?.u()?.try_into().map_err(err)?,
                rift: o.at(2)?.u()?.try_into().map_err(err)?,
                public_key: public,
                evidence: Default::default(),
            };
            let material = keys::Material::from_seed(point, &seed).map_err(err)?;
            k.store
                .import(reference(o.at(0)?)?, material)
                .map_err(err)?;
            data(V::row([V::bytes(public), V::num(created as u128)]))
        }
        "KeysStep" => {
            let mut it = slots(h);
            let k = want!(it.next().flatten(), Keys);
            let a = want!(it.next().flatten(), Authority).authority();
            let token = |v: &V| -> Result<keys::Token> {
                Ok(keys::Token {
                    incarnation: v.at(1)?.n()?,
                    revision: v.at(2)?.u()?,
                })
            };
            let offer = |p: keys::Proposal| {
                proposed(
                    p.token.incarnation,
                    p.token.revision,
                    p.base_revision,
                    &p.checkpoint,
                )
            };
            data(match d.at(0)?.str()? {
                "Prepare" => offer(
                    k.activation
                        .prepare(a, &k.store, reference(d.at(1)?)?, None)
                        .map_err(err)?,
                ),
                "Commit" => {
                    k.activation.commit(a, &k.store, token(d)?).map_err(err)?;
                    V::num(0)
                }
                "Retire" => offer(k.activation.retire(a, reference(d.at(1)?)?).map_err(err)?),
                "Delete" => {
                    k.activation
                        .delete_retired(&mut k.store, reference(d.at(1)?)?)
                        .map_err(err)?;
                    V::num(0)
                }
                "Snapshot" => {
                    let s = k.activation.snapshot();
                    V::row([
                        V::num(match s.phase {
                            keys::Phase::Quiescent => 0,
                            keys::Phase::Prepared => 1,
                            keys::Phase::Committed => 2,
                            keys::Phase::Active => 3,
                        }),
                        V::num(s.manifest.revision as u128),
                        s.manifest
                            .active
                            .map_or(V::num(0), |c| V::num(c.reference.version as u128)),
                        s.pending.map_or(V::num(0), |t| {
                            V::row([V::num(t.incarnation), V::num(t.revision as u128)])
                        }),
                        optional(s.deadline),
                    ])
                }
                _ => return Err("unknown keys command".into()),
            })
        }
        "KeysSync" => {
            let mut it = slots(h);
            let k = want!(it.next().flatten(), Keys);
            let a = want!(it.next().flatten(), Authority).authority();
            let engine = want!(it.next().flatten(), Engine);
            let endpoint = endpoint_of(it.next().flatten())?;
            let gate = engine.send_gate();
            let result = k.activation.sync_core(a, &k.store, engine);
            let (row, _) = outcome(result, &gate, endpoint);
            data(V::row([row, optional(engine.next_timer())]))
        }
        "KeysAgree" => {
            let mut it = slots(h);
            let k = want!(it.next().flatten(), Keys);
            let a = want!(it.next().flatten(), Authority).authority();
            let request = k
                .activation
                .operation(
                    a,
                    &k.store,
                    Purpose::Agreement {
                        peer: d.at(0)?.n()?,
                    },
                )
                .map_err(err)?;
            let made = k.store.execute(&request).map_err(err)?;
            match k
                .activation
                .complete(a, request.ticket(), made)
                .map_err(err)?
            {
                Reply::Agreement(s) => output(Object::SecretKey(Zeroizing::new(*s.as_bytes()))),
                Reply::Signature(_) => return Err("expected an agreement".into()),
            }
        }
        "SecretDerive" => {
            let key = want!(slots(h).next().flatten(), SecretKey);
            output(Object::SecretKey(Zeroizing::new(
                *blake3::keyed_hash(key, d.at(0)?.bar()?).as_bytes(),
            )))
        }
        "SecretHash" => {
            let key = want!(slots(h).next().flatten(), SecretKey);
            data(V::bytes(
                blake3::keyed_hash(key, d.at(0)?.bar()?).as_bytes(),
            ))
        }
        "SecretSeal" => {
            let key = want!(slots(h).next().flatten(), SecretKey);
            let nonce: [u8; 24] = d.at(0)?.fixed()?;
            data(V::bytes(
                sealer(key)
                    .encrypt(
                        XNonce::from_slice(&nonce),
                        Payload {
                            msg: d.at(2)?.bar()?,
                            aad: d.at(1)?.bar()?,
                        },
                    )
                    .map_err(err)?,
            ))
        }
        // 0 when the bytes were not sealed with this key, nonce and name.
        "SecretOpen" => {
            let key = want!(slots(h).next().flatten(), SecretKey);
            let nonce: [u8; 24] = d.at(0)?.fixed()?;
            data(
                sealer(key)
                    .decrypt(
                        XNonce::from_slice(&nonce),
                        Payload {
                            msg: d.at(2)?.bar()?,
                            aad: d.at(1)?.bar()?,
                        },
                    )
                    .map_or(V::num(0), |m| V::row([V::bytes(m)])),
            )
        }
        "Persist" => {
            persist(d.at(0)?.str()?, d.at(1)?.bar()?, 0o644)?;
            data(V::num(0))
        }
        // The file is gone, and its folder knows it, when this returns. A
        // file that is not there is not an error: the caller may ask again
        // after a stop.
        "Remove" => {
            let target = Path::new(d.at(0)?.str()?);
            match std::fs::remove_file(target) {
                Ok(()) => {}
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(err(e)),
            }
            std::fs::File::open(target.parent().ok_or("path has no folder")?)
                .and_then(|f| f.sync_all())
                .map_err(err)?;
            data(V::num(0))
        }
        _ => return Err("unknown host operation".into()),
    })
}
