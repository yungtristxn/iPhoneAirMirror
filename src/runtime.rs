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
        Arc, Condvar, Mutex,
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
        rotation_cw_degrees: u32,
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
#[derive(Default)]
struct AudioOwnership(AtomicU64);
impl AudioOwnership {
    fn activate(&self, epoch: u64) -> Result<bool> {
        ensure!(epoch != 0, "audio transport requires an active session");
        match self
            .0
            .compare_exchange(0, epoch, Ordering::AcqRel, Ordering::Acquire)
        {
            Ok(_) => Ok(true),
            Err(current) if current == epoch => Ok(false),
            Err(_) => anyhow::bail!("audio transport belongs to another session"),
        }
    }
    fn clear(&self, epoch: u64) -> bool {
        epoch != 0
            && self
                .0
                .compare_exchange(epoch, 0, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
    }
    fn synchronized(&self, owner: u64) -> bool {
        owner == 0 || self.owns(owner)
    }
    fn owns(&self, epoch: u64) -> bool {
        epoch != 0 && self.0.load(Ordering::Acquire) == epoch
    }
}
#[derive(Clone, Copy, Debug, Default)]
pub struct AudioFlush {
    pub serial: u64,
    pub cutoff: Option<protocol::AudioCutoff>,
    pub anchor_after: Option<f64>,
}
#[derive(Default)]
struct MediaTransitions {
    revision: u64,
    applied: u64,
    flush: AudioFlush,
}
impl MediaTransitions {
    fn apply<T>(&mut self, operation: impl FnOnce(AudioFlush) -> Result<T>) -> Result<T> {
        let result = operation(self.flush)?;
        self.applied = self.revision;
        Ok(result)
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ReceiverStatus {
    Ready,
    Connecting(&'static str),
    Streaming,
    Error(String),
    Unavailable(String),
}
impl ReceiverStatus {
    fn replace(&mut self, next: Self, same_failed_session: bool) -> bool {
        if matches!(self, Self::Unavailable(_)) && !matches!(next, Self::Unavailable(_)) {
            return false;
        }
        if matches!(self, Self::Error(_))
            && (matches!(next, Self::Ready | Self::Connecting(_))
                || (same_failed_session && matches!(next, Self::Streaming)))
        {
            return false;
        }
        let changed = *self != next;
        *self = next;
        changed
    }
    fn dismiss(&mut self) {
        if matches!(self, Self::Error(_)) {
            *self = Self::Ready;
        }
    }
    fn native(&self) -> (u32, &str) {
        match self {
            Self::Ready => (0, "Ready — select AirMirrorLite on iPhone"),
            Self::Connecting(text) => (1, text),
            Self::Streaming => (2, "Mirroring"),
            Self::Error(text) => (4, text),
            Self::Unavailable(text) => (5, text),
        }
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
    audio_owner: AudioOwnership,
    transitions: Mutex<MediaTransitions>,
    transition_applied: Condvar,
    pub owner_started: AtomicU64,
    // Negotiation hint only; wire presentation timestamps are mapped unchanged.
    pub playout_ms: u32,
    pub next: AtomicU64,
    pub connections: AtomicUsize,
    pub identity: SigningKey,
    pub device: String,
    pub pi: String,
    pub video_profile: protocol::VideoProfile,
    display_updates: Mutex<Option<(u64, Arc<protocol::events::DisplayUpdates>)>>,
    video: SyncSender<Media>,
    audio: SyncSender<Media>,
    log: Mutex<(File, u64)>,
    status: Mutex<ReceiverStatus>,
    status_error_owner: AtomicU64,
    wake: native::Wake,
}
impl Shared {
    pub(crate) fn display_geometry(&self) -> (u32, u32) {
        let registered = self.display_updates.lock().unwrap();
        let live = self.owner.load(Ordering::Acquire);
        if let Some((owner, updates)) = &*registered
            && live != 0
            && *owner == live
        {
            updates.requested_size()
        } else {
            (self.video_profile.width, self.video_profile.height)
        }
    }
    pub fn register_display_updates(
        &self,
        epoch: u64,
        updates: Arc<protocol::events::DisplayUpdates>,
    ) -> Result<()> {
        let _transition = self.transitions.lock().unwrap();
        ensure!(
            self.owner.load(Ordering::Acquire) == epoch,
            "display session ownership changed"
        );
        *self.display_updates.lock().unwrap() = Some((epoch, updates));
        Ok(())
    }
    fn update_presented_display(&self, epoch: u64, size: (u32, u32)) {
        let Some(target) = display_target(self.video_profile, size) else {
            return;
        };
        let registered = self.display_updates.lock().unwrap();
        if let Some((owner, updates)) = &*registered
            && *owner == epoch
            && self.owner.load(Ordering::Acquire) == epoch
            && let Err(error) = updates.request(target.0, target.1)
        {
            self.log(&format!("display update rejected locally: {error}"));
        }
    }
    pub fn claim(&self, epoch: u64) -> Result<()> {
        let mut transition = self.transitions.lock().unwrap();
        ensure!(
            self.owner
                .compare_exchange(0, epoch, Ordering::AcqRel, Ordering::Acquire)
                .is_ok(),
            "receiver busy"
        );
        self.owner_started
            .store(native::now().to_bits(), Ordering::Release);
        transition.revision += 1;
        drop(transition);
        self.wake.signal();
        Ok(())
    }
    pub fn release(&self, epoch: u64) {
        let _ = self.release_transition(epoch);
    }
    pub fn release_applied(&self, epoch: u64) -> Result<()> {
        if let Some(revision) = self.release_transition(epoch) {
            self.wait_applied(revision)?;
        }
        Ok(())
    }
    fn release_transition(&self, epoch: u64) -> Option<u64> {
        let mut transition = self.transitions.lock().unwrap();
        if self
            .owner
            .compare_exchange(epoch, 0, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            let mut display = self.display_updates.lock().unwrap();
            if display.as_ref().is_some_and(|(owner, _)| *owner == epoch) {
                *display = None;
            }
            drop(display);
            transition.revision += 1;
            let revision = transition.revision;
            drop(transition);
            self.wake.signal();
            Some(revision)
        } else {
            None
        }
    }
    fn wait_applied(&self, revision: u64) -> Result<()> {
        let deadline = Instant::now() + Duration::from_secs(2);
        let mut transition = self.transitions.lock().unwrap();
        while transition.applied < revision {
            ensure!(
                !self.shutdown.load(Ordering::Acquire),
                "receiver is shutting down"
            );
            ensure!(
                Instant::now() < deadline,
                "media transition application timed out"
            );
            transition = self
                .transition_applied
                .wait_timeout(transition, Duration::from_millis(100))
                .unwrap()
                .0;
        }
        Ok(())
    }
    pub fn flush_snapshot(&self) -> AudioFlush {
        self.transitions.lock().unwrap().flush
    }
    pub fn request_flush(&self, epoch: u64, cutoff: protocol::AudioCutoff) -> Result<()> {
        let mut transition = self.transitions.lock().unwrap();
        ensure!(
            self.owner.load(Ordering::Acquire) == epoch,
            "flush session ownership changed"
        );
        transition.flush = AudioFlush {
            serial: transition.flush.serial + 1,
            cutoff: Some(cutoff),
            anchor_after: Some(native::now()),
        };
        self.flush.store(transition.flush.serial, Ordering::Release);
        transition.revision += 1;
        let revision = transition.revision;
        drop(transition);
        self.wake.signal();
        self.wait_applied(revision)
    }
    pub fn audio_started(&self, epoch: u64) -> Result<()> {
        let mut transition = self.transitions.lock().unwrap();
        ensure!(
            self.owner.load(Ordering::Acquire) == epoch,
            "audio session ownership changed"
        );
        if self.audio_owner.activate(epoch)? {
            // A genuinely new transport has its own wire sequence and anchor.
            // Advance the PCM generation while removing the prior transport's fence.
            transition.flush = AudioFlush {
                serial: transition.flush.serial + 1,
                ..AudioFlush::default()
            };
            self.flush.store(transition.flush.serial, Ordering::Release);
            transition.revision += 1;
        }
        let revision = transition.revision;
        drop(transition);
        self.wake.signal();
        self.wait_applied(revision)
    }
    pub fn audio_stopped(&self, epoch: u64) -> Result<()> {
        let mut transition = self.transitions.lock().unwrap();
        if self.owner.load(Ordering::Acquire) != epoch {
            return Ok(());
        }
        transition.flush = AudioFlush {
            serial: transition.flush.serial + 1,
            ..AudioFlush::default()
        };
        self.flush.store(transition.flush.serial, Ordering::Release);
        self.audio_owner.clear(epoch);
        transition.revision += 1;
        let revision = transition.revision;
        drop(transition);
        self.wake.signal();
        if self.shutdown.load(Ordering::Acquire) {
            return Ok(());
        }
        self.wait_applied(revision)
    }
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
    pub fn status(&self, status: ReceiverStatus) {
        let mut current = self.status.lock().unwrap();
        // Cleanup from a released session must not replace a newer owner's
        // Connecting/Streaming state while waiting for its applied boundary.
        if matches!(status, ReceiverStatus::Ready) && self.owner.load(Ordering::Acquire) != 0 {
            return;
        }
        let same_failed_session =
            self.status_error_owner.load(Ordering::Acquire) == self.owner.load(Ordering::Acquire);
        if current.replace(status, same_failed_session) {
            self.log(current.native().1);
            self.wake.signal();
        }
    }
    pub fn failure(&self, reason: &str) {
        self.status_error_owner
            .store(self.owner.load(Ordering::Acquire), Ordering::Release);
        self.status(ReceiverStatus::Error(
            reason
                .chars()
                .filter(|c| !c.is_control())
                .take(240)
                .collect(),
        ));
    }
    fn unavailable(&self, reason: &str) {
        self.status(ReceiverStatus::Unavailable(
            reason
                .chars()
                .filter(|c| !c.is_control())
                .take(240)
                .collect(),
        ));
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
fn discovery_bind_request(bind: IpAddr) -> Result<Option<std::net::Ipv4Addr>> {
    match bind {
        IpAddr::V4(address) if !address.is_unspecified() => Ok(Some(address)),
        IpAddr::V4(_) => Ok(None),
        IpAddr::V6(_) => anyhow::bail!("discovery requires a local IPv4 address"),
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
            Some(native::lan_ipv4(discovery_bind_request(bind)?)?)
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
            audio_owner: AudioOwnership::default(),
            transitions: Mutex::new(MediaTransitions::default()),
            transition_applied: Condvar::new(),
            owner_started: AtomicU64::new(0),
            playout_ms,
            next: AtomicU64::new(1),
            connections: AtomicUsize::new(0),
            identity,
            video_profile,
            display_updates: Mutex::new(None),
            device: device.clone(),
            pi: pi.clone(),
            video: vtx,
            audio: atx,
            log: Mutex::new((log, size)),
            status: Mutex::new(ReceiverStatus::Ready),
            status_error_owner: AtomicU64::new(0),
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
        let mut last_status = None;
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
            // A teardown publishes its flush before clearing audio ownership.
            // Read ownership first so an observed interactive transition also
            // observes and flushes the previous audio before changing mode.
            let video_synchronized = self.shared.audio_owner.synchronized(owner);
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
            // Negotiated transport ownership decides synchronization. Muting,
            // volume and silence never move an audio session to interactive mode.
            app.video_sync(video_synchronized);
            let changed_status = {
                let mut status = self.shared.status.lock().unwrap();
                if app.take_status_dismissed() {
                    status.dismiss();
                }
                (Some(&*status) != last_status.as_ref()).then(|| status.clone())
            };
            if let Some(status) = changed_status {
                let (kind, detail) = status.native();
                app.status(kind, detail);
                last_status = Some(status);
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
                    }) if epoch == owner && self.shared.audio_owner.owns(epoch) => {
                        audio_drain.consumed();
                        let next_flush = self.shared.flush.load(Ordering::Acquire);
                        if next_flush != flush {
                            app.flush_audio();
                            flush = next_flush;
                        }
                        if serial != flush {
                            continue;
                        }
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
                        // Audio may have been configured while the main thread
                        // was waiting. Synchronize before its first PCM is queued.
                        app.video_sync(true);
                        if let Err(e) = app.audio(&pcm, pts) {
                            self.shared.failure(&format!(
                                "Audio playback failed. Reconnect mirroring. {e}"
                            ));
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
                            self.shared.failure(&format!(
                                "Video preparation failed. Reconnect mirroring. {e}"
                            ));
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
                        rotation_cw_degrees,
                    }) if epoch == owner && owner != 0 => {
                        video_drain.consumed();
                        video_timing.record(payload_received_at, ready_at, pts, native::now());
                        if let Err(e) =
                            app.video(&data, pts, codec, width, height, rotation_cw_degrees)
                        {
                            self.shared.failure(&format!(
                                "Video playback failed. Reconnect mirroring. {e}"
                            ));
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
            if got_video && got_audio && first_picture && !reported {
                self.shared.status(ReceiverStatus::Streaming);
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
            if app.wait(pump_wait_ms(discovery_ready, backlog_possible))? == 0 {
                break;
            }
            // Wait never dispatches. Serialize this complete applied boundary
            // against SETUP/FLUSH/teardown publishers, including native tick.
            let mut transition = self.shared.transitions.lock().unwrap();
            let dispatched = transition.apply(|boundary| {
                let dispatch_owner = self.shared.owner.load(Ordering::Acquire);
                if dispatch_owner != active {
                    video_timing = VideoTiming::default();
                    app.reset()?;
                    active = dispatch_owner;
                    presented_base = app.presented();
                    first_picture = false;
                    got_video = false;
                    got_audio = false;
                    reported = false;
                }
                if boundary.serial != flush {
                    app.flush_audio();
                    flush = boundary.serial;
                }
                app.video_sync(self.shared.audio_owner.synchronized(dispatch_owner));
                app.dispatch()
            });
            self.shared.transition_applied.notify_all();
            drop(transition);
            match dispatched? {
                0 => break,
                2 => {
                    self.shared.disconnect.fetch_add(1, Ordering::AcqRel);
                }
                _ => {}
            }
            if cfg!(feature = "display-update-experiment") && active != 0 {
                self.shared
                    .update_presented_display(active, app.presented_size());
            }
            if active != 0 && !first_picture && app.presented() > presented_base {
                self.shared.status(ReceiverStatus::Streaming);
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
fn display_target(profile: protocol::VideoProfile, size: (u32, u32)) -> Option<(u32, u32)> {
    let (w, h) = size;
    if w == 0 || h == 0 || w > 3840 || h > 3840 {
        return None;
    }
    let short = profile.width.min(profile.height);
    let long = profile.width.max(profile.height);
    // Only rotate an existing phone-shaped hint within native admission caps.
    // Arbitrary external-media ratios and sender destination rectangles cannot
    // drive this feedback path. No upscaling/resolution negotiation is invented.
    if short < 48
        || !short.is_multiple_of(2)
        || !long.is_multiple_of(2)
        || long > if profile.hevc { 2160 } else { 1920 }
    {
        return None;
    }
    let expected = f64::from(short) / f64::from(long);
    let actual = f64::from(w.min(h)) / f64::from(w.max(h));
    if (actual - expected).abs() > expected * 0.01 {
        return None;
    }
    Some(if w > h { (long, short) } else { (short, long) })
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
                shared.unavailable(&format!(
                    "Receiver listener stopped. Restart AirMirrorLite. {e}"
                ));
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
    fn display_updates_follow_only_the_established_hint_shape() {
        let profile = protocol::VideoProfile::new(2160, true)
            .unwrap()
            .with_display_width(994)
            .unwrap();
        assert_eq!(display_target(profile, (994, 2160)), Some((994, 2160)));
        assert_eq!(display_target(profile, (2160, 994)), Some((2160, 994)));
        assert_eq!(display_target(profile, (994, 458)), Some((2160, 994)));
        assert_eq!(display_target(profile, (498, 1080)), Some((994, 2160)));
        for geometry in [(0, 0), (3840, 2160), (1600, 1200), (65535, 1)] {
            assert_eq!(display_target(profile, geometry), None);
        }
        let wide = protocol::VideoProfile::new(2160, true).unwrap();
        assert_eq!(display_target(wide, (3840, 2160)), None); // No unsupported tall4K hint.
        let low = protocol::VideoProfile::new(1080, false)
            .unwrap()
            .with_display_width(498)
            .unwrap();
        assert_eq!(display_target(low, (1080, 498)), Some((1080, 498)));
    }
    #[test]
    fn explicit_discovery_bind_is_forwarded_as_selection_filter() {
        assert_eq!(
            discovery_bind_request("0.0.0.0".parse().unwrap()).unwrap(),
            None
        );
        for address in ["192.168.1.20", "172.16.2.30"] {
            assert_eq!(
                discovery_bind_request(address.parse().unwrap()).unwrap(),
                Some(address.parse().unwrap())
            );
        }
        assert!(discovery_bind_request("::1".parse().unwrap()).is_err());
    }
    #[test]
    fn error_status_survives_cleanup_until_new_session_or_dismissal() {
        let mut status = ReceiverStatus::Error("Video failed; reconnect".into());
        assert!(!status.replace(ReceiverStatus::Ready, false));
        assert!(!status.replace(ReceiverStatus::Connecting("Connecting"), false));
        assert!(!status.replace(ReceiverStatus::Streaming, true));
        assert_eq!(status.native().0, 4);
        assert!(status.replace(ReceiverStatus::Streaming, false));
        status = ReceiverStatus::Error("Audio failed".into());
        status.dismiss();
        assert_eq!(status, ReceiverStatus::Ready);
        status = ReceiverStatus::Unavailable("Listener stopped; restart".into());
        assert!(!status.replace(ReceiverStatus::Ready, false));
        assert!(!status.replace(ReceiverStatus::Streaming, false));
        status.dismiss();
        assert_eq!(status.native().0, 5);
    }
    #[test]
    fn transition_published_during_wait_is_applied_before_dispatch_and_ack() {
        use std::sync::Barrier;
        let gate = Arc::new(Mutex::new(MediaTransitions::default()));
        let wait = Arc::new(Barrier::new(2));
        let wake = Arc::new(Barrier::new(2));
        let writer_gate = gate.clone();
        let writer_wait = wait.clone();
        let writer_wake = wake.clone();
        let writer = std::thread::spawn(move || {
            writer_wait.wait();
            let mut state = writer_gate.lock().unwrap();
            state.revision = 1;
            state.flush = AudioFlush {
                serial: 1,
                cutoff: Some(protocol::AudioCutoff {
                    sequence: 1,
                    timestamp: 480,
                }),
                anchor_after: Some(20.0),
            };
            assert_eq!(state.applied, 0);
            drop(state);
            writer_wake.wait();
        });
        // Simulated wait contains no native dispatch; the real native barrier
        // regression separately verifies am_wait preserves that boundary.
        wait.wait();
        wake.wait();
        let mut state = gate.lock().unwrap();
        let mut pending_pcm = vec![0u64];
        state
            .apply(|boundary| {
                pending_pcm.retain(|serial| *serial == boundary.serial);
                assert!(
                    pending_pcm.is_empty(),
                    "old PCM reached dispatch after wake"
                );
                assert!(
                    gate.try_lock().is_err(),
                    "publisher can race the applied dispatch"
                );
                Ok(())
            })
            .unwrap();
        assert_eq!(state.applied, 1);
        state.revision = 2;
        assert!(
            state
                .apply::<()>(|_| anyhow::bail!("native apply failed"))
                .is_err()
        );
        assert_eq!(
            state.applied, 1,
            "failed application cannot acknowledge revision2"
        );
        drop(state);
        writer.join().unwrap();
    }
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
            rotation_cw_degrees: 270,
        };
        assert!(defer_new_owner(frame, 1, 2, &mut pending));
        match pending.take().unwrap() {
            Media::Video {
                epoch,
                data,
                rotation_cw_degrees,
                ..
            } => {
                assert_eq!(epoch, 2);
                assert_eq!(data, vec![1, 2, 3]);
                assert_eq!(rotation_cw_degrees, 270);
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
    #[test]
    fn negotiated_audio_owns_sync_until_explicit_teardown() {
        let audio = AudioOwnership::default();
        assert!(audio.synchronized(0));
        assert!(!audio.synchronized(1));
        assert!(audio.activate(1).unwrap());
        assert!(!audio.activate(1).unwrap());
        // No packet arrival, volume or mute state is involved in this decision.
        for _ in 0..4 {
            assert!(audio.synchronized(1));
        }
        assert!(audio.clear(1));
        assert!(!audio.clear(1));
        assert!(!audio.synchronized(1));
        assert!(audio.synchronized(0));
    }
    #[test]
    fn stale_audio_teardown_cannot_clear_new_session() {
        let audio = AudioOwnership::default();
        assert!(audio.activate(1).unwrap());
        assert!(audio.activate(2).is_err());
        assert!(audio.synchronized(1));
        assert!(audio.clear(1));
        assert!(audio.activate(2).unwrap());
        assert!(!audio.clear(1));
        assert!(audio.synchronized(2));
        assert!(!audio.synchronized(3));
        assert!(audio.clear(2));
        assert!(audio.synchronized(0));
        assert!(!audio.synchronized(3));
        assert!(audio.activate(0).is_err());
        assert!(!audio.clear(0));
    }
}
