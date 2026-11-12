use mesa_transport::{PacketIo, Received, Stream};
use std::{
    collections::VecDeque,
    io::{self, Read, Write},
    sync::{Arc, Mutex},
};
#[derive(Default)]
struct State {
    input: VecDeque<u8>,
    output: Vec<u8>,
    read_chunk: usize,
    write_chunk: usize,
    eof: bool,
    writes: usize,
}
#[derive(Clone, Default)]
struct Io(Arc<Mutex<State>>);
impl Read for Io {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        let mut s = self.0.lock().unwrap();
        if s.input.is_empty() {
            return if s.eof {
                Ok(0)
            } else {
                Err(io::ErrorKind::WouldBlock.into())
            };
        }
        let n = out.len().min(s.input.len()).min(s.read_chunk);
        if n == 0 {
            return Err(io::ErrorKind::WouldBlock.into());
        }
        for byte in &mut out[..n] {
            *byte = s.input.pop_front().unwrap();
        }
        Ok(n)
    }
}
impl Write for Io {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let mut s = self.0.lock().unwrap();
        s.writes += 1;
        if s.write_chunk == 0 {
            return Err(io::ErrorKind::WouldBlock.into());
        }
        let n = bytes.len().min(s.write_chunk);
        s.output.extend_from_slice(&bytes[..n]);
        Ok(n)
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
fn frame(bytes: &[u8]) -> Vec<u8> {
    let mut v = (bytes.len() as u32).to_be_bytes().to_vec();
    v.extend_from_slice(bytes);
    v
}
#[test]
fn every_split_and_coalesced_frames_preserve_exact_bytes() {
    let data = frame(&[42; 73]);
    for split in 0..=data.len() {
        let io = Io::default();
        io.0.lock().unwrap().read_chunk = 10000;
        let mut stream = Stream::new(io.clone(), 1472, 4096).unwrap();
        io.0.lock().unwrap().input.extend(&data[..split]);
        let first = stream.receive().unwrap();
        assert!(stream.buffered_bytes() <= 1476);
        if split < data.len() {
            assert_eq!(first, Received::Idle);
        } else {
            assert_eq!(first, Received::Packet(vec![42; 73]));
        }
        io.0.lock().unwrap().input.extend(&data[split..]);
        if split < data.len() {
            assert_eq!(stream.receive().unwrap(), Received::Packet(vec![42; 73]));
        }
        let mut both = frame(&[1; 8]);
        both.extend(frame(&[2; 1472]));
        io.0.lock().unwrap().input.extend(both);
        assert_eq!(stream.receive().unwrap(), Received::Packet(vec![1; 8]));
        assert_eq!(stream.receive().unwrap(), Received::Packet(vec![2; 1472]));
        assert_eq!(stream.buffered_bytes(), 0);
        assert_eq!(stream.receive().unwrap(), Received::Idle);
    }
}
#[test]
fn bytewise_input_and_partial_writes_remain_bounded() {
    let io = Io::default();
    io.0.lock().unwrap().read_chunk = 1;
    let mut stream = Stream::new(io.clone(), 1472, 1476).unwrap();
    assert!(stream.send(&[9; 1472]).unwrap());
    assert!(!stream.send(&[1; 8]).unwrap());
    stream.flush().unwrap();
    assert_eq!(stream.queued_bytes(), 1476);
    io.0.lock().unwrap().write_chunk = 1;
    stream.flush().unwrap();
    assert_eq!(io.0.lock().unwrap().output.len(), 64);
    assert_eq!(stream.queued_bytes(), 1476);
    for _ in 0..24 {
        stream.flush().unwrap();
    }
    assert_eq!(stream.queued_bytes(), 0);
    let output = io.0.lock().unwrap().output.clone();
    assert_eq!(output, frame(&[9; 1472]));
    io.0.lock().unwrap().input.extend(output);
    let mut received = None;
    for _ in 0..1476 {
        match stream.receive().unwrap() {
            Received::Packet(p) => {
                received = Some(p);
                break;
            }
            Received::Idle => (),
            other => panic!("{other:?}"),
        }
    }
    assert_eq!(received, Some(vec![9; 1472]));
    assert!(!stream.send(&[0; 1473]).unwrap());
    assert!(!stream.send(&[]).unwrap());
}
#[test]
fn invalid_length_and_eof_poison_lane_and_release_buffers() {
    for n in [0u32, 1, 7, 1473, u32::MAX] {
        let io = Io::default();
        {
            let mut s = io.0.lock().unwrap();
            s.read_chunk = 4;
            s.input.extend(n.to_be_bytes());
        }
        let mut stream = Stream::new(io, 1472, 1476).unwrap();
        stream.send(&[2; 8]).unwrap();
        assert_eq!(
            stream.receive().unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
        assert_eq!(stream.buffered_bytes(), 0);
        assert_eq!(stream.queued_bytes(), 0);
        assert_eq!(
            stream.send(&[3; 8]).unwrap_err().kind(),
            io::ErrorKind::NotConnected
        );
    }
    let data = frame(&[5; 30]);
    for split in 0..data.len() {
        let io = Io::default();
        {
            let mut s = io.0.lock().unwrap();
            s.read_chunk = 100;
            s.input.extend(&data[..split]);
            s.eof = true;
        }
        let mut stream = Stream::new(io, 1472, 1476).unwrap();
        let result = stream.receive();
        let error = if let Err(error) = result {
            error
        } else {
            stream.receive().unwrap_err()
        };
        assert_eq!(error.kind(), io::ErrorKind::UnexpectedEof);
        assert_eq!(stream.buffered_bytes(), 0);
    }
}
