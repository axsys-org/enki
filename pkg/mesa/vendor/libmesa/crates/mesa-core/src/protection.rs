//! Explicit v2 signed-public profile. Plaintext is intentional and available
//! only through Access::Public. No request authentication or private-space API.
use crate::{KeyHandle, PreconfiguredPeer, Protection};
use mesa_crypto::{KeyPair, PublicKey};
use mesa_wire::Name;
use std::collections::BTreeMap;

/// Fixed trusted key table. Key handles are host-local and never transmitted.
/// Include future handles at construction to permit host-injected key updates.
pub struct SignedPublic {
    local_handle: KeyHandle,
    local: KeyPair,
    peers: BTreeMap<KeyHandle, PublicKey>,
}
impl SignedPublic {
    pub fn new(
        local_handle: KeyHandle,
        local: KeyPair,
        peers: BTreeMap<KeyHandle, PublicKey>,
    ) -> Self {
        Self {
            local_handle,
            local,
            peers,
        }
    }
}

/// Canonical, length-delimited context. The root commits to fragment contents
/// and order; this transcript commits to the object's namespace and key life.
/// See spec/08-signed-public.md. Fragment coordinates are deliberately excluded:
/// one authenticated root covers all leaves of this immutable public object.
pub fn root_context(
    peer: &PreconfiguredPeer,
    name: Name<'_>,
    total: u64,
    root: &[u8; 32],
) -> Vec<u8> {
    let mut out = b"libmesa/v2/public-lss/1\0".to_vec();
    out.push(name.domain.as_str().len() as u8);
    out.extend_from_slice(name.domain.as_str().as_bytes());
    out.extend_from_slice(&name.publisher.0.to_le_bytes());
    out.extend_from_slice(&name.rift.to_le_bytes());
    out.extend_from_slice(&peer.life.to_le_bytes());
    out.push(name.bloq);
    out.extend_from_slice(&(name.path.len() as u16).to_le_bytes());
    out.extend_from_slice(name.path);
    out.extend_from_slice(&total.to_le_bytes());
    out.extend_from_slice(root);
    out
}
impl Protection for SignedPublic {
    const SIGNED_PUBLIC: bool = true;
    fn local_key_available(&self, local: &PreconfiguredPeer) -> bool {
        local.key == self.local_handle && local.life > 0
    }
    fn peer_key_available(&self, peer: &PreconfiguredPeer) -> bool {
        peer.life > 0 && self.peers.contains_key(&peer.key)
    }
    fn verify(&mut self, _: &PreconfiguredPeer, _: &PreconfiguredPeer, _: &[u8]) -> bool {
        // This hook is reserved for the original controlled-verdict harness.
        // Public peeks confer no requester identity; pages use verify_root + LSS.
        false
    }
    fn sign_root(
        &self,
        local: &PreconfiguredPeer,
        name: Name<'_>,
        total: u64,
        root: &[u8; 32],
    ) -> Option<[u8; 64]> {
        self.local_key_available(local)
            .then(|| self.local.sign(&root_context(local, name, total, root)))
    }
    fn verify_root(
        &self,
        peer: &PreconfiguredPeer,
        name: Name<'_>,
        total: u64,
        root: &[u8; 32],
        signature: &[u8; 64],
    ) -> bool {
        self.peers.get(&peer.key).is_some_and(|key| {
            key.verify(&root_context(peer, name, total, root), signature)
                .is_ok()
        })
    }
}
