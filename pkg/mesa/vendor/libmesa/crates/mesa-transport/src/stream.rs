use crate::{invalid, validate_max, Capabilities, Framing, PacketIo, Received};
use std::{
    collections::VecDeque,
    io::{self, Read, Write},
    net::TcpStream,
};

/// Four-byte big-endian length followed by one unchanged v2 pact. The host
/// supplies a connected nonblocking stream. Replacement starts a fresh frame.
pub struct Stream<S> {
    io: S,
    max: usize,
    capacity: usize,
    queued: usize,
    tx: VecDeque<Vec<u8>>,
    offset: usize,
    prefix: [u8; 4],
    prefix_used: usize,
    body: Vec<u8>,
    body_used: usize,
    failed: bool,
}
impl Stream<TcpStream> {
    pub fn tcp(socket: TcpStream, max: usize, queue_bytes: usize) -> io::Result<Self> {
        socket.set_nonblocking(true)?;
        socket.set_nodelay(true)?;
        Self::new(socket, max, queue_bytes)
    }
}
impl<S: Read + Write> Stream<S> {
    /// S must be nonblocking. Queue capacity includes prefixes and the already
    /// written portion of the front frame until that frame is fully released.
    pub fn new(io: S, max: usize, queue_bytes: usize) -> io::Result<Self> {
        validate_max(max)?;
        if !(max + 4..=16 * 1024 * 1024).contains(&queue_bytes) {
            return Err(invalid("invalid stream queue bound"));
        }
        Ok(Self {
            io,
            max,
            capacity: queue_bytes,
            queued: 0,
            tx: VecDeque::new(),
            offset: 0,
            prefix: [0; 4],
            prefix_used: 0,
            body: vec![],
            body_used: 0,
            failed: false,
        })
    }
    pub fn queued_bytes(&self) -> usize {
        self.queued
    }
    pub fn buffered_bytes(&self) -> usize {
        self.body.len() + self.prefix_used
    }
    fn check(&self) -> io::Result<()> {
        if self.failed {
            Err(io::Error::new(io::ErrorKind::NotConnected, "failed stream"))
        } else {
            Ok(())
        }
    }
    fn fail<T>(&mut self, error: io::Error) -> io::Result<T> {
        self.failed = true;
        self.tx.clear();
        self.queued = 0;
        self.offset = 0;
        self.body = vec![];
        self.body_used = 0;
        self.prefix_used = 0;
        Err(error)
    }
    fn read_part(&mut self, prefix: bool) -> io::Result<bool> {
        let target = if prefix {
            &mut self.prefix[self.prefix_used..]
        } else {
            &mut self.body[self.body_used..]
        };
        match self.io.read(target) {
            Ok(0) => self.fail(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "stream closed during receive",
            )),
            Ok(n) => {
                if prefix {
                    self.prefix_used += n;
                } else {
                    self.body_used += n;
                }
                Ok(true)
            }
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                ) =>
            {
                Ok(false)
            }
            Err(e) => self.fail(e),
        }
    }
}
impl<S: Read + Write> PacketIo for Stream<S> {
    fn capabilities(&self) -> Capabilities {
        Capabilities {
            max_packet: self.max,
            reliable: true,
            ordered: true,
            framing: Framing::LengthPrefixed,
        }
    }
    fn send(&mut self, packet: &[u8]) -> io::Result<bool> {
        self.check()?;
        if packet.len() < mesa_wire::HEADER_LEN
            || packet.len() > self.max
            || packet.len() + 4 > self.capacity - self.queued
        {
            return Ok(false);
        }
        let mut frame = Vec::with_capacity(packet.len() + 4);
        frame.extend_from_slice(&(packet.len() as u32).to_be_bytes());
        frame.extend_from_slice(packet);
        self.queued += frame.len();
        self.tx.push_back(frame);
        Ok(true)
    }
    fn receive(&mut self) -> io::Result<Received> {
        self.check()?;
        if self.prefix_used < 4 {
            self.read_part(true)?;
            if self.prefix_used < 4 {
                return Ok(Received::Idle);
            }
            let n = u32::from_be_bytes(self.prefix) as usize;
            if !(mesa_wire::HEADER_LEN..=self.max).contains(&n) {
                return self.fail(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "invalid frame length",
                ));
            }
            self.body = vec![0; n];
        }
        self.read_part(false)?;
        if self.body_used != self.body.len() {
            return Ok(Received::Idle);
        }
        self.prefix_used = 0;
        self.body_used = 0;
        Ok(Received::Packet(std::mem::take(&mut self.body)))
    }
    fn flush(&mut self) -> io::Result<()> {
        self.check()?;
        for _ in 0..64 {
            let Some(frame) = self.tx.front() else {
                break;
            };
            match self.io.write(&frame[self.offset..]) {
                Ok(0) => {
                    return self.fail(io::Error::new(
                        io::ErrorKind::WriteZero,
                        "stream write returned zero",
                    ))
                }
                Ok(n) => {
                    self.offset += n;
                    if self.offset == frame.len() {
                        self.queued -= frame.len();
                        self.tx.pop_front();
                        self.offset = 0;
                    }
                }
                Err(e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) =>
                {
                    break
                }
                Err(e) => return self.fail(e),
            }
        }
        Ok(())
    }
}
