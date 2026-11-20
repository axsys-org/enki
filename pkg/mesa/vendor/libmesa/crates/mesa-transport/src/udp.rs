use crate::{validate_max, Capabilities, Framing, PacketIo, Received};
use std::{
    io,
    net::{SocketAddr, UdpSocket},
};

/// One explicitly configured next hop. Its address is a claim, not identity proof.
pub struct Udp {
    socket: UdpSocket,
    next_hop: SocketAddr,
    receive: Vec<u8>,
    max: usize,
}
impl Udp {
    pub fn new(socket: UdpSocket, next_hop: SocketAddr, max: usize) -> io::Result<Self> {
        validate_max(max)?;
        socket.set_nonblocking(true)?;
        Ok(Self {
            socket,
            next_hop,
            receive: vec![0; max + 1],
            max,
        })
    }
    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        self.socket.local_addr()
    }
}
impl PacketIo for Udp {
    fn capabilities(&self) -> Capabilities {
        Capabilities {
            max_packet: self.max,
            reliable: false,
            ordered: false,
            framing: Framing::Datagram,
        }
    }
    fn send(&mut self, packet: &[u8]) -> io::Result<bool> {
        if packet.len() < mesa_wire::HEADER_LEN || packet.len() > self.max {
            return Ok(false);
        }
        match self.socket.send_to(packet, self.next_hop) {
            Ok(n) => Ok(n == packet.len()),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => Ok(false),
            Err(e) => Err(e),
        }
    }
    fn receive(&mut self) -> io::Result<Received> {
        match self.socket.recv_from(&mut self.receive) {
            Ok((n, from))
                if from == self.next_hop && (mesa_wire::HEADER_LEN..=self.max).contains(&n) =>
            {
                Ok(Received::Packet(self.receive[..n].to_vec()))
            }
            Ok(_) => Ok(Received::Discarded),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => Ok(Received::Idle),
            Err(e) => Err(e),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
