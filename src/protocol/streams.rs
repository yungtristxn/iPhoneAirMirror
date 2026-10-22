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
    collections::{BTreeMap, VecDeque},
    io::Read,
    net::{IpAddr, SocketAddr, TcpListener, TcpStream, UdpSocket},
    os::windows::io::AsRawSocket,
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
    pub event_port: u16,
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
            clock: Arc::new(Mutex::new(Clock::new(shared.playout_ms)?)),
            video_port: None,
            audio_ports: None,
            timing_port,
            event_port: 0,
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
        s.spawn("timing", move |stop, clock, shared, _| {
            timing_loop(sock, stop, clock, shared)
        });
        if cfg!(feature = "display-update-experiment") {
            let prepared = (|| -> Result<_> {
                let listener = TcpListener::bind(SocketAddr::new(local, 0))?;
                listener.set_nonblocking(true)?;
                let port = listener.local_addr()?.port();
                let updates = Arc::new(super::events::DisplayUpdates::new(
                    s.shared.video_profile.width,
                    s.shared.video_profile.height,
                )?);
                Ok((listener, port, updates))
            })();
            match prepared {
                Ok((listener, port, updates)) => {
                    // A stale session must still fail closed; optional socket
                    // allocation failure alone must not disable mirroring.
                    s.shared.register_display_updates(epoch, updates.clone())?;
                    s.event_port = port;
                    let base_display = super::display_description(&s.shared);
                    s.spawn("events", move |stop, _, shared, _| {
                        super::events::run(listener, peer, stop, updates, base_display, |message| {
                            shared.log(message)
                        })
                    });
                }
                Err(error) => s
                    .shared
                    .log(&format!("display updates unavailable at setup: {error}")),
            }
        }
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
                shared.failure(&format!("{name} stream stopped. Reconnect mirroring. {e}"));
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
                shared.failure(&format!("Audio stream stopped. Reconnect mirroring. {e}"));
                stop.store(true, Ordering::Release);
            }
        }));
        self.audio_ports = Some(ports);
        // Publish only once sockets and the transport thread exist, before the
        // caller acknowledges SETUP. Decoder initialization may still be pending.
        self.shared.audio_started(self.epoch)?;
        Ok(ports)
    }
    pub fn stop_audio(&mut self) -> Result<()> {
        if let Some(stop) = self.audio_stop.take() {
            stop.store(true, Ordering::Release);
        }
        if let Some(thread) = self.audio_thread.take() {
            let _ = thread.join();
        }
        self.audio_ports = None;
        self.clock.lock().unwrap().clear_audio();
        self.shared.audio_stopped(self.epoch)
    }
}
impl Drop for Streams {
    fn drop(&mut self) {
        use zeroize::Zeroize;
        self.stop.store(true, Ordering::Release);
        let _ = self.stop_audio();
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
#[derive(Default)]
struct TimingRequests(VecDeque<(u64, f64)>);
impl TimingRequests {
    fn sent(&mut self, stamp: u64, local: f64) {
        if self.0.len() == 8 {
            self.0.pop_front();
        }
        self.0.push_back((stamp, local));
    }
    fn reply(&mut self, stamp: u64) -> Option<f64> {
        let i = self.0.iter().position(|(s, _)| *s == stamp)?;
        self.0.remove(i).map(|(_, t)| t)
    }
}
fn timing_loop(
    sock: UdpSocket,
    stop: Arc<AtomicBool>,
    clock: Arc<Mutex<Clock>>,
    shared: Arc<Shared>,
) -> Result<()> {
    timing_loop_until(sock, stop, clock, Duration::from_secs(15), |rtt| {
        shared.log(&format!("clock synchronized rtt_ms={:.1}", rtt * 1000.0));
    })
}
fn timing_loop_until(
    sock: UdpSocket,
    stop: Arc<AtomicBool>,
    clock: Arc<Mutex<Clock>>,
    timeout: Duration,
    mut on_synchronized: impl FnMut(f64),
) -> Result<()> {
    let mut last_send = native::now() - 5.0;
    let mut sent = 0;
    let mut pending = TimingRequests::default();
    let mut synchronized = false;
    let mut last_reply = Instant::now();
    while !stop.load(Ordering::Acquire) {
        let now = native::now();
        let interval = if sent < 8 { 0.25 } else { 2.0 };
        if now - last_send >= interval {
            let mut req = [0; 32];
            req[..4].copy_from_slice(&[0x80, 0xd2, 0, 7]);
            let t1 = ntp(now);
            req[24..32].copy_from_slice(&t1.to_be_bytes());
            sock.send(&req)?;
            last_send = now;
            pending.sent(t1, now);
            sent += 1;
        }
        let mut reply = [0; 64];
        match sock.recv(&mut reply) {
            Ok(32) if reply[0] >> 6 == 2 && reply[1] & 0x7f == 0x53 => {
                let r1 = u64::from_be_bytes(reply[8..16].try_into()?);
                let r2 = u64::from_be_bytes(reply[16..24].try_into()?);
                let r3 = u64::from_be_bytes(reply[24..32].try_into()?);
                // Unmatched/replayed replies must still reach the liveness check.
                if let Some(sent_local) = pending.reply(r1) {
                    let t4 = native::now();
                    let processing = difference(r3, r2);
                    let rtt = t4 - sent_local - processing;
                    if (0.0..0.5).contains(&rtt)
                        && (0.0..0.2).contains(&processing)
                        && clock
                            .lock()
                            .unwrap()
                            .observe(r3, t4 - rtt / 2.0, rtt)
                            .is_ok()
                    {
                        last_reply = Instant::now();
                        if !synchronized {
                            on_synchronized(rtt);
                            synchronized = true;
                        }
                    }
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
        ensure!(last_reply.elapsed() < timeout, "sender timing lost");
    }
    Ok(())
}
fn read_exact_stop(
    s: &mut TcpStream,
    out: &mut [u8],
    stop: &AtomicBool,
    started: &mut Option<Instant>,
    limit: Duration,
) -> Result<bool> {
    let mut n = 0;
    while n < out.len() {
        if stop.load(Ordering::Acquire) {
            return Ok(false);
        }
        match s.read(&mut out[n..]) {
            Ok(0) if n == 0 && started.is_none() => return Ok(false),
            Ok(0) => bail!("truncated video record"),
            Ok(c) => {
                started.get_or_insert_with(Instant::now);
                n += c;
            }
            Err(e) if timed_out(&e) => {}
            Err(e) => return Err(e.into()),
        }
        ensure!(
            started.is_none_or(|start| start.elapsed() < limit),
            "video record deadline"
        );
    }
    Ok(true)
}
fn wait_video_clock(
    clock: &Mutex<Clock>,
    raw: u64,
    stop: &AtomicBool,
    limit: Duration,
) -> Result<Option<f64>> {
    let start = Instant::now();
    loop {
        if stop.load(Ordering::Acquire) {
            return Ok(None);
        }
        if let Some(pts) = clock.lock().unwrap().video(raw) {
            return Ok(Some(pts));
        }
        ensure!(start.elapsed() < limit, "initial sender clock timeout");
        thread::sleep(Duration::from_millis(5));
    }
}
fn configuration_header_size(header: &[u8], offset: usize) -> Option<(u32, u32)> {
    let bytes = header.get(offset..offset.checked_add(8)?)?;
    let width = f32::from_le_bytes(bytes[..4].try_into().ok()?);
    let height = f32::from_le_bytes(bytes[4..].try_into().ok()?);
    if [width, height]
        .iter()
        .all(|value| value.is_finite() && (48.0..=3840.0).contains(value) && value.fract() == 0.0)
    {
        Some((width as u32, height as u32))
    } else {
        None
    }
}
fn size_label(size: Option<(u32, u32)>) -> String {
    size.map(|(width, height)| format!("{width}x{height}"))
        .unwrap_or_else(|| "unknown".into())
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
            Err(e) if timed_out(&e) => {
                native::wait_socket_readable(listener.as_raw_socket() as usize, 100)?;
            }
            Err(e) => return Err(e.into()),
        }
    };
    super::framing::configure_accepted_stream(&s)?;
    let connected_at = native::now();
    shared.log(&format!(
        "video TCP connected accept_wait_ms={:.1}",
        start.elapsed().as_secs_f64() * 1000.0
    ));
    let mut cipher = video_cipher(&key, id);
    let mut config = Vec::new();
    let mut codec = None;
    let mut dimensions = (0, 0);
    let mut hevc_sequence = None;
    let mut rotation_cw_degrees = 0;
    let mut visibility_serial = 0;
    let mut frame_options = None;
    let mut first_frame = true;
    let mut first_config = true;
    while !stop.load(Ordering::Acquire) {
        let mut header = [0; 128];
        let mut record_started = None;
        if !read_exact_stop(
            &mut s,
            &mut header,
            &stop,
            &mut record_started,
            Duration::from_secs(10),
        )? {
            break;
        }
        let len = u32::from_le_bytes(header[..4].try_into()?) as usize;
        let kind = u16::from_le_bytes(header[4..6].try_into()?) & 0xff;
        let frame_budget = if codec == Some(265) {
            super::hevc::MAX_FRAME
        } else {
            h264::MAX_FRAME
        };
        let budget = match kind {
            0 => frame_budget,
            1 | 2 | 5 => 65536,
            _ => bail!("unsupported mirror packet type {kind}"),
        };
        ensure!(len <= budget, "mirror payload budget");
        let mut payload = vec![0; len];
        if !read_exact_stop(
            &mut s,
            &mut payload,
            &stop,
            &mut record_started,
            Duration::from_secs(10),
        )? {
            break;
        }
        let payload_received_at = native::now();
        match kind {
            0 => {
                cipher.apply_keystream(&mut payload);
                let active_codec =
                    codec.ok_or_else(|| anyhow::anyhow!("video before codec configuration"))?;
                if super::ROTATABLE_DISPLAY {
                    let options = (
                        u16::from_le_bytes(header[4..6].try_into()?) & !0x1000,
                        u16::from_le_bytes(header[6..8].try_into()?),
                    );
                    if frame_options != Some(options) {
                        shared.log(&format!(
                            "video frame flags=0x{:04x} options=0x{:04x} (diagnostic only)",
                            options.0, options.1
                        ));
                        frame_options = Some(options);
                    }
                }
                if active_codec == 265 {
                    if let Some(info) = super::hevc::annex_b(&mut payload)? {
                        if hevc_sequence != Some(info) {
                            log_hevc_sequence(&shared, info);
                        }
                        dimensions = (info.width, info.height);
                        hevc_sequence = Some(info);
                    }
                } else {
                    h264::annex_b(&mut payload)?;
                }
                if !config.is_empty() {
                    ensure!(
                        config.len() + payload.len() <= frame_budget,
                        "combined frame budget"
                    );
                    let mut joined = std::mem::take(&mut config);
                    joined.extend_from_slice(&payload);
                    payload = joined;
                }
                let ready_at = native::now();
                let raw = u64::from_le_bytes(header[8..16].try_into()?);
                let Some(pts) = wait_video_clock(&clock, raw, &stop, Duration::from_millis(1500))?
                else {
                    break;
                };
                ensure!(
                    (pts - native::now()).abs() < 2.0,
                    "video timestamp outside clock window"
                );
                if first_frame {
                    shared.log(&format!(
                        "first video payload receive_since_tcp_ms={:.1} validate_ms={:.1} clock_wait_ms={:.1}",
                        (payload_received_at - connected_at) * 1000.0,
                        (ready_at - payload_received_at) * 1000.0,
                        (native::now() - ready_at) * 1000.0
                    ));
                    first_frame = false;
                }
                shared.send_video(Media::Video {
                    epoch,
                    codec: active_codec,
                    width: dimensions.0,
                    height: dimensions.1,
                    data: payload,
                    pts,
                    payload_received_at,
                    ready_at,
                    rotation_cw_degrees,
                    visibility_serial,
                })?;
            }
            1 => {
                let suspended = super::orientation::configuration_suspended(&header)?;
                if first_config {
                    shared.log(&format!(
                        "first video configuration receive_since_tcp_ms={:.1}",
                        (payload_received_at - connected_at) * 1000.0
                    ));
                    first_config = false;
                }
                let next_rotation =
                    super::orientation::configuration_rotation(&header, super::ROTATABLE_DISPLAY)?;
                let next = if payload.get(4..8) == Some(b"hvc1") {
                    265
                } else {
                    264
                };
                ensure!(
                    codec.is_none_or(|old| old == next),
                    "codec changed within a session"
                );
                if next == 265 {
                    ensure!(shared.video_profile.hevc, "HEVC not advertised");
                    let parsed = super::hevc::config(&payload)?;
                    dimensions = (parsed.width, parsed.height);
                    log_hevc_sequence(&shared, parsed.sequence);
                    hevc_sequence = Some(parsed.sequence);
                    config = parsed.annex_b;
                } else {
                    config = h264::avcc_config(&payload)?;
                }
                codec = Some(next);
                rotation_cw_degrees = next_rotation;
                // Priority state is independent of the bounded compressed-video
                // queue. It reaches presentation even while that queue is full.
                if let Some(suspended) = suspended {
                    let Some(serial) = shared.publish_screen_state(epoch, suspended)? else {
                        break; // Shutdown or replaced owner: quiet stream cancellation.
                    };
                    visibility_serial = serial;
                }
                shared.log(&format!("H.{next} configuration accepted"));
                shared.log(&format!(
                    "AirPlay display transform={} clockwise_degrees={rotation_cw_degrees}",
                    header[5]
                ));
                // These observed header fields are diagnostics only. Codec SPS
                // dimensions remain authoritative for video; none of these fields
                // establish the phone's physical orientation.
                shared.log(&format!(
                    "video configuration header source={} display={} type_flags=0x{:04x} options=0x{:04x} auxiliary={} transform_words={} (diagnostic only)",
                    size_label(configuration_header_size(&header, 40)),
                    size_label(configuration_header_size(&header, 56)),
                    u16::from_le_bytes(header[4..6].try_into()?),
                    u16::from_le_bytes(header[6..8].try_into()?),
                    [48usize,52].iter().map(|offset| {
                        let value=f32::from_le_bytes(header[*offset..*offset+4].try_into().unwrap());
                        if value.is_finite() && value.abs()<=16384.0 {value.to_string()} else {"invalid".into()}
                    }).collect::<Vec<_>>().join(","),
                    [24usize,28,32,36].iter().map(|offset| {
                        let word=u32::from_le_bytes(header[*offset..*offset+4].try_into().unwrap());
                        let value=f32::from_bits(word);
                        if word<=360 {format!("{offset}:small_uint={word}")}
                        else if value.is_finite() && value.abs()<=360.0 {format!("{offset}:float={value}")}
                        else {format!("{offset}:unrecognized")}
                    }).collect::<Vec<_>>().join(","),
                ));
                shared.send_video(Media::VideoConfig {
                    epoch,
                    codec: next,
                    width: dimensions.0,
                    height: dimensions.1,
                })?;
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
fn log_hevc_sequence(shared: &Shared, info: super::hevc::SequenceInfo) {
    // Scalar codec metadata only; never store parameter-set bytes or phone media.
    shared.log(&format!(
        "HEVC SPS coded={}x{} display={}x{} level_idc={} dpb_pictures={} max_num_reorder_pics={}",
        info.coded_width,
        info.coded_height,
        info.width,
        info.height,
        info.level_idc,
        info.max_dec_pic_buffering,
        info.max_num_reorder_pics
    ));
}

#[derive(Debug)]
struct AudioPacket {
    rtp: u32,
    payload: Vec<u8>,
    arrival: Instant,
}
#[derive(Clone, Copy, Debug)]
struct RtpPacket<'a> {
    seq: u16,
    rtp: u32,
    payload: &'a [u8],
}
fn audio_packet(packet: &[u8]) -> Result<RtpPacket<'_>> {
    ensure!(
        packet.len() > 12 && packet.len() <= 8192 && packet[0] == 0x80,
        "RTP audio framing"
    );
    Ok(RtpPacket {
        seq: u16::from_be_bytes(packet[2..4].try_into()?),
        rtp: u32::from_be_bytes(packet[4..8].try_into()?),
        payload: &packet[12..],
    })
}
#[derive(Default)]
struct AudioQueue {
    packets: BTreeMap<u16, AudioPacket>,
    expected: Option<u16>,
    restart: Option<u16>,
    fence: Option<super::AudioCutoff>,
}
#[derive(Debug, PartialEq, Eq)]
enum AudioAdmission {
    Ignored,
    Queued,
    Resynchronized,
}
impl AudioQueue {
    fn clear(&mut self) {
        self.packets.clear();
        self.expected = self.fence.map(|cutoff| cutoff.sequence);
        self.restart = None;
    }
    fn flush(&mut self, cutoff: Option<super::AudioCutoff>) {
        self.fence = cutoff;
        self.clear();
    }
    fn consumed(&mut self, sequence: u16, timestamp: u32, samples_per_frame: u32) {
        self.expected = Some(sequence.wrapping_add(1));
        // Move the fence as playback advances, so a long session remains valid
        // across both half-range and complete RTP counter wraparounds.
        if self.fence.is_some() {
            self.fence = Some(super::AudioCutoff {
                sequence: sequence.wrapping_add(1),
                timestamp: timestamp.wrapping_add(samples_per_frame),
            });
        }
    }
    fn insert(&mut self, packet: RtpPacket<'_>, retransmit: bool) -> AudioAdmission {
        if self.fence.is_some_and(|cutoff| {
            (packet.seq.wrapping_sub(cutoff.sequence) as i16) < 0
                || (packet.rtp.wrapping_sub(cutoff.timestamp) as i32) < 0
        }) {
            return AudioAdmission::Ignored;
        }
        if retransmit && self.expected.is_none() {
            return AudioAdmission::Ignored;
        }
        let expected = *self.expected.get_or_insert(packet.seq);
        let delta = packet.seq.wrapping_sub(expected) as i16;
        let mut admission = AudioAdmission::Queued;
        if !(0..128).contains(&delta) {
            // A gap larger than the queue must not leave audio permanently stuck.
            // Two consecutive forward packets confirm progress; a lone outlier,
            // old packet or retransmission cannot reset the live sequence.
            if retransmit || delta < 128 {
                return AudioAdmission::Ignored;
            }
            let confirmed = self.restart == Some(packet.seq);
            self.restart = Some(packet.seq.wrapping_add(1));
            if !confirmed {
                return AudioAdmission::Ignored;
            }
            self.packets.clear();
            self.expected = Some(packet.seq);
            admission = AudioAdmission::Resynchronized;
        }
        self.restart = None;
        if let std::collections::btree_map::Entry::Vacant(entry) = self.packets.entry(packet.seq) {
            entry.insert(AudioPacket {
                rtp: packet.rtp,
                payload: packet.payload.to_vec(),
                arrival: Instant::now(),
            });
            admission
        } else {
            AudioAdmission::Ignored
        }
    }
}
fn advance_audio_rtp(next: &mut Option<u32>, rtp: u32, samples: u32) -> Result<()> {
    ensure!(
        next.is_none_or(|expected| (rtp.wrapping_sub(expected) as i32) >= 0),
        "audio RTP rewind requires FLUSH or reconnect"
    );
    *next = Some(rtp.wrapping_add(samples));
    Ok(())
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
    let mut audio_timing = crate::audio_timing::AudioTiming::new(worker.generation())?;
    // Transport ordering survives decoder errors/repriming. Only an explicit
    // FLUSH/new transport may rewind into audio already submitted to playback.
    let mut next_audio_rtp = None;
    shared.log(&format!(
        "audio codec={} backend={} sample_rate=44100 channels=2 AppContainer=yes",
        if codec == 8 { "AAC-ELD" } else { "AAC-LC" },
        native::aac_backend()
    ));
    let initial_flush = shared.flush_snapshot();
    let mut queue = AudioQueue::default();
    queue.flush(initial_flush.cutoff);
    let mut anchor_after = initial_flush.anchor_after;
    let mut pinned_port = None;
    let mut last = Instant::now();
    let mut errors = 0;
    let mut resend_at = Instant::now() - Duration::from_secs(1);
    let mut serial = initial_flush.serial;
    let mut packet = [0u8; 8193];
    while !stop.load(Ordering::Acquire) && !local_stop.load(Ordering::Acquire) {
        let next_serial = shared.flush.load(Ordering::Acquire);
        if next_serial != serial {
            let flush = shared.flush_snapshot();
            queue.flush(flush.cutoff);
            clock.lock().unwrap().clear_audio();
            anchor_after = flush.anchor_after;
            worker.configure(codec)?;
            audio_timing = crate::audio_timing::AudioTiming::new(worker.generation())?;
            next_audio_rtp = None;
            serial = flush.serial;
        }
        match data.recv_from(&mut packet) {
            Ok((n, addr)) if addr.ip() == peer && pinned_port.is_none_or(|p| p == addr.port()) => {
                if let Ok(p) = audio_packet(&packet[..n]) {
                    pinned_port = Some(addr.port());
                    let admitted = queue.insert(p, false);
                    if admitted != AudioAdmission::Ignored {
                        last = Instant::now();
                    }
                    if admitted == AudioAdmission::Resynchronized {
                        worker.configure(codec)?;
                        audio_timing = crate::audio_timing::AudioTiming::new(worker.generation())?;
                        errors = 0;
                        shared.log("audio sequence resynchronized after packet loss");
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
                        let mut clock = clock.lock().unwrap();
                        let mapped = clock.map(remote);
                        if let Some(t) = mapped
                            && (t - native::now()).abs() < 3.0
                            && anchor_after.is_none_or(|boundary| t >= boundary)
                            && clock.sync_audio(rtp, remote)
                        {
                            anchor_after = None;
                        }
                    } else if n > 16
                        && packet[1] & 0x7f == 0x56
                        && let Ok(p) = audio_packet(&packet[4..n])
                    {
                        queue.insert(p, true);
                    }
                }
                Ok(_) => {}
                Err(e) if timed_out(&e) => break,
                Err(e) => return Err(e.into()),
            }
        }
        ensure!(queue.packets.len() <= 128, "audio jitter budget");
        for _ in 0..32 {
            let Some(seq) = queue.expected else { break };
            if let Some(p) = queue.packets.get(&seq) {
                let pts = clock.lock().unwrap().audio(p.rtp);
                if pts.is_none() && p.arrival.elapsed() < Duration::from_millis(500) {
                    break;
                }
                let mut p = queue.packets.remove(&seq).unwrap();
                advance_audio_rtp(
                    &mut next_audio_rtp,
                    p.rtp,
                    if codec == 8 { 480 } else { 1024 },
                )?;
                queue.consumed(seq, p.rtp, if codec == 8 { 480 } else { 1024 });
                if pts.is_none() {
                    continue;
                }
                // A missing packet or sender timestamp discontinuity invalidates
                // overlap/concealment history. Reprime before decoding the next
                // packet, including a short loss skipped by the resend deadline.
                if audio_timing.needs_reset(p.rtp) {
                    worker.configure(codec)?;
                    audio_timing = crate::audio_timing::AudioTiming::new(worker.generation())?;
                }
                let encrypted = p.payload.len() / 16 * 16;
                cbc::Decryptor::<aes::Aes128>::new(&key.into(), &iv.into())
                    .decrypt_padded_mut::<NoPadding>(&mut p.payload[..encrypted])
                    .map_err(|_| anyhow::anyhow!("audio ciphertext length"))?;
                match worker.decode_block(&p.payload) {
                    Ok(mut block) => {
                        errors = 0;
                        let mapped = audio_timing.map_block(
                            block.generation,
                            p.rtp,
                            block.pcm.len() / 2,
                            block.additional_delay_samples,
                        )?;
                        if mapped.output_frames == 0 {
                            continue;
                        }
                        // Remove only decoder-added priming before this generation.
                        // Encoder/transform priming is sender-owned; never infer it
                        // from silence or add a backend-specific playout buffer.
                        block.pcm.drain(..mapped.skip_frames * 2);
                        let pts = clock.lock().unwrap().audio(mapped.output_rtp);
                        if let Some(pts) = pts
                            && pts >= native::now() - 0.1
                        {
                            shared.send_audio(
                                Media::Audio {
                                    epoch,
                                    serial,
                                    pcm: block.pcm,
                                    pts,
                                },
                                || {
                                    stop.load(Ordering::Acquire)
                                        || local_stop.load(Ordering::Acquire)
                                },
                            )?;
                        }
                    }
                    Err(_) => {
                        errors += 1;
                        ensure!(errors < 20, "repeated AAC decoding failures");
                        worker.configure(codec)?;
                        audio_timing = crate::audio_timing::AudioTiming::new(worker.generation())?;
                    }
                }
            } else if !queue.packets.is_empty() {
                let next = *queue
                    .packets
                    .keys()
                    .min_by_key(|s| s.wrapping_sub(seq))
                    .unwrap();
                let earliest = queue.packets.values().map(|p| p.arrival).min().unwrap();
                if remote_control != 0 && resend_at.elapsed() > Duration::from_millis(15) {
                    let missing = next.wrapping_sub(seq).min(32);
                    let mut req = [0x80, 0xd5, 0, 1, 0, 0, 0, 0];
                    req[4..6].copy_from_slice(&seq.to_be_bytes());
                    req[6..8].copy_from_slice(&missing.to_be_bytes());
                    control.send_to(&req, SocketAddr::new(peer, remote_control))?;
                    resend_at = Instant::now();
                }
                if earliest.elapsed() > Duration::from_millis(40) {
                    queue.expected = Some(next);
                } else {
                    break;
                }
            } else {
                break;
            }
        }
        // Silence/lock may legitimately stop audio datagrams. Control/timing owns session liveness.
        if last.elapsed() > Duration::from_secs(2) {
            queue.clear();
        }
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    #[test]
    fn audio_transport_rewind_fence_survives_decoder_reprime() {
        let mut next = None;
        advance_audio_rtp(&mut next, u32::MAX - 479, 480).unwrap();
        advance_audio_rtp(&mut next, 0, 480).unwrap();
        advance_audio_rtp(&mut next, 960, 480).unwrap(); // Forward packet loss.
        // A fresh decoder alone must not permit old queued audio to overlap.
        let _fresh_decoder_timing = crate::audio_timing::AudioTiming::new(2).unwrap();
        assert!(advance_audio_rtp(&mut next, 960, 480).is_err());
        assert!(advance_audio_rtp(&mut next, 0, 480).is_err());
        assert_eq!(next, Some(1440));
        next = None; // Explicit FLUSH clears the transport fence too.
        advance_audio_rtp(&mut next, 0, 480).unwrap();
    }
    fn tcp_pair() -> (TcpStream, TcpStream) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let client = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (server, _) = listener.accept().unwrap();
        server
            .set_read_timeout(Some(Duration::from_millis(10)))
            .unwrap();
        (server, client)
    }
    #[test]
    fn idle_video_is_not_a_partial_record_timeout() {
        let (mut server, mut client) = tcp_pair();
        let stop = AtomicBool::new(false);
        let writer = thread::spawn(move || {
            thread::sleep(Duration::from_millis(80));
            client.write_all(&[1, 2, 3, 4]).unwrap();
        });
        let mut bytes = [0; 4];
        let mut started = None;
        assert!(
            read_exact_stop(
                &mut server,
                &mut bytes,
                &stop,
                &mut started,
                Duration::from_millis(30)
            )
            .unwrap()
        );
        assert_eq!(bytes, [1, 2, 3, 4]);
        writer.join().unwrap();
    }
    #[test]
    fn partial_video_deadline_and_eof_remain_strict() {
        let (mut server, mut client) = tcp_pair();
        client.write_all(&[1]).unwrap();
        let mut started = None;
        let stop = AtomicBool::new(false);
        assert!(
            read_exact_stop(
                &mut server,
                &mut [0; 4],
                &stop,
                &mut started,
                Duration::from_millis(30)
            )
            .is_err()
        );
        drop(client);
        assert!(
            read_exact_stop(
                &mut server,
                &mut [0; 4],
                &stop,
                &mut started,
                Duration::from_millis(30)
            )
            .is_err()
        );
        let (mut server, client) = tcp_pair();
        drop(client);
        let mut started = None;
        assert!(
            !read_exact_stop(
                &mut server,
                &mut [0; 4],
                &stop,
                &mut started,
                Duration::from_millis(30)
            )
            .unwrap()
        );
    }
    #[test]
    fn payload_uses_the_headers_existing_deadline() {
        let (mut server, mut client) = tcp_pair();
        let stop = AtomicBool::new(false);
        let mut started = None;
        client.write_all(&[1, 2]).unwrap();
        assert!(
            read_exact_stop(
                &mut server,
                &mut [0; 2],
                &stop,
                &mut started,
                Duration::from_millis(20)
            )
            .unwrap()
        );
        thread::sleep(Duration::from_millis(30));
        client.write_all(&[3, 4]).unwrap();
        assert!(
            read_exact_stop(
                &mut server,
                &mut [0; 2],
                &stop,
                &mut started,
                Duration::from_millis(20)
            )
            .is_err()
        );
    }
    #[test]
    fn timing_replies_can_be_reordered_but_not_replayed() {
        let mut pending = TimingRequests::default();
        pending.sent(1, 1.0);
        pending.sent(2, 2.0);
        assert_eq!(pending.reply(1), Some(1.0));
        assert_eq!(pending.reply(1), None);
        assert_eq!(pending.reply(2), Some(2.0));
        for i in 0..10 {
            pending.sent(i, i as f64);
        }
        assert_eq!(pending.0.len(), 8);
        assert_eq!(pending.reply(0), None);
        assert_eq!(pending.reply(2), Some(2.0));
    }
    #[test]
    fn unmatched_timing_traffic_does_not_extend_liveness() {
        let receiver = UdpSocket::bind("127.0.0.1:0").unwrap();
        let sender = UdpSocket::bind("127.0.0.1:0").unwrap();
        receiver.connect(sender.local_addr().unwrap()).unwrap();
        sender.connect(receiver.local_addr().unwrap()).unwrap();
        receiver
            .set_read_timeout(Some(Duration::from_millis(10)))
            .unwrap();
        let flood_running = Arc::new(AtomicBool::new(true));
        let running = flood_running.clone();
        let flood = thread::spawn(move || {
            let mut reply = [0; 32];
            reply[..2].copy_from_slice(&[0x80, 0xd3]);
            let start = Instant::now();
            while running.load(Ordering::Acquire) && start.elapsed() < Duration::from_secs(2) {
                let _ = sender.send(&reply);
                thread::sleep(Duration::from_millis(2));
            }
        });
        let started = Instant::now();
        let result = timing_loop_until(
            receiver,
            Arc::new(AtomicBool::new(false)),
            Arc::new(Mutex::new(Clock::default())),
            Duration::from_millis(60),
            |_| panic!("unsolicited timing reply synchronized the clock"),
        );
        flood_running.store(false, Ordering::Release);
        flood.join().unwrap();
        assert!(
            result
                .unwrap_err()
                .to_string()
                .contains("sender timing lost")
        );
        assert!(started.elapsed() < Duration::from_secs(1));
    }
    fn rtp(seq: u16) -> RtpPacket<'static> {
        RtpPacket {
            seq,
            rtp: u32::from(seq) * 480,
            payload: &[1, 2, 3],
        }
    }
    #[test]
    fn audio_recovers_after_loss_larger_than_jitter_window() {
        let mut queue = AudioQueue::default();
        assert_eq!(queue.insert(rtp(1000), false), AudioAdmission::Queued);
        queue.packets.remove(&1000);
        queue.expected = Some(1001);
        assert_eq!(queue.insert(rtp(1200), false), AudioAdmission::Ignored);
        assert_eq!(queue.expected, Some(1001));
        assert_eq!(
            queue.insert(rtp(1201), false),
            AudioAdmission::Resynchronized
        );
        assert_eq!(queue.expected, Some(1201));
        assert_eq!(queue.packets.keys().copied().collect::<Vec<_>>(), [1201]);
        assert_eq!(queue.insert(rtp(1202), false), AudioAdmission::Queued);
    }
    #[test]
    fn flush_fence_rejects_old_future_audio_and_retransmits_before_decode() {
        let mut queue = AudioQueue::default();
        let old = RtpPacket {
            seq: 100,
            rtp: 48000,
            payload: &[1, 2],
        };
        assert_eq!(queue.insert(old, false), AudioAdmission::Queued);
        // A still-future presentation timestamp cannot rescue pre-FLUSH wire data.
        queue.flush(Some(super::super::AudioCutoff {
            sequence: 101,
            timestamp: 48480,
        }));
        assert!(queue.packets.is_empty());
        let mut decoded = 0;
        for retransmit in [false, true] {
            if queue.insert(old, retransmit) != AudioAdmission::Ignored {
                decoded += 1;
            }
        }
        assert_eq!(decoded, 0);
        assert_eq!(
            queue.insert(
                RtpPacket {
                    seq: 101,
                    rtp: 48000,
                    ..old
                },
                false
            ),
            AudioAdmission::Ignored
        );
        assert_eq!(
            queue.insert(
                RtpPacket {
                    seq: 100,
                    rtp: 48480,
                    ..old
                },
                true
            ),
            AudioAdmission::Ignored
        );
        let next = RtpPacket {
            seq: 101,
            rtp: 48480,
            ..old
        };
        assert_eq!(queue.insert(next, false), AudioAdmission::Queued);
        decoded += 1;
        assert_eq!(decoded, 1);
        queue.packets.remove(&101);
        queue.consumed(101, 48480, 480);
        assert_eq!(queue.insert(next, true), AudioAdmission::Ignored);
        queue.clear(); // Idle reset must retain the moving transport fence.
        assert_eq!(queue.insert(old, false), AudioAdmission::Ignored);
    }
    #[test]
    fn flush_fence_tracks_sequence_and_timestamp_wrap_for_long_sessions() {
        let mut queue = AudioQueue::default();
        let mut sequence = u16::MAX - 1;
        let mut timestamp = u32::MAX - 959;
        queue.flush(Some(super::super::AudioCutoff {
            sequence,
            timestamp,
        }));
        assert_eq!(
            queue.insert(
                RtpPacket {
                    seq: sequence.wrapping_sub(1),
                    rtp: timestamp.wrapping_sub(480),
                    payload: &[1]
                },
                false
            ),
            AudioAdmission::Ignored
        );
        for _ in 0..65540 {
            let packet = RtpPacket {
                seq: sequence,
                rtp: timestamp,
                payload: &[1],
            };
            assert_eq!(queue.insert(packet, false), AudioAdmission::Queued);
            queue.packets.remove(&sequence);
            queue.consumed(sequence, timestamp, 480);
            assert_eq!(queue.insert(packet, true), AudioAdmission::Ignored);
            sequence = sequence.wrapping_add(1);
            timestamp = timestamp.wrapping_add(480);
        }
        assert!(queue.packets.is_empty());
    }
    #[test]
    fn audio_reordering_duplicates_wrap_and_outliers_stay_bounded() {
        let mut queue = AudioQueue::default();
        assert_eq!(queue.insert(rtp(65534), false), AudioAdmission::Queued);
        assert_eq!(queue.insert(rtp(0), false), AudioAdmission::Queued);
        assert_eq!(queue.insert(rtp(65535), true), AudioAdmission::Queued);
        assert_eq!(queue.insert(rtp(65535), false), AudioAdmission::Ignored);
        assert_eq!(queue.insert(rtp(65533), false), AudioAdmission::Ignored);
        assert_eq!(queue.insert(rtp(200), false), AudioAdmission::Ignored);
        assert_eq!(queue.insert(rtp(201), true), AudioAdmission::Ignored);
        assert_eq!(queue.expected, Some(65534));
        assert_eq!(queue.insert(rtp(1), false), AudioAdmission::Queued);
        assert_eq!(queue.insert(rtp(201), false), AudioAdmission::Ignored);
        for seq in 2..126 {
            assert_eq!(queue.insert(rtp(seq), false), AudioAdmission::Queued);
        }
        assert_eq!(queue.packets.len(), 128);
        queue.clear();
        assert!(queue.packets.is_empty());
        assert_eq!(queue.insert(rtp(100), true), AudioAdmission::Ignored);
        assert_eq!(queue.insert(rtp(100), false), AudioAdmission::Queued);
    }
    #[test]
    fn first_picture_waits_for_delayed_clock_and_is_cancellable() {
        let clock = Arc::new(Mutex::new(Clock::default()));
        let ready = clock.clone();
        let raw = ntp(1000.0);
        let setter = thread::spawn(move || {
            thread::sleep(Duration::from_millis(30));
            ready
                .lock()
                .unwrap()
                .observe(
                    raw.wrapping_add(2_208_988_800u64 << 32),
                    native::now(),
                    0.002,
                )
                .unwrap();
        });
        let stop = AtomicBool::new(false);
        assert!(
            wait_video_clock(&clock, raw, &stop, Duration::from_millis(500))
                .unwrap()
                .is_some()
        );
        setter.join().unwrap();
        let empty = Mutex::new(Clock::default());
        assert!(wait_video_clock(&empty, raw, &stop, Duration::from_millis(20)).is_err());
        stop.store(true, Ordering::Release);
        assert!(
            wait_video_clock(&empty, raw, &stop, Duration::from_secs(1))
                .unwrap()
                .is_none()
        );
    }
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
    #[test]
    fn configuration_header_sizes_are_bounded_optional_diagnostics() {
        let mut header = [0; 128];
        header[40..44].copy_from_slice(&994f32.to_le_bytes());
        header[44..48].copy_from_slice(&2160f32.to_le_bytes());
        header[56..60].copy_from_slice(&2160f32.to_le_bytes());
        header[60..64].copy_from_slice(&994f32.to_le_bytes());
        assert_eq!(configuration_header_size(&header, 40), Some((994, 2160)));
        assert_eq!(configuration_header_size(&header, 56), Some((2160, 994)));
        for invalid in [f32::NAN, f32::INFINITY, -1.0, 0.0, 47.0, 994.5, 3841.0] {
            header[40..44].copy_from_slice(&invalid.to_le_bytes());
            assert_eq!(configuration_header_size(&header, 40), None);
            assert_eq!(configuration_header_size(&header, 56), Some((2160, 994)));
        }
        assert_eq!(configuration_header_size(&header[..47], 40), None);
        assert_eq!(configuration_header_size(&header, usize::MAX), None);
        assert_eq!(size_label(None), "unknown");
        for valid in [48f32, 3840f32] {
            header[40..44].copy_from_slice(&valid.to_le_bytes());
            assert_eq!(
                configuration_header_size(&header, 40),
                Some((valid as u32, 2160))
            );
        }
    }
}
