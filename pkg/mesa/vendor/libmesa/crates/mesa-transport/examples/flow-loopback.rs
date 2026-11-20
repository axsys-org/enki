//! Public test keys and a fake atomic store; never use this as durable storage.
#[path = "../tests/support/mod.rs"]
mod support;
fn main() {
    support::run(support::udp_pair());
    println!("UDP: four ordered flow messages each direction, exact bytes and committed receipts");
    support::run(support::tcp_pair());
    println!("TCP: four ordered flow messages each direction, exact bytes and committed receipts");
    println!("Storage is a fake atomic host; no disk durability claim.");
}
