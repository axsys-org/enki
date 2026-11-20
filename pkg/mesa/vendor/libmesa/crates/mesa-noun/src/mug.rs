//! Urbit byte mug: MurmurHash3 x86-32 with the Urbit seed and 31-bit fold.
//! This hashes exact bytes. Callers handling atoms must normalize separately.

pub fn mug_bytes(bytes: &[u8]) -> u32 {
    for seed in 0xcafe_babe..0xcafe_babe + 8 {
        let hash = murmur3(bytes, seed);
        let mug = (hash >> 31) ^ (hash & 0x7fff_ffff);
        if mug != 0 {
            return mug;
        }
    }
    0x7fff
}

fn mix(mut k: u32) -> u32 {
    k = k.wrapping_mul(0xcc9e_2d51);
    k.rotate_left(15).wrapping_mul(0x1b87_3593)
}

fn murmur3(bytes: &[u8], seed: u32) -> u32 {
    let mut h = seed;
    let (chunks, tail) = bytes.as_chunks::<4>();
    for chunk in chunks {
        h ^= mix(u32::from_le_bytes(*chunk));
        h = h.rotate_left(13).wrapping_mul(5).wrapping_add(0xe654_6b64);
    }
    if !tail.is_empty() {
        let mut word = [0; 4];
        word[..tail.len()].copy_from_slice(tail);
        h ^= mix(u32::from_le_bytes(word));
    }
    h ^= bytes.len() as u32;
    h ^= h >> 16;
    h = h.wrapping_mul(0x85eb_ca6b);
    h ^= h >> 13;
    h = h.wrapping_mul(0xc2b2_ae35);
    h ^ (h >> 16)
}
