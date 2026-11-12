//! Conformance runner: <name> <hex> lines; reject:<name> expects rejection.
use mesa_wire::legacy_v1::{decode, encode, Checksum, Limits};

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args().nth(1).ok_or("expected vector file")?;
    let input = std::fs::read_to_string(path)?;
    let mut count = 0;
    for line in input.lines() {
        let (name, hex) = line.split_once(' ').ok_or("missing separator")?;
        if name.is_empty() || hex.len() & 1 != 0 {
            return Err("invalid vector".into());
        }
        let bytes: Vec<u8> = hex
            .as_bytes()
            .as_chunks::<2>()
            .0
            .iter()
            .map(|p| u8::from_str_radix(std::str::from_utf8(p)?, 16).map_err(Into::into))
            .collect::<Result<_, Box<dyn std::error::Error>>>()?;
        let parsed = decode(&bytes, Checksum::VereBytes, Limits::DATAGRAM);
        if name.starts_with("reject:") {
            if parsed.is_ok() {
                return Err(format!("{name}: unexpected acceptance").into());
            }
        } else {
            let packet = parsed.map_err(|e| format!("{name}: {e}"))?;
            if encode(&packet, Checksum::VereBytes, Limits::DATAGRAM)? != bytes {
                return Err(format!("{name}: roundtrip differs").into());
            }
        }
        count += 1;
    }
    if count == 0 {
        return Err("empty corpus".into());
    }
    println!("{count} conformance cases passed");
    Ok(())
}

fn main() {
    if let Err(e) = run() {
        eprintln!("{e}");
        std::process::exit(1);
    }
}
