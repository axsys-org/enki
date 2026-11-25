//! Datagram-profile C ABI. See include/mesa.h for ownership and status codes.
use legacy_v1::Checksum;
use mesa_wire::{legacy_v1, Limits};
use std::{panic::catch_unwind, ptr, slice};

const OK: i32 = 0;
const ARGUMENT: i32 = 1;
const INVALID: i32 = 2;
const CAPACITY: i32 = 3;
const PANIC: i32 = 4;

#[derive(Clone, Copy)]
enum Profile {
    V1(Checksum),
    V2,
}

impl Profile {
    fn validate(self, bytes: &[u8]) -> Result<(), i32> {
        match self {
            Self::V1(checksum) => legacy_v1::decode(bytes, checksum, legacy_v1::Limits::DATAGRAM)
                .map(|_| ())
                .map_err(|_| INVALID),
            Self::V2 => mesa_wire::decode(bytes, Limits::DATAGRAM)
                .map(|_| ())
                .map_err(|_| INVALID),
        }
    }

    fn roundtrip(self, bytes: &[u8]) -> Result<Vec<u8>, i32> {
        match self {
            Self::V1(checksum) => {
                let packet = legacy_v1::decode(bytes, checksum, legacy_v1::Limits::DATAGRAM)
                    .map_err(|_| INVALID)?;
                legacy_v1::encode(&packet, checksum, legacy_v1::Limits::DATAGRAM)
                    .map_err(|_| INVALID)
            }
            Self::V2 => {
                let packet = mesa_wire::decode(bytes, Limits::DATAGRAM).map_err(|_| INVALID)?;
                mesa_wire::encode(&packet, Limits::DATAGRAM).map_err(|_| INVALID)
            }
        }
    }
}

fn mode(mode: u32) -> Result<Profile, i32> {
    match mode {
        0 => Ok(Profile::V1(Checksum::HoonAtom)),
        1 => Ok(Profile::V1(Checksum::VereBytes)),
        _ => Err(ARGUMENT),
    }
}

unsafe fn input<'a>(data: *const u8, len: usize) -> Result<&'a [u8], i32> {
    if data.is_null() {
        return Err(ARGUMENT);
    }
    if len > Limits::DATAGRAM.max_packet {
        return Err(INVALID);
    }
    // SAFETY: the caller supplies a readable region; length is bounded above.
    Ok(unsafe { slice::from_raw_parts(data, len) })
}

/// Validate historical v1 only, without authenticating the payload.
///
/// # Safety
/// `data` must be readable for `len` bytes throughout this call.
#[no_mangle]
pub unsafe extern "C" fn mesa_wire_validate(data: *const u8, len: usize, checksum: u32) -> i32 {
    // SAFETY: forwards the exported caller contract to the shared implementation.
    unsafe { validate_with(data, len, mode(checksum)) }
}

/// Validate experimental v2 only, using the exact-body-byte checksum.
///
/// # Safety
/// `data` must be readable for `len` bytes throughout this call.
#[no_mangle]
pub unsafe extern "C" fn mesa_wire_v2_validate(data: *const u8, len: usize) -> i32 {
    // SAFETY: forwards the exported caller contract to the shared implementation.
    unsafe { validate_with(data, len, Ok(Profile::V2)) }
}

unsafe fn validate_with(data: *const u8, len: usize, profile: Result<Profile, i32>) -> i32 {
    catch_unwind(|| {
        let result = (|| {
            let profile = profile?;
            // SAFETY: forwarded from the exported function's caller contract.
            let bytes = unsafe { input(data, len) }?;
            profile.validate(bytes)?;
            Ok::<_, i32>(())
        })();
        result.map_or_else(|e| e, |_| OK)
    })
    .unwrap_or(PANIC)
}

/// Validate and re-encode into caller-owned output. No Rust allocation escapes.
/// On CAPACITY, `written` receives the required size and output is untouched.
/// On other errors, `written` is zero and output is untouched.
///
/// # Safety
/// `data` must be readable for `len` bytes. `written` must be aligned and
/// writable. A non-null `out` must be writable for `capacity` bytes. These
/// regions must not overlap and must remain valid throughout the call.
#[no_mangle]
pub unsafe extern "C" fn mesa_wire_roundtrip(
    data: *const u8,
    len: usize,
    checksum: u32,
    out: *mut u8,
    capacity: usize,
    written: *mut usize,
) -> i32 {
    // SAFETY: forwards all pointer, capacity, and non-overlap requirements.
    unsafe { roundtrip_with(data, len, mode(checksum), out, capacity, written) }
}

/// Roundtrip experimental v2 only. Ownership and error behavior match the v1 ABI.
///
/// # Safety
/// `data` addresses `len` readable bytes; `written` addresses an aligned writable
/// size_t; non-null `out` addresses `capacity` writable bytes. No regions overlap.
#[no_mangle]
pub unsafe extern "C" fn mesa_wire_v2_roundtrip(
    data: *const u8,
    len: usize,
    out: *mut u8,
    capacity: usize,
    written: *mut usize,
) -> i32 {
    // SAFETY: forwards all pointer, capacity, and non-overlap requirements.
    unsafe { roundtrip_with(data, len, Ok(Profile::V2), out, capacity, written) }
}

unsafe fn roundtrip_with(
    data: *const u8,
    len: usize,
    profile: Result<Profile, i32>,
    out: *mut u8,
    capacity: usize,
    written: *mut usize,
) -> i32 {
    if written.is_null() {
        return ARGUMENT;
    }
    // SAFETY: required writable result pointer per the caller contract.
    unsafe {
        *written = 0;
    }
    catch_unwind(|| {
        let result = (|| {
            let profile = profile?;
            // SAFETY: forwarded from the exported function's caller contract.
            let bytes = unsafe { input(data, len) }?;
            let encoded = profile.roundtrip(bytes)?;
            if capacity < encoded.len() {
                // SAFETY: caller's writable result pointer.
                unsafe {
                    *written = encoded.len();
                }
                return Err(CAPACITY);
            }
            if out.is_null() {
                return Err(ARGUMENT);
            }
            // SAFETY: capacity checked, caller provides distinct writable storage.
            unsafe {
                ptr::copy_nonoverlapping(encoded.as_ptr(), out, encoded.len());
                *written = encoded.len();
            }
            Ok::<_, i32>(())
        })();
        result.map_or_else(|e| e, |_| OK)
    })
    .unwrap_or(PANIC)
}
