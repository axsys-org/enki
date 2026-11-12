use mesa_wire::{reconstruct_data_fragment, Error};

#[derive(serde::Deserialize)]
struct Case {
    name: String,
    total: u64,
    bloq: u8,
    index: u64,
    wire: String,
    length: usize,
    prefix: String,
    zero_tail: usize,
}

fn unhex(s: &str) -> Vec<u8> {
    s.as_bytes()
        .as_chunks::<2>()
        .0
        .iter()
        .map(|b| u8::from_str_radix(std::str::from_utf8(b).unwrap(), 16).unwrap())
        .collect()
}

#[test]
fn reconstruct_logical_bytes_before_lss() {
    let cases: Vec<Case> =
        serde_json::from_str(include_str!("../../../vectors/pact/v1-reassembly.json")).unwrap();
    assert_eq!(cases.len(), 5);
    for c in cases {
        let bytes =
            reconstruct_data_fragment(c.total, c.bloq, c.index, &unhex(&c.wire), 1024).unwrap();
        let prefix = unhex(&c.prefix);
        assert_eq!(bytes.len(), c.length, "{}", c.name);
        assert_eq!(&bytes[..prefix.len()], prefix);
        assert_eq!(bytes[prefix.len()..], vec![0; c.zero_tail]);
    }
}

#[test]
fn hostile_coordinates_do_not_allocate_messages() {
    assert_eq!(
        reconstruct_data_fragment(2048, 13, 2, &[], 1024),
        Err(Error::FragmentRange)
    );
    assert_eq!(
        reconstruct_data_fragment(0, 13, 0, &[], 1024),
        Err(Error::FragmentRange)
    );
    assert_eq!(
        reconstruct_data_fragment(1, 13, 0, &[1, 2], 1024),
        Err(Error::FragmentRange)
    );
    assert_eq!(
        reconstruct_data_fragment(u64::MAX, 13, u64::MAX, &[], 1024),
        Err(Error::Limit)
    );
    for bloq in [0, 2, 67, 255] {
        assert_eq!(
            reconstruct_data_fragment(1, bloq, 0, &[], 1024),
            Err(Error::Limit)
        );
    }
    assert_eq!(
        reconstruct_data_fragment(u64::MAX, 31, 0, &[], 1024),
        Err(Error::Limit)
    );
    assert_eq!(
        reconstruct_data_fragment(u64::MAX, 13, 0, &[], 1024)
            .unwrap()
            .len(),
        1024
    );
}
