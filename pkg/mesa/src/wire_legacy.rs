use crate::{
    object::{data, Output, Packet},
    value::{err, nums, Result, Value as V},
};
use mesa_wire::legacy_v1 as w;
fn name(n: w::Name<'_>) -> V {
    let f = match n.fragment {
        w::Fragment::Init => nums([0, 0]),
        w::Fragment::Auth(i) => nums([1, i as u128]),
        w::Fragment::Data(i) => nums([2, i as u128]),
    };
    V::row([
        V::num(n.ship),
        V::num(n.rift as u128),
        V::num(n.bloq as u128),
        f,
        V::bytes(n.path),
    ])
}
fn dat(d: w::Data<'_>) -> V {
    let a = match d.auth {
        w::Auth::None => V::row([V::num(0), V::bytes([])]),
        w::Auth::Signature(b) => V::row([V::num(1), V::bytes(b)]),
        w::Auth::Mac(b) => V::row([V::num(2), V::bytes(b)]),
        w::Auth::Pair(b) => V::row([V::num(3), V::bytes(b)]),
    };
    V::row([V::num(d.total_bytes as u128), a, V::bytes(d.fragment)])
}
pub fn fields(p: &Packet) -> Result<Output> {
    let p = w::decode(
        &p.bytes,
        p.legacy.unwrap(),
        w::Limits {
            max_packet: p.limits.max_packet,
            max_path: p.limits.max_path,
            max_fragment: p.limits.max_fragment,
        },
    )
    .map_err(err)?;
    let b = match p.body {
        w::Body::Peek(n) => V::row([V::num(0), name(n)]),
        w::Body::Page {
            name: n,
            data: d,
            next,
        } => V::row([
            V::num(1),
            name(n),
            dat(d),
            next.map_or(V::num(0), |b| V::row([V::bytes(b)])),
        ]),
        w::Body::Poke {
            ack,
            payload,
            data: d,
        } => V::row([V::num(2), name(ack), name(payload), dat(d)]),
    };
    Ok(data(V::row([V::num(p.hop as u128), b])))
}

fn parse_name(d: &V) -> Result<w::Name<'_>> {
    let f = d.at(3)?;
    Ok(w::Name {
        ship: d.at(0)?.n()?,
        rift: d.at(1)?.u()?.try_into().map_err(err)?,
        bloq: d.at(2)?.u()?.try_into().map_err(err)?,
        fragment: match f.at(0)?.u()? {
            0 => w::Fragment::Init,
            1 => w::Fragment::Auth(f.at(1)?.u()?),
            2 => w::Fragment::Data(f.at(1)?.u()?),
            _ => return Err("fragment tag".into()),
        },
        path: d.at(4)?.bar()?,
    })
}
fn parse_data(d: &V) -> Result<w::Data<'_>> {
    let a = d.at(1)?;
    let b = a.at(1)?.bar()?;
    Ok(w::Data {
        total_bytes: d.at(0)?.u()?,
        auth: match a.at(0)?.u()? {
            0 => w::Auth::None,
            1 => w::Auth::Signature(b.try_into().map_err(err)?),
            2 => w::Auth::Mac(b.try_into().map_err(err)?),
            3 => w::Auth::Pair(b.try_into().map_err(err)?),
            _ => return Err("auth tag".into()),
        },
        fragment: d.at(2)?.bar()?,
    })
}
pub fn construct(d: &V) -> Result<Output> {
    let hop = d.at(0)?.u()?.try_into().map_err(err)?;
    let b = d.at(1)?;
    let body = match b.at(0)?.u()? {
        0 => w::Body::Peek(parse_name(b.at(1)?)?),
        1 => w::Body::Page {
            name: parse_name(b.at(1)?)?,
            data: parse_data(b.at(2)?)?,
            next: if b.at(3)?.list()?.is_empty() {
                None
            } else {
                Some(b.at(3)?.at(0)?.fixed()?)
            },
        },
        2 => w::Body::Poke {
            ack: parse_name(b.at(1)?)?,
            payload: parse_name(b.at(2)?)?,
            data: parse_data(b.at(3)?)?,
        },
        _ => return Err("body tag".into()),
    };
    let limits = crate::primitives::wire_limits(d.at(2)?)?;
    let checksum = match d.at(3)?.u()? {
        0 => w::Checksum::HoonAtom,
        1 => w::Checksum::VereBytes,
        _ => return Err("checksum mode".into()),
    };
    let bytes = w::encode(
        &w::Packet { hop, body },
        checksum,
        w::Limits {
            max_packet: limits.max_packet,
            max_path: limits.max_path,
            max_fragment: limits.max_fragment,
        },
    )
    .map_err(err)?;
    Ok(crate::object::output(crate::object::Object::Packet(
        Packet {
            bytes,
            legacy: Some(checksum),
            limits,
        },
    )))
}
