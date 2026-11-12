use super::*;
use crate::value::nums;
fn n(x: u128) -> V {
    V::num(x)
}
fn b(x: impl AsRef<[u8]>) -> V {
    V::bytes(x)
}
fn r(x: impl IntoIterator<Item = V>) -> V {
    V::row(x)
}
fn run(op: &str, h: &mut Handles, d: V) -> Output {
    dispatch(op, h, &d).unwrap_or_else(|e| panic!("{op}: {e}"))
}
fn new(op: &str, h: &mut Handles, d: V) -> Object {
    let (_, mut o) = run(op, h, d);
    assert_eq!(o.len(), 1);
    o.remove(0)
}
fn fixture(id: u128, other: u128, seed: u8, other_seed: u8) -> (Object, Object) {
    let p = new(
        "Peer",
        &mut vec![],
        r([n(id), b("gw-btc"), n(1), n(0), n(id), n(1)]),
    );
    let mut keys = vec![
        Some(new("KeyFromSeed", &mut vec![], r([b([seed; 32])]))),
        Some(Object::Public(
            mesa_crypto::KeyPair::from_seed(&[other_seed; 32]).public_key(),
        )),
    ];
    let protection = new("Protection", &mut keys, r([n(id), nums([other])]));
    assert!(keys[0].is_none());
    (p, protection)
}
#[test]
fn lossless_values_and_bounds() {
    let v = r([n(u128::MAX), b([0, 1, 0, 255, 0]), r([n(0), r([])])]);
    assert_eq!(V::parse(&v.encode().unwrap()).unwrap(), v);
    assert!(V::parse(&[0, 2, 0, 0, 0, 1, 0]).is_err());
    assert!(V::parse(&[1, 255, 255, 255, 255]).is_err());
}
#[test]
fn crypto_and_lss() {
    let mut h = vec![Some(new("KeyFromSeed", &mut vec![], r([b([9; 32])])))];
    let pubkey = new("KeyPublic", &mut h, r([]));
    let (sig, _) = run("Sign", &mut h, r([b("hello\0")]));
    let mut p = vec![Some(pubkey)];
    run("Verify", &mut p, r([b("hello\0"), sig.clone()]));
    assert!(dispatch("Verify", &mut p, &r([b("wrong"), sig])).is_err());
    let bytes = vec![5; 2049];
    let mut tree = vec![Some(new("Tree", &mut vec![], r([b(&bytes)])))];
    let (proof, _) = run("TreeProof", &mut tree, r([]));
    let mut verifier = vec![Some(new(
        "Verifier",
        &mut vec![],
        r([n(bytes.len() as u128), proof]),
    ))];
    for (i, chunk) in bytes.chunks(1024).enumerate() {
        let (pair, _) = run("TreePair", &mut tree, nums([i as u128]));
        run("VerifierIngest", &mut verifier, r([b(chunk), pair]));
    }
    let (state, _) = run("VerifierState", &mut verifier, r([]));
    assert_eq!(state.at(1).unwrap().n().unwrap(), 1);
}
#[test]
fn owned_noun_and_packet_roundtrip() {
    let atom = new("Atom", &mut vec![], r([b([42])]));
    let mut h = vec![Some(atom)];
    let m = new("Mat", &mut h, nums([256]));
    h[0] = Some(m);
    let (_, o) = run("MatDecode", &mut h, nums([256]));
    assert!(matches!(o[0], Object::Bits(_)));
    let mut bits = vec![Some(new("Bits", &mut vec![], r([b([2]), n(2)])))];
    let noun = new("Cue", &mut bits, r([]));
    drop(bits);
    let mut noun = vec![Some(noun)];
    let jam = new("Jam", &mut noun, r([]));
    let Object::Bits(jam) = jam else { panic!() };
    assert_eq!(jam.bytes, vec![2]);
    assert_eq!(jam.len, 2);
    let name = r([n(1), b("gw-btc"), n(0), n(13), nums([0, 0]), b("publ/x")]);
    let fields = r([n(0), r([n(0), name])]);
    let mut packet = vec![Some(new(
        "Packet",
        &mut vec![],
        r([
            fields.at(0).unwrap().clone(),
            fields.at(1).unwrap().clone(),
            r([]),
        ]),
    ))];
    let (wire, _) = run("WireEncode", &mut packet, r([]));
    let mut decoded = vec![Some(new("WireDecode", &mut vec![], r([wire, r([])])))];
    assert_eq!(run("PacketFields", &mut decoded, r([])).0, fields);
}
#[test]
fn signed_two_peer_exchange_through_bridge_operations() {
    let mut engines = Vec::new();
    for (id, other, seed, other_seed) in [(1, 2, 11, 22), (2, 1, 22, 11)] {
        let (p, protection) = fixture(id, other, seed, other_seed);
        let mut h = vec![
            Some(new("Config", &mut vec![], r([]))),
            Some(p),
            Some(protection),
        ];
        let engine = new("Engine", &mut h, r([]));
        assert!(h[2].is_none());
        engines.push(Some(engine));
    }
    let mut messages = std::collections::VecDeque::new();
    fn collect(
        side: usize,
        output: Output,
        q: &mut std::collections::VecDeque<(usize, V)>,
    ) -> Option<Vec<u8>> {
        let mut h = output.1.into_iter().map(Some).collect();
        loop {
            let (a, _) = run("ActionsNext", &mut h, r([]));
            if a == n(0) {
                return None;
            }
            match a.at(0).unwrap().str().unwrap() {
                "Send" => {
                    q.push_back((
                        1 - side,
                        r([n((side + 1) as u128), n(7), a.at(3).unwrap().clone()]),
                    ));
                }
                "ReadFinished" => {
                    assert_eq!(a.at(2).unwrap().n().unwrap(), 0);
                    return Some(a.at(3).unwrap().bar().unwrap().to_vec());
                }
                _ => (),
            }
        }
    }
    for (side, engine) in engines.iter_mut().enumerate() {
        let (other, _, seed, other_seed) = if side == 0 {
            (2, 1, 22, 11)
        } else {
            (1, 2, 11, 22)
        };
        let (p, _) = fixture(other, (side + 1) as u128, seed, other_seed);
        let mut h = vec![engine.take(), Some(p)];
        collect(side, run("EngineInstallPeer", &mut h, r([])), &mut messages);
        collect(
            side,
            run("EngineSetAttachment", &mut h, r([n(other), nums([7])])),
            &mut messages,
        );
        *engine = h[0].take();
    }
    let payload = vec![123; 5000];
    let mut h = vec![engines[1].take()];
    collect(
        1,
        run(
            "EngineBind",
            &mut h,
            r([b("publ/example"), b(&payload), r([])]),
        ),
        &mut messages,
    );
    engines[1] = h[0].take();
    let mut h = vec![engines[0].take()];
    collect(
        0,
        run("EngineRead", &mut h, r([n(99), n(2), b("publ/example")])),
        &mut messages,
    );
    engines[0] = h[0].take();
    for _ in 0..1000 {
        let (side, event) = messages.pop_front().expect("protocol stalled");
        let mut h = vec![engines[side].take()];
        let result = collect(side, run("EngineReceive", &mut h, event), &mut messages);
        engines[side] = h[0].take();
        if let Some(bytes) = result {
            assert_eq!(bytes, payload);
            return;
        }
    }
    panic!("protocol did not finish")
}
#[test]
fn ffi_rejects_duplicate_handles_and_closed_aliases() {
    unsafe {
        let input = r([b([8; 32])]).encode().unwrap();
        let op = std::ffi::CString::new("MesaKeyFromSeed").unwrap();
        let reply = em_call(
            op.as_ptr(),
            std::ptr::null(),
            0,
            input.as_ptr(),
            input.len(),
        );
        assert_eq!(em_reply_status(reply), 0);
        let handle = em_reply_take(reply, 0);
        em_reply_free(reply);
        let input = r([]).encode().unwrap();
        let op = std::ffi::CString::new("MesaKind").unwrap();
        let duplicates = [handle, handle];
        let reply = em_call(
            op.as_ptr(),
            duplicates.as_ptr(),
            2,
            input.as_ptr(),
            input.len(),
        );
        assert_eq!(em_reply_status(reply), 1);
        em_reply_free(reply);
        let close = std::ffi::CString::new("MesaClose").unwrap();
        let reply = em_call(close.as_ptr(), &handle, 1, input.as_ptr(), input.len());
        assert_eq!(em_reply_status(reply), 0);
        em_reply_free(reply);
        assert!(em_handle_charge(handle) >= std::mem::size_of::<Handle>());
        let reply = em_call(op.as_ptr(), &handle, 1, input.as_ptr(), input.len());
        assert_eq!(em_reply_status(reply), 1);
        em_reply_free(reply);
        em_handle_free(handle.cast());
    }
}

