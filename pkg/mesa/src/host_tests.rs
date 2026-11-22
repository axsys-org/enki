//! The raw host embedding, through the same dispatch that PLAN calls.
use super::*;
use crate::value::Value;

const DOMAIN: &str = "shrine-test";
fn n(x: u128) -> V {
    V::num(x)
}
fn b(x: impl AsRef<[u8]>) -> V {
    V::bytes(x)
}
fn t(x: &str) -> V {
    V::text(x)
}
fn r(x: impl IntoIterator<Item = V>) -> V {
    V::row(x)
}
fn attempt(op: &str, slots: &mut [&mut Option<Object>], d: V) -> Result<Output> {
    let mut h: Handles = slots.iter_mut().map(|s| s.take()).collect();
    let out = dispatch(op, &mut h, &d);
    for (s, o) in slots.iter_mut().zip(h) {
        **s = o;
    }
    out
}
fn call(op: &str, slots: &mut [&mut Option<Object>], d: V) -> V {
    attempt(op, slots, d)
        .unwrap_or_else(|e| panic!("{op}: {e}"))
        .0
}
fn make(op: &str, slots: &mut [&mut Option<Object>], d: V) -> (V, Option<Object>) {
    let (v, mut o) = attempt(op, slots, d).unwrap_or_else(|e| panic!("{op}: {e}"));
    assert_eq!(o.len(), 1, "{op}");
    (v, o.pop())
}
fn folder(name: &str) -> String {
    let dir = std::env::temp_dir().join(format!("enki-mesa-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir.to_str().unwrap().to_string()
}
fn point(peer: u128, life: u128, rift: u128, key: &V) -> V {
    r([n(peer), t(DOMAIN), n(life), n(rift), key.clone(), b([])])
}
/// The checkpoint of the pending proposal goes to a file, and then the
/// authority is told: the order that libmesa requires.
fn settle(authority: &mut Option<Object>, file: &str, reply: &V) {
    let pending = reply.at(1).unwrap();
    call(
        "Persist",
        &mut [],
        r([t(file), pending.at(2).unwrap().clone()]),
    );
    let token = pending.at(0).unwrap();
    let done = call(
        "AuthorityStep",
        &mut [authority],
        r([r([r([
            t("Commit"),
            token.at(0).unwrap().clone(),
            token.at(1).unwrap().clone(),
        ])])]),
    );
    assert_eq!(done.at(0).unwrap().at(0).unwrap().at(0).unwrap(), &n(0));
    assert_eq!(done.at(1).unwrap(), &n(0));
}
fn all_accepted(reply: &V) {
    for row in reply.at(0).unwrap().list().unwrap() {
        assert_eq!(row.at(0).unwrap(), &n(0), "{row:?}");
    }
}

struct World {
    id: u128,
    dir: String,
    public: V,
    authority: Option<Object>,
    keys: Option<Object>,
    engine: Option<Object>,
    endpoint: Option<Object>,
    address: V,
    fd: V,
}
impl World {
    /// A world with its own key file, and nothing accepted yet.
    fn new(id: u128, name: &str) -> Self {
        let dir = folder(name);
        let (_, authority) = make("Authority", &mut [], r([n(id * 100 + 1), n(0), n(0)]));
        let (_, mut keys) = make(
            "Keys",
            &mut [],
            r([n(id), t(DOMAIN), n(id * 100 + 2), n(7), n(1000), n(0)]),
        );
        let imported = call(
            "KeysImport",
            &mut [&mut keys],
            r([t(&format!("{dir}/ring")), r([n(1), n(1), n(0), n(1)])]),
        );
        assert_eq!(imported.at(1).unwrap(), &n(1));
        let (_, config) = make("Config", &mut [], r([]));
        let (_, engine) = make(
            "EngineConfigured",
            &mut [&mut Some(config.unwrap())],
            r([n(id), t(DOMAIN)]),
        );
        let (_, mut endpoint) = make("Endpoint", &mut [], r([t("127.0.0.1:0"), n(1472), n(8)]));
        let made = call("EndpointSocket", &mut [&mut endpoint], r([]));
        Self {
            id,
            dir,
            public: imported.at(0).unwrap().clone(),
            authority,
            keys,
            engine,
            endpoint,
            address: made.at(0).unwrap().clone(),
            fd: made.at(1).unwrap().clone(),
        }
    }
    /// Accept this world's own point and each given peer, make it durable,
    /// activate the key, and open a route to each peer.
    fn open(&mut self, peers: &[(u128, V, V)]) {
        let mut rules = vec![
            r([t("Register"), t(DOMAIN), n(0), n(1)]),
            r([t("Ready"), t(DOMAIN), n(1), n(1)]),
            r([t("Accept"), n(0), point(self.id, 1, 0, &self.public)]),
        ];
        for (id, key, _) in peers {
            rules.push(r([t("Accept"), n(0), point(*id, 1, 0, key)]));
        }
        let reply = call("AuthorityStep", &mut [&mut self.authority], r([r(rules)]));
        all_accepted(&reply);
        settle(
            &mut self.authority,
            &format!("{}/authority", self.dir),
            &reply,
        );
        let offer = call(
            "KeysStep",
            &mut [&mut self.keys, &mut self.authority],
            r([t("Prepare"), n(1)]),
        );
        call(
            "Persist",
            &mut [],
            r([
                t(&format!("{}/manifest", self.dir)),
                offer.at(2).unwrap().clone(),
            ]),
        );
        let token = offer.at(0).unwrap();
        call(
            "KeysStep",
            &mut [&mut self.keys, &mut self.authority],
            r([
                t("Commit"),
                token.at(0).unwrap().clone(),
                token.at(1).unwrap().clone(),
            ]),
        );
        let synced = call(
            "KeysSync",
            &mut [&mut self.keys, &mut self.authority, &mut self.engine],
            r([]),
        );
        assert_eq!(synced.at(0).unwrap().at(0).unwrap(), &n(0), "{synced:?}");
        for (id, _, address) in peers {
            let synced = call(
                "AuthoritySync",
                &mut [&mut self.authority, &mut self.engine],
                r([r([n(*id)])]),
            );
            all_accepted(&synced);
            let routed = call(
                "EndpointStep",
                &mut [&mut self.endpoint],
                r([r([r([t("SetRoute"), n(*id), n(0), n(1), address.clone()])])]),
            );
            assert_eq!(routed.at(0).unwrap().at(0).unwrap(), &n(0), "{routed:?}");
            self.step(vec![r([t("ReplaceAttachment"), n(*id), n(0), n(1), n(1)])]);
        }
    }
    /// Give events to the engine with its endpoint: sends go to the socket.
    /// The answer is each finished read as (id, status, bytes or reason).
    fn step(&mut self, events: Vec<V>) -> Vec<(u128, u128, Vec<u8>)> {
        let reply = call(
            "EngineStep",
            &mut [&mut self.engine, &mut self.endpoint],
            r([r(events)]),
        );
        let mut finished = Vec::new();
        for result in reply.at(0).unwrap().list().unwrap() {
            assert_eq!(result.at(0).unwrap(), &n(0), "{result:?}");
            for record in result.at(1).unwrap().list().unwrap() {
                match record.at(0).unwrap().str().unwrap() {
                    "ReadFinished" => finished.push((
                        record.at(1).unwrap().n().unwrap(),
                        record.at(2).unwrap().n().unwrap(),
                        record.at(3).unwrap().bar().unwrap().to_vec(),
                    )),
                    "Send" => panic!("a send came to the caller though an endpoint was given"),
                    "Unsent" => panic!("{record:?}"),
                    _ => {}
                }
            }
        }
        finished
    }
    /// Take what the socket holds and give it to the engine.
    fn pump(&mut self, now: u128) -> Vec<(u128, u128, Vec<u8>)> {
        let got = call(
            "EndpointStep",
            &mut [&mut self.endpoint],
            r([r([r([t("Receive"), n(64)])])]),
        );
        let got = got.at(0).unwrap();
        assert_eq!(got.at(0).unwrap(), &n(0));
        assert_eq!(got.at(3).unwrap(), &n(0));
        let mut events = vec![r([t("AdvanceTo"), n(now)])];
        for packet in got.at(1).unwrap().list().unwrap() {
            events.push(r([
                t("Receive"),
                packet.at(0).unwrap().clone(),
                packet.at(1).unwrap().clone(),
                packet.at(2).unwrap().clone(),
            ]));
        }
        self.step(events)
    }
}
fn exchange(reader: &mut World, publisher: &mut World, from: u128) -> Vec<(u128, u128, Vec<u8>)> {
    for now in from..from + 400 {
        publisher.pump(now);
        let finished = reader.pump(now);
        if !finished.is_empty() {
            return finished;
        }
        std::thread::sleep(std::time::Duration::from_millis(2));
    }
    panic!("no read finished");
}

#[test]
fn two_worlds_exchange_a_page_over_udp_with_keys_in_custody() {
    let mut a = World::new(1, "udp-a");
    let mut c = World::new(2, "udp-b");
    // The file holds the seed; the caller got the public key only.
    assert_eq!(std::fs::read(format!("{}/ring", a.dir)).unwrap().len(), 32);
    assert_eq!(a.public.bar().unwrap().len(), 32);
    // Before activation the engine takes no read.
    let early = attempt(
        "EngineStep",
        &mut [&mut a.engine],
        r([r([r([t("Read"), n(1), n(2), b("publ/early")])])]),
    )
    .unwrap()
    .0;
    assert_eq!(
        early.at(0).unwrap().at(0).unwrap(),
        &r([n(1), t("Blocked")])
    );
    let (a_peer, c_peer) = (
        (1, a.public.clone(), a.address.clone()),
        (2, c.public.clone(), c.address.clone()),
    );
    a.open(&[c_peer]);
    c.open(&[a_peer]);

    assert_eq!(
        call("EndpointSocket", &mut [&mut a.endpoint], r([])),
        r([a.address.clone(), a.fd.clone()])
    );
    // Nothing has arrived: the wait gives 0 when its time passes.
    assert_eq!(call("WaitReadable", &mut [], r([a.fd.clone(), n(5)])), n(0));
    let payload: Vec<u8> = (0..5000u32).map(|i| (i % 251) as u8).collect();
    c.step(vec![r([t("Bind"), b("publ/page"), b(&payload), r([])])]);
    a.step(vec![r([t("Read"), n(9), n(2), b("publ/page")])]);
    // The ask is on the wire: the publisher's socket can be read.
    assert_eq!(
        call("WaitReadable", &mut [], r([c.fd.clone(), n(2000)])),
        n(1)
    );
    assert_eq!(exchange(&mut a, &mut c, 1), vec![(9, 0, payload.clone())]);

    // The two worlds agree on one secret, and neither gets its bytes.
    let (_, mut ours) = make("KeysAgree", &mut [&mut a.keys, &mut a.authority], r([n(2)]));
    let (_, mut theirs) = make("KeysAgree", &mut [&mut c.keys, &mut c.authority], r([n(1)]));
    assert!(attempt("SecretBytes", &mut [&mut ours], r([])).is_err());
    let (_, mut seal_a) = make("SecretDerive", &mut [&mut ours], r([b("seal")]));
    let (_, mut seal_c) = make("SecretDerive", &mut [&mut theirs], r([b("seal")]));
    assert_eq!(
        call("SecretHash", &mut [&mut seal_a], r([b("name")])),
        call("SecretHash", &mut [&mut seal_c], r([b("name")]))
    );
    assert_ne!(
        call("SecretHash", &mut [&mut ours], r([b("name")])),
        call("SecretHash", &mut [&mut seal_a], r([b("name")]))
    );
    let sealed = call(
        "SecretSeal",
        &mut [&mut seal_a],
        r([b([3u8; 24]), b("where"), b("what")]),
    );
    assert_eq!(
        call(
            "SecretOpen",
            &mut [&mut seal_c],
            r([b([3u8; 24]), b("where"), sealed.clone()])
        ),
        r([b("what")])
    );
    assert_eq!(
        call(
            "SecretOpen",
            &mut [&mut seal_c],
            r([b([3u8; 24]), b("elsewhere"), sealed])
        ),
        n(0)
    );
}

#[test]
fn a_restored_authority_keeps_what_it_refused() {
    let dir = folder("authority");
    let file = format!("{dir}/authority");
    let key = |seed: u8| {
        b(mesa_crypto::KeyPair::from_seed(&[seed; 32])
            .public_key()
            .as_bytes())
    };
    let (_, mut authority) = make("Authority", &mut [], r([n(11), n(0), n(0)]));
    let reply = call(
        "AuthorityStep",
        &mut [&mut authority],
        r([r([
            r([t("Register"), t(DOMAIN), n(0), n(1)]),
            r([t("Ready"), t(DOMAIN), n(1), n(1)]),
            r([t("Accept"), n(0), point(5, 2, 0, &key(1))]),
        ])]),
    );
    all_accepted(&reply);
    settle(&mut authority, &file, &reply);
    let held = call("AuthorityPeer", &mut [&mut authority], r([n(5)]));
    let revision = held.at(0).unwrap().clone();
    assert_eq!(held.at(2).unwrap().at(0).unwrap(), &n(1));
    // The operator blocks the peer; the block is durable too.
    let reply = call(
        "AuthorityStep",
        &mut [&mut authority],
        r([r([r([t("Block"), n(5), n(1)])])]),
    );
    all_accepted(&reply);
    settle(&mut authority, &file, &reply);
    drop(authority);

    // A new process: the checkpoint from the file, a new incarnation.
    let saved = std::fs::read(&file).unwrap();
    let (_, mut authority) = make("Authority", &mut [], r([n(12), b(&saved), n(0)]));
    let held = call("AuthorityPeer", &mut [&mut authority], r([n(5)]));
    // Not ordinary: blocked, and not vouched for again since the restart.
    assert_eq!(
        held.at(2).unwrap(),
        &r([n(0), n(1), n(0), n(1), n(0), n(1), n(0)])
    );
    let revision_now = held.at(0).unwrap().clone();
    assert_ne!(revision, revision_now);
    let refused = |authority: &mut Option<Object>, candidate: V| {
        let reply = call(
            "AuthorityStep",
            &mut [authority],
            r([r([
                r([t("Ready"), t(DOMAIN), n(1), n(1)]),
                r([t("Accept"), revision_now.clone(), candidate]),
            ])]),
        );
        reply.at(0).unwrap().at(1).unwrap().clone()
    };
    // An older life, and a different key at the same life, are refused.
    assert_eq!(
        refused(&mut authority, point(5, 1, 0, &key(1))),
        r([n(1), t("Rollback")])
    );
    assert_eq!(
        refused(&mut authority, point(5, 2, 0, &key(2))),
        r([n(1), t("Conflict")])
    );
    // The same point is not taken either while the block stands.
    assert_eq!(
        refused(&mut authority, point(5, 2, 0, &key(1))),
        r([n(1), t("Unavailable")])
    );
    let held = call("AuthorityPeer", &mut [&mut authority], r([n(5)]));
    assert_eq!(held.at(2).unwrap().at(0).unwrap(), &n(0));
    assert_eq!(held.at(2).unwrap().at(1).unwrap(), &n(1));
    // The block is lifted: the point is accepted, and the peer is ordinary.
    let reply = call(
        "AuthorityStep",
        &mut [&mut authority],
        r([r([r([t("Block"), n(5), n(0)])])]),
    );
    all_accepted(&reply);
    settle(&mut authority, &file, &reply);
    let now = call("AuthorityPeer", &mut [&mut authority], r([n(5)]));
    let reply = call(
        "AuthorityStep",
        &mut [&mut authority],
        r([r([
            r([t("Advance"), n(5000)]),
            r([
                t("Accept"),
                now.at(0).unwrap().clone(),
                point(5, 2, 0, &key(1)),
            ]),
        ])]),
    );
    all_accepted(&reply);
    settle(&mut authority, &file, &reply);
    let held = call("AuthorityPeer", &mut [&mut authority], r([n(5)]));
    assert_eq!(held.at(2).unwrap().at(0).unwrap(), &n(1));
    // A clock that goes back does not stop the caller from naming a key.
    let reply = call(
        "AuthorityStep",
        &mut [&mut authority],
        r([r([
            r([t("Advance"), n(10)]),
            r([
                t("Accept"),
                held.at(0).unwrap().clone(),
                point(5, 3, 0, &key(3)),
            ]),
        ])]),
    );
    all_accepted(&reply);
}

#[test]
fn a_restored_manifest_is_not_older_than_its_floor() {
    let mut w = World::new(3, "manifest");
    w.open(&[]);
    let saved = std::fs::read(format!("{}/manifest", w.dir)).unwrap();
    let restore = |floor: u128| {
        attempt(
            "Keys",
            &mut [],
            r([
                n(3),
                t(DOMAIN),
                n(999),
                n(7),
                n(1000),
                r([b(&saved), n(floor)]),
            ]),
        )
    };
    assert!(restore(1).is_ok());
    assert_eq!(restore(2).err().unwrap(), "Rollback");
    // The key file is kept: a second import of it gives the same key and
    // makes no new file.
    let (_, mut keys) = restore(1).map(|(v, mut o)| (v, o.pop())).unwrap();
    let again = call(
        "KeysImport",
        &mut [&mut keys],
        r([t(&format!("{}/ring", w.dir)), r([n(1), n(1), n(0), n(0)])]),
    );
    assert_eq!(again, r([w.public.clone(), n(0)]));
    // Without the file and without leave to make one, there is no key.
    assert!(
        attempt(
            "KeysImport",
            &mut [&mut keys],
            r([t(&format!("{}/absent", w.dir)), r([n(2), n(1), n(0), n(0)])]),
        )
        .is_err()
    );
}

#[test]
fn durable_write_replaces_a_file_whole_and_names_resolve() {
    let dir = folder("persist");
    let file = format!("{dir}/mark");
    call("Persist", &mut [], r([t(&file), b("one")]));
    // A write that stopped before its end left a part of a file. The file
    // from before is whole, and the next write does not use the part.
    std::fs::write(format!("{file}.partial"), b"sec").unwrap();
    assert_eq!(std::fs::read(&file).unwrap(), b"one");
    call("Persist", &mut [], r([t(&file), b("second")]));
    assert_eq!(std::fs::read(&file).unwrap(), b"second");
    assert!(!std::path::Path::new(&format!("{file}.partial")).exists());
    // A folder that is not there is an error, and not a file somewhere else.
    assert!(
        attempt(
            "Persist",
            &mut [],
            r([t(&format!("{dir}/absent/mark")), b("x")])
        )
        .is_err()
    );
    call("Remove", &mut [], r([t(&file), n(0)]));
    call("Remove", &mut [], r([t(&file), n(0)]));
    assert!(!std::path::Path::new(&file).exists());
    let found = call("Resolve", &mut [], r([t("localhost"), n(4000)]));
    assert!(found.list().unwrap().iter().any(|a| {
        let a: &Value = a;
        a.str().unwrap().ends_with(":4000")
    }));
    // A socket number that is not open is told as such.
    assert_eq!(call("WaitReadable", &mut [], r([n(1_000_000), n(5)])), n(2));
}

/// A publisher changes its key. The reader is told the new key, and the
/// names of the publisher go on: a read in progress stops, and the next read
/// of the same peer gives its page.
#[test]
fn a_peer_changes_its_key_and_its_names_go_on() {
    let mut a = World::new(1, "turn-a");
    let mut c = World::new(2, "turn-b");
    let (a_peer, c_peer) = (
        (1, a.public.clone(), a.address.clone()),
        (2, c.public.clone(), c.address.clone()),
    );
    a.open(&[c_peer]);
    c.open(&[a_peer]);
    let k1 = c.public.clone();
    c.step(vec![r([t("Bind"), b("publ/one"), b("first"), r([])])]);
    a.step(vec![r([t("Read"), n(1), n(2), b("publ/one")])]);
    assert_eq!(exchange(&mut a, &mut c, 1), vec![(1, 0, b"first".to_vec())]);

    // The publisher gets a second key at life 2 and activates it. The old
    // key is then retired, and its version is deleted from the store.
    let imported = call(
        "KeysImport",
        &mut [&mut c.keys],
        r([t(&format!("{}/ring.2", c.dir)), r([n(2), n(2), n(0), n(1)])]),
    );
    let k2 = imported.at(0).unwrap().clone();
    assert_ne!(k1, k2);
    let accept = |world: &mut World, peer: u128, key: &V| {
        let held = call("AuthorityPeer", &mut [&mut world.authority], r([n(peer)]));
        let reply = call(
            "AuthorityStep",
            &mut [&mut world.authority],
            r([r([
                r([t("Advance"), n(2000)]),
                r([
                    t("Accept"),
                    held.at(0).unwrap().clone(),
                    point(peer, 2, 0, key),
                ]),
            ])]),
        );
        all_accepted(&reply);
        settle(
            &mut world.authority,
            &format!("{}/authority", world.dir),
            &reply,
        );
    };
    accept(&mut c, 2, &k2);
    let commit = |world: &mut World, offer: &V| {
        call(
            "Persist",
            &mut [],
            r([
                t(&format!("{}/manifest", world.dir)),
                offer.at(2).unwrap().clone(),
            ]),
        );
        let token = offer.at(0).unwrap();
        call(
            "KeysStep",
            &mut [&mut world.keys, &mut world.authority],
            r([
                t("Commit"),
                token.at(0).unwrap().clone(),
                token.at(1).unwrap().clone(),
            ]),
        );
    };
    let offer = call(
        "KeysStep",
        &mut [&mut c.keys, &mut c.authority],
        r([t("Prepare"), n(2)]),
    );
    commit(&mut c, &offer);
    let synced = call(
        "KeysSync",
        &mut [&mut c.keys, &mut c.authority, &mut c.engine],
        r([]),
    );
    assert_eq!(synced.at(0).unwrap().at(0).unwrap(), &n(0), "{synced:?}");
    let offer = call(
        "KeysStep",
        &mut [&mut c.keys, &mut c.authority],
        r([t("Retire"), n(1)]),
    );
    commit(&mut c, &offer);
    call(
        "KeysStep",
        &mut [&mut c.keys, &mut c.authority],
        r([t("Delete"), n(1)]),
    );
    // A retirement closes the engine (libmesa stops all work for a change of
    // the manifest). The key is activated again, and the engine is open.
    let closed = call(
        "KeysStep",
        &mut [&mut c.keys, &mut c.authority],
        r([t("Snapshot")]),
    );
    assert_eq!(closed.at(0).unwrap(), &n(0));
    let offer = call(
        "KeysStep",
        &mut [&mut c.keys, &mut c.authority],
        r([t("Prepare"), n(2)]),
    );
    commit(&mut c, &offer);
    let synced = call(
        "KeysSync",
        &mut [&mut c.keys, &mut c.authority, &mut c.engine],
        r([]),
    );
    assert_eq!(synced.at(0).unwrap().at(0).unwrap(), &n(0), "{synced:?}");
    let state = call(
        "KeysStep",
        &mut [&mut c.keys, &mut c.authority],
        r([t("Snapshot")]),
    );
    assert_eq!(state.at(0).unwrap(), &n(3));
    assert_eq!(state.at(2).unwrap(), &n(2));

    // The reader still holds the old key: a page under the new key does not
    // come to it. Then the reader is told the new key, and the read that is
    // in progress stops.
    c.step(vec![r([t("Bind"), b("publ/two"), b("second"), r([])])]);
    a.step(vec![r([t("Read"), n(2), n(2), b("publ/two")])]);
    for now in 10..40 {
        c.pump(now);
        assert!(a.pump(now).is_empty());
        std::thread::sleep(std::time::Duration::from_millis(2));
    }
    accept(&mut a, 2, &k2);
    let synced = call(
        "AuthoritySync",
        &mut [&mut a.authority, &mut a.engine, &mut a.endpoint],
        r([r([n(2)])]),
    );
    let stopped = synced.at(0).unwrap().at(0).unwrap().at(1).unwrap();
    assert!(
        stopped.list().unwrap().iter().any(|record| {
            record.at(0).unwrap().str().unwrap() == "ReadFinished"
                && record.at(1).unwrap() == &n(2)
                && record.at(2).unwrap() == &n(1)
        }),
        "{stopped:?}"
    );
    // A lower life is refused from now on: the old key does not come back.
    let held = call("AuthorityPeer", &mut [&mut a.authority], r([n(2)]));
    let back = call(
        "AuthorityStep",
        &mut [&mut a.authority],
        r([r([r([
            t("Accept"),
            held.at(0).unwrap().clone(),
            point(2, 1, 0, &k1),
        ])])]),
    );
    assert_eq!(
        back.at(0).unwrap().at(0).unwrap(),
        &r([n(1), t("Rollback")])
    );

    // The same name of the same peer is read again, under the new key.
    a.step(vec![r([t("Read"), n(3), n(2), b("publ/two")])]);
    assert_eq!(
        exchange(&mut a, &mut c, 1000),
        vec![(3, 0, b"second".to_vec())]
    );
}

/// A file that does not hold what Mesa wrote is refused. Nothing is made
/// from it, and nothing starts as if the file were not there.
#[test]
fn files_that_are_not_whole_are_refused() {
    let mut w = World::new(4, "spoiled");
    w.open(&[]);
    let manifest = std::fs::read(format!("{}/manifest", w.dir)).unwrap();
    let authority = std::fs::read(format!("{}/authority", w.dir)).unwrap();
    let keys = |bytes: &[u8]| {
        attempt(
            "Keys",
            &mut [],
            r([n(4), t(DOMAIN), n(77), n(7), n(1000), r([b(bytes), n(0)])]),
        )
        .map(|_| ())
    };
    let trusted =
        |bytes: &[u8]| attempt("Authority", &mut [], r([n(78), b(bytes), n(0)])).map(|_| ());
    assert!(keys(&manifest).is_ok());
    assert!(trusted(&authority).is_ok());
    // Cut short, changed, empty, or the file of the other kind.
    assert!(keys(&manifest[..manifest.len() / 2]).is_err());
    assert!(trusted(&authority[..authority.len() / 2]).is_err());
    assert!(keys(b"").is_err());
    assert!(trusted(b"").is_err());
    assert!(keys(&authority).is_err());
    assert!(trusted(&manifest).is_err());
    let mut bent = manifest.clone();
    let at = bent.iter().position(|c| *c == b'4').unwrap();
    bent[at] = b'5';
    assert!(keys(&bent).is_err());
    // The manifest of another world, and of another host epoch.
    assert_eq!(
        attempt(
            "Keys",
            &mut [],
            r([
                n(5),
                t(DOMAIN),
                n(77),
                n(7),
                n(1000),
                r([b(&manifest), n(0)])
            ]),
        )
        .err()
        .unwrap(),
        "Rollback"
    );
    assert_eq!(
        attempt(
            "Keys",
            &mut [],
            r([
                n(4),
                t(DOMAIN),
                n(77),
                n(8),
                n(1000),
                r([b(&manifest), n(0)])
            ]),
        )
        .err()
        .unwrap(),
        "Rollback"
    );
    // A key file that is not 32 bytes is not a key, and is not replaced.
    let ring = format!("{}/ring", w.dir);
    std::fs::write(&ring, b"short").unwrap();
    let (_, mut again) = make(
        "Keys",
        &mut [],
        r([
            n(4),
            t(DOMAIN),
            n(79),
            n(7),
            n(1000),
            r([b(&manifest), n(0)]),
        ]),
    );
    assert!(
        attempt(
            "KeysImport",
            &mut [&mut again],
            r([t(&ring), r([n(1), n(1), n(0), n(1)])]),
        )
        .is_err()
    );
    assert_eq!(std::fs::read(&ring).unwrap(), b"short");
}
