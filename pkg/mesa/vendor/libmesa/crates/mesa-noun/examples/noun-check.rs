//! Positive conformance corpus: <exact-bit-length> <canonical-jam-hex> per line.
use mesa_noun::{
    bits::Bits,
    serial::{cue, jam, Limits},
};

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let file = std::env::args().nth(1).ok_or("expected vector file")?;
    let input = std::fs::read_to_string(file)?;
    let mut count = 0;
    for line in input.lines() {
        let (bits, hex) = line.split_once(' ').ok_or("missing separator")?;
        let bits: usize = bits.parse()?;
        if bits == 0 || hex.is_empty() || !hex.len().is_multiple_of(2) {
            return Err("invalid vector".into());
        }
        let bytes: Vec<u8> = hex
            .as_bytes()
            .as_chunks::<2>()
            .0
            .iter()
            .map(|p| u8::from_str_radix(std::str::from_utf8(p)?, 16).map_err(Into::into))
            .collect::<Result<_, Box<dyn std::error::Error>>>()?;
        let input = Bits::from_atom_bytes(&bytes)?;
        if bits != input.len() {
            return Err("bit length mismatch".into());
        }
        let noun = cue(input, Limits::default())?;
        if jam(&noun, Limits::default())?.bytes != bytes {
            return Err("roundtrip mismatch".into());
        }
        count += 1;
    }
    if count == 0 {
        return Err("empty corpus".into());
    }
    println!("{count} canonical noun cases passed");
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
