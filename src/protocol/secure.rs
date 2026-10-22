//! Bounded HAP control/event records. Wire reference: shairport-sync
//! b972e900d9e68d3cb29745c7e46cf9d74abb6260, pair_ap/pair_homekit.c.
//! A verified pairing chooses encryption explicitly; failures never retry plaintext.
use anyhow::{Result, ensure};
use chacha20poly1305::{ChaCha20Poly1305, KeyInit, Nonce, Tag, aead::AeadInPlace};
use hkdf::Hkdf;
use sha2::Sha512;
use std::{
    io::{self, Read, Write},
    net::TcpStream,
    time::{Duration, Instant},
};
use zeroize::{Zeroize, Zeroizing};

const MAX_PLAIN: usize = 1024;
const TAG_BYTES: usize = 16;
const MAX_RECORD: usize = 2 + MAX_PLAIN + TAG_BYTES;
// read_request may already hold the start of the first encrypted request when
// M3 and that request arrive together. This is a separate, fixed upgrade budget.
const MAX_UPGRADE_PENDING: usize = 4096;
const IO_STEP: Duration = Duration::from_millis(100);
const RECORD_DEADLINE: Duration = Duration::from_secs(5);

pub fn derive_key(shared: &[u8], salt: &[u8], info: &[u8]) -> Result<[u8; 32]> {
    ensure!(matches!(shared.len(), 32 | 64), "pairing secret length");
    ensure!(
        salt.len() <= 128 && info.len() <= 128,
        "pairing key label budget"
    );
    let mut key = [0; 32];
    Hkdf::<Sha512>::new(Some(salt), shared)
        .expand(info, &mut key)
        .map_err(|_| anyhow::anyhow!("pairing key derivation failed"))?;
    Ok(key)
}

/// Direction is from this receiver's perspective. No secret-bearing Debug/Clone.
pub struct RecordKeys {
    read: Zeroizing<[u8; 32]>,
    write: Zeroizing<[u8; 32]>,
}
impl RecordKeys {
    pub fn control_receiver(shared: &[u8]) -> Result<Self> {
        Self::derive(
            shared,
            b"Control-Salt",
            b"Control-Write-Encryption-Key",
            b"Control-Read-Encryption-Key",
        )
    }

    pub fn event_receiver(shared: &[u8]) -> Result<Self> {
        // Events reverse the control-channel directions: the receiver sends
        // commands and the sender responds on its accepted reverse connection.
        Self::derive(
            shared,
            b"Events-Salt",
            b"Events-Read-Encryption-Key",
            b"Events-Write-Encryption-Key",
        )
    }

    fn derive(shared: &[u8], salt: &[u8], read: &[u8], write: &[u8]) -> Result<Self> {
        Ok(Self {
            read: Zeroizing::new(derive_key(shared, salt, read)?),
            write: Zeroizing::new(derive_key(shared, salt, write)?),
        })
    }
}

struct Records {
    keys: RecordKeys,
    read_counter: u64,
    write_counter: u64,
    incoming: Zeroizing<[u8; MAX_RECORD]>,
    incoming_len: usize,
    incoming_started: Option<Instant>,
    plain: Zeroizing<[u8; MAX_PLAIN]>,
    plain_len: usize,
    plain_pos: usize,
    outgoing: Zeroizing<[u8; MAX_RECORD]>,
    outgoing_len: usize,
    outgoing_pos: usize,
    early: Vec<u8>,
    early_pos: usize,
}
impl Records {
    fn new(keys: RecordKeys, early: Vec<u8>) -> Self {
        Self {
            keys,
            read_counter: 0,
            write_counter: 0,
            incoming: Zeroizing::new([0; MAX_RECORD]),
            incoming_len: 0,
            incoming_started: None,
            plain: Zeroizing::new([0; MAX_PLAIN]),
            plain_len: 0,
            plain_pos: 0,
            outgoing: Zeroizing::new([0; MAX_RECORD]),
            outgoing_len: 0,
            outgoing_pos: 0,
            early,
            early_pos: 0,
        }
    }

