use super::*;

impl<P: Protection> Engine<P> {
    pub(super) fn serve_protected(
        &self,
        peer: PeerId,
        attachment: Attachment,
        name: Name<'_>,
        binding: &Binding,
        actions: &mut Vec<Action>,
    ) -> Result<(), DropReason> {
        let tree = binding.tree.as_ref().ok_or(DropReason::Protection)?;
        let proof;
        let mut pair_bytes = [0; 64];
        let (fragment, auth) = match name.fragment {
            Fragment::Auth(0) => {
                proof = tree.proof();
                (
                    proof.as_slice(),
                    Auth::Signature(binding.signature.as_ref().ok_or(DropReason::Protection)?),
                )
            }
            Fragment::Data(i) => {
                let index = usize::try_from(i).map_err(|_| DropReason::Fragment)?;
                let count = binding.bytes.len().div_ceil(self.capacity).max(1);
                if index >= count {
                    return Err(DropReason::Fragment);
                }
                let offset = index * self.capacity;
                let fragment =
                    &binding.bytes[offset..binding.bytes.len().min(offset + self.capacity)];
                let auth = if let Some(pair) = tree.pair(index) {
                    pair_bytes[..32].copy_from_slice(&pair[0]);
                    pair_bytes[32..].copy_from_slice(&pair[1]);
                    Auth::Pair(&pair_bytes)
                } else {
                    Auth::None
                };
                (fragment, auth)
            }
            _ => return Err(DropReason::Unsupported),
        };
        let bytes = encode(
            &Packet {
                hop: 0,
                body: Body::Page {
                    name,
                    data: Data {
                        total_bytes: binding.bytes.len() as u64,
                        auth,
                        fragment,
                    },
                    next: None,
                },
            },
            self.config.wire,
        )
        .expect("validated protected profile");
        actions.push(Action::Send {
            peer,
            attachment,
            bytes,
        });
        Ok(())
    }

    pub(super) fn protected_page(
        &mut self,
        peer: PeerId,
        name: Name<'_>,
        data: Data<'_>,
        actions: &mut Vec<Action>,
    ) -> Result<(), DropReason> {
        let index = match name.fragment {
            Fragment::Auth(0) => AUTH_INDEX,
            Fragment::Data(i) if i != AUTH_INDEX => i,
            _ => return Err(DropReason::NoInterest),
        };
        let id = self
            .reads
            .iter()
            .find(|(_, r)| {
                r.peer == peer
                    && r.rift == name.rift
                    && r.path == name.path
                    && r.interests.get(&index).is_some_and(|a| a.count > 0)
            })
            .map(|(&id, _)| id)
            .ok_or(DropReason::NoInterest)?;
        let total = usize::try_from(data.total_bytes).map_err(|_| DropReason::Fragment)?;
        if index == AUTH_INDEX {
            let Auth::Signature(signature) = data.auth else {
                return Err(DropReason::Protection);
            };
            let signature: &[u8; 64] = signature.try_into().map_err(|_| DropReason::Protection)?;
            // Exact spine shape and protocol cap precede any signature work or
            // message reservation. Unauthenticated claims cannot terminate reads.
            let verifier =
                mesa_lss::Verifier::new(total, data.fragment).map_err(|_| DropReason::Fragment)?;
            if !self.protection.verify_root(
                &self.peers[&peer].view,
                name,
                data.total_bytes,
                &verifier.root(),
                signature,
            ) {
                return Err(DropReason::Protection);
            }
            if total > self.config.max_read_bytes
                || total.div_ceil(self.capacity).max(1) > self.config.max_fragments
                || total
                    > self
                        .config
                        .max_reassembly_bytes
                        .saturating_sub(self.usage().reassembly_bytes)
            {
                self.finish(id, Err(ReadFailure::ResourceLimit), actions);
                return Ok(());
            }
            let read = self.reads.get_mut(&id).unwrap();
            read.total = Some(total);
            read.bytes = vec![0; total];
            read.remaining = total.div_ceil(self.capacity).max(1);
            read.verifier = Some(verifier);
            read.interests.remove(&AUTH_INDEX);
            self.peers
                .get_mut(&peer)
                .unwrap()
                .controller
                .verified(self.policy.congestion);
            self.fill_window(id);
            return Ok(());
        }
        if self.reads[&id].total != Some(total) {
            return Err(DropReason::Fragment);
        }
        let pair = match data.auth {
            Auth::None => None,
            Auth::Pair(p) => Some([p[..32].try_into().unwrap(), p[32..].try_into().unwrap()]),
            _ => return Err(DropReason::Protection),
        };
        let leaves = total.div_ceil(self.capacity).max(1);
        if pair.is_some() != mesa_lss::expects_pair(leaves, index as usize) {
            return Err(DropReason::Protection);
        }
        let fragment = if total == 0 {
            if index != 0 || !data.fragment.is_empty() {
                return Err(DropReason::Fragment);
            }
            Vec::new()
        } else {
            reconstruct_data_fragment(
                data.total_bytes,
                name.bloq,
                index,
                data.fragment,
                self.capacity,
            )
            .map_err(|_| DropReason::Fragment)?
        };
        if self.reads[&id].verifier.as_ref().unwrap().next_index() as u64 != index {
            let old = self.reads[&id]
                .unverified
                .get(&index)
                .map_or(0, |(b, _)| b.len());
            if fragment.len()
                > self
                    .config
                    .max_unverified_bytes
                    .saturating_sub(self.usage().unverified_bytes - old)
            {
                return Err(DropReason::ResourceLimit);
            }
            self.reads
                .get_mut(&id)
                .unwrap()
                .unverified
                .insert(index, (fragment, pair));
            return Ok(());
        }
        self.accept_leaf(id, index, &fragment, pair.as_ref())?;
        loop {
            let read = self.reads.get_mut(&id).unwrap();
            let next = read.verifier.as_ref().unwrap().next_index() as u64;
            let Some((bytes, pair)) = read.unverified.remove(&next) else {
                break;
            };
            if let Err(reason) = self.accept_leaf(id, next, &bytes, pair.as_ref()) {
                actions.push(Action::Dropped(reason));
                break;
            }
        }
        let read = self.reads.get_mut(&id).unwrap();
        if read.remaining == 0 {
            let bytes = std::mem::take(&mut read.bytes);
            self.finish(id, Ok(bytes), actions);
        } else {
            self.fill_window(id);
        }
        Ok(())
    }

    fn accept_leaf(
        &mut self,
        id: ReadId,
        index: u64,
        bytes: &[u8],
        pair: Option<&mesa_lss::Pair>,
    ) -> Result<(), DropReason> {
        let read = self.reads.get_mut(&id).unwrap();
        read.verifier
            .as_mut()
            .unwrap()
            .ingest(bytes, pair)
            .map_err(|_| DropReason::Protection)?;
        let offset = index as usize * self.capacity;
        read.bytes[offset..offset + bytes.len()].copy_from_slice(bytes);
        read.unverified.remove(&index);
        read.interests.remove(&index);
        self.peers
            .get_mut(&read.peer)
            .unwrap()
            .controller
            .verified(self.policy.congestion);
        read.remaining -= 1;
        read.received += 1;
        Ok(())
    }
}
