//! Enki's ownership-aware bridge. No Rust layout or reference crosses the ABI.
mod engine;
mod flow;
mod object;
mod primitives;
mod storage;
mod transport;
mod value;
mod wire_legacy;
use object::*;
use std::{
    ffi::{c_char, CStr},
    panic::{catch_unwind, AssertUnwindSafe},
    sync::{Mutex, MutexGuard},
};
use value::{err, Result, Value as V};

pub struct Handle {
    object: Mutex<Option<Object>>,
}
pub struct Reply {
    status: u32,
    bytes: Vec<u8>,
    handles: Vec<*mut Handle>,
}
impl Drop for Reply {
    fn drop(&mut self) {
        for h in self.handles.drain(..) {
            if !h.is_null() {
                // SAFETY: replies own each non-detached handle once.
                unsafe {
                    drop(Box::from_raw(h));
                }
            }
        }
    }
}
struct Call<'a> {
    guards: Vec<MutexGuard<'a, Option<Object>>>,
    objects: Handles,
}
impl Drop for Call<'_> {
    fn drop(&mut self) {
        for (g, o) in self.guards.iter_mut().zip(self.objects.iter_mut()) {
            **g = o.take();
        }
    }
}
fn dispatch(op: &str, h: &mut Handles, d: &V) -> Result<Output> {
    match op {
        "Close" => {
            take(h, 0)?;
            Ok(data(V::num(0)))
        }
        "Kind" => Ok(data(V::text(get(h, 0)?.kind()))),
        _ => match module(op) {
            Some("primitives") => primitives::dispatch(op, h, d),
            Some("engine") => engine::dispatch(op, h, d),
            Some("flow") => flow::dispatch(op, h, d),
            Some("storage") => storage::dispatch(op, h, d),
            Some("transport") => transport::dispatch(op, h, d),
            _ => Err("unknown Mesa operation".into()),
        },
    }
}
include!("operations.rs");
fn reply(r: Result<Output>) -> Reply {
    match r {
        Ok((v, objects)) => match v.encode() {
            Ok(bytes) => Reply {
                status: 0,
                bytes,
                handles: objects
                    .into_iter()
                    .map(|o| {
                        Box::into_raw(Box::new(Handle {
                            object: Mutex::new(Some(o)),
                        }))
                    })
                    .collect(),
            },
            Err(e) => reply(Err(e)),
        },
        Err(e) => Reply {
            status: 1,
            bytes: V::text(e).encode().unwrap(),
            handles: vec![],
        },
    }
}
/// Execute a bounded operation on live native handles.
///
/// # Safety
/// `op` must point to a terminated C string; `bytes` must cover `len` readable
/// bytes and `handles` must cover `count` pointers (or be null for count zero).
/// Each handle must remain live and rooted throughout the call. Callers must
/// not concurrently free any input. The returned reply is owned by the caller.
///
/// All pointers are owned by the C adapter. Inputs remain rooted/in a mutator
/// scope throughout. Duplicate handles are rejected before mutable borrowing.
#[no_mangle]
pub unsafe extern "C" fn em_call(
    op: *const c_char,
    handles: *const *mut Handle,
    count: usize,
    bytes: *const u8,
    len: usize,
) -> *mut Reply {
    let r = catch_unwind(AssertUnwindSafe(|| -> Result<Output> {
        if op.is_null()
            || bytes.is_null()
            || len > value::LIMIT
            || count > 64
            || (count > 0 && handles.is_null())
        {
            return Err("invalid bridge arguments".into());
        }
        // SAFETY: C adapter supplies bounded live allocations for this call.
        let op = unsafe { CStr::from_ptr(op) }
            .to_str()
            .map_err(err)?
            .strip_prefix("Mesa")
            .ok_or("invalid operation prefix")?;
        let d = V::parse(unsafe { std::slice::from_raw_parts(bytes, len) })?;
        let raw = if count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(handles, count) }
        };
        let mut ordered: Vec<_> = raw.iter().copied().enumerate().collect();
        ordered.sort_by_key(|(_, p)| *p as usize);
        for (i, (_, p)) in ordered.iter().enumerate() {
            if p.is_null() || (i > 0 && ordered[i - 1].1 == *p) {
                return Err("null or duplicate Mesa handle".into());
            }
        }
        // Acquire in address order, then restore caller order. No cross-call deadlock.
        let mut guards = Vec::new();
        for (i, p) in ordered {
            guards.push((
                i,
                unsafe { &*p }
                    .object
                    .lock()
                    .map_err(|_| "poisoned Mesa handle")?,
            ));
        }
        guards.sort_by_key(|(i, _)| *i);
        let mut call = Call {
            guards: guards.into_iter().map(|(_, g)| g).collect(),
            objects: vec![],
        };
        for g in &mut call.guards {
            call.objects.push(g.take());
        }
        dispatch(op, &mut call.objects, &d)
    }));
    Box::into_raw(Box::new(reply(match r {
        Ok(r) => r,
        Err(_) => Err("Mesa panic: affected handles are poisoned".into()),
    })))
}
/// # Safety
/// `r` must be a live reply returned by `em_call`, without concurrent mutation.
#[no_mangle]
pub unsafe extern "C" fn em_reply_status(r: *const Reply) -> u32 {
    unsafe { (*r).status }
}
/// # Safety
/// `r` must be a live reply and `n` a writable usize. The returned slice
/// remains valid until `em_reply_free`; callers must not mutate it.
#[no_mangle]
pub unsafe extern "C" fn em_reply_bytes(r: *const Reply, n: *mut usize) -> *const u8 {
    unsafe {
        *n = (*r).bytes.len();
        (*r).bytes.as_ptr()
    }
}
/// # Safety
/// `r` must be a live reply returned by `em_call`, without concurrent mutation.
#[no_mangle]
pub unsafe extern "C" fn em_reply_count(r: *const Reply) -> usize {
    unsafe { (*r).handles.len() }
}
/// # Safety
/// `r` must be an exclusively accessed live reply and `i` less than its
/// handle count. A non-null result transfers ownership to the caller.
#[no_mangle]
pub unsafe extern "C" fn em_reply_take(r: *mut Reply, i: usize) -> *mut Handle {
    unsafe { std::mem::replace(&mut (&mut (*r).handles)[i], std::ptr::null_mut()) }
}
/// # Safety
/// `r` must be null or an exclusively owned live reply. This consumes it
/// and all handles not previously detached.
#[no_mangle]
pub unsafe extern "C" fn em_reply_free(r: *mut Reply) {
    if !r.is_null() {
        unsafe { drop(Box::from_raw(r)) }
    }
}
/// # Safety
/// `h` must be null or an exclusively owned live detached handle. It
/// must have no concurrent callers and must be freed exactly once.
#[no_mangle]
pub unsafe extern "C" fn em_handle_free(h: *mut std::ffi::c_void) {
    if !h.is_null() {
        unsafe { drop(Box::from_raw(h.cast::<Handle>())) }
    }
}
/// # Safety
/// `h` must be a live handle whose owner prevents concurrent freeing.
#[no_mangle]
pub unsafe extern "C" fn em_handle_charge(h: *const Handle) -> usize {
    // A closed handle still owns its alias-safe control block until gc2 drops
    // it. Charge that allocation even after its native Object has been moved.
    std::mem::size_of::<Handle>()
        + unsafe { &*h }
            .object
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .map_or(0, |o| o.charge() - std::mem::size_of::<Object>())
}
#[cfg(test)]
mod tests;
