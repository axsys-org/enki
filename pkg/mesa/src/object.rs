use crate::value::{err, Result, Value};
use mesa_core::{protection::SignedPublic, Engine, PreconfiguredPeer};
use mesa_crypto::{KeyPair, PublicKey, SharedSecret};
use std::{
    collections::VecDeque,
    net::{TcpListener, TcpStream, UdpSocket},
    sync::{Arc, Mutex},
};
use zeroize::Zeroizing;

pub struct Bits {
    pub bytes: Vec<u8>,
    pub len: usize,
}
impl Bits {
    pub fn view(&self) -> Result<mesa_noun::bits::Bits<'_>> {
        mesa_noun::bits::Bits::new(&self.bytes, self.len).map_err(err)
    }
}
pub struct Packet {
    pub bytes: Vec<u8>,
    pub legacy: Option<mesa_wire::legacy_v1::Checksum>,
    pub limits: mesa_wire::Limits,
}
pub struct Pass {
    pub bytes: Zeroizing<Vec<u8>>,
    pub domain: String,
    pub bound: bool,
    pub ring: bool,
    pub limits: mesa_identity::suite_c::Limits,
}
impl Pass {
    pub fn pass(&self) -> Result<mesa_identity::suite_c::UnverifiedPass<'_>> {
        if self.ring {
            return Err("expected pass".into());
        }
        mesa_identity::suite_c::parse_pass(
            &self.bytes,
            mesa_wire::DomainTag::new(&self.domain).map_err(err)?,
            self.limits,
        )
        .map_err(err)
    }
    pub fn ring(&self) -> Result<mesa_identity::suite_c::UnverifiedRing<'_>> {
        if !self.ring {
            return Err("expected ring".into());
        }
        mesa_identity::suite_c::parse_current_ring(
            &self.bytes,
            mesa_wire::DomainTag::new(&self.domain).map_err(err)?,
            self.limits,
        )
        .map_err(err)
    }
}
// The arena borrows a stable Box allocation. It is dropped before its backing.
// No reference escapes a syscall; neither the bytes nor arena are mutable.
pub struct Noun {
    pub noun: mesa_noun::serial::Noun<'static>,
    pub _bytes: Box<[u8]>,
}
impl Noun {
    pub fn new(bytes: Vec<u8>, len: usize, limits: mesa_noun::serial::Limits) -> Result<Self> {
        let bytes = bytes.into_boxed_slice();
        // SAFETY: Box contents never move; field order drops noun before bytes.
        let b: &'static [u8] = unsafe { std::slice::from_raw_parts(bytes.as_ptr(), bytes.len()) };
        let noun = mesa_noun::serial::cue(mesa_noun::bits::Bits::new(b, len).map_err(err)?, limits)
            .map_err(err)?;
        Ok(Self {
            noun,
            _bytes: bytes,
        })
    }
}
pub enum Actions {
    Core(VecDeque<mesa_core::Action>),
    Flow(VecDeque<mesa_flow::Action>),
}
// A prepared transaction borrows this stable Box. All access is under the
// session mutex; Store operations reject an outstanding transaction. The
// transaction drops before the Store. A Tx handle keeps the session alive.
pub struct Session {
    pub prepared: Option<mesa_store::Prepared<'static>>,
    pub store: Box<mesa_store::Store>,
    pub generation: u64,
}
// SAFETY: neither connection nor transaction is accessed outside the mutex.
// rusqlite Connection is Send; its Transaction borrows that same connection.
unsafe impl Send for Session {}
pub struct Transaction {
    pub session: Arc<Mutex<Session>>,
    pub generation: u64,
}
impl Drop for Transaction {
    fn drop(&mut self) {
        let mut s = self.session.lock().unwrap_or_else(|e| e.into_inner());
        if s.generation == self.generation {
            s.prepared.take();
        }
    }
}
pub enum Lane {
    Udp(mesa_transport::Udp),
    Tcp(mesa_transport::Stream<TcpStream>),
}
impl mesa_transport::PacketIo for Lane {
    fn capabilities(&self) -> mesa_transport::Capabilities {
        match self {
            Self::Udp(x) => x.capabilities(),
            Self::Tcp(x) => x.capabilities(),
        }
    }
    fn send(&mut self, b: &[u8]) -> std::io::Result<bool> {
        match self {
            Self::Udp(x) => x.send(b),
            Self::Tcp(x) => x.send(b),
        }
    }
    fn receive(&mut self) -> std::io::Result<mesa_transport::Received> {
        match self {
            Self::Udp(x) => x.receive(),
            Self::Tcp(x) => x.receive(),
        }
    }
    fn flush(&mut self) -> std::io::Result<()> {
        match self {
            Self::Udp(x) => x.flush(),
            Self::Tcp(x) => x.flush(),
        }
    }
}
pub enum Object {
    Buffer(Vec<u8>),
    Atom(mesa_noun::atom::Atom),
    Bits(Bits),
    Noun(Noun),
    Packet(Packet),
    Pass(Pass),
    Key(KeyPair),
    Public(PublicKey),
    Secret(SharedSecret),
    Tree(mesa_lss::Tree),
    Verifier(mesa_lss::Verifier),
    Peer(PreconfiguredPeer),
    Config(mesa_core::Config),
    Policy(mesa_core::policy::TransferPolicy),
    Protection(SignedPublic),
    Engine(Engine<SignedPublic>),
    Actions(Actions),
    FlowConfig(mesa_flow::Config),
    Flows(Vec<mesa_flow::FlowConfig>),
    Checkpoint(mesa_flow::Checkpoint),
    Node(mesa_flow::Node),
    Commit(mesa_flow::CommitRequest),
    Token(mesa_flow::CommitToken),
    Store(Arc<Mutex<Session>>),
    Transaction(Transaction),
    Snapshot(mesa_store::Snapshot),
    Socket(UdpSocket),
    Listener(TcpListener),
    Tcp(TcpStream),
    Lane(Lane),
    Host(mesa_transport::Host<Lane>),
    UdpHost(mesa_udp::UdpHost),
    Relay(mesa_transport::relay::Relay),
}
impl Object {
    pub fn kind(&self) -> &'static str {
        match self {
            Self::Buffer(_) => "Buffer",
            Self::Atom(_) => "Atom",
            Self::Bits(_) => "Bits",
            Self::Noun(_) => "Noun",
            Self::Packet(_) => "Packet",
            Self::Pass(p) => {
                if p.ring {
                    "Ring"
                } else if p.bound {
                    "BoundPass"
                } else {
                    "Pass"
                }
            }
            Self::Key(_) => "KeyPair",
            Self::Public(_) => "PublicKey",
            Self::Secret(_) => "SharedSecret",
            Self::Tree(_) => "Tree",
            Self::Verifier(_) => "Verifier",
            Self::Peer(_) => "Peer",
            Self::Config(_) => "Config",
            Self::Policy(_) => "Policy",
            Self::Protection(_) => "Protection",
            Self::Engine(_) => "Engine",
            Self::Actions(_) => "Actions",
            Self::FlowConfig(_) => "FlowConfig",
            Self::Flows(_) => "Flows",
            Self::Checkpoint(_) => "Checkpoint",
            Self::Node(_) => "Node",
            Self::Commit(_) => "CommitRequest",
            Self::Token(_) => "CommitToken",
            Self::Store(_) => "Store",
            Self::Transaction(_) => "Transaction",
            Self::Snapshot(_) => "Snapshot",
            Self::Socket(_) => "UdpSocket",
            Self::Listener(_) => "TcpListener",
            Self::Tcp(_) => "TcpSocket",
            Self::Lane(_) => "Lane",
            Self::Host(_) => "Host",
            Self::UdpHost(_) => "UdpHost",
            Self::Relay(_) => "Relay",
        }
    }
    pub fn charge(&self) -> usize {
        use std::mem::size_of_val;
        size_of_val(self)
            + match self {
                Self::Buffer(b) => b.capacity(),
                Self::Bits(b) => b.bytes.capacity(),
                Self::Atom(a) => a.met3(),
                Self::Noun(n) => n._bytes.len() + n.noun.node_count() * 64,
                Self::Packet(p) => p.bytes.capacity(),
                Self::Pass(p) => p.bytes.capacity() + p.domain.capacity(),
                Self::Tree(t) => t.storage_bytes(),
                Self::Engine(e) => core_charge(e.usage()),
                Self::Node(n) => node_charge(n),
                Self::Checkpoint(c) => checkpoint_charge(c),
                Self::Commit(c) => commit_charge(c),
                Self::Snapshot(s) => {
                    checkpoint_charge(&s.checkpoint)
                        + s.deliveries
                            .iter()
                            .map(|d| size_of_val(d) + d.bytes.capacity())
                            .sum::<usize>()
                        + s.settlements.capacity() * std::mem::size_of::<mesa_store::Settlement>()
                }
                Self::Flows(f) => f.capacity() * std::mem::size_of::<mesa_flow::FlowConfig>(),
                Self::Actions(Actions::Core(a)) => a
                    .iter()
                    .map(|x| {
                        size_of_val(x)
                            + match x {
                                mesa_core::Action::Send { bytes, .. }
                                | mesa_core::Action::ReadFinished {
                                    result: Ok(bytes), ..
                                } => bytes.capacity(),
                                _ => 0,
                            }
                    })
                    .sum(),
                Self::Actions(Actions::Flow(a)) => a
                    .iter()
                    .map(|x| {
                        size_of_val(x)
                            + match x {
                                mesa_flow::Action::Send { bytes, .. }
                                | mesa_flow::Action::Deliver { bytes, .. } => bytes.capacity(),
                                mesa_flow::Action::Commit(c) => commit_charge(c),
                                _ => 0,
                            }
                    })
                    .sum(),
                Self::Lane(Lane::Tcp(s)) => s.queued_bytes() + s.buffered_bytes(),
                // Host hides its lane; reserve the native maximum TCP queue instead.
                Self::Host(h) => {
                    node_charge(h.node()) + if h.is_connected() { 16 << 20 } else { 0 }
                }
                Self::UdpHost(h) => core_charge(h.engine().usage()),
                Self::Relay(r) => r.pending() * 512,
                // SQLite and crypto internals do not expose allocation metrics. These are
                // pressure estimates, not allocator accounting or a memory quota.
                Self::Store(_) | Self::Transaction(_) => 2 << 20,
                _ => 1024,
            }
    }
}
pub type Handles = Vec<Option<Object>>;
pub fn get(h: &mut Handles, i: usize) -> Result<&mut Object> {
    h.get_mut(i)
        .and_then(Option::as_mut)
        .ok_or_else(|| "closed or moved handle".into())
}
pub fn take(h: &mut Handles, i: usize) -> Result<Object> {
    h.get_mut(i)
        .and_then(Option::take)
        .ok_or_else(|| "closed or moved handle".into())
}
pub fn output(o: Object) -> (Value, Vec<Object>) {
    (Value::num(0), vec![o])
}
pub fn data(v: Value) -> (Value, Vec<Object>) {
    (v, vec![])
}
pub type Output = (Value, Vec<Object>);
#[macro_export]
macro_rules! obj {
    ($h:expr,$i:expr,$v:ident) => {
        match $crate::object::get($h, $i)? {
            $crate::object::Object::$v(x) => x,
            _ => return Err(concat!("expected ", stringify!($v)).into()),
        }
    };
}
pub fn take_kind(h: &mut Handles, i: usize, kind: &str) -> Result<Object> {
    if get(h, i)?.kind() != kind {
        return Err(format!("expected {kind}"));
    }
    take(h, i)
}

