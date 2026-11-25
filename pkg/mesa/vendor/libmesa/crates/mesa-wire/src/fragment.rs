//! Restore omitted terminal zero bytes before a data fragment is hashed.
//! This operation does not verify LSS or authorize allocation of a message.
use crate::Error;

/// Reconstruct a data fragment from its logical position and message length.
/// `index` counts wire fragments at `bloq`; proof fragments must not use this.
/// The caller must verify the returned bytes against LSS before delivery.
/// Allocation is limited to `max_fragment`, independent of total message size.
pub fn reconstruct_data_fragment(
    total_bytes: u64,
    bloq: u8,
    index: u64,
    wire: &[u8],
    max_fragment: usize,
) -> Result<Vec<u8>, Error> {
    let shift = bloq.checked_sub(3).ok_or(Error::Limit)?;
    let capacity = 1u64.checked_shl(shift as u32).ok_or(Error::Limit)?;
    let offset = index.checked_mul(capacity).ok_or(Error::Limit)?;
    let remaining = total_bytes
        .checked_sub(offset)
        .filter(|n| *n > 0)
        .ok_or(Error::FragmentRange)?;
    let length = remaining.min(capacity);
    if length > max_fragment as u64 {
        return Err(Error::Limit);
    }
    let length = usize::try_from(length).map_err(|_| Error::Limit)?;
    if wire.len() > length {
        return Err(Error::FragmentRange);
    }
    let mut bytes = vec![0; length];
    bytes[..wire.len()].copy_from_slice(wire);
    Ok(bytes)
}
