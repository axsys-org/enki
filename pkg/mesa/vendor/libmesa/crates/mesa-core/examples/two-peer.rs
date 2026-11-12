//! Runnable in-memory vertical slice; protection is a test fixture, not crypto.
#[path = "../tests/support/mod.rs"]
mod support;

use mesa_core::{Access, Action, Config, Event};
use support::{Harness, A, B};

fn main() {
    let mut h = Harness::new(Config::default());
    let data = b"hello from the preconfigured Mesa peer".to_vec();
    h.step(
        B,
        Event::Bind {
            path: b"publ/hello".to_vec(),
            bytes: data.clone(),
            access: Access::Peer(A),
        },
    )
    .unwrap();
    h.step(
        A,
        Event::Read {
            id: 1,
            peer: B,
            path: b"publ/hello".to_vec(),
        },
    )
    .unwrap();
    h.pump();
    assert_eq!(
        h.results(A),
        vec![Action::ReadFinished {
            id: 1,
            result: Ok(data.clone())
        }]
    );
    assert_eq!(h.a.usage().reads, 0);
    println!("two preconfigured peers, v2 peek/page, controlled test protection");
    println!(
        "read {} exact bytes: {}",
        data.len(),
        String::from_utf8(data).unwrap()
    );
}
