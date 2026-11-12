use crate::{Delivery, Error, Limits, Settlement};
use mesa_flow::{Change, Checkpoint, CommitRequest, MessageId, Receipt};

pub(super) fn initial(s: &Checkpoint) -> Result<(), Error> {
    state(s, Limits::default())?;
    if s.revision != 0 {
        return Err(Error::InvalidState);
    }
    Ok(())
}
pub(super) fn scope(a: &Checkpoint, b: &Checkpoint) -> Result<(), Error> {
    if a.version != b.version
        || a.batch_size != b.batch_size
        || a.local != b.local
        || a.flows.len() != b.flows.len()
        || a.flows
            .iter()
            .zip(&b.flows)
            .any(|(a, b)| a.flow != b.flow || a.remote != b.remote)
    {
        return Err(Error::Scope);
    }
    Ok(())
}
pub(super) fn state(s: &Checkpoint, limits: Limits) -> Result<(), Error> {
    if s.version != 2
        || s.flows.is_empty()
        || s.flows.len() > 16
        || Some(s.revision) != s.expected_revision()
    {
        return Err(Error::InvalidState);
    }
    for (i, f) in s.flows.iter().enumerate() {
        if i > 0 && s.flows[i - 1].flow >= f.flow
            || f.remote.name == s.local.name
            || !f.valid(s.batch_size)
            || f.outbox
                .values()
                .any(|bytes| bytes.len() > limits.message_bytes)
        {
            return Err(Error::InvalidState);
        }
        for endpoint in [&s.local, &f.remote] {
            if mesa_core::PreconfiguredPeer::new(
                mesa_core::PeerId(endpoint.name),
                &endpoint.domain,
                0,
                endpoint.rift,
                mesa_core::KeyHandle(0),
            )
            .is_err()
            {
                return Err(Error::InvalidState);
            }
        }
    }
    Ok(())
}
fn matches_id(s: &Checkpoint, id: &MessageId, incoming: bool) -> bool {
    let Some(flow) = s.flow(id.flow) else {
        return false;
    };
    let (sender, receiver) = if incoming {
        (&flow.remote, &s.local)
    } else {
        (&s.local, &flow.remote)
    };
    id.sender == sender.name
        && id.sender_rift == sender.rift
        && id.receiver == receiver.name
        && id.receiver_rift == receiver.rift
}
pub(super) fn transition(old: &Checkpoint, r: &CommitRequest, limits: Limits) -> Result<(), Error> {
    state(&r.checkpoint, limits)?;
    scope(old, &r.checkpoint)?;
    if old.revision.checked_add(1) != Some(r.checkpoint.revision)
        || r.token.revision != r.checkpoint.revision
    {
        return Err(Error::Revision);
    }
    let (id, incoming) = match &r.change {
        Change::Enqueue { id } | Change::Settle { id, .. } | Change::RetireSent { id } => {
            (id, false)
        }
        Change::Deliver { id, .. } | Change::RetireReceived { id } => (id, true),
    };
    if !matches_id(old, id, incoming) {
        return Err(Error::Transition);
    }
    let mut expected = old.clone();
    expected.revision += 1;
    let flow = expected
        .flows
        .iter_mut()
        .find(|f| f.flow == id.flow)
        .ok_or(Error::Transition)?;
    match &r.change {
        Change::RetireReceived { id } => {
            if flow.received.len() as u64 != old.batch_size || id.sequence != flow.next_receive() {
                return Err(Error::Transition);
            }
            flow.receive_floor = id.sequence;
            flow.received.clear();
        }
        Change::RetireSent { id } => {
            if !flow.batch_settled(old.batch_size) || id.sequence != flow.next_send {
                return Err(Error::Transition);
            }
            flow.send_floor = id.sequence;
            flow.settled.clear();
        }
        Change::Enqueue { id } => {
            if id.sequence != flow.next_send || id.sequence == u64::MAX {
                return Err(Error::Transition);
            }
            let bytes = r
                .checkpoint
                .flow(id.flow)
                .and_then(|f| f.outbox.get(&id.sequence))
                .ok_or(Error::Transition)?;
            if bytes.len() > limits.message_bytes {
                return Err(Error::Limit);
            }
            flow.outbox.insert(id.sequence, bytes.clone());
            flow.next_send += 1;
        }
        Change::Deliver { id, bytes, outcome } => {
            if id.sequence != flow.next_receive() || bytes.len() > limits.message_bytes {
                return Err(Error::Transition);
            }
            flow.received.push(Receipt {
                digest: mesa_crypto::sha256(bytes),
                outcome: *outcome,
            });
        }
        Change::Settle { id, outcome } => {
            if flow.outbox.remove(&id.sequence).is_none() {
                return Err(Error::Transition);
            }
            flow.settled.insert(id.sequence, *outcome);
        }
    }
    if expected != r.checkpoint {
        return Err(Error::Transition);
    }
    Ok(())
}
pub(super) fn inbox(s: &Checkpoint, deliveries: &[Delivery]) -> Result<(), Error> {
    let mut seen = std::collections::BTreeMap::<u64, u64>::new();
    for d in deliveries {
        if d.revision > s.revision || !matches_id(s, &d.id, true) {
            return Err(Error::InvalidState);
        }
        let seq = seen.entry(d.id.flow).or_default();
        if *seq != d.id.sequence {
            return Err(Error::InvalidState);
        }
        let flow = s.flow(d.id.flow).ok_or(Error::InvalidState)?;
        if *seq >= flow.receive_floor {
            let receipt = flow
                .received
                .get((*seq - flow.receive_floor) as usize)
                .ok_or(Error::InvalidState)?;
            if receipt.digest != mesa_crypto::sha256(&d.bytes) || receipt.outcome != d.outcome {
                return Err(Error::InvalidState);
            }
        }
        *seq += 1;
    }
    if s.flows
        .iter()
        .any(|f| seen.get(&f.flow).copied().unwrap_or(0) != f.next_receive())
    {
        return Err(Error::InvalidState);
    }
    Ok(())
}

pub(super) fn settlements(s: &Checkpoint, records: &[Settlement]) -> Result<(), Error> {
    let mut seen = std::collections::BTreeMap::<u64, std::collections::BTreeSet<u64>>::new();
    for r in records {
        if r.revision == 0 || r.revision > s.revision || !matches_id(s, &r.id, false) {
            return Err(Error::InvalidState);
        }
        let f = s.flow(r.id.flow).ok_or(Error::InvalidState)?;
        if r.id.sequence >= f.next_send
            || f.outbox.contains_key(&r.id.sequence)
            || r.id.sequence >= f.send_floor && f.settled.get(&r.id.sequence) != Some(&r.outcome)
            || !seen.entry(r.id.flow).or_default().insert(r.id.sequence)
        {
            return Err(Error::InvalidState);
        }
    }
    for f in &s.flows {
        if seen.get(&f.flow).map_or(0, |v| v.len() as u64) != f.send_floor + f.settled.len() as u64
        {
            return Err(Error::InvalidState);
        }
    }
    Ok(())
}
