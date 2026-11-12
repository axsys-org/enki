#![allow(dead_code)]
#[path = "../../../mesa-flow/tests/support/mod.rs"]
pub mod fixture;
use std::{
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};
static NEXT: AtomicU64 = AtomicU64::new(0);
pub struct Temp(pub PathBuf);
impl Temp {
    pub fn new() -> Self {
        let p = std::env::temp_dir().join(format!(
            "libmesa-store-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir(&p).unwrap();
        Self(p)
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
pub fn initial(i: usize) -> mesa_flow::Checkpoint {
    mesa_flow::Checkpoint::empty(&fixture::view(i), &fixture::flows(i))
}