fn core_charge(u: mesa_core::Usage) -> usize {
    u.binding_bytes
        + u.binding_proof_bytes
        + u.reassembly_bytes
        + u.unverified_bytes
        + 256
            * (u.peers
                + u.namespace_slots
                + u.bindings
                + u.reads
                + u.pending_interests
                + u.received_fragments
                + u.unverified_fragments)
}
fn node_charge(n: &mesa_flow::Node) -> usize {
    let u = n.usage();
    core_charge(n.core_usage())
        + u.outbox_bytes
        + u.buffered_message_bytes
        + u.pending_commit_bytes
        + 256
            * (u.outbox_messages
                + u.buffered_messages
                + u.retained_receipts
                + u.active_reads
                + u.stalled_reads)
}
fn checkpoint_charge(c: &mesa_flow::Checkpoint) -> usize {
    c.flows
        .iter()
        .map(|f| {
            std::mem::size_of_val(f)
                + f.outbox.values().map(|b| b.capacity() + 64).sum::<usize>()
                + f.settled.len() * 64
                + f.received.capacity() * std::mem::size_of::<mesa_flow::Receipt>()
        })
        .sum()
}
fn commit_charge(c: &mesa_flow::CommitRequest) -> usize {
    checkpoint_charge(&c.checkpoint)
        + match &c.change {
            mesa_flow::Change::Deliver { bytes, .. } => bytes.capacity(),
            _ => 0,
        }
}
