//! Urbit interoperability acceptance executable with PUBLIC, deterministic test seeds. Never use
//! these identities/keys for real data. Separate processes share no verifier state.
use mesa_core::{
    policy::TransferPolicy, protection::SignedPublic, Access, Action, Attachment, Config, Engine,
    Event, KeyHandle, PeerId, PreconfiguredPeer, ReadFailure,
};
use mesa_crypto::KeyPair;
use mesa_udp::UdpHost;
use std::{
    collections::BTreeMap,
    net::UdpSocket,
    time::{Duration, Instant},
};

fn payload(peer: u8, length: usize) -> Vec<u8> {
    (0..length)
        .map(|i| {
            if i % 29 >= 24 {
                0
            } else {
                ((i + peer as usize) % 251) as u8
            }
        })
        .collect()
}
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = std::env::args().collect();
    if args.len() != 6 {
        return Err(
            "usage: interop-peer <1|2> <loopback-bind> <loopback-peer> <bytes> <complete|timeout|cancel>"
                .into(),
        );
    }
    let id: u8 = args[1].parse()?;
    if id != 1 && id != 2 {
        return Err("peer must be 1 or 2".into());
    }
    let remote = 3 - id;
    let bind: std::net::SocketAddr = args[2].parse()?;
    let address: std::net::SocketAddr = args[3].parse()?;
    if !bind.ip().is_loopback() || !address.ip().is_loopback() {
        return Err("demo requires loopback addresses".into());
    }
    let length: usize = args[4].parse()?;
    if length > 64 * 1024 {
        return Err("interop limit is 64 KiB".into());
    }
    let local = PreconfiguredPeer::new(PeerId(id.into()), "gw-btc", 1, 0, KeyHandle(id.into()))?;
    let peer = PreconfiguredPeer::new(
        PeerId(remote.into()),
        "gw-btc",
        1,
        0,
        KeyHandle(remote.into()),
    )?;
    let protection = SignedPublic::new(
        local.key,
        KeyPair::from_seed(&[id; 32]),
        BTreeMap::from([(peer.key, KeyPair::from_seed(&[remote; 32]).public_key())]),
    );
    let config = Config {
        read_timeout: 5000,
        max_attempts: 200,
        ..Config::default()
    };
    let mode = args[5].as_str();
    if !["complete", "timeout", "cancel"].contains(&mode) {
        return Err("invalid outcome".into());
    }
    let policy = TransferPolicy::fixed(&config);
    let mut engine = Engine::with_policy(config, local, protection, policy)?;
    engine.step(Event::InstallPeer(peer.clone()))?;
    engine.step(Event::Bind {
        path: b"example/value".into(),
        bytes: payload(id, length),
        access: Access::Public,
    })?;
    let mut host = UdpHost::new(
        engine,
        UdpSocket::bind(bind)?,
        BTreeMap::from([(peer.id, (Attachment(1), address))]),
    )?;
    host.step(Event::Read {
        id: 1,
        peer: peer.id,
        path: b"example/value".into(),
    })?;
    let start = Instant::now();
    let mut completed = None;
    let mut cancelled = false;
    while start.elapsed() < Duration::from_secs(8) {
        let mut actions = host.poll(start.elapsed().as_millis() as u64)?;
        if mode == "cancel" && !cancelled && start.elapsed() >= Duration::from_millis(250) {
            actions.extend(host.step(Event::Cancel { id: 1 })?);
            cancelled = true;
        }
        for action in actions {
            if let Action::ReadFinished { result, .. } = action {
                if mode != "complete" {
                    let expected = if mode == "cancel" {
                        ReadFailure::Cancelled
                    } else {
                        ReadFailure::Timeout
                    };
                    if completed.is_some() || result != Err(expected) {
                        return Err(format!("unexpected read outcome: {result:?}").into());
                    }
                    println!("peer={id} outcome={mode} delivered=0");
                    completed = Some(start.elapsed());
                    continue;
                }
                let data = result.map_err(|e| format!("read failed: {e:?}"))?;
                if completed.is_some() || data != payload(remote, length) {
                    return Err("incorrect or repeated delivery".into());
                }
                let hash: String = mesa_crypto::sha256(&data)
                    .iter()
                    .map(|b| format!("{b:02x}"))
                    .collect();
                println!(
                    "peer={id} bytes={} sha256={hash} signature=verified lss=verified",
                    data.len()
                );
                completed = Some(start.elapsed());
            }
        }
        // Keep serving after our own read so the other process can finish.
        if completed.is_some_and(|at| start.elapsed() - at > Duration::from_millis(1500)) {
            break;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    if completed.is_none() {
        return Err("UDP demo deadline exceeded".into());
    }
    let usage = host.engine().usage();
    if usage.reads != 0
        || usage.pending_interests != 0
        || usage.reassembly_bytes != 0
        || usage.unverified_bytes != 0
    {
        return Err("read state leaked".into());
    }
    Ok(())
}
