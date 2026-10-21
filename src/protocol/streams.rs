use super::{
    clock::{Clock, difference, ntp},
    h264,
    pairing::video_cipher,
};
use crate::{
    native,
    runtime::{Media, Shared},
};
use aes::cipher::{BlockDecryptMut, KeyIvInit, StreamCipher, block_padding::NoPadding};
use anyhow::{Result, bail, ensure};
use std::{
    collections::BTreeMap,
    io::Read,
    net::{IpAddr, SocketAddr, TcpListener, TcpStream, UdpSocket},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

pub struct Streams {
    pub stop: Arc<AtomicBool>,
    pub clock: Arc<Mutex<Clock>>,
    pub video_port: Option<u16>,
    pub audio_ports: Option<(u16, u16)>,
    pub timing_port: u16,
    threads: Vec<JoinHandle<()>>,
    audio_stop: Option<Arc<AtomicBool>>,
    audio_thread: Option<JoinHandle<()>>,
    peer: IpAddr,
    local: IpAddr,
    key: [u8; 16],
    iv: [u8; 16],
    shared: Arc<Shared>,
    epoch: u64,
}
impl Streams {
    pub fn new(
        peer: IpAddr,
        local: IpAddr,
        timing_remote: u16,
        key: [u8; 16],
        iv: [u8; 16],
        shared: Arc<Shared>,
        epoch: u64,
    ) -> Result<Self> {
        ensure!(timing_remote > 0, "NTP timing port required");
        let sock = UdpSocket::bind(SocketAddr::new(local, 0))?;
        let timing_port = sock.local_addr()?.port();
        sock.connect(SocketAddr::new(peer, timing_remote))?;
        sock.set_read_timeout(Some(Duration::from_millis(100)))?;
        let mut s = Self {
            stop: Arc::new(AtomicBool::new(false)),
            clock: Arc::new(Mutex::new(Clock::default())),
            video_port: None,
            audio_ports: None,
            timing_port,
            threads: Vec::new(),
            audio_stop: None,
            audio_thread: None,
            peer,
            local,
            key,
            iv,
            shared,
            epoch,
        };
        s.spawn("timing", move |stop, clock, _, _| {
            timing_loop(sock, stop, clock)
        });
        Ok(s)
    }
    fn spawn<F>(&mut self, name: &'static str, job: F)
    where
        F: FnOnce(Arc<AtomicBool>, Arc<Mutex<Clock>>, Arc<Shared>, u64) -> Result<()>
            + Send
            + 'static,
    {
        let stop = self.stop.clone();
        let clock = self.clock.clone();
        let shared = self.shared.clone();
        let epoch = self.epoch;
        self.threads.push(thread::spawn(move || {
            if let Err(e) = job(stop.clone(), clock, shared.clone(), epoch) {
                shared.log(&format!("{name}: {e}"));
                stop.store(true, Ordering::Release);
            }
        }));
    }
    pub fn start_video(&mut self, id: u64) -> Result<u16> {
        ensure!(self.video_port.is_none(), "duplicate video stream");
        let listener = TcpListener::bind(SocketAddr::new(self.local, 0))?;
        listener.set_nonblocking(true)?;
        let port = listener.local_addr()?.port();
        let peer = self.peer;
        let key = self.key;
        self.spawn("video", move |stop, clock, shared, epoch| {
            video_loop(listener, peer, key, id, stop, clock, shared, epoch)
        });
        self.video_port = Some(port);
        Ok(port)
    }
    pub fn start_audio(&mut self, codec: u8, spf: u16, remote_control: u16) -> Result<(u16, u16)> {
        ensure!(self.audio_ports.is_none(), "duplicate audio stream");
        ensure!(
            (codec == 8 && spf == 480) || (codec == 4 && spf == 1024),
            "unsupported audio codec/frame size"
        );
        let data = UdpSocket::bind(SocketAddr::new(self.local, 0))?;
        let control = UdpSocket::bind(SocketAddr::new(self.local, 0))?;
        data.set_read_timeout(Some(Duration::from_millis(10)))?;
        control.set_nonblocking(true)?;
        let ports = (data.local_addr()?.port(), control.local_addr()?.port());
        let key = self.key;
        let iv = self.iv;
        let peer = self.peer;
        let local_stop = Arc::new(AtomicBool::new(false));
        self.audio_stop = Some(local_stop.clone());
        let stop = self.stop.clone();
        let clock = self.clock.clone();
        let shared = self.shared.clone();
        let epoch = self.epoch;
        self.audio_thread = Some(thread::spawn(move || {
            if let Err(e) = audio_loop(
                data,
                control,
                peer,
                remote_control,
                key,
                iv,
                codec,
                stop.clone(),
                local_stop,
                clock,
                shared.clone(),
                epoch,
            ) {
                shared.log(&format!("audio: {e}"));
                stop.store(true, Ordering::Release);
            }
        }));
        self.audio_ports = Some(ports);
        Ok(ports)
    }
    pub fn stop_audio(&mut self) {
        if let Some(stop) = self.audio_stop.take() {
            stop.store(true, Ordering::Release);
        }
        if let Some(thread) = self.audio_thread.take() {
            let _ = thread.join();
        }
        self.audio_ports = None;
        self.clock.lock().unwrap().clear_audio();
        self.shared.flush.fetch_add(1, Ordering::AcqRel);
    }
}
impl Drop for Streams {
    fn drop(&mut self) {
        use zeroize::Zeroize;
        self.stop.store(true, Ordering::Release);
        self.stop_audio();
        for t in self.threads.drain(..) {
            let _ = t.join();
        }
        self.key.zeroize();
        self.iv.zeroize();
    }
}
fn timed_out(e: &std::io::Error) -> bool {
    matches!(
        e.kind(),
        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
    )
}
fn timing_loop(sock: UdpSocket, stop: Arc<AtomicBool>, clock: Arc<Mutex<Clock>>) -> Result<()> {
    let mut last_send = native::now() - 5.0;
    let mut sent = 0;
    let mut t1 = 0;
    let mut sent_local = 0.0;
    let mut last_reply = Instant::now();
    while !stop.load(Ordering::Acquire) {
        let now = native::now();
        let interval = if sent < 8 { 0.25 } else { 2.0 };
        if now - last_send >= interval {
            let mut req = [0; 32];
            req[..4].copy_from_slice(&[0x80, 0xd2, 0, 7]);
            t1 = ntp(now);
            req[24..32].copy_from_slice(&t1.to_be_bytes());
            sock.send(&req)?;
            last_send = now;
            sent_local = now;
            sent += 1;
        }
        let mut reply = [0; 64];
        match sock.recv(&mut reply) {
            Ok(32) if reply[0] >> 6 == 2 && reply[1] & 0x7f == 0x53 => {
                let r1 = u64::from_be_bytes(reply[8..16].try_into()?);
                let r2 = u64::from_be_bytes(reply[16..24].try_into()?);
                let r3 = u64::from_be_bytes(reply[24..32].try_into()?);
                if r1 != t1 {
                    continue;
                }
                let t4 = native::now();
                let processing = difference(r3, r2);
                let rtt = t4 - sent_local - processing;
                if (0.0..0.5).contains(&rtt) && (0.0..0.2).contains(&processing) {
                    clock.lock().unwrap().observe(r3, t4 - rtt / 2.0, rtt)?;
                    last_reply = Instant::now();
                }
            }
            Ok(32) if reply[1] & 0x7f == 0x52 => {
                let mut r = [0; 32];
                r[..4].copy_from_slice(&[0x80, 0xd3, 0, 7]);
                r[8..16].copy_from_slice(&reply[24..32]);
                let time = ntp(native::now()).to_be_bytes();
                r[16..24].copy_from_slice(&time);
                r[24..32].copy_from_slice(&time);
                sock.send(&r)?;
            }
            Ok(_) => {}
            Err(e) if timed_out(&e) => {}
            Err(e) => return Err(e.into()),
        }
        ensure!(
            last_reply.elapsed() < Duration::from_secs(15),
            "sender timing lost"
        );
    }
    Ok(())
}
fn read_exact_stop(s: &mut TcpStream, out: &mut [u8], stop: &AtomicBool) -> Result<bool> {
    let mut n = 0;
    let start = Instant::now();
    while n < out.len() {
        if stop.load(Ordering::Acquire) {
            return Ok(false);
        }
        match s.read(&mut out[n..]) {
            Ok(0) => return Ok(false),
            Ok(c) => n += c,
            Err(e) if timed_out(&e) => {}
            Err(e) => return Err(e.into()),
        }
        ensure!(
            start.elapsed() < Duration::from_secs(10),
            "video record deadline"
        );
    }
    Ok(true)
}
#[allow(clippy::too_many_arguments)]
fn video_loop(
    listener: TcpListener,
    peer: IpAddr,
    key: [u8; 16],
    id: u64,
    stop: Arc<AtomicBool>,
    clock: Arc<Mutex<Clock>>,
    shared: Arc<Shared>,
    epoch: u64,
) -> Result<()> {
    let start = Instant::now();
    let mut s = loop {
        if stop.load(Ordering::Acquire) {
            return Ok(());
        }
        ensure!(
            start.elapsed() < Duration::from_secs(15),
            "video connect timeout"
        );
        match listener.accept() {
            Ok((s, a)) if a.ip() == peer => break s,
            Ok(_) => {}
            Err(e) if timed_out(&e) => thread::sleep(Duration::from_millis(20)),
            Err(e) => return Err(e.into()),
        }
    };
    super::framing::configure_accepted_stream(&s)?;
    let mut cipher = video_cipher(&key, id);
    let mut config = Vec::new();
    while !stop.load(Ordering::Acquire) {
        let mut header = [0; 128];
        if !read_exact_stop(&mut s, &mut header, &stop)? {
            break;
        }
        let len = u32::from_le_bytes(header[..4].try_into()?) as usize;
        let kind = u16::from_le_bytes(header[4..6].try_into()?) & 0xff;
        ensure!(len <= h264::MAX_FRAME, "mirror payload exceeds 2 MiB");
        let mut payload = vec![0; len];
        if !read_exact_stop(&mut s, &mut payload, &stop)? {
            break;
        }
        match kind {
            0 => {
                cipher.apply_keystream(&mut payload);
                h264::annex_b(&mut payload)?;
                if !config.is_empty() {
                    ensure!(
                        config.len() + payload.len() <= h264::MAX_FRAME,
                        "combined frame budget"
                    );
                    let mut joined = std::mem::take(&mut config);
                    joined.extend_from_slice(&payload);
                    payload = joined;
                }
                let raw = u64::from_le_bytes(header[8..16].try_into()?);
                let pts = clock
                    .lock()
                    .unwrap()
                    .video(raw)
                    .ok_or_else(|| anyhow::anyhow!("video before clock sync"))?;
                ensure!(
                    (pts - native::now()).abs() < 2.0,
                    "video timestamp outside clock window"
                );
                shared.send_video(Media::Video {
                    epoch,
                    data: payload,
                    pts,
                })?;
            }
            1 => {
                config = h264::avcc_config(&payload)?;
                shared.log("H.264 configuration accepted");
            }
            2 | 5 => {
                ensure!(len <= 65536, "mirror status budget");
            }
            _ => bail!("unsupported mirror packet type {kind}"),
        }
    }
    stop.store(true, Ordering::Release);
    Ok(())
}
#[derive(Debug)]
struct AudioPacket {
    seq: u16,
    rtp: u32,
    payload: Vec<u8>,
    arrival: Instant,
}
fn audio_packet(packet: &[u8]) -> Result<AudioPacket> {
    ensure!(
        packet.len() > 12 && packet.len() <= 8192 && packet[0] == 0x80,
        "RTP audio framing"
    );
    Ok(AudioPacket {
        seq: u16::from_be_bytes(packet[2..4].try_into()?),
        rtp: u32::from_be_bytes(packet[4..8].try_into()?),
        payload: packet[12..].to_vec(),
        arrival: Instant::now(),
    })
}
#[allow(clippy::too_many_arguments)]
fn audio_loop(
    data: UdpSocket,
    control: UdpSocket,
    peer: IpAddr,
    remote_control: u16,
    key: [u8; 16],
    iv: [u8; 16],
    codec: u8,
    stop: Arc<AtomicBool>,
    local_stop: Arc<AtomicBool>,
    clock: Arc<Mutex<Clock>>,
    shared: Arc<Shared>,
    epoch: u64,
) -> Result<()> {
    let mut worker = native::Worker::start()?;
    worker.configure(codec)?;
    shared.log(&format!(
        "audio codec={} sample_rate=44100 channels=2 AppContainer=yes",
        if codec == 8 { "AAC-ELD" } else { "AAC-LC" }
    ));
    let mut packets = BTreeMap::<u16, AudioPacket>::new();
    let mut expected = None;
    let mut pinned_port = None;
    let mut last = Instant::now();
    let mut errors = 0;
    let mut resend_at = Instant::now() - Duration::from_secs(1);
    let mut serial = shared.flush.load(Ordering::Acquire);
    while !stop.load(Ordering::Acquire) && !local_stop.load(Ordering::Acquire) {
        let next_serial = shared.flush.load(Ordering::Acquire);
        if next_serial != serial {
            packets.clear();
            expected = None;
            worker.configure(codec)?;
            serial = next_serial;
        }
        let mut packet = [0u8; 8193];
        match data.recv_from(&mut packet) {
            Ok((n, addr)) if addr.ip() == peer && pinned_port.is_none_or(|p| p == addr.port()) => {
                if let Ok(p) = audio_packet(&packet[..n]) {
                    pinned_port = Some(addr.port());
                    last = Instant::now();
                    if expected.is_none() {
                        expected = Some(p.seq);
                    }
                    let delta = p.seq.wrapping_sub(expected.unwrap()) as i16;
                    if (0..128).contains(&delta) {
                        packets.entry(p.seq).or_insert(p);
                    }
                }
            }
            Ok(_) => {}
            Err(e) if timed_out(&e) => {}
            Err(e) => return Err(e.into()),
        }
        // Bound work on the control socket even under a datagram flood.
        for _ in 0..16 {
            match control.recv_from(&mut packet) {
                Ok((n, addr))
                    if addr.ip() == peer
                        && (remote_control == 0 || addr.port() == remote_control) =>
                {
                    if n == 20 && packet[0] >> 6 == 2 && packet[1] & 0x7f == 0x54 {
                        let rtp = u32::from_be_bytes(packet[4..8].try_into()?);
                        let remote = u64::from_be_bytes(packet[8..16].try_into()?);
                        let mapped = clock.lock().unwrap().map(remote);
                        if let Some(t) = mapped
                            && (t - native::now()).abs() < 3.0
                        {
                            clock.lock().unwrap().sync_audio(rtp, remote);
                        }
                    } else if n > 16
                        && packet[1] & 0x7f == 0x56
                        && let Ok(p) = audio_packet(&packet[4..n])
                        && expected
                            .is_some_and(|e| (0..128).contains(&(p.seq.wrapping_sub(e) as i16)))
                    {
                        packets.entry(p.seq).or_insert(p);
                    }
                }
                Ok(_) => {}
                Err(e) if timed_out(&e) => break,
                Err(e) => return Err(e.into()),
            }
        }
        ensure!(packets.len() <= 128, "audio jitter budget");
        for _ in 0..32 {
            let Some(seq) = expected else { break };
            if let Some(p) = packets.get(&seq) {
                let pts = clock.lock().unwrap().audio(p.rtp);
                if pts.is_none() && p.arrival.elapsed() < Duration::from_millis(500) {
                    break;
                }
                let mut p = packets.remove(&seq).unwrap();
                expected = Some(seq.wrapping_add(1));
                let Some(pts) = pts else { continue };
                let encrypted = p.payload.len() / 16 * 16;
                cbc::Decryptor::<aes::Aes128>::new(&key.into(), &iv.into())
                    .decrypt_padded_mut::<NoPadding>(&mut p.payload[..encrypted])
                    .map_err(|_| anyhow::anyhow!("audio ciphertext length"))?;
                match worker.decode(&p.payload) {
                    Ok(pcm) => {
                        errors = 0;
                        if pts >= native::now() - 0.1 {
                            shared.send_audio(Media::Audio {
                                epoch,
                                serial,
                                pcm,
                                pts,
                            })?;
                        }
                    }
                    Err(_) => {
                        errors += 1;
                        ensure!(errors < 20, "repeated AAC decoding failures");
                    }
                }
            } else if !packets.is_empty() {
                let next = *packets.keys().min_by_key(|s| s.wrapping_sub(seq)).unwrap();
                let earliest = packets.values().map(|p| p.arrival).min().unwrap();
                if remote_control != 0 && resend_at.elapsed() > Duration::from_millis(15) {
                    let missing = next.wrapping_sub(seq).min(32);
                    let mut req = [0x80, 0xd5, 0, 1, 0, 0, 0, 0];
                    req[4..6].copy_from_slice(&seq.to_be_bytes());
                    req[6..8].copy_from_slice(&missing.to_be_bytes());
                    control.send_to(&req, SocketAddr::new(peer, remote_control))?;
                    resend_at = Instant::now();
                }
                if earliest.elapsed() > Duration::from_millis(40) {
                    expected = Some(next);
                } else {
                    break;
                }
            } else {
                break;
            }
        }
        // Silence/lock may legitimately stop audio datagrams. Control/timing owns session liveness.
        if last.elapsed() > Duration::from_secs(2) {
            packets.clear();
            expected = None;
        }
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rtp_lengths() {
        for n in 0..13 {
            assert!(audio_packet(&vec![0; n]).is_err());
        }
        let mut p = vec![0; 20];
        p[0] = 0x80;
        assert!(audio_packet(&p).is_ok());
        p[0] = 0x90;
        assert!(audio_packet(&p).is_err());
    }
}
