mod support;
use support::*;
#[test]
fn udp_flows_commit_and_settle_both_directions() {
    run(udp_pair());
}
#[test]
fn tcp_flows_commit_and_settle_both_directions() {
    run(tcp_pair());
}

#[test]
fn tcp_eof_removes_route_and_fresh_connection_recovers_outbox() {
    use fixture::*;
    use mesa_flow::Event;
    use mesa_transport::Notice;
    use std::time::{Duration, Instant};
    let mut stores = [Store::new(0), Store::new(1)];
    let [a, b] = tcp_pair();
    let mut a = host(0, &stores[0], a);
    let batch = a
        .step(Event::Enqueue {
            flow: 7,
            sequence: 0,
            bytes: payload(17425),
        })
        .unwrap();
    commit(&mut a, &mut stores[0], batch);
    drop(b);
    let start = Instant::now();
    loop {
        assert!(start.elapsed() < Duration::from_secs(5));
        let batch = a.poll(start.elapsed().as_millis() as u64).unwrap();
        if batch
            .notices
            .iter()
            .any(|n| matches!(n, Notice::Disconnected(_)))
        {
            break;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    assert!(!a.is_connected());
    assert_eq!(a.node().usage().outbox_messages, 1);
    let [next_a, next_b] = tcp_pair();
    let batch = a.reconnect(next_a).unwrap();
    commit(&mut a, &mut stores[0], batch);
    let mut b = host(1, &stores[1], next_b);
    loop {
        assert!(start.elapsed() < Duration::from_secs(5));
        let now = start.elapsed().as_millis() as u64;
        let batch = a.poll(now).unwrap();
        commit(&mut a, &mut stores[0], batch);
        let batch = b.poll(now).unwrap();
        commit(&mut b, &mut stores[1], batch);
        if stores[0].checkpoint().flow(7).unwrap().settled.len() == 1 {
            break;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(
        stores[1].effects.values().collect::<Vec<_>>(),
        vec![&payload(17425)]
    );
}