    fn flush(&mut self, socket: &mut impl Write) -> io::Result<()> {
        let started = Instant::now();
        while self.outgoing_pos < self.outgoing_len {
            if started.elapsed() >= IO_STEP {
                return Err(io::ErrorKind::TimedOut.into());
            }
            let n = socket.write(&self.outgoing[self.outgoing_pos..self.outgoing_len])?;
            if n == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "encrypted record write closed",
                ));
            }
            self.outgoing_pos += n;
        }
        self.outgoing[..self.outgoing_len].zeroize();
        self.outgoing_len = 0;
        self.outgoing_pos = 0;
        Ok(())
    }

    fn queue(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let next = self.write_counter.checked_add(1).ok_or_else(exhausted)?;
        let n = bytes.len().min(MAX_PLAIN);
        let aad = (n as u16).to_le_bytes();
        self.outgoing[..2].copy_from_slice(&aad);
        self.outgoing[2..2 + n].copy_from_slice(&bytes[..n]);
        let cipher = ChaCha20Poly1305::new((&*self.keys.write).into());
        let tag = cipher
            .encrypt_in_place_detached(
                Nonce::from_slice(&nonce(self.write_counter)),
                &aad,
                &mut self.outgoing[2..2 + n],
            )
            .map_err(|_| invalid("encrypted record encoding failed"))?;
        self.outgoing[2 + n..2 + n + TAG_BYTES].copy_from_slice(&tag);
        self.outgoing_len = 2 + n + TAG_BYTES;
        self.outgoing_pos = 0;
        self.write_counter = next;
        Ok(n)
    }

    fn read(&mut self, socket: &mut impl Read, bytes: &mut [u8]) -> io::Result<usize> {
        let started = Instant::now();
        if self.plain_pos == self.plain_len {
            loop {
                if self
                    .incoming_started
                    .is_some_and(|start| start.elapsed() >= RECORD_DEADLINE)
                {
                    return Err(invalid("encrypted record receive deadline"));
                }
                // Even a peer trickling bytes without a kernel timeout must
                // return control for the caller's cancellation/overall deadline.
                if started.elapsed() >= IO_STEP {
                    return Err(io::ErrorKind::TimedOut.into());
                }
                let needed = if self.incoming_len < 2 {
                    2
                } else {
                    let n = usize::from(u16::from_le_bytes(self.incoming[..2].try_into().unwrap()));
                    if !(1..=MAX_PLAIN).contains(&n) {
                        return Err(invalid("encrypted record length outside bounds"));
                    }
                    2 + n + TAG_BYTES
                };
                if self.incoming_len == needed && needed > 2 {
                    let next = self.read_counter.checked_add(1).ok_or_else(exhausted)?;
                    let n = needed - 2 - TAG_BYTES;
                    let aad: [u8; 2] = self.incoming[..2].try_into().unwrap();
                    let tag = Tag::clone_from_slice(&self.incoming[2 + n..needed]);
                    self.plain[..n].copy_from_slice(&self.incoming[2..2 + n]);
                    let cipher = ChaCha20Poly1305::new((&*self.keys.read).into());
                    cipher
                        .decrypt_in_place_detached(
                            Nonce::from_slice(&nonce(self.read_counter)),
                            &aad,
                            &mut self.plain[..n],
                            &tag,
                        )
                        .map_err(|_| invalid("encrypted record authentication failed"))?;
                    self.read_counter = next;
                    self.incoming[..needed].zeroize();
                    self.incoming_len = 0;
                    self.incoming_started = None;
                    self.plain_pos = 0;
                    self.plain_len = n;
                    break;
                }
                let target = &mut self.incoming[self.incoming_len..needed];
                let n = if self.early_pos < self.early.len() {
                    let n = target.len().min(self.early.len() - self.early_pos);
                    target[..n].copy_from_slice(&self.early[self.early_pos..self.early_pos + n]);
                    self.early_pos += n;
                    if self.early_pos == self.early.len() {
                        self.early.clear();
                    }
                    n
                } else {
                    socket.read(target)?
                };
                if n == 0 {
                    return if self.incoming_len == 0 {
                        Ok(0)
                    } else {
                        Err(io::Error::new(
                            io::ErrorKind::UnexpectedEof,
                            "truncated encrypted record",
                        ))
                    };
                }
                self.incoming_len += n;
                self.incoming_started.get_or_insert_with(Instant::now);
            }
        }
        let n = bytes.len().min(self.plain_len - self.plain_pos);
        bytes[..n].copy_from_slice(&self.plain[self.plain_pos..self.plain_pos + n]);
        self.plain[self.plain_pos..self.plain_pos + n].zeroize();
        self.plain_pos += n;
        Ok(n)
    }
}

fn nonce(counter: u64) -> [u8; 12] {
    let mut nonce = [0; 12];
    nonce[4..].copy_from_slice(&counter.to_le_bytes());
    nonce
}
fn invalid(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}
fn exhausted() -> io::Error {
    invalid("encrypted record counter exhausted")
}
fn retryable(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted
    )
}

