//! Loopback-only reference host with public fixture keys. Fault gates support
//! the subprocess tests; application effects are durable inbox rows only.
#[path = "../../../mesa-flow/tests/support/mod.rs"]
mod fixture;
use fixture::{config, flows, payload, protection, view};
use mesa_core::Attachment;
use mesa_flow::{Action, Change, Checkpoint, Event, Node, Outcome};
use mesa_store::{Limits, Store};
use mesa_transport::{Batch, Capabilities, Host, PacketIo, Received, Stream, Udp};
use std::{
    collections::VecDeque,
    io,
    net::{SocketAddr, TcpListener, TcpStream, UdpSocket},
    path::Path,
    time::{Duration, Instant},
};

enum Lane {
    Udp(Udp),
    Tcp(Stream<TcpStream>),
}
impl PacketIo for Lane {
    fn capabilities(&self) -> Capabilities {
        match self {
            Self::Udp(v) => v.capabilities(),
            Self::Tcp(v) => v.capabilities(),
        }
    }
    fn send(&mut self, p: &[u8]) -> io::Result<bool> {
        match self {
            Self::Udp(v) => v.send(p),
            Self::Tcp(v) => v.send(p),
        }
    }
    fn receive(&mut self) -> io::Result<Received> {
        match self {
            Self::Udp(v) => v.receive(),
            Self::Tcp(v) => v.receive(),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        match self {
            Self::Udp(v) => v.flush(),
            Self::Tcp(v) => v.flush(),
        }
    }
}
fn lane(
    id: usize,
    mode: &str,
    bind: SocketAddr,
    remote: SocketAddr,
) -> Result<Lane, Box<dyn std::error::Error>> {
    if !bind.ip().is_loopback() || !remote.ip().is_loopback() {
        return Err("fixture keys require loopback".into());
    }
    if mode == "udp" {
        return Ok(Lane::Udp(Udp::new(UdpSocket::bind(bind)?, remote, 1472)?));
    }
    if mode != "tcp" {
        return Err("mode must be udp or tcp".into());
    }
    let deadline = Instant::now() + Duration::from_secs(10);
    let socket = if id == 0 {
        let listener = TcpListener::bind(bind)?;
        listener.set_nonblocking(true)?;
        loop {
            match listener.accept() {
                Ok((s, _)) => break s,
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => (),
                Err(e) => return Err(e.into()),
            }
            if Instant::now() > deadline {
                return Err("accept deadline".into());
            }
            std::thread::sleep(Duration::from_millis(5));
        }
    } else {
        loop {
            match TcpStream::connect_timeout(&remote, Duration::from_millis(100)) {
                Ok(s) => break s,
                Err(_) if Instant::now() < deadline => (),
                Err(e) => return Err(e.into()),
            }
            std::thread::sleep(Duration::from_millis(5));
        }
    };
    Ok(Lane::Tcp(Stream::tcp(socket, 1472, 65536)?))
}
fn mark(path: &str, value: &str) -> io::Result<()> {
    let temporary = format!("{path}.tmp");
    std::fs::write(&temporary, value)?;
    std::fs::rename(temporary, path)
}
fn pause(gate: &str, kind: &str, phase: &str, marker: &str, revision: u64) -> io::Result<()> {
    if gate != format!("{kind}/{phase}") {
        return Ok(());
    }
    mark(
        &format!("{marker}.gate"),
        &format!("{kind}/{phase}/{revision}"),
    )?;
    // Tests kill without running Drop, or release a stale writer after takeover.
    loop {
        if Path::new(&format!("{marker}.release")).exists() {
            return Ok(());
        }
        std::thread::park_timeout(Duration::from_millis(10));
    }
}
fn actions(
    host: &mut Host<Lane>,
    store: &mut Store,
    batch: Batch,
    gate: &str,
    marker: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    for notice in batch.notices {
        eprintln!("network: {notice:?}");
    }
    let mut queue: VecDeque<_> = batch.actions.into();
    for _ in 0..10000 {
        let Some(action) = queue.pop_front() else {
            return Ok(());
        };
        let more = match action {
            Action::Deliver { id, .. } => {
                let outcome = if id.sequence == 1 {
                    Outcome::Rejected(23)
                } else {
                    Outcome::Accepted
                };
                host.step(Event::Decide { id, outcome })?
            }
            Action::Commit(request) => {
                let kind = match request.change {
                    Change::Enqueue { .. } => "enqueue",
                    Change::Deliver { .. } => "deliver",
                    Change::Settle { ref id, .. }
                        if request
                            .checkpoint
                            .flow(id.flow)
                            .unwrap()
                            .batch_settled(request.checkpoint.batch_size) =>
                    {
                        "release"
                    }
                    Change::Settle { .. } => "settle",
                    Change::RetireReceived { .. } => "retire-received",
                    Change::RetireSent { .. } => "retire-sent",
                };
                let revision = request.checkpoint.revision;
                pause(gate, kind, "before", marker, revision)?;
                let prepared = store.prepare(&request)?;
                pause(gate, kind, "prepared", marker, revision)?;
                let token = prepared.commit()?;
                pause(gate, kind, "committed", marker, revision)?;
                let batch = host.step(Event::Committed(token))?;
                pause(gate, kind, "callback", marker, revision)?;
                batch
            }
            Action::Stalled { .. } | Action::AnnouncementStalled { .. } => {
                return Err(format!("unexpected stall: {action:?}").into())
            }
            _ => continue,
        };
        for notice in more.notices {
            eprintln!("network: {notice:?}");
        }
        queue.extend(more.actions);
    }
    Err("application action budget".into())
}
fn run(args: &[String]) -> Result<(), Box<dyn std::error::Error>> {
    let id: usize = args[1].parse()?;
    if id > 1 {
        return Err("peer must be 0 or 1".into());
    }
    let count: u64 = args.get(8).map_or(Ok(4), |s| s.parse())?;
    let batch_size: u64 = args.get(9).map_or(Ok(64), |s| s.parse())?;
    if count == 0 || count > 16384 || !(1..=1024).contains(&batch_size) {
        return Err("invalid count or batch size".into());
    }
    let gate = &args[6];
    let marker = &args[7];
    if gate != "none"
        && ![
            "enqueue",
            "deliver",
            "settle",
            "release",
            "retire-received",
            "retire-sent",
        ]
        .iter()
        .any(|kind| {
            ["before", "prepared", "committed", "callback"]
                .iter()
                .any(|phase| gate == &format!("{kind}/{phase}"))
        })
    {
        return Err("invalid fault gate".into());
    }
    let io = lane(id, &args[2], args[4].parse()?, args[5].parse()?)?;
    let mut store = Store::open(
        &args[3],
        &Checkpoint::empty_with_batch(&view(id), &flows(id), batch_size),
        Limits::default(),
    )?;
    let mut c = config();
    c.max_messages = batch_size;
    c.core.max_bindings = c.core.max_bindings.max(2 * batch_size as usize + 6);
    c.window = c.window.min(batch_size as usize);
    c.transfer.timers.read_timeout = 15000;
    c.transfer.recovery.max_attempts = 200;
    let node = Node::new(
        c,
        view(id),
        protection(id),
        flows(id),
        store.checkpoint()?,
        store.incarnation(),
    )?;
    let (mut host, initial) = Host::new(node, view(1 - id).id, Attachment(7), io)?;
    actions(&mut host, &mut store, initial, gate, marker)?;
    let start = Instant::now();
    let sizes = [0, 1024, 1025, 17425];
    let mut done = false;
    loop {
        let batch = host.poll(start.elapsed().as_millis() as u64)?;
        actions(&mut host, &mut store, batch, gate, marker)?;
        let flow = host.node().checkpoint().flow(7).unwrap();
        if flow.next_send < count
            && flow.outbox.len() < 4.min(batch_size as usize)
            && flow.next_send - flow.send_floor < batch_size
        {
            let sequence = flow.next_send;
            let batch = host.step(Event::Enqueue {
                flow: 7,
                sequence,
                bytes: payload(sizes[sequence as usize % sizes.len()]),
            })?;
            actions(&mut host, &mut store, batch, gate, marker)?;
        }
        let flow = host.node().checkpoint().flow(7).unwrap();
        if !done
            && flow.send_floor + flow.settled.len() as u64 == count
            && flow.next_receive() == count
            && flow.send_floor == count / batch_size * batch_size
            && flow.receive_floor == count / batch_size * batch_size
        {
            mark(
                &format!("{marker}.done"),
                &format!("settled {count}, received {count}"),
            )?;
            done = true;
            println!(
                "peer {id}: {count} settled, {} accepted inbox effects, {} durable rejections",
                count - u64::from(count > 1),
                u64::from(count > 1)
            );
        }
        if !done && start.elapsed() > Duration::from_secs(60) {
            return Err("flow deadline".into());
        }
        std::thread::sleep(Duration::from_millis(1));
    }
}
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("init") if matches!(args.len(), 3 | 4) => {
            let id: usize = args[1].parse()?;
            if id > 1 {
                return Err("peer must be 0 or 1".into());
            }
            let batch_size = args.get(3).map_or(Ok(64), |s| s.parse())?;
            Store::create(
                Path::new(&args[2]),
                &Checkpoint::empty_with_batch(&view(id), &flows(id), batch_size),
                Limits::default(),
            )?;
            println!("initialized peer {id}");
            Ok(())
        }
        Some("inspect") if args.len() == 2 => {
            let snapshot = Store::inspect(&args[1], Limits::default())?;
            println!("{}", serde_json::to_string_pretty(&snapshot)?);
            Ok(())
        }
        Some("run") if matches!(args.len(), 8 | 10) => run(&args),
        _ => Err(concat!(
            "usage: persistent-peer init <0|1> <db> [batch-size] | inspect <db> | ",
            "run <0|1> <udp|tcp> <db> <bind> <remote> <fault-gate|none> <marker-prefix> [count batch-size]; ",
            "public fixture keys, loopback only"
        )
        .into()),
    }
}
