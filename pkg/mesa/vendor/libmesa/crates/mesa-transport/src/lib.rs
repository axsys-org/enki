//! Bounded attachments for a preconfigured flow peer. Transport never commits
//! storage or changes endpoint recovery policy. See spec/11-flow-transports.md.
mod host;
pub mod relay;
mod stream;
mod udp;
pub use host::{Batch, Host, Notice};
use std::io;
pub use stream::Stream;
pub use udp::Udp;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Framing {
    Datagram,
    LengthPrefixed,
}
/// Local attachment properties, never an assertion about the complete path.
/// Each instance is one full-duplex lane to a configured next hop.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Capabilities {
    pub max_packet: usize,
    pub reliable: bool,
    pub ordered: bool,
    pub framing: Framing,
}
#[derive(Debug, PartialEq, Eq)]
pub enum Received {
    Packet(Vec<u8>),
    Idle,
    Discarded,
}
/// Nonblocking, bounded packet I/O. Implementations must bound work and memory
/// per call. A successful send means local admission, not remote delivery.
pub trait PacketIo {
    fn capabilities(&self) -> Capabilities;
    /// False means queue pressure or datagram loss; endpoint recovery remains on.
    fn send(&mut self, packet: &[u8]) -> io::Result<bool>;
    fn receive(&mut self) -> io::Result<Received>;
    fn flush(&mut self) -> io::Result<()>;
}
fn invalid(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}
fn validate_max(max: usize) -> io::Result<()> {
    if !(mesa_wire::HEADER_LEN..=65535).contains(&max) {
        return Err(invalid("packet bound must be 8..=65535"));
    }
    Ok(())
}