/// One TCP connection, explicitly plaintext or authenticated records.
/// Like BufWriter, successful write() may retain one accepted record. Call
/// flush() to complete a response before upgrade, shutdown or awaiting a reply.
pub struct ControlStream {
    socket: TcpStream,
    records: Option<Box<Records>>,
    failed: bool,
}
impl ControlStream {
    pub fn new(socket: TcpStream) -> Self {
        Self {
            socket,
            records: None,
            failed: false,
        }
    }

    pub fn new_encrypted(socket: TcpStream, keys: RecordKeys) -> Self {
        Self {
            socket,
            records: Some(Box::new(Records::new(keys, Vec::new()))),
            failed: false,
        }
    }

    /// Call only after the plaintext M4 response was fully flushed. Bytes already
    /// read by the request parser belong to the encrypted next request.
    pub fn upgrade(&mut self, keys: RecordKeys, already_read_ciphertext: Vec<u8>) -> Result<()> {
        ensure!(
            !self.failed && self.records.is_none(),
            "control encryption already selected or failed"
        );
        if already_read_ciphertext.len() > MAX_UPGRADE_PENDING {
            self.failed = true;
            anyhow::bail!("encrypted upgrade residual budget");
        }
        self.records = Some(Box::new(Records::new(keys, already_read_ciphertext)));
        Ok(())
    }

    fn check(&self) -> io::Result<()> {
        if self.failed {
            Err(invalid("encrypted channel unavailable after failure"))
        } else {
            Ok(())
        }
    }

    fn checked<T>(&mut self, result: io::Result<T>) -> io::Result<T> {
        if result.as_ref().is_err_and(|error| !retryable(error)) {
            self.failed = true;
            self.records = None; // erase keys and any unauthenticated/plain buffers
        }
        result
    }
}
impl Read for ControlStream {
    fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
        self.check()?;
        if bytes.is_empty() {
            return Ok(0);
        }
        let result = if let Some(records) = &mut self.records {
            records
                .flush(&mut self.socket)
                .and_then(|()| records.read(&mut self.socket, bytes))
        } else {
            self.socket.read(bytes)
        };
        self.checked(result)
    }
}
impl Write for ControlStream {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        self.check()?;
        if bytes.is_empty() {
            return Ok(0);
        }
        let result = if let Some(records) = &mut self.records {
            records.flush(&mut self.socket).and_then(|()| {
                let n = records.queue(bytes)?;
                match records.flush(&mut self.socket) {
                    Ok(()) => Ok(n),
                    // The plaintext has been accepted exactly once. Preserve its
                    // partial ciphertext; flush/next write retries only that tail.
                    Err(error) if retryable(&error) => Ok(n),
                    Err(error) => Err(error),
                }
            })
        } else {
            self.socket.write(bytes)
        };
        self.checked(result)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.check()?;
        let result = if let Some(records) = &mut self.records {
            records.flush(&mut self.socket)
        } else {
            self.socket.flush()
        };
        self.checked(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        net::{Shutdown, TcpListener},
        sync::{
            Arc, Mutex,
            atomic::{AtomicBool, Ordering},
        },
        thread,
    };