#[test]
fn durable_store_prepare_rollback_commit_and_reopen() {
    let dir = std::env::temp_dir().join(format!(
        "enki-mesa-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::create_dir(&dir).unwrap();
    let path = dir.join("inbox.sqlite");
    let (local, protection) = fixture(1, 2, 11, 22);
    let (remote, _) = fixture(2, 1, 22, 11);
    let mut peers = vec![Some(remote)];
    let flows = new("Flows", &mut peers, r([r([n(1), nums([7])])]));
    let mut h = vec![Some(local), Some(flows)];
    let cp = new("Checkpoint", &mut h, nums([2]));
    let mut store_args = vec![Some(cp)];
    let store = new(
        "StoreCreate",
        &mut store_args,
        r([b(path.to_str().unwrap()), r([])]),
    );
    let mut stores = vec![Some(store)];
    let (inc, _) = run("StoreIncarnation", &mut stores, r([]));
    let core = new("Config", &mut vec![], r([]));
    let mut config_args = vec![Some(core)];
    let policy = new("PolicyFixed", &mut config_args, r([]));
    config_args.push(Some(policy));
    let cfg = new(
        "FlowConfig",
        &mut config_args,
        nums([1, 2, 2, 65536, 1024 * 1024]),
    );
    let mut node_args = vec![
        Some(cfg),
        h[0].take(),
        Some(protection),
        h[1].take(),
        store_args[0].take(),
    ];
    let node = new("Node", &mut node_args, r([inc]));
    let mut node = vec![Some(node)];
    let actions = new(
        "NodeEnqueue",
        &mut node,
        r([n(1), n(0), b("durable\0message")]),
    );
    let mut actions = vec![Some(actions)];
    let (tag, mut objects) = run("ActionsNext", &mut actions, r([]));
    assert_eq!(tag.at(0).unwrap().str().unwrap(), "Commit");
    let request = objects.remove(0);
    stores.push(Some(request));
    let tx = new("StorePrepare", &mut stores, r([]));
    assert!(dispatch("StoreCheckpoint", &mut stores, &r([])).is_err());
    let mut tx = vec![Some(tx)];
    run("TransactionRollback", &mut tx, r([]));
    let cp_before = new("StoreCheckpoint", &mut stores, r([]));
    let Object::Checkpoint(c) = cp_before else {
        panic!()
    };
    assert_eq!(c.revision, 0);
    let initial = c.clone();
    let abandoned = new("StorePrepare", &mut stores, r([]));
    drop(abandoned);
    let prepared = new("StorePrepare", &mut stores, r([]));
    let mut tx = vec![Some(prepared)];
    let token = new("TransactionCommit", &mut tx, r([]));
    assert!(tx[0].is_none());
    node.push(Some(token));
    let _ = run("NodeCommitted", &mut node, r([]));
    let cp_after = new("StoreCheckpoint", &mut stores, r([]));
    let Object::Checkpoint(c) = cp_after else {
        panic!()
    };
    assert_eq!(c.revision, 1);
    assert_eq!(c.flows[0].outbox.get(&0).unwrap(), b"durable\0message");

    let mut scope = vec![Some(new(
        "StoreInspect",
        &mut vec![],
        r([b(path.to_str().unwrap()), r([])]),
    ))];
    let cp = new("SnapshotCheckpoint", &mut scope, r([]));
    let Object::Checkpoint(c) = cp else { panic!() };
    assert_eq!(c.revision, 1);
    // Reopening fences the old incarnation and recovers committed bytes.
    let old_inc = run("StoreIncarnation", &mut stores, r([])).0;
    let mut cp = vec![Some(Object::Checkpoint(initial))];
    let reopened = new("StoreOpen", &mut cp, r([b(path.to_str().unwrap()), r([])]));
    let mut reopened = vec![Some(reopened)];
    assert_ne!(run("StoreIncarnation", &mut reopened, r([])).0, old_inc);
    // Use a new proposal at revision 1, then close the parent while prepared.
    let actions = new("NodeEnqueue", &mut node, r([n(1), n(1), b("second")]));
    let mut actions = vec![Some(actions)];
    let (_, mut requests) = run("ActionsNext", &mut actions, r([]));
    let Object::Commit(mut request) = requests.remove(0) else {
        panic!()
    };
    request.token.incarnation = run("StoreIncarnation", &mut reopened, r([])).0.n().unwrap();
    reopened.push(Some(Object::Commit(request)));
    let tx = new("StorePrepare", &mut reopened, r([]));
    run("Close", &mut reopened, r([])); // the transaction retains the connection
    let mut tx = vec![Some(tx)];
    let token = new("TransactionCommit", &mut tx, r([]));
    assert!(matches!(token, Object::Token(_)));
    let snapshot = new(
        "StoreInspect",
        &mut vec![],
        r([b(path.to_str().unwrap()), r([])]),
    );
    let Object::Snapshot(snapshot) = snapshot else {
        panic!()
    };
    assert_eq!(snapshot.checkpoint.revision, 2);
    drop(stores);
    drop(node);
    drop(scope);
    drop(reopened);
    std::fs::remove_dir_all(dir).unwrap();
}
#[test]
fn parsed_views_own_their_backing_and_all_bridged_types_are_send() {
    fn assert_send<T: Send>() {}
    assert_send::<Object>();
    let input = b"c"
        .iter()
        .copied()
        .chain([1u8; 64])
        .chain([0u8; 1])
        .collect::<Vec<_>>();
    assert!(dispatch("Pass", &mut vec![], &r([b(input), b("gw-btc"), r([])])).is_err());
    // Constructors must reject invalid/absent arguments rather than escaping FFI.
    for op in [
        "CheckRing",
        "Node",
        "StorePrepare",
        "Host",
        "Protection",
        "Packet",
        "Cue",
    ] {
        assert!(
            std::panic::catch_unwind(|| dispatch(op, &mut vec![], &r([])))
                .unwrap()
                .is_err(),
            "{op}"
        );
    }
}
#[test]
fn transport_udp_tcp_and_relay() {
    let mut a = vec![Some(new("UdpBind", &mut vec![], r([b("127.0.0.1:0")])))];
    let (addr_a, _) = run("UdpAddress", &mut a, r([]));
    let mut z = vec![Some(new("UdpBind", &mut vec![], r([b("127.0.0.1:0")])))];
    let (addr_z, _) = run("UdpAddress", &mut z, r([]));
    let la = new("UdpLane", &mut a, r([addr_z, n(1472)]));
    let lz = new("UdpLane", &mut z, r([addr_a, n(1472)]));
    let mut a = vec![Some(la)];
    let mut z = vec![Some(lz)];
    let packet = vec![7; 8];
    assert_eq!(run("LaneSend", &mut a, r([b(&packet)])).0, n(1));
    receive_packet(&mut z, &packet);
    let mut listener = vec![Some(new("TcpListen", &mut vec![], r([b("127.0.0.1:0")])))];
    let (addr, _) = run("TcpListenAddress", &mut listener, r([]));
    let tcp = new("TcpConnect", &mut vec![], r([addr, n(1000)]));
    let (_, mut accepted) = run("TcpAccept", &mut listener, r([]));
    assert_eq!(accepted.len(), 1);
    let mut tcp = vec![Some(tcp)];
    let mut accepted = vec![Some(accepted.remove(0))];
    let a = new("TcpLane", &mut tcp, nums([1472, 10000]));
    let z = new("TcpLane", &mut accepted, nums([1472, 10000]));
    let mut a = vec![Some(a)];
    let mut z = vec![Some(z)];
    run("LaneSend", &mut a, r([b(&packet)]));
    run("LaneFlush", &mut a, r([]));
    receive_packet(&mut z, &packet);
    let relay = new(
        "Relay",
        &mut vec![],
        r([n(1), n(2), n(1472), n(1472), r([]), n(32), n(100)]),
    );
    let mut relay = vec![Some(relay)];
    assert_eq!(run("RelayPending", &mut relay, r([])).0, n(0));
    let (v, _) = run("RelayReceive", &mut relay, r([n(0), b("malformed")]));
    assert_eq!(v.at(0).unwrap().n().unwrap(), 0);
}

fn receive_packet(lane: &mut Handles, packet: &[u8]) {
    for _ in 0..1000 {
        let (v, _) = run("LaneReceive", lane, r([]));
        if v.at(0).unwrap().n().unwrap() == 2 {
            assert_eq!(v.at(1).unwrap().bar().unwrap(), packet);
            return;
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    panic!("timed out receiving packet");
}

#[test]
fn opcode_registry_dispatch_and_reference_agree() {
    let registry = include_str!("../include/ops.def");
    let reference = include_str!("../API.md");
    let mut names = std::collections::BTreeSet::new();
    for line in registry.lines().filter(|l| l.starts_with("MESA_OP")) {
        let name = line
            .strip_prefix("MESA_OP(\"Mesa")
            .unwrap()
            .strip_suffix("\")")
            .unwrap();
        assert!(names.insert(name), "duplicate {name}");
        assert!(
            matches!(name, "Close" | "Kind") || module(name).is_some(),
            "unrouted {name}"
        );
        assert!(
            reference.contains(&format!("| `{name}` |")),
            "undocumented {name}"
        );
    }
    assert_eq!(names.len(), 199);
}
