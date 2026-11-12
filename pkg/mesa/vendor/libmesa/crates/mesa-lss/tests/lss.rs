use mesa_lss::{Tree, Verifier, LEAF_BYTES, MAX_LEAVES};
fn data(n: usize) -> Vec<u8> {
    (0..n)
        .map(|i| if i % 29 >= 24 { 0 } else { (i % 251) as u8 })
        .collect()
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn c_reference_roots_spines_and_every_pair_match() {
    let fixtures: serde_json::Value =
        serde_json::from_str(include_str!("../../../vectors/lss/vere.json")).unwrap();
    for case in fixtures["cases"].as_array().unwrap() {
        let bytes = data(case["length"].as_u64().unwrap() as usize);
        let tree = Tree::build(&bytes).unwrap();
        assert_eq!(hex(&tree.root()), case["root"]);
        assert_eq!(hex(&tree.proof()), case["proof"]);
        let mut verifier = Verifier::new(bytes.len(), &tree.proof()).unwrap();
        for (i, chunk) in bytes.chunks(1024).enumerate() {
            let actual = tree
                .pair(i)
                .map(|pair| hex(&pair.iter().flatten().copied().collect::<Vec<_>>()));
            assert_eq!(actual.as_deref(), case["pairs"][i].as_str());
            verifier.ingest(chunk, tree.pair(i)).unwrap();
        }
        assert!(verifier.complete());
    }
}

#[test]
fn irregular_trees_and_failed_ingestion_are_transactional() {
    for n in 1..=65 {
        let bytes = data(n * 1024 - 13);
        let tree = Tree::build(&bytes).unwrap();
        let mut verifier = Verifier::new(bytes.len(), &tree.proof()).unwrap();
        assert_eq!(verifier.root(), tree.root());
        for (i, chunk) in bytes.chunks(1024).enumerate() {
            let mut bad = chunk.to_vec();
            bad[0] ^= 1;
            assert!(verifier.ingest(&bad, tree.pair(i)).is_err());
            assert_eq!(verifier.next_index(), i);
            if let Some(pair) = tree.pair(i) {
                let mut bad_pair = *pair;
                bad_pair[0][0] ^= 1;
                assert!(verifier.ingest(chunk, Some(&bad_pair)).is_err());
                assert!(verifier.ingest(chunk, None).is_err());
            } else {
                assert!(verifier.ingest(chunk, Some(&[[0; 32]; 2])).is_err());
            }
            assert_eq!(verifier.next_index(), i);
            verifier.ingest(chunk, tree.pair(i)).unwrap();
        }
        assert!(verifier.complete());
        assert!(verifier.ingest(&[], None).is_err());
    }
}

#[test]
fn empty_extension_and_strict_bounds() {
    let tree = Tree::build(&[]).unwrap();
    assert_eq!(tree.root(), *blake3::hash(&[]).as_bytes());
    let mut v = Verifier::new(0, &tree.proof()).unwrap();
    v.ingest(&[], None).unwrap();
    assert!(v.complete());
    assert!(Tree::build(&vec![0; MAX_LEAVES * LEAF_BYTES + 1]).is_err());
    assert!(Verifier::new(usize::MAX, &[]).is_err());
    for n in [0, 1, 1024, 1025, 4096] {
        let proof = Tree::build(&data(n)).unwrap().proof();
        for end in 0..proof.len() {
            assert!(Verifier::new(n, &proof[..end]).is_err());
        }
        let mut extra = proof;
        extra.push(0);
        assert!(Verifier::new(n, &extra).is_err());
    }
}