    fn sockets() -> (TcpStream, TcpStream) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let peer = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (receiver, _) = listener.accept().unwrap();
        for stream in [&peer, &receiver] {
            stream
                .set_read_timeout(Some(Duration::from_millis(50)))
                .unwrap();
            stream
                .set_write_timeout(Some(Duration::from_secs(1)))
                .unwrap();
            stream.set_nodelay(true).unwrap();
        }
        (peer, receiver)
    }

    fn wire(key: &[u8; 32], counter: u64, plain: &[u8]) -> Vec<u8> {
        let aad = (plain.len() as u16).to_le_bytes();
        let mut body = plain.to_vec();
        ChaCha20Poly1305::new(key.into())
            .encrypt_in_place(Nonce::from_slice(&nonce(counter)), &aad, &mut body)
            .unwrap();
        [aad.as_slice(), body.as_slice()].concat()
    }

    #[test]
    fn fragmented_record_survives_header_and_body_timeouts() {
        let (mut peer, receiver) = sockets();
        let keys = RecordKeys::control_receiver(&[0x31; 64]).unwrap();
        let packet = wire(&keys.read, 0, &[0x5a; 700]);
        let mut stream = ControlStream::new_encrypted(receiver, keys);
        let mut output = [0xa5; 700];
        peer.write_all(&packet[..1]).unwrap();
        let started = Instant::now();
        assert!(retryable(&stream.read(&mut output).unwrap_err()));
        assert!(started.elapsed() >= Duration::from_millis(25));
        assert_eq!(output, [0xa5; 700]);
        peer.write_all(&packet[1..211]).unwrap();
        assert!(retryable(&stream.read(&mut output).unwrap_err()));
        assert_eq!(output, [0xa5; 700]);
        peer.write_all(&packet[211..]).unwrap();
        stream.read_exact(&mut output).unwrap();
        assert_eq!(output, [0x5a; 700]);
        assert_eq!(stream.records.as_ref().unwrap().read_counter, 1);
    }

    #[test]
    fn coalesced_records_and_partial_plain_reads_preserve_order() {
        let (mut peer, receiver) = sockets();
        let keys = RecordKeys::control_receiver(&[0x32; 64]).unwrap();
        let expected: Vec<u8> = (0..2345).map(|i| (i % 251) as u8).collect();
        let mut packets = Vec::new();
        for (counter, plain) in expected.chunks(MAX_PLAIN).enumerate() {
            packets.extend(wire(&keys.read, counter as u64, plain));
        }
        peer.write_all(&packets).unwrap();
        let mut stream = ControlStream::new_encrypted(receiver, keys);
        let mut actual = Vec::new();
        while actual.len() < expected.len() {
            let mut small = [0; 73];
            let n = stream.read(&mut small).unwrap();
            actual.extend_from_slice(&small[..n]);
        }
        assert_eq!(actual, expected);
        assert_eq!(stream.records.as_ref().unwrap().read_counter, 3);
    }

    #[test]
    fn bidirectional_records_split_at_bound_and_use_distinct_keys() {
        let (peer, receiver) = sockets();
        let receive_keys = RecordKeys::control_receiver(&[0x33; 64]).unwrap();
        let mut peer_keys = RecordKeys::control_receiver(&[0x33; 64]).unwrap();
        std::mem::swap(&mut peer_keys.read, &mut peer_keys.write);
        let mut peer = ControlStream::new_encrypted(peer, peer_keys);
        let mut receiver = ControlStream::new_encrypted(receiver, receive_keys);
        let expected = vec![0x4c; MAX_PLAIN * 2 + 3];
        receiver.write_all(&expected).unwrap();
        receiver.flush().unwrap();
        let mut actual = vec![0; expected.len()];
        peer.read_exact(&mut actual).unwrap();
        assert_eq!(actual, expected);
        assert_eq!(receiver.records.as_ref().unwrap().write_counter, 3);
        peer.write_all(b"reply").unwrap();
        peer.flush().unwrap();
        let mut reply = [0; 5];
        receiver.read_exact(&mut reply).unwrap();
        assert_eq!(&reply, b"reply");
    }

    #[test]
    fn tag_header_and_ciphertext_tampering_poison_both_directions() {
        for changed in [0, 2, 25] {
            let (mut peer, receiver) = sockets();
            let keys = RecordKeys::control_receiver(&[0x34; 64]).unwrap();
            let mut packet = wire(&keys.read, 0, b"tamper-test");
            packet[changed] ^= 1;
            peer.write_all(&packet).unwrap();
            peer.shutdown(Shutdown::Write).unwrap();
            let mut stream = ControlStream::new_encrypted(receiver, keys);
            let mut output = [0xa5; 100];
            assert!(stream.read(&mut output).is_err());
            assert_eq!(output, [0xa5; 100]);
            assert!(stream.records.is_none());
            assert!(stream.read(&mut output).is_err());
            assert!(stream.write(b"RTSP/1.0 200 OK\r\n\r\n").is_err());
            assert!(stream.flush().is_err());
        }
    }

    #[test]
    fn replay_and_cross_channel_keys_cannot_authenticate() {
        for cross_channel in [false, true] {
            let (mut peer, receiver) = sockets();
            let keys = RecordKeys::control_receiver(&[0x35; 64]).unwrap();
            let events = RecordKeys::event_receiver(&[0x35; 64]).unwrap();
            assert_ne!(*keys.read, *events.read);
            assert_ne!(*keys.read, *keys.write);
            let packet = wire(
                if cross_channel {
                    &events.read
                } else {
                    &keys.read
                },
                0,
                b"one",
            );
            peer.write_all(&packet).unwrap();
            peer.write_all(&packet).unwrap();
            let mut stream = ControlStream::new_encrypted(receiver, keys);
            let mut output = [0; 3];
            if !cross_channel {
                stream.read_exact(&mut output).unwrap();
            }
            assert!(stream.read(&mut output).is_err());
        }
    }

    #[test]
    fn zero_oversize_and_truncated_records_fail_closed() {
        let packets = [vec![0, 0], vec![1, 4], vec![1], vec![1, 0], vec![1, 0, 0]];
        for packet in packets {
            let (mut peer, receiver) = sockets();
            peer.write_all(&packet).unwrap();
            peer.shutdown(Shutdown::Write).unwrap();
            let mut stream = ControlStream::new_encrypted(
                receiver,
                RecordKeys::control_receiver(&[0x36; 64]).unwrap(),
            );
            assert!(stream.read(&mut [0; 100]).is_err());
            assert!(stream.failed);
        }
    }

    #[test]
    fn counters_never_wrap_and_upgrade_never_reverts() {
        for reading in [false, true] {
            let (mut peer, receiver) = sockets();
            let keys = RecordKeys::control_receiver(&[0x37; 64]).unwrap();
            let packet = wire(&keys.read, u64::MAX, b"last");
            let mut stream = ControlStream::new_encrypted(receiver, keys);
            if reading {
                stream.records.as_mut().unwrap().read_counter = u64::MAX;
                peer.write_all(&packet).unwrap();
                assert!(stream.read(&mut [0; 4]).is_err());
            } else {
                stream.records.as_mut().unwrap().write_counter = u64::MAX;
                assert!(stream.write(b"last").is_err());
            }
            assert!(stream.failed);
            assert!(
                stream
                    .upgrade(
                        RecordKeys::control_receiver(&[0x37; 64]).unwrap(),
                        Vec::new()
                    )
                    .is_err()
            );
        }
    }

    #[test]
    fn m4_is_plaintext_and_upgrade_consumes_buffered_encrypted_tail() {
        let (mut peer, receiver) = sockets();
        let keys = RecordKeys::control_receiver(&[0x38; 64]).unwrap();
        let packet = wire(&keys.read, 0, b"GET /info RTSP/1.0\r\nCSeq: 4\r\n\r\n");
        let mut stream = ControlStream::new(receiver);
        stream.write_all(b"M4-plaintext").unwrap();
        stream.flush().unwrap();
        let mut m4 = [0; 12];
        peer.read_exact(&mut m4).unwrap();
        assert_eq!(&m4, b"M4-plaintext");
        stream.upgrade(keys, packet[..7].to_vec()).unwrap();
        peer.write_all(&packet[7..]).unwrap();
        let request = super::super::framing::read_request(
            &mut stream,
            &mut Vec::new(),
            || false,
            Duration::from_secs(1),
        )
        .unwrap()
        .unwrap();
        assert_eq!(request.target, "/info");
        assert_eq!(request.headers["cseq"], "4");
        assert!(
            stream
                .upgrade(
                    RecordKeys::control_receiver(&[0x38; 64]).unwrap(),
                    Vec::new()
                )
                .is_err()
        );
    }

    #[test]
    fn oversized_upgrade_residual_and_invalid_secret_are_rejected() {
        let (_peer, receiver) = sockets();
        let mut stream = ControlStream::new(receiver);
        assert!(
            stream
                .upgrade(
                    RecordKeys::control_receiver(&[0x39; 64]).unwrap(),
                    vec![0; MAX_UPGRADE_PENDING + 1]
                )
                .is_err()
        );
        assert!(stream.write(b"plaintext").is_err());
        assert!(derive_key(&[0; 31], b"salt", b"info").is_err());
        assert!(derive_key(&[0; 65], b"salt", b"info").is_err());
        assert!(derive_key(&[0; 64], &[0; 129], b"info").is_err());
    }

    #[test]
    fn partial_write_timeout_resumes_exact_ciphertext_without_reencrypting() {
        struct ShortWrite {
            bytes: Vec<u8>,
            calls: usize,
        }
        impl Write for ShortWrite {
            fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
                self.calls += 1;
                if self.calls == 2 {
                    return Err(io::ErrorKind::TimedOut.into());
                }
                let n = if self.calls == 1 {
                    7.min(bytes.len())
                } else {
                    bytes.len()
                };
                self.bytes.extend_from_slice(&bytes[..n]);
                Ok(n)
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }
        let keys = RecordKeys::control_receiver(&[0x40; 64]).unwrap();
        let expected = wire(&keys.write, 0, b"exactly-once");
        let mut records = Records::new(keys, Vec::new());
        records.queue(b"exactly-once").unwrap();
        let mut sink = ShortWrite {
            bytes: Vec::new(),
            calls: 0,
        };
        assert_eq!(
            records.flush(&mut sink).unwrap_err().kind(),
            io::ErrorKind::TimedOut
        );
        assert_eq!(records.outgoing_pos, 7);
        records.flush(&mut sink).unwrap();
        assert_eq!(sink.bytes, expected);
        assert_eq!(records.write_counter, 1);
        assert_eq!(records.outgoing_len, 0);
    }

    #[test]
    fn partial_record_has_total_deadline_despite_repeated_progress() {
        let (_peer, receiver) = sockets();
        let mut stream = ControlStream::new_encrypted(
            receiver,
            RecordKeys::control_receiver(&[0x41; 64]).unwrap(),
        );
        let records = stream.records.as_mut().unwrap();
        records.incoming[0] = 1;
        records.incoming_len = 1;
        records.incoming_started = Some(Instant::now() - RECORD_DEADLINE);
        let error = stream.read(&mut [0; 4]).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidData);
        assert!(stream.failed);
    }

    #[test]
    fn encrypted_event_ack_and_bad_tag_retain_optional_connection() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let logs = Arc::new(Mutex::new(Vec::<String>::new()));
        let updates = Arc::new(super::super::events::DisplayUpdates::new(994, 2160).unwrap());
        let (worker_stop, worker_logs, worker_updates) =
            (stop.clone(), logs.clone(), updates.clone());
        let worker = thread::spawn(move || {
            super::super::events::run_with_keys(
                listener,
                address.ip(),
                worker_stop,
                worker_updates,
                super::super::plist::dict([
                    (
                        "uuid",
                        plist::Value::String("synthetic-encrypted-display".into()),
                    ),
                    ("rotation", plist::Value::Boolean(true)),
                ]),
                Some(RecordKeys::event_receiver(&[0x42; 64]).unwrap()),
                |line| worker_logs.lock().unwrap().push(line.into()),
            )
        });
        let socket = TcpStream::connect(address).unwrap();
        socket
            .set_read_timeout(Some(Duration::from_millis(100)))
            .unwrap();
        socket
            .set_write_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        let mut peer_keys = RecordKeys::event_receiver(&[0x42; 64]).unwrap();
        std::mem::swap(&mut peer_keys.read, &mut peer_keys.write);
        let mut peer = ControlStream::new_encrypted(socket, peer_keys);
        let mut pending = Vec::new();
        updates.request(2160, 994).unwrap();
        let first = super::super::framing::read_request(
            &mut peer,
            &mut pending,
            || false,
            Duration::from_secs(2),
        )
        .unwrap()
        .unwrap();
        assert_eq!(first.target, "/command");
        assert_eq!(first.headers["cseq"], "1");
        peer.write_all(b"RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n")
            .unwrap();
        peer.flush().unwrap();
        updates.request(994, 2160).unwrap();
        let second = super::super::framing::read_request(
            &mut peer,
            &mut pending,
            || false,
            Duration::from_secs(2),
        )
        .unwrap()
        .unwrap();
        assert_eq!(second.headers["cseq"], "2");
        let receive_keys = RecordKeys::event_receiver(&[0x42; 64]).unwrap();
        let mut corrupt = wire(
            &receive_keys.read,
            1,
            b"RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n",
        );
        *corrupt.last_mut().unwrap() ^= 1;
        peer.socket.write_all(&corrupt).unwrap();
        let deadline = Instant::now() + Duration::from_secs(2);
        while !logs
            .lock()
            .unwrap()
            .iter()
            .any(|line| line.contains("authentication failed"))
        {
            assert!(
                Instant::now() < deadline,
                "missing optional authentication failure"
            );
            thread::sleep(Duration::from_millis(5));
        }
        assert_eq!(
            logs.lock()
                .unwrap()
                .iter()
                .filter(|line| line.contains("disabled"))
                .count(),
            1
        );
        assert!(retryable(&peer.socket.read(&mut [0; 1]).unwrap_err()));
        assert!(!stop.load(Ordering::Acquire));
        let stopping = Instant::now();
        stop.store(true, Ordering::Release);
        worker.join().unwrap().unwrap();
        assert!(stopping.elapsed() < Duration::from_millis(500));
    }
}
