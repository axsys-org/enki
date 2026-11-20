#[path = "../tests/support/mod.rs"]
mod support;
use support::*;
fn main() {
    let mut h = Harness::new(config());
    for i in 0..2 {
        h.send(i, 0, b"a durably committed Mesa message".to_vec());
        h.send(i, 1, payload(17425));
    }
    h.run(100);
    for i in 0..2 {
        h.settled(i, 2);
    }
    h.restart(0);
    h.restart(1);
    h.run(100);
    for i in 0..2 {
        h.settled(i, 2);
        assert_eq!(h.stores[i].records.len(), 2);
    }
    println!("two signed peers: 4 ordered deliveries, 4 durable settlements; restart repeats no application effects");
}
