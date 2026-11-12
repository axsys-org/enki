use crate::{
    obj,
    object::*,
    value::{err, nums, Result, Value as V},
};
use mesa_identity::suite_c;
use mesa_noun::{atom::Atom, bits::Bits as View, mat, serial};
use mesa_wire::{self as wire, legacy_v1 as v1};
use zeroize::Zeroizing;

fn serial_limits(d: &V) -> Result<serial::Limits> {
    let mut l = serial::Limits::default();
    let a = d.list()?;
    if !a.is_empty() {
        if a.len() != 6 {
            return Err("noun limits require six fields".into());
        }
        l = serial::Limits {
            max_input_bits: a[0].size()?,
            max_output_bits: a[1].size()?,
            max_nodes: a[2].size()?,
            max_depth: a[3].size()?,
            max_atom_bits: a[4].size()?,
            max_backrefs: a[5].size()?,
        };
    }
    Ok(l)
}
pub fn wire_limits(d: &V) -> Result<wire::Limits> {
    if d.list()?.is_empty() {
        Ok(wire::Limits::DATAGRAM)
    } else {
        Ok(wire::Limits {
            max_packet: d.at(0)?.size()?,
            max_path: d.at(1)?.size()?,
            max_fragment: d.at(2)?.size()?,
        })
    }
}
fn identity_limits(d: &V) -> Result<suite_c::Limits> {
    if d.list()?.is_empty() {
        Ok(suite_c::Limits::default())
    } else {
        Ok(suite_c::Limits {
            max_blob_bytes: d.at(0)?.size()?,
            max_dat_bits: d.at(1)?.size()?,
            max_evidence_bits: d.at(2)?.size()?,
        })
    }
}
fn bits(v: View<'_>) -> Result<Bits> {
    let a = v.to_atom(v.len().div_ceil(8)).map_err(err)?;
    let mut bytes = a.as_le_bytes().to_vec();
    bytes.resize(v.len().div_ceil(8), 0);
    Ok(Bits {
        bytes,
        len: v.len(),
    })
}
fn encoded(e: mat::Encoded) -> Bits {
    Bits {
        bytes: e.bytes,
        len: e.bit_len,
    }
}
fn checksum(n: u64) -> Result<v1::Checksum> {
    match n {
        0 => Ok(v1::Checksum::HoonAtom),
        1 => Ok(v1::Checksum::VereBytes),
        _ => Err("checksum mode".into()),
    }
}
fn fragment(f: wire::Fragment) -> V {
    match f {
        wire::Fragment::Init => nums([0, 0]),
        wire::Fragment::Auth(n) => nums([1, n as u128]),
        wire::Fragment::Data(n) => nums([2, n as u128]),
    }
}
fn name(n: wire::Name<'_>) -> V {
    V::row([
        V::num(n.publisher.0),
        V::text(n.domain.as_str()),
        V::num(n.rift as u128),
        V::num(n.bloq as u128),
        fragment(n.fragment),
        V::bytes(n.path),
    ])
}
fn auth(a: wire::Auth<'_>) -> V {
    match a {
        wire::Auth::None => V::row([V::num(0), V::bytes([])]),
        wire::Auth::Signature(b) => V::row([V::num(1), V::bytes(b)]),
        wire::Auth::Mac(b) => V::row([V::num(2), V::bytes(b)]),
        wire::Auth::Pair(b) => V::row([V::num(3), V::bytes(b)]),
    }
}
fn pdata(d: wire::Data<'_>) -> V {
    V::row([
        V::num(d.total_bytes as u128),
        auth(d.auth),
        V::bytes(d.fragment),
    ])
}
fn parse_name(d: &V) -> Result<wire::Name<'_>> {
    if d.at(5)?.bar()?.len() > u16::MAX as usize {
        return Err("name path limit".into());
    }
    let f = d.at(4)?;
    Ok(wire::Name {
        publisher: wire::PeerId(d.at(0)?.n()?),
        domain: wire::DomainTag::new(d.at(1)?.str()?).map_err(err)?,
        rift: d.at(2)?.u()?.try_into().map_err(err)?,
        bloq: d.at(3)?.u()?.try_into().map_err(err)?,
        fragment: match f.at(0)?.u()? {
            0 => wire::Fragment::Init,
            1 => wire::Fragment::Auth(f.at(1)?.u()?),
            2 => wire::Fragment::Data(f.at(1)?.u()?),
            _ => return Err("fragment tag".into()),
        },
        path: d.at(5)?.bar()?,
    })
}
fn parse_data(d: &V) -> Result<wire::Data<'_>> {
    let a = d.at(1)?;
    let b = a.at(1)?.bar()?;
    Ok(wire::Data {
        total_bytes: d.at(0)?.u()?,
        auth: match a.at(0)?.u()? {
            0 => wire::Auth::None,
            1 => wire::Auth::Signature(b),
            2 => wire::Auth::Mac(b.try_into().map_err(err)?),
            3 => wire::Auth::Pair(b.try_into().map_err(err)?),
            _ => return Err("auth tag".into()),
        },
        fragment: d.at(2)?.bar()?,
    })
}
pub fn dispatch(op: &str, h: &mut Handles, d: &V) -> Result<Output> {
    Ok(match op {
        "Buffer" => output(Object::Buffer(d.at(0)?.bar()?.to_vec())),
        "BufferBytes" => data(V::bytes(obj!(h, 0, Buffer))),
        "BufferSlice" => {
            let start = d.at(0)?.size()?;
            let len = d.at(1)?.size()?;
            let b = obj!(h, 0, Buffer)
                .get(start..start.checked_add(len).ok_or("slice overflow")?)
                .ok_or("buffer range")?;
            output(Object::Buffer(b.to_vec()))
        }
        "Atom" => output(Object::Atom(Atom::from_le_bytes(d.at(0)?.bar()?.to_vec()))),
        "AtomBytes" => data(V::bytes(obj!(h, 0, Atom).as_le_bytes())),
        "AtomSize" => data(V::num(obj!(h, 0, Atom).met3() as u128)),
        "Bits" => {
            let bytes = d.at(0)?.bar()?.to_vec();
            let len = d.at(1)?.size()?;
            View::new(&bytes, len).map_err(err)?;
            output(Object::Bits(Bits { bytes, len }))
        }
        "BitsFromAtom" => output(Object::Bits(bits(
            View::from_atom_bytes(obj!(h, 0, Atom).as_le_bytes()).map_err(err)?,
        )?)),
        "BitsSize" => data(V::num(obj!(h, 0, Bits).len as u128)),
        "BitsBit" => {
            let b = obj!(h, 0, Bits).view()?.bit(d.at(0)?.size()?);
            data(match b {
                None => V::num(0),
                Some(b) => V::row([V::num(b as u128)]),
            })
        }
        "BitsSlice" => output(Object::Bits(bits(
            obj!(h, 0, Bits)
                .view()?
                .slice(d.at(0)?.size()?, d.at(1)?.size()?)
                .map_err(err)?,
        )?)),
        "BitsAtom" => output(Object::Atom(
            obj!(h, 0, Bits)
                .view()?
                .to_atom(d.at(0)?.size()?)
                .map_err(err)?,
        )),
        "Mat" => output(Object::Bits(encoded(
            mat::mat(obj!(h, 0, Atom), d.at(0)?.size()?).map_err(err)?,
        ))),
        "Rub" | "MatDecode" => {
            let b = obj!(h, 0, Bits).view()?;
            let r = if op == "Rub" {
                mat::rub(b, d.at(0)?.size()?)
            } else {
                mat::decode(b, d.at(0)?.size()?)
            }
            .map_err(err)?;
            (
                V::num(r.consumed as u128),
                vec![Object::Bits(bits(r.atom)?)],
            )
        }
        "Mug" => data(V::num(mesa_noun::mug::mug_bytes(d.at(0)?.bar()?) as u128)),
        "Cue" => {
            let b = obj!(h, 0, Bits);
            output(Object::Noun(Noun::new(
                b.bytes.clone(),
                b.len,
                serial_limits(d)?,
            )?))
        }
        "Jam" => output(Object::Bits(encoded(
            serial::jam(&obj!(h, 0, Noun).noun, serial_limits(d)?).map_err(err)?,
        ))),
        "NounRoot" => data(V::num(obj!(h, 0, Noun).noun.root() as u128)),
        "NounCount" => data(V::num(obj!(h, 0, Noun).noun.node_count() as u128)),
        "NounNode" => match obj!(h, 0, Noun).noun.node(d.at(0)?.size()?) {
            None => data(V::num(0)),
            Some(serial::Node::Cell { head, tail }) => data(nums([2, head as u128, tail as u128])),
            Some(serial::Node::Atom(b)) => (V::num(1), vec![Object::Bits(bits(b)?)]),
        },
        "Sha256" => data(V::bytes(mesa_crypto::sha256(d.at(0)?.bar()?))),
        "KeyFromSeed" => output(Object::Key(mesa_crypto::KeyPair::from_seed(
            &d.at(0)?.fixed()?,
        ))),
        "PublicKey" => output(Object::Public(
            mesa_crypto::PublicKey::from_bytes(d.at(0)?.fixed()?).map_err(err)?,
        )),
        "KeyPublic" => output(Object::Public(obj!(h, 0, Key).public_key())),
        "PublicBytes" => data(V::bytes(obj!(h, 0, Public).as_bytes())),
        "PublicClone" => output(Object::Public(obj!(h, 0, Public).clone())),
        "Sign" => data(V::bytes(obj!(h, 0, Key).sign(d.at(0)?.bar()?))),
        "Verify" => {
            obj!(h, 0, Public)
                .verify(d.at(0)?.bar()?, &d.at(1)?.fixed()?)
                .map_err(err)?;
            data(V::num(0))
        }
        "PublicAddScalar" => output(Object::Public(
            obj!(h, 0, Public)
                .add_scalar(d.at(0)?.fixed()?)
                .map_err(err)?,
        )),
        "Agree" => {
            let p = obj!(h, 1, Public).clone();
            output(Object::Secret(obj!(h, 0, Key).agree(&p).map_err(err)?))
        }
        "SecretBytes" => data(V::bytes(obj!(h, 0, Secret).as_bytes())),
        "Tree" => output(Object::Tree(
            mesa_lss::Tree::build(d.at(0)?.bar()?).map_err(err)?,
        )),
        "TreeRoot" => data(V::bytes(obj!(h, 0, Tree).root())),
        "TreeProof" => data(V::bytes(obj!(h, 0, Tree).proof())),
        "TreePair" => data(match obj!(h, 0, Tree).pair(d.at(0)?.size()?) {
            None => V::num(0),
            Some(p) => V::row(p.iter().map(V::bytes)),
        }),
        "TreeSize" => data(V::num(obj!(h, 0, Tree).storage_bytes() as u128)),
        "ProofHashes" => data(V::num(
            mesa_lss::proof_hashes(d.at(0)?.size()?).map_err(err)? as u128,
        )),
        "ExpectsPair" => data(V::num(
            mesa_lss::expects_pair(d.at(0)?.size()?, d.at(1)?.size()?) as u128,
        )),
        "Verifier" => output(Object::Verifier(
            mesa_lss::Verifier::new(d.at(0)?.size()?, d.at(1)?.bar()?).map_err(err)?,
        )),
        "VerifierRoot" => data(V::bytes(obj!(h, 0, Verifier).root())),
        "VerifierState" => {
            let v = obj!(h, 0, Verifier);
            data(nums([v.next_index() as u128, v.complete() as u128]))
        }
        "VerifierIngest" => {
            let p = d.at(1)?.list()?;
            let pair = if p.is_empty() {
                None
            } else {
                Some([p[0].fixed()?, p.get(1).ok_or("pair length")?.fixed()?])
            };
            obj!(h, 0, Verifier)
                .ingest(d.at(0)?.bar()?, pair.as_ref())
                .map_err(err)?;
            data(V::num(0))
        }
        "Pass" | "Ring" => {
            let p = Pass {
                bytes: Zeroizing::new(d.at(0)?.bar()?.to_vec()),
                domain: d.at(1)?.str()?.to_owned(),
                bound: false,
                ring: op == "Ring",
                limits: identity_limits(d.at(2)?)?,
            };
            if p.ring {
                p.ring()?;
            } else {
                p.pass()?;
            }
            output(Object::Pass(p))
        }
        "PassBind" => {
            let p = obj!(h, 0, Pass);
            suite_c::crypto::BoundPass::new(p.pass()?).map_err(err)?;
            output(Object::Pass(Pass {
                bytes: Zeroizing::new(p.bytes.to_vec()),
                domain: p.domain.clone(),
                bound: true,
                ring: false,
                limits: p.limits,
            }))
        }
        "PassKeys" => {
            let p = obj!(h, 0, Pass).pass()?;
            data(V::row([V::bytes(p.genesis_key()), V::bytes(p.live_key())]))
        }
        "RingKeys" => {
            let p = obj!(h, 0, Pass).ring()?;
            data(V::row([V::bytes(p.genesis_key()), V::bytes(p.live_seed())]))
        }
        "PassClaims" => {
            let p = obj!(h, 0, Pass);
            fn claims(c: &suite_c::Claims<'_>) -> Result<Output> {
                Ok((
                    V::text(c.domain().as_str()),
                    vec![
                        Object::Bits(bits(c.dat())?),
                        Object::Bits(bits(c.domain_tail())?),
                        Object::Bits(bits(c.evidence())?),
                    ],
                ))
            }
            if p.ring {
                claims(p.ring()?.claims())?
            } else {
                claims(p.pass()?.claims())?
            }
        }
        "BoundName" | "BoundKeys" | "BoundRequireName" => {
            let p = obj!(h, 0, Pass);
            if !p.bound {
                return Err("expected BoundPass".into());
            }
            let b = suite_c::crypto::BoundPass::new(p.pass()?).map_err(err)?;
            match op {
                "BoundName" => data(V::num(b.name().0)),
                "BoundKeys" => (
                    V::num(0),
                    vec![
                        Object::Public(b.name_key().clone()),
                        Object::Public(b.live_key().clone()),
                    ],
                ),
                _ => {
                    b.require_name(wire::PeerId(d.at(0)?.n()?)).map_err(err)?;
                    data(V::num(0))
                }
            }
        }
        "CheckRing" => {
            if h.len() < 2 {
                return Err("expected pass and ring".into());
            }
            let (a, b) = h.split_at_mut(1);
            let p = match a[0].as_ref() {
                Some(Object::Pass(p)) if p.bound => p,
                _ => return Err("expected BoundPass".into()),
            };
            let r = match b.first().and_then(Option::as_ref) {
                Some(Object::Pass(r)) => r,
                _ => return Err("expected Ring".into()),
            };
            suite_c::crypto::BoundPass::new(p.pass()?)
                .map_err(err)?
                .check_ring(&r.ring()?)
                .map_err(err)?;
            data(V::num(0))
        }
        "SiftHead" | "SiftV1Head" => {
            let b = d.at(0)?.bar()?;
            let head = if op == "SiftHead" {
                wire::sift_head(b)
            } else {
                v1::sift_head(b)
            };
            data(head.map_or(V::num(0), |h| {
                nums([h.nex as u128, h.typ as u128, h.hop as u128, h.gum as u128])
            }))
        }
        "V1Checksum" => data(V::num(checksum(d.at(0)?.u()?)?.gum(d.at(1)?.bar()?) as u128)),
        "ReconstructFragment" => {
            let max = d.at(4)?.size()?;
            if max > crate::value::LIMIT {
                return Err("fragment limit".into());
            }
            data(V::bytes(
                wire::reconstruct_data_fragment(
                    d.at(0)?.u()?,
                    d.at(1)?.u()?.try_into().map_err(err)?,
                    d.at(2)?.u()?,
                    d.at(3)?.bar()?,
                    max,
                )
                .map_err(err)?,
            ))
        }
        "RootContext" => {
            let peer = obj!(h, 0, Peer);
            data(V::bytes(mesa_core::protection::root_context(
                peer,
                parse_name(d.at(0)?)?,
                d.at(1)?.u()?,
                &d.at(2)?.fixed()?,
            )))
        }
        "ProtectionLocalKey" | "ProtectionPeerKey" => {
            use mesa_core::Protection;
            let peer = obj!(h, 1, Peer).clone();
            let p = obj!(h, 0, Protection);
            data(V::num(if op == "ProtectionLocalKey" {
                p.local_key_available(&peer)
            } else {
                p.peer_key_available(&peer)
            } as u128))
        }
        "ProtectionSignRoot" => {
            use mesa_core::Protection;
            let peer = obj!(h, 1, Peer).clone();
            data(
                obj!(h, 0, Protection)
                    .sign_root(
                        &peer,
                        parse_name(d.at(0)?)?,
                        d.at(1)?.u()?,
                        &d.at(2)?.fixed()?,
                    )
                    .map_or(V::num(0), |s| V::row([V::bytes(s)])),
            )
        }
        "ProtectionVerifyRoot" => {
            use mesa_core::Protection;
            let peer = obj!(h, 1, Peer).clone();
            data(V::num(obj!(h, 0, Protection).verify_root(
                &peer,
                parse_name(d.at(0)?)?,
                d.at(1)?.u()?,
                &d.at(2)?.fixed()?,
                &d.at(3)?.fixed()?,
            ) as u128))
        }
        "ProtectionVerify" => {
            use mesa_core::Protection;
            let local = obj!(h, 1, Peer).clone();
            let remote = obj!(h, 2, Peer).clone();
            data(V::num(
                obj!(h, 0, Protection).verify(&local, &remote, d.at(0)?.bar()?) as u128,
            ))
        }
        "PacketV1" => return super::wire_legacy::construct(d),
        "Domain" => data(V::text(
            wire::DomainTag::new(d.at(0)?.str()?).map_err(err)?.as_str(),
        )),
        "IsMesa" => data(V::num(wire::is_mesa(d.at(0)?.bar()?) as u128)),
        "WireDecode" | "WireV1Decode" => {
            let bytes = d.at(0)?.bar()?.to_vec();
            let limits = wire_limits(d.at(1)?)?;
            let legacy = if op == "WireV1Decode" {
                let c = checksum(d.at(2)?.u()?)?;
                v1::decode(
                    &bytes,
                    c,
                    v1::Limits {
                        max_packet: limits.max_packet,
                        max_path: limits.max_path,
                        max_fragment: limits.max_fragment,
                    },
                )
                .map_err(err)?;
                Some(c)
            } else {
                wire::decode(&bytes, limits).map_err(err)?;
                None
            };
            output(Object::Packet(Packet {
                bytes,
                legacy,
                limits,
            }))
        }
        "WireEncode" => {
            let p = obj!(h, 0, Packet);
            let b = if let Some(c) = p.legacy {
                let l = v1::Limits {
                    max_packet: p.limits.max_packet,
                    max_path: p.limits.max_path,
                    max_fragment: p.limits.max_fragment,
                };
                v1::encode(&v1::decode(&p.bytes, c, l).map_err(err)?, c, l).map_err(err)?
            } else {
                wire::encode(&wire::decode(&p.bytes, p.limits).map_err(err)?, p.limits)
                    .map_err(err)?
            };
            data(V::bytes(b))
        }
        "PacketFields" => {
            let p = obj!(h, 0, Packet);
            if p.legacy.is_some() {
                return super::wire_legacy::fields(p);
            }
            let p = wire::decode(&p.bytes, p.limits).map_err(err)?;
            let body = match p.body {
                wire::Body::Peek(n) => V::row([V::num(0), name(n)]),
                wire::Body::Page {
                    name: n,
                    data: d,
                    next,
                } => V::row([
                    V::num(1),
                    name(n),
                    pdata(d),
                    next.map_or(V::num(0), |b| V::row([V::bytes(b)])),
                ]),
                wire::Body::Poke {
                    ack,
                    payload,
                    data: d,
                } => V::row([V::num(2), name(ack), name(payload), pdata(d)]),
            };
            data(V::row([V::num(p.hop as u128), body]))
        }
        "Packet" => {
            let hop = d.at(0)?.u()?.try_into().map_err(err)?;
            let b = d.at(1)?;
            let body = match b.at(0)?.u()? {
                0 => wire::Body::Peek(parse_name(b.at(1)?)?),
                1 => wire::Body::Page {
                    name: parse_name(b.at(1)?)?,
                    data: parse_data(b.at(2)?)?,
                    next: if b.at(3)?.list()?.is_empty() {
                        None
                    } else {
                        Some(b.at(3)?.at(0)?.fixed()?)
                    },
                },
                2 => wire::Body::Poke {
                    ack: parse_name(b.at(1)?)?,
                    payload: parse_name(b.at(2)?)?,
                    data: parse_data(b.at(3)?)?,
                },
                _ => return Err("body tag".into()),
            };
            let limits = wire_limits(d.at(2)?)?;
            let bytes = wire::encode(&wire::Packet { hop, body }, limits).map_err(err)?;
            output(Object::Packet(Packet {
                bytes,
                legacy: None,
                limits,
            }))
        }
        _ => return Err("unknown primitive".into()),
    })
}
