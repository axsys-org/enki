//! UDP I/O belongs to the host, outside the deterministic engine. This initial
//! adapter accepts only the real signed-public profile, never fixture verdicts.
use mesa_core::{protection::SignedPublic, Action, Attachment, Engine, Event, PeerId, Tick};
use std::{
    collections::BTreeMap,
    io,
    net::{SocketAddr, UdpSocket},
};

pub struct UdpHost {
    engine: Engine<SignedPublic>,
    socket: UdpSocket,
    routes: BTreeMap<PeerId, (Attachment, SocketAddr)>,
    receive: Vec<u8>,
    pub discarded_datagrams: u64,
}
impl UdpHost {
    /// Peers must already be installed. A configured source address supplies a
    /// claim for dispatch only; page authentication remains inside the engine.
    pub fn new(
        mut engine: Engine<SignedPublic>,
        socket: UdpSocket,
        routes: BTreeMap<PeerId, (Attachment, SocketAddr)>,
    ) -> io::Result<Self> {
        if routes.len() > engine.usage().peers {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "too many routes",
            ));
        }
        let mut addresses = std::collections::BTreeSet::new();
        let mut initial_actions = Vec::new();
        for (peer, (attachment, address)) in &routes {
            if !addresses.insert(*address) {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "ambiguous source address",
                ));
            }
            let actions = engine
                .step(Event::SetAttachment {
                    peer: *peer,
                    attachment: Some(*attachment),
                })
                .map_err(io::Error::other)?;
            initial_actions.extend(actions);
        }
        socket.set_nonblocking(true)?;
        // One extra byte distinguishes an oversized/truncated datagram from a
        // valid maximum-sized frame. No wire length controls allocation.
        let size = engine
            .max_packet_bytes()
            .checked_add(1)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "packet limit overflow"))?;
        let mut host = Self {
            engine,
            socket,
            routes,
            receive: vec![0; size],
            discarded_datagrams: 0,
        };
        // Installing a route can release queued interests. SetAttachment emits
        // only Send/SetTimer; polling supplies the timer driver for this host.
        host.dispatch(initial_actions)?;
        Ok(host)
    }
    pub fn engine(&self) -> &Engine<SignedPublic> {
        &self.engine
    }
    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        self.socket.local_addr()
    }
    pub fn step(&mut self, event: Event<'_>) -> io::Result<Vec<Action>> {
        let actions = self.engine.step(event).map_err(io::Error::other)?;
        self.dispatch(actions)
    }
    fn dispatch(&mut self, actions: Vec<Action>) -> io::Result<Vec<Action>> {
        let mut returned = Vec::new();
        for action in actions {
            if let Action::Send {
                peer,
                attachment,
                bytes,
            } = action
            {
                let Some((configured, address)) = self.routes.get(&peer) else {
                    self.discarded_datagrams += 1;
                    continue;
                };
                if *configured != attachment {
                    self.discarded_datagrams += 1;
                    continue;
                }
                match self.socket.send_to(&bytes, address) {
                    Ok(n) if n == bytes.len() => {}
                    Ok(_) => {
                        return Err(io::Error::new(io::ErrorKind::WriteZero, "partial UDP send"))
                    }
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                        self.discarded_datagrams += 1;
                    }
                    Err(e) => return Err(e),
                }
            } else {
                returned.push(action);
            }
        }
        Ok(returned)
    }
    /// Host chooses polling frequency and tick units. Expiry runs before receive
    /// at the same time. At most 64 datagrams are consumed per call; no hidden
    /// output queue grows under backpressure. WouldBlock sends count as loss and
    /// the core's normal retry/deadline policy applies.
    pub fn poll(&mut self, now: Tick) -> io::Result<Vec<Action>> {
        let mut returned = self.step(Event::AdvanceTo(now))?;
        for _ in 0..64 {
            let (length, source) = match self.socket.recv_from(&mut self.receive) {
                Ok(value) => value,
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
                Err(e) => return Err(e),
            };
            if length > self.engine.max_packet_bytes() {
                self.discarded_datagrams += 1;
                continue;
            }
            let Some((&peer, &(attachment, _))) =
                self.routes.iter().find(|(_, (_, addr))| *addr == source)
            else {
                self.discarded_datagrams += 1;
                continue;
            };
            let actions = self
                .engine
                .step(Event::Receive {
                    claimed_peer: peer,
                    attachment,
                    bytes: &self.receive[..length],
                })
                .map_err(io::Error::other)?;
            returned.extend(self.dispatch(actions)?);
        }
        Ok(returned)
    }
}
