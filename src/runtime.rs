use crate::{native, protocol};
use anyhow::{Result, ensure};
use ed25519_dalek::SigningKey;
use rand::{RngCore, rngs::OsRng};
use std::{
    fs::{self, File, OpenOptions},
    io::Write,
    net::{IpAddr, TcpListener, TcpStream},
    path::Path,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering},
        mpsc::{Receiver, SyncSender, sync_channel},
    },
    time::{Duration, Instant},
};
pub enum Media {
    VideoConfig {
        epoch: u64,
        codec: u32,
        width: u32,
        height: u32,
    },
    Video {
        epoch: u64,
        codec: u32,
        width: u32,
        height: u32,
        data: Vec<u8>,
        pts: f64,
        payload_received_at: f64,
        ready_at: f64,
    },
    Audio {
        epoch: u64,
        serial: u64,
        pcm: Vec<i16>,
        pts: f64,
    },
}
#[derive(Default)]
struct TimingRange {
    min: f64,
    sum: f64,
    max: f64,
}
impl TimingRange {
    fn record(&mut self, value: f64, first: bool) {
        self.min = if first { value } else { self.min.min(value) };
        self.max = if first { value } else { self.max.max(value) };
        self.sum += value;
    }
}
#[derive(Default)]
struct VideoTiming {
    samples: u64,
    arrival_age: TimingRange,
    parse: TimingRange,
    receive_to_submit: TimingRange,
}
impl VideoTiming {
    fn record(&mut self, received: f64, ready: f64, pts: f64, submit: f64) {
        // PTS is the mapped sender presentation deadline. Negative age means the
        // packet arrived early; it does not measure screen capture latency.
        let arrival_age = (received - pts) * 1000.0;
        let parse = (ready - received) * 1000.0;
        let receive_to_submit = (submit - received) * 1000.0;
        if ![arrival_age, parse, receive_to_submit]
            .iter()
            .all(|value| value.is_finite())
        {
            return;
        }
        let first = self.samples == 0;
        self.arrival_age.record(arrival_age, first);
        self.parse.record(parse, first);
        self.receive_to_submit.record(receive_to_submit, first);
        self.samples += 1;
    }
    fn log(&self, shared: &Shared, epoch: u64) {
        if self.samples == 0 {
            return;
        }
        let count = self.samples as f64;
        shared.log(&format!(
            "video_timing epoch={epoch} frames={} arrival_age_min_ms={:.3} arrival_age_avg_ms={:.3} arrival_age_max_ms={:.3} parse_min_ms={:.3} parse_avg_ms={:.3} parse_max_ms={:.3} receive_to_submit_min_ms={:.3} receive_to_submit_avg_ms={:.3} receive_to_submit_max_ms={:.3}",
            self.samples,
            self.arrival_age.min, self.arrival_age.sum / count, self.arrival_age.max,
            self.parse.min, self.parse.sum / count, self.parse.max,
            self.receive_to_submit.min, self.receive_to_submit.sum / count, self.receive_to_submit.max,
        ));
    }
}
struct DrainBudget {
    limit: usize,
    received: usize,
}
impl DrainBudget {
    fn new(limit: usize) -> Self {
        Self { limit, received: 0 }
    }
    fn consumed(&mut self) {
        self.received += 1;
    }
    fn exhausted(&self) -> bool {
        self.received == self.limit
    }
}
fn pump_wait_ms(discovery_ready: bool, backlog_possible: bool) -> u32 {
    if backlog_possible {
        0
    } else if !discovery_ready {
        25
    } else {
        // Native presentation uses a deadline timer; queues and UI/audio wake it.
        1000
    }
}
impl Media {
    fn epoch(&self) -> u64 {
        match self {
            Self::Video { epoch, .. }
            | Self::VideoConfig { epoch, .. }
            | Self::Audio { epoch, .. } => *epoch,
        }
    }
}
// A queue item from a newly claimed epoch must survive a stale main-loop owner
// snapshot; otherwise the sender's first keyframe can be lost before decoding.
fn defer_new_owner(
    message: Media,
    observed_owner: u64,
    live_owner: u64,
    pending: &mut Option<Media>,
) -> bool {
    if live_owner != 0 && message.epoch() != observed_owner && message.epoch() == live_owner {
        *pending = Some(message);
        true
    } else {
        false
    }
}
pub struct Shared {
    pub shutdown: AtomicBool,
    pub disconnect: AtomicU64,
    pub flush: AtomicU64,
    pub volume_db: AtomicU32,
    pub owner: AtomicU64,
    pub owner_started: AtomicU64,
    // Negotiation hint only; wire presentation timestamps are mapped unchanged.
    pub playout_ms: u32,
    pub next: AtomicU64,
    pub connections: AtomicUsize,
    pub identity: SigningKey,
    pub device: String,
    pub pi: String,
    pub video_profile: protocol::VideoProfile,
    video: SyncSender<Media>,
    audio: SyncSender<Media>,
    log: Mutex<(File, u64)>,
    pub status: Mutex<String>,
    wake: native::Wake,
}
impl Shared {
    pub fn log(&self, text: &str) {
        let text: String = text
            .chars()
            .filter(|c| !c.is_control() || *c == ' ')
            .take(2048)
            .collect();
        let line = format!("{:.3} {text}\n", native::now());
        if let Ok(mut log) = self.log.lock() {
            if log.1 + line.len() as u64 > 1024 * 1024 {
                let _ = log.0.set_len(0);
                log.1 = 0;
            }
            let _ = log.0.write_all(line.as_bytes());
            log.1 += line.len() as u64;
        }
    }
    pub fn status(&self, text: &str) {
        *self.status.lock().unwrap() = text.to_string();
        self.log(text);
        self.wake.signal();
    }
    pub fn send_video(&self, m: Media) -> Result<()> {
        let start = Instant::now();
        let mut message = m;
        let generation = self.disconnect.load(Ordering::Acquire);
        loop {
            match self.video.try_send(message) {
                Ok(()) => {
                    self.wake.signal();
                    return Ok(());
                }
                Err(std::sync::mpsc::TrySendError::Full(m)) => {
                    ensure!(
                        start.elapsed() < Duration::from_millis(500)
                            && !self.shutdown.load(Ordering::Acquire)
                            && self.disconnect.load(Ordering::Acquire) == generation,
                        "video consumer stalled; session ended without discarding compressed dependencies"
                    );
                    message = m;
                    self.wake.signal();
                    std::thread::sleep(Duration::from_millis(1));
                }
                Err(std::sync::mpsc::TrySendError::Disconnected(_)) => {
                    anyhow::bail!("video consumer closed")
                }
            }
        }
    }
    pub fn send_audio(&self, m: Media) -> Result<()> {
        self.audio
            .try_send(m)
            .map_err(|_| anyhow::anyhow!("audio queue full"))?;
        self.wake.signal();
        Ok(())
    }
}
fn identity(path: &Path) -> Result<[u8; 32]> {
    use zeroize::Zeroize;
    if path.exists() {
        ensure!(fs::metadata(path)?.len() <= 4096, "identity file size");
        let mut data = native::protect(&fs::read(path)?, true)?;
        ensure!(data.len() == 32, "identity format");
        let key = data.as_slice().try_into()?;
        data.zeroize();
        Ok(key)
    } else {
        let mut key = [0; 32];
        OsRng.fill_bytes(&mut key);
        let protected = native::protect(&key, false)?;
        let mut file = OpenOptions::new().write(true).create_new(true).open(path)?;
        file.write_all(&protected)?;
        file.sync_all()?;
        Ok(key)
    }
}
pub struct ReceiverApp {
    pub shared: Arc<Shared>,
    video: Receiver<Media>,
    audio: Receiver<Media>,
    server: Option<std::thread::JoinHandle<()>>,
    discovery: Vec<native::Discovery>,
}
impl ReceiverApp {
    pub fn new(
        data: &Path,
        bind: IpAddr,
        port: u16,
        advertise: bool,
        wake: native::Wake,
        video_profile: protocol::VideoProfile,
        playout_ms: u32,
    ) -> Result<Self> {
        protocol::clock::Clock::new(playout_ms)?;
        let lan = if advertise {
            Some(native::lan_ipv4()?)
        } else {
            None
        };
        let bind = if let Some((address, _, _)) = lan {
            ensure!(
                bind.is_unspecified() || bind == IpAddr::V4(address),
                "advertised bind must be the selected private LAN"
            );
            IpAddr::V4(address)
        } else {
            bind
        };
        fs::create_dir_all(data)?;
        let key = identity(&data.join("identity.dpapi"))?;
        let identity = SigningKey::from_bytes(&key);
        let pubkey = identity.verifying_key().to_bytes();
        let mut mac: [u8; 6] = pubkey[..6].try_into()?;
        mac[0] = (mac[0] | 2) & !1;
        let device = mac
            .iter()
            .map(|b| format!("{b:02X}"))
            .collect::<Vec<_>>()
            .join(":");
        let hex = pubkey
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<String>();
        let pi = format!(
            "{}-{}-{}-{}-{}",
            &hex[..8],
            &hex[8..12],
            &hex[12..16],
            &hex[16..20],
            &hex[20..32]
        );
        let log = OpenOptions::new()
            .create(true)
            .append(true)
            .open(data.join("receiver.log"))?;
        let size = log.metadata()?.len();
        let (vtx, video) = sync_channel(8);
        let (atx, audio) = sync_channel(32);
        let shared = Arc::new(Shared {
            shutdown: AtomicBool::new(false),
            disconnect: AtomicU64::new(0),
            flush: AtomicU64::new(0),
            volume_db: AtomicU32::new(0.0f32.to_bits()),
            owner: AtomicU64::new(0),
            owner_started: AtomicU64::new(0),
            playout_ms,
            next: AtomicU64::new(1),
            connections: AtomicUsize::new(0),
            identity,
            video_profile,
            device: device.clone(),
            pi: pi.clone(),
            video: vtx,
            audio: atx,
            log: Mutex::new((log, size)),
            status: Mutex::new("Ready — select AirMirrorLite on iPhone".into()),
            wake,
        });
        shared.log(&format!(
            "requested_video={}x{} fps={} hevc={} advertised_latency_ms={}",
            video_profile.width,
            video_profile.height,
            video_profile.fps,
            video_profile.hevc,
            playout_ms
        ));
        let listener = TcpListener::bind((bind, port))?;
        listener.set_nonblocking(true)?;
        let port = listener.local_addr()?.port();
        let mut discovery = Vec::new();
        if advertise {
            let (address, interface_index, _) = lan.unwrap();
            // DnsServiceRegister publishes the service but does not create an A/AAAA
            // record for an arbitrary synthetic hostname, even with pIp4 supplied.
            let computer = std::env::var("COMPUTERNAME")?;
            ensure!(
                !computer.is_empty()
                    && computer.len() <= 63
                    && computer
                        .bytes()
                        .all(|b| b.is_ascii_alphanumeric() || b == b'-'),
                "invalid computer DNS hostname"
            );
            let host = format!("{computer}.local");
            let features = video_profile.features_text();
            discovery.push(native::Discovery::register(
                "AirMirrorLite._airplay._tcp.local",
                &host,
                port,
                address,
                interface_index,
                &[
                    ("deviceid", device.clone()),
                    ("features", features.clone()),
                    ("flags", "0x84".into()),
                    ("model", "AppleTV3,2".into()),
                    ("srcvers", "220.68".into()),
                    ("vv", "2".into()),
                    ("pi", pi),
                    ("pk", hex.clone()),
                    ("pw", "false".into()),
                ],
            )?);
            discovery.push(native::Discovery::register(
                &format!("{}@AirMirrorLite._raop._tcp.local", device.replace(':', "")),
                &host,
                port,
                address,
                interface_index,
                &[
                    ("txtvers", "1".into()),
                    ("ch", "2".into()),
                    ("cn", "2,3".into()),
                    ("et", "3,5".into()),
                    ("sv", "false".into()),
                    ("da", "true".into()),
                    ("sr", "44100".into()),
                    ("ss", "16".into()),
                    ("tp", "UDP".into()),
                    ("vn", "65537".into()),
                    ("vs", "220.68".into()),
                    ("am", "AppleTV3,2".into()),
                    ("sf", "0x4".into()),
                    ("ft", features),
                    ("pk", hex),
                    ("pw", "false".into()),
                ],
            )?);
        }
        shared.log(&format!(
            "listening address={bind} port={port} discovery={advertise}; no PIN; one private-network peer"
        ));
        let server_shared = shared.clone();
        let server = std::thread::spawn(move || server_loop(listener, server_shared));
        Ok(Self {
            shared,
            video,
            audio,
            server: Some(server),
            discovery,
        })
    }
    pub fn run(&mut self, app: &native::App, seconds: Option<u64>) -> Result<()> {
        let start = Instant::now();
        let mut active = 0;
        let (mut got_video, mut got_audio, mut reported) = (false, false, false);
        let mut flush = self.shared.flush.load(Ordering::Acquire);
        let mut last_stats = Instant::now();
        let mut last_title = String::new();
        let mut discovery_ready = self.discovery.is_empty();
        let (mut pending_audio, mut pending_video) = (None, None);
        let mut presented_base = app.presented();
        let mut first_picture = false;
        let mut video_timing = VideoTiming::default();
        loop {
            if seconds.is_some_and(|s| start.elapsed() >= Duration::from_secs(s)) {
                break;
            }
            if !discovery_ready {
                let mut ready = true;
                for service in &self.discovery {
                    ready &= service.ready()?;
                }
                if ready {
                    discovery_ready = true;
                    self.shared.log(&format!(
                        "discovery ready elapsed_ms={:.1}",
                        start.elapsed().as_secs_f64() * 1000.0
                    ));
                }
            }
            let owner = self.shared.owner.load(Ordering::Acquire);
            let next_flush = self.shared.flush.load(Ordering::Acquire);
            if next_flush != flush {
                app.flush_audio();
                flush = next_flush;
            }
            if owner != active {
                video_timing = VideoTiming::default();
                app.reset()?;
                active = owner;
                presented_base = app.presented();
                first_picture = false;
                got_video = false;
                got_audio = false;
                reported = false;
            }
            let changed_status = {
                let status = self.shared.status.lock().unwrap();
                (status.as_str() != last_title).then(|| status.clone())
            };
            if let Some(status) = changed_status {
                app.title(&format!(
                    "AirMirrorLite — {status} | F11 fullscreen · D disconnect"
                ));
                last_title = status;
            }
            let mut audio_drain = DrainBudget::new(if owner == 0 { 33 } else { 32 });
            for _ in 0..audio_drain.limit {
                match pending_audio
                    .take()
                    .map(Ok)
                    .unwrap_or_else(|| self.audio.try_recv())
                {
                    Ok(Media::Audio {
                        epoch,
                        serial,
                        mut pcm,
                        pts,
                    }) if epoch == owner && owner != 0 && serial == flush => {
                        audio_drain.consumed();
                        let db = f32::from_bits(self.shared.volume_db.load(Ordering::Relaxed));
                        let gain = if db <= -144.0 {
                            0.0
                        } else {
                            10.0f32.powf(db / 20.0)
                        };
                        if gain < 1.0 {
                            for s in &mut pcm {
                                *s = (f32::from(*s) * gain) as i16;
                            }
                        }
                        if let Err(e) = app.audio(&pcm, pts) {
                            self.shared.log(&e.to_string());
                            self.shared.disconnect.fetch_add(1, Ordering::AcqRel);
                        } else {
                            got_audio = true;
                        }
                    }
                    Ok(message) => {
                        audio_drain.consumed();
                        if defer_new_owner(
                            message,
                            owner,
                            self.shared.owner.load(Ordering::Acquire),
                            &mut pending_audio,
                        ) {
                            break;
                        }
                    }
                    Err(_) => break,
                }
            }
            let mut video_drain = DrainBudget::new(if owner == 0 { 9 } else { 2 });
            for _ in 0..video_drain.limit {
                if owner != 0 && !app.video_ready() {
                    break;
                }
                match pending_video
                    .take()
                    .map(Ok)
                    .unwrap_or_else(|| self.video.try_recv())
                {
                    Ok(Media::VideoConfig {
                        epoch,
                        codec,
                        width,
                        height,
                    }) if epoch == owner && owner != 0 => {
                        video_drain.consumed();
                        let begin = Instant::now();
                        if let Err(e) =
                            app.prepare_video(codec, width, height, self.shared.video_profile.fps)
                        {
                            self.shared.status(&e.to_string());
                            self.shared.disconnect.fetch_add(1, Ordering::AcqRel);
                            break;
                        }
                        if !got_video {
                            self.shared.log(&format!(
                                "decoder prepared codec={codec} elapsed_ms={:.1}",
                                begin.elapsed().as_secs_f64() * 1000.0
                            ));
                        }
                    }
                    Ok(Media::Video {
                        epoch,
                        data,
                        pts,
                        codec,
                        width,
                        height,
                        payload_received_at,
                        ready_at,
                    }) if epoch == owner && owner != 0 => {
                        video_drain.consumed();
                        video_timing.record(payload_received_at, ready_at, pts, native::now());
                        if let Err(e) = app.video(&data, pts, codec, width, height) {
                            self.shared.status(&e.to_string());
                            self.shared.disconnect.fetch_add(1, Ordering::AcqRel);
                        } else {
                            got_video = true;
                        }
                    }
                    Ok(message) => {
                        video_drain.consumed();
                        if defer_new_owner(
                            message,
                            owner,
                            self.shared.owner.load(Ordering::Acquire),
                            &mut pending_video,
                        ) {
                            break;
                        }
                    }
                    Err(_) => break,
                }
            }
            if got_video && got_audio && !reported {
                self.shared.status("Mirroring — picture and sound");
                reported = true;
            }
            // Event notifications coalesce. A consumed whole drain budget can
            // leave messages queued after the last signal; check again without
            // sleeping. The next pass still respects native video backpressure.
            let backlog_possible = audio_drain.exhausted()
                || video_drain.exhausted()
                || pending_audio.is_some()
                || pending_video.is_some()
                || self.shared.owner.load(Ordering::Acquire) != owner;
            match app.pump(pump_wait_ms(discovery_ready, backlog_possible))? {
                0 => break,
                2 => {
                    self.shared.disconnect.fetch_add(1, Ordering::AcqRel);
                }
                _ => {}
            }
            if owner != 0 && !first_picture && app.presented() > presented_base {
                let claim = f64::from_bits(self.shared.owner_started.load(Ordering::Acquire));
                self.shared.log(&format!(
                    "first picture presented since_claim_ms={:.1}",
                    (native::now() - claim) * 1000.0
                ));
                first_picture = true;
            }
            if last_stats.elapsed() >= Duration::from_secs(5) {
                video_timing.log(&self.shared, active);
                self.shared.log(&app.stats());
                last_stats = Instant::now();
            }
        }
        self.shared.log(&app.stats());
        Ok(())
    }
}
impl Drop for ReceiverApp {
    fn drop(&mut self) {
        self.shared.shutdown.store(true, Ordering::Release);
        self.discovery.clear();
        if let Some(t) = self.server.take() {
            let _ = t.join();
        }
        self.shared
            .log("receiver stopped; all session threads joined");
    }
}
fn permitted(peer: IpAddr) -> bool {
    match peer {
        IpAddr::V4(p) => p.is_private() || p.is_link_local() || p.is_loopback(),
        IpAddr::V6(p) => p.is_unique_local() || p.is_unicast_link_local() || p.is_loopback(),
    }
}
fn server_loop(listener: TcpListener, shared: Arc<Shared>) {
    let mut threads = Vec::<std::thread::JoinHandle<()>>::new();
    let mut rate_start = Instant::now();
    let mut rate = 0;
    while !shared.shutdown.load(Ordering::Acquire) {
        let mut i = 0;
        while i < threads.len() {
            if threads[i].is_finished() {
                let _ = threads.swap_remove(i).join();
            } else {
                i += 1;
            }
        }
        match listener.accept() {
            Ok((stream, addr)) => {
                if rate_start.elapsed() >= Duration::from_secs(1) {
                    rate_start = Instant::now();
                    rate = 0;
                }
                rate += 1;
                if !permitted(addr.ip()) || threads.len() >= 4 || rate > 16 {
                    continue;
                }
                let shared = shared.clone();
                threads.push(std::thread::spawn(move || connection(stream, shared)));
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(20))
            }
            Err(e) => {
                shared.status(&format!("listener failed: {e}"));
                break;
            }
        }
    }
    for thread in threads {
        let _ = thread.join();
    }
}
fn connection(stream: TcpStream, shared: Arc<Shared>) {
    shared.log("control connection accepted");
    shared.connections.fetch_add(1, Ordering::AcqRel);
    if let Err(e) = protocol::serve(stream, shared.clone()) {
        shared.log(&format!("control closed: {e}"));
    }
    shared.connections.fetch_sub(1, Ordering::AcqRel);
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn startup_keyframe_survives_stale_owner_snapshot() {
        let mut pending = None;
        let frame = Media::Video {
            epoch: 2,
            codec: 265,
            width: 994,
            height: 2160,
            data: vec![1, 2, 3],
            pts: 0.0,
            payload_received_at: 0.0,
            ready_at: 0.0,
        };
        assert!(defer_new_owner(frame, 1, 2, &mut pending));
        match pending.take().unwrap() {
            Media::Video { epoch, data, .. } => {
                assert_eq!(epoch, 2);
                assert_eq!(data, vec![1, 2, 3]);
            }
            _ => panic!("wrong queued message"),
        }
        let old = Media::VideoConfig {
            epoch: 1,
            codec: 264,
            width: 0,
            height: 0,
        };
        assert!(!defer_new_owner(old, 1, 2, &mut pending));
        assert!(pending.is_none());
        let stale_audio = Media::Audio {
            epoch: 2,
            serial: 0,
            pcm: vec![],
            pts: 0.0,
        };
        assert!(!defer_new_owner(stale_audio, 2, 2, &mut pending));
        assert!(pending.is_none());
    }
    #[test]
    fn private_peer_scope() {
        assert!(permitted("192.168.1.2".parse().unwrap()));
        assert!(!permitted("8.8.8.8".parse().unwrap()));
        assert!(!permitted("2001:4860:4860::8888".parse().unwrap()));
    }
    #[test]
    fn video_timing_preserves_signed_presentation_age() {
        let mut timing = VideoTiming::default();
        timing.record(100.0, 100.001, 99.980, 100.006);
        timing.record(100.1, 100.102, 100.120, 100.105);
        assert_eq!(timing.samples, 2);
        assert!((timing.arrival_age.min + 20.0).abs() < 1e-6);
        assert!((timing.arrival_age.max - 20.0).abs() < 1e-6);
        assert!(timing.arrival_age.sum.abs() < 1e-6);
        assert!((timing.parse.sum / 2.0 - 1.5).abs() < 1e-6);
        assert!((timing.receive_to_submit.min - 5.0).abs() < 1e-6);
        assert!((timing.receive_to_submit.max - 6.0).abs() < 1e-6);
        timing.record(f64::NAN, 0.0, 0.0, 0.0);
        assert_eq!(timing.samples, 2);

        // A new session starts with fresh extrema, including negative-only ages.
        timing = VideoTiming::default();
        timing.record(200.0, 200.001, 200.010, 200.002);
        assert_eq!(timing.samples, 1);
        assert!((timing.arrival_age.min + 10.0).abs() < 1e-6);
        assert!((timing.arrival_age.max + 10.0).abs() < 1e-6);
    }
    #[test]
    fn coalesced_notifications_cannot_leave_budget_backlog_asleep() {
        for limit in [2, 32] {
            let (send, receive) = sync_channel(limit + 1);
            for value in 0..=limit {
                send.send(value).unwrap();
            }
            let mut budget = DrainBudget::new(limit);
            for _ in 0..budget.limit {
                receive.try_recv().unwrap();
                budget.consumed();
            }
            assert_eq!(pump_wait_ms(true, budget.exhausted()), 0);
            let mut next = DrainBudget::new(limit);
            while receive.try_recv().is_ok() {
                next.consumed();
            }
            assert_eq!(next.received, 1);
            assert_eq!(pump_wait_ms(true, next.exhausted()), 1000);
        }
        assert_eq!(pump_wait_ms(false, false), 25);
        assert_eq!(pump_wait_ms(false, true), 0);
        // Backpressure without consumed messages waits for the native deadline.
        assert_eq!(pump_wait_ms(true, DrainBudget::new(2).exhausted()), 1000);
    }
}
