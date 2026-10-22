use crate::{native, protocol};
use anyhow::{Result, ensure};
use ed25519_dalek::SigningKey;
use rand::{RngCore, rngs::OsRng};
use std::{
    fs::{self, File, OpenOptions},
    io::{self, Seek, SeekFrom, Write},
    net::{IpAddr, TcpListener, TcpStream},
    os::windows::io::AsRawSocket,
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
        visibility_serial: u64,
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
    blocked: bool,
}
impl DrainBudget {
    fn new(limit: usize) -> Self {
        Self {
            limit,
            received: 0,
            blocked: false,
        }
    }
    fn consumed(&mut self) {
        self.received += 1;
    }
    fn exhausted(&self) -> bool {
        self.received == self.limit
    }
    fn block(&mut self) {
        self.blocked = true;
    }
    fn backlog_possible(&self, pending: bool) -> bool {
        !self.blocked && (self.exhausted() || pending)
    }
}
fn audio_can_drain(owner: u64, failed_owner: u64, native_ready: bool) -> bool {
    // With no owner, drain canceled messages even if the device is unavailable.
    owner == 0 || (failed_owner != owner && native_ready)
}
fn send_bounded_audio(
    sender: &SyncSender<Media>,
    mut message: Media,
    canceled: impl Fn() -> bool,
    wake: impl Fn(),
    limit: Duration,
) -> Result<()> {
    let started = Instant::now();
    let mut notified_full = false;
    loop {
        if canceled() {
            return Ok(());
        }
        match sender.try_send(message) {
            Ok(()) => {
                wake();
                return Ok(());
            }
            Err(std::sync::mpsc::TrySendError::Full(returned)) => {
                ensure!(
                    started.elapsed() < limit,
                    "audio consumer stalled; reconnect required"
                );
                message = returned;
                if !notified_full {
                    wake();
                    notified_full = true;
                }
                std::thread::sleep(Duration::from_millis(1));
            }
            Err(std::sync::mpsc::TrySendError::Disconnected(_)) => {
                anyhow::bail!("audio consumer closed");
            }
        }
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
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct ScreenVisibility {
    epoch: u64,
    serial: u64,
    suspended: bool,
}
#[derive(Default)]
struct MediaTransitions {
    revision: u64,
    applied: u64,
    flush: AudioFlush,
    screen: ScreenVisibility,
}
impl MediaTransitions {
    fn reset_screen(&mut self, epoch: u64) {
        self.screen = ScreenVisibility {
            epoch,
            ..ScreenVisibility::default()
        };
    }
    fn publish_screen(&mut self, owner: u64, epoch: u64, suspended: bool) -> Result<(u64, bool)> {
        ensure!(
            epoch != 0 && owner == epoch && self.screen.epoch == epoch,
            "screen-state session ownership changed"
        );
        if self.screen.suspended == suspended {
            return Ok((self.screen.serial, false));
        }
        let serial = self
            .screen
            .serial
            .checked_add(1)
            .ok_or_else(|| anyhow::anyhow!("screen visibility serial exhausted"))?;
        let revision = self
            .revision
            .checked_add(1)
            .ok_or_else(|| anyhow::anyhow!("media transition serial exhausted"))?;
        self.screen.serial = serial;
        self.screen.suspended = suspended;
        self.revision = revision;
        Ok((serial, true))
    }
    fn publish_screen_if_current(
        &mut self,
        shutting_down: bool,
        owner: u64,
        epoch: u64,
        suspended: bool,
    ) -> Result<Option<(u64, bool)>> {
        ensure!(
            epoch != 0,
            "screen-state publication requires an active epoch"
        );
        if shutting_down || owner != epoch {
            return Ok(None);
        }
        // A live owner's inconsistent state or exhausted serial is still an
        // invariant failure. Only ordinary lifetime cancellation returns None.
        self.publish_screen(owner, epoch, suspended).map(Some)
    }
    fn screen_for(&self, owner: u64, epoch: u64) -> Option<ScreenVisibility> {
        (epoch != 0 && owner == epoch && self.screen.epoch == epoch).then_some(self.screen)
    }
    fn apply<T>(
        &mut self,
        operation: impl FnOnce(AudioFlush, ScreenVisibility) -> Result<T>,
    ) -> Result<T> {
        let result = operation(self.flush, self.screen)?;
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
    audio_failed_owner: AtomicU64,
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
    // Consume this pristine, preflighted worker once; never reuse a worker that
    // has processed another session's key material or untrusted codec input.
    key_worker: Mutex<Option<native::Worker>>,
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
        transition.reset_screen(epoch);
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
            transition.reset_screen(0);
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
    pub fn publish_screen_state(&self, epoch: u64, suspended: bool) -> Result<Option<u64>> {
        let mut transition = self.transitions.lock().unwrap();
        let Some((serial, changed)) = transition.publish_screen_if_current(
            self.shutdown.load(Ordering::Acquire),
            self.owner.load(Ordering::Acquire),
            epoch,
            suspended,
        )?
        else {
            return Ok(None);
        };
        drop(transition);
        if changed {
            self.wake.signal();
            self.log(&format!(
                "phone screen suspended={suspended} epoch={epoch} visibility_serial={serial}"
            ));
        }
        Ok(Some(serial))
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
    pub fn take_key_worker(&self) -> Result<(native::Worker, bool)> {
        let prepared = self
            .key_worker
            .lock()
            .map_err(|_| anyhow::anyhow!("prepared key-worker state unavailable"))?
            .take();
        match prepared {
            Some(worker) => Ok((worker, true)),
            None => Ok((native::Worker::start()?, false)),
        }
    }
    pub fn log(&self, text: &str) {
        let text: String = text
            .chars()
            .filter(|c| !c.is_control() || *c == ' ')
            .take(2048)
            .collect();
        let line = format!("{:.3} {text}\n", native::now());
        if let Ok(mut log) = self.log.lock() {
            let _ = append_log_line(&mut log, line.as_bytes());
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
    pub fn send_audio(&self, m: Media, canceled: impl Fn() -> bool) -> Result<()> {
        let (epoch, serial) = match &m {
            Media::Audio { epoch, serial, .. } => (*epoch, *serial),
            _ => anyhow::bail!("non-audio message on audio channel"),
        };
        let disconnect = self.disconnect.load(Ordering::Acquire);
        send_bounded_audio(
            &self.audio,
            m,
            || {
                canceled()
                    || self.shutdown.load(Ordering::Acquire)
                    || self.disconnect.load(Ordering::Acquire) != disconnect
                    || self.owner.load(Ordering::Acquire) != epoch
                    || !self.audio_owner.owns(epoch)
                    || self.audio_failed_owner.load(Ordering::Acquire) == epoch
                    || self.flush.load(Ordering::Acquire) != serial
            },
            || self.wake.signal(),
            Duration::from_millis(500),
        )
    }
}
const MAX_LOG_BYTES: u64 = 1024 * 1024;
fn append_log_line(log: &mut (File, u64), line: &[u8]) -> io::Result<()> {
    if line.len() as u64 > MAX_LOG_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "log line exceeds file budget",
        ));
    }
    if log.1 > MAX_LOG_BYTES - line.len() as u64 {
        log.0.set_len(0)?;
        log.0.seek(SeekFrom::Start(0))?;
        log.1 = 0;
    }
    // Reserve the full line before writing so even a partial write cannot cause
    // the next append to underestimate file usage. Refresh the count on failure.
    log.1 += line.len() as u64;
    if let Err(error) = log.0.write_all(line) {
        if let Ok(metadata) = log.0.metadata() {
            log.1 = metadata.len();
        }
        return Err(error);
    }
    Ok(())
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
                "advertised bind must be the selected LAN address"
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
        // FILE_APPEND_DATA-only handles cannot truncate on Windows. One mutex
        // owns this writable handle and its cursor, preserving serialized append.
        let mut log = OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(false)
            .open(data.join("receiver.log"))?;
        let size = log.seek(SeekFrom::End(0))?;
        let worker_started = Instant::now();
        let mut key_worker = native::Worker::start()?;
        ensure!(key_worker.call(4, &[])? == [1], "worker probe");
        key_worker.configure(8)?;
        key_worker.configure(4)?;
        let worker_preflight_ms = worker_started.elapsed().as_secs_f64() * 1000.0;
        let (vtx, video) = sync_channel(8);
        let (atx, audio) = sync_channel(32);
        let shared = Arc::new(Shared {
            shutdown: AtomicBool::new(false),
            disconnect: AtomicU64::new(0),
            flush: AtomicU64::new(0),
            volume_db: AtomicU32::new(0.0f32.to_bits()),
            owner: AtomicU64::new(0),
            audio_owner: AudioOwnership::default(),
            audio_failed_owner: AtomicU64::new(0),
            transitions: Mutex::new(MediaTransitions::default()),
            transition_applied: Condvar::new(),
            owner_started: AtomicU64::new(0),
            playout_ms,
            next: AtomicU64::new(1),
            connections: AtomicUsize::new(0),
            identity,
            video_profile,
            key_worker: Mutex::new(Some(key_worker)),
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
        shared.log(&format!(
            "key worker preflight ready elapsed_ms={worker_preflight_ms:.1}"
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
            "listening address={bind} port={port} discovery={advertise}; no PIN; one LAN peer"
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
                if !audio_can_drain(
                    owner,
                    self.shared.audio_failed_owner.load(Ordering::Acquire),
                    app.audio_ready(),
                ) {
                    audio_drain.block();
                    break;
                }
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
                            self.shared
                                .audio_failed_owner
                                .store(epoch, Ordering::Release);
                            // Preserve the original submission error before any
                            // cleanup call, and stop queued playback immediately.
                            let reason = format!("Audio playback failed. Reconnect mirroring. {e}");
                            app.flush_audio();
                            self.shared.log(&reason);
                            self.shared.failure(
                                "Audio playback stopped. Reconnect mirroring to resume sound.",
                            );
                            self.shared.disconnect.fetch_add(1, Ordering::AcqRel);
                            audio_drain.block();
                            break;
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
                        let prepared = {
                            let transition = self.shared.transitions.lock().unwrap();
                            let Some(screen) = transition
                                .screen_for(self.shared.owner.load(Ordering::Acquire), epoch)
                            else {
                                continue;
                            };
                            app.screen_state(screen.serial, screen.suspended)
                                .and_then(|_| {
                                    app.prepare_video(
                                        codec,
                                        width,
                                        height,
                                        self.shared.video_profile.fps,
                                    )
                                })
                        };
                        if let Err(e) = prepared {
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
                        visibility_serial,
                    }) if epoch == owner && owner != 0 => {
                        video_drain.consumed();
                        video_timing.record(payload_received_at, ready_at, pts, native::now());
                        let submitted = {
                            let transition = self.shared.transitions.lock().unwrap();
                            let Some(screen) = transition
                                .screen_for(self.shared.owner.load(Ordering::Acquire), epoch)
                            else {
                                continue;
                            };
                            // Apply a newly published resume before decoding its
                            // first frame, then keep publication serialized with
                            // submission. Old serials still decode for references.
                            app.screen_state(screen.serial, screen.suspended)
                                .and_then(|_| {
                                    app.video(
                                        &data,
                                        pts,
                                        codec,
                                        (width, height),
                                        rotation_cw_degrees,
                                        visibility_serial,
                                    )
                                })
                        };
                        if let Err(e) = submitted {
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
            // A full native audio queue must wait for WASAPI/presentation events,
            // even when the Rust drain budget or pending-owner slot is occupied.
            if !audio_can_drain(
                owner,
                self.shared.audio_failed_owner.load(Ordering::Acquire),
                app.audio_ready(),
            ) {
                audio_drain.block();
            }
            let backlog_possible = audio_drain.backlog_possible(pending_audio.is_some())
                || video_drain.exhausted()
                || pending_video.is_some()
                || self.shared.owner.load(Ordering::Acquire) != owner;
            if app.wait(pump_wait_ms(discovery_ready, backlog_possible))? == 0 {
                break;
            }
            // Wait never dispatches. Serialize this complete applied boundary
            // against SETUP/FLUSH/teardown publishers, including native tick.
            let mut transition = self.shared.transitions.lock().unwrap();
            let dispatched = transition.apply(|boundary, screen| {
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
                if dispatch_owner != 0 {
                    ensure!(
                        screen.epoch == dispatch_owner,
                        "screen-state owner invariant"
                    );
                    app.screen_state(screen.serial, screen.suspended)?;
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
        {
            // Linearize shutdown with screen-state publication: an in-flight
            // update finishes before shutdown, or observes cancellation unchanged.
            let _transition = self.shared.transitions.lock().unwrap();
            self.shared.shutdown.store(true, Ordering::Release);
        }
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
                if let Err(error) =
                    native::wait_socket_readable(listener.as_raw_socket() as usize, 100)
                {
                    shared.unavailable(&format!(
                        "Receiver listener stopped. Restart AirMirrorLite. {error}"
                    ));
                    break;
                }
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
    struct TestLogPath(std::path::PathBuf);
    impl TestLogPath {
        fn new() -> Self {
            Self(std::env::temp_dir().join(format!(
                "airmirror-bounded-log-{}-{:016x}.tmp",
                std::process::id(),
                rand::random::<u64>()
            )))
        }
    }
    impl Drop for TestLogPath {
        fn drop(&mut self) {
            let _ = fs::remove_file(&self.0);
        }
    }
    #[test]
    fn log_rotation_reclaims_oversized_file_and_rewinds_the_cursor() {
        let path = TestLogPath::new();
        fs::write(&path.0, vec![b'x'; MAX_LOG_BYTES as usize + 27]).unwrap();
        let mut file = OpenOptions::new().write(true).open(&path.0).unwrap();
        let size = file.seek(SeekFrom::End(0)).unwrap();
        let mut log = (file, size);
        append_log_line(&mut log, b"first\n").unwrap();
        assert_eq!(fs::read(&path.0).unwrap(), b"first\n");
        assert_eq!(log.1, 6);
        append_log_line(&mut log, b"second\n").unwrap();
        assert_eq!(fs::read(&path.0).unwrap(), b"first\nsecond\n");
        assert_eq!(log.1, 13);
        // Existing content also survives a normal reopen/append below the cap.
        drop(log);
        let mut file = OpenOptions::new().write(true).open(&path.0).unwrap();
        let size = file.seek(SeekFrom::End(0)).unwrap();
        let mut log = (file, size);
        append_log_line(&mut log, b"third\n").unwrap();
        assert_eq!(fs::read(&path.0).unwrap(), b"first\nsecond\nthird\n");
        append_log_line(&mut log, &vec![b'z'; MAX_LOG_BYTES as usize]).unwrap();
        assert_eq!(log.1, MAX_LOG_BYTES);
        assert_eq!(log.0.metadata().unwrap().len(), MAX_LOG_BYTES);
        append_log_line(&mut log, b"reset\n").unwrap();
        assert_eq!(fs::read(&path.0).unwrap(), b"reset\n");
    }
    #[test]
    fn failed_log_truncation_keeps_the_count_and_existing_file() {
        let path = TestLogPath::new();
        fs::write(&path.0, vec![b'x'; MAX_LOG_BYTES as usize]).unwrap();
        // Reproduce a handle without truncation rights without modifying ACLs.
        let file = File::open(&path.0).unwrap();
        let mut log = (file, MAX_LOG_BYTES);
        assert!(append_log_line(&mut log, b"not appended\n").is_err());
        assert_eq!(log.1, MAX_LOG_BYTES);
        assert_eq!(log.0.metadata().unwrap().len(), MAX_LOG_BYTES);
        assert!(append_log_line(&mut log, &vec![b'z'; MAX_LOG_BYTES as usize + 1]).is_err());
        assert_eq!(log.1, MAX_LOG_BYTES);
    }
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
            .apply(|boundary, _screen| {
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
                .apply::<()>(|_, _| anyhow::bail!("native apply failed"))
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
            visibility_serial: 0,
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
    fn explicit_screen_transitions_are_idempotent_and_preserve_audio() {
        let mut transition = MediaTransitions::default();
        transition.reset_screen(7);
        transition.flush = AudioFlush {
            serial: 19,
            cutoff: Some(protocol::AudioCutoff {
                sequence: 33,
                timestamp: 1024,
            }),
            anchor_after: Some(4.0),
        };
        assert_eq!(transition.publish_screen(7, 7, false).unwrap(), (0, false));
        let revision = transition.revision;
        assert_eq!(transition.publish_screen(7, 7, true).unwrap(), (1, true));
        assert_eq!(transition.publish_screen(7, 7, true).unwrap(), (1, false));
        assert_eq!(transition.publish_screen(7, 7, false).unwrap(), (2, true));
        // Off/on may coalesce before presentation; the serial must still change
        // so native presentation invalidates its old cached picture.
        let mut applied = ScreenVisibility {
            epoch: 7,
            serial: 0,
            suspended: false,
        };
        transition
            .apply(|audio, screen| {
                assert_eq!(audio.serial, 19);
                assert_eq!(
                    audio.cutoff,
                    Some(protocol::AudioCutoff {
                        sequence: 33,
                        timestamp: 1024
                    })
                );
                assert_eq!(audio.anchor_after, Some(4.0));
                assert!(!screen.suspended);
                assert_ne!(screen.serial, applied.serial);
                applied = screen;
                Ok(())
            })
            .unwrap();
        assert_eq!(applied.serial, 2);
        assert_eq!(transition.applied, revision + 2);
        assert_eq!(transition.publish_screen(7, 7, false).unwrap(), (2, false));
        assert_eq!(transition.revision, revision + 2); // Paused pictures stay visible.
    }
    #[test]
    fn screen_visibility_cannot_cross_canceled_or_replaced_owner() {
        let mut transition = MediaTransitions::default();
        transition.reset_screen(7);
        transition.publish_screen(7, 7, true).unwrap();
        transition.reset_screen(0);
        assert!(transition.publish_screen(0, 7, false).is_err());
        assert!(transition.screen_for(0, 7).is_none());
        transition.reset_screen(8);
        assert!(transition.publish_screen(8, 7, false).is_err());
        assert!(transition.screen_for(8, 7).is_none());
        assert_eq!(
            transition.screen_for(8, 8),
            Some(ScreenVisibility {
                epoch: 8,
                serial: 0,
                suspended: false
            })
        );
        transition.screen.serial = u64::MAX;
        let revision = transition.revision;
        assert!(transition.publish_screen(8, 8, true).is_err());
        assert_eq!(transition.screen.serial, u64::MAX);
        assert!(!transition.screen.suspended);
        assert_eq!(transition.revision, revision);
    }
    #[test]
    fn screen_publication_cancels_shutdown_and_replaced_owner_without_mutation() {
        let mut transition = MediaTransitions::default();
        transition.reset_screen(7);
        transition.publish_screen(7, 7, true).unwrap();
        let before = (transition.screen, transition.revision, transition.applied);
        // The valid resume record raced with normal shutdown while epoch7 was
        // still live. No new publication, revision, wake or failure is needed.
        assert_eq!(
            transition
                .publish_screen_if_current(true, 7, 7, false)
                .unwrap(),
            None
        );
        assert_eq!(
            (transition.screen, transition.revision, transition.applied),
            before
        );
        transition.reset_screen(8);
        let before = (transition.screen, transition.revision, transition.applied);
        assert_eq!(
            transition
                .publish_screen_if_current(false, 8, 7, true)
                .unwrap(),
            None
        );
        assert_eq!(
            (transition.screen, transition.revision, transition.applied),
            before
        );
        assert_eq!(
            transition
                .publish_screen_if_current(false, 0, 7, true)
                .unwrap(),
            None
        );
        assert_eq!(
            (transition.screen, transition.revision, transition.applied),
            before
        );
        // Cancellation must not turn genuine live-owner invariants into success.
        assert!(
            transition
                .publish_screen_if_current(false, 7, 7, true)
                .is_err()
        );
        assert!(
            transition
                .publish_screen_if_current(false, 0, 0, true)
                .is_err()
        );
        transition.screen.serial = u64::MAX;
        assert!(
            transition
                .publish_screen_if_current(false, 8, 8, true)
                .is_err()
        );
        assert_eq!(transition.revision, before.1);
        assert!(!transition.screen.suspended);
    }
    #[test]
    fn priority_screen_state_bypasses_full_video_queue_without_losing_decode_references() {
        let (send, receive) = sync_channel(1);
        let old = Media::Video {
            epoch: 7,
            codec: 265,
            width: 994,
            height: 2160,
            data: vec![1, 2, 3],
            pts: 1.0,
            payload_received_at: 0.0,
            ready_at: 0.0,
            rotation_cw_degrees: 0,
            visibility_serial: 0,
        };
        assert!(send.try_send(old).is_ok());
        let gate = Mutex::new(MediaTransitions::default());
        {
            let mut state = gate.lock().unwrap();
            state.reset_screen(7);
            state.publish_screen(7, 7, true).unwrap();
        }
        assert!(
            send.try_send(Media::VideoConfig {
                epoch: 7,
                codec: 265,
                width: 994,
                height: 2160
            })
            .is_err()
        );
        let mut native_screen = ScreenVisibility::default();
        gate.lock()
            .unwrap()
            .apply(|_, screen| {
                native_screen = screen;
                Ok(())
            })
            .unwrap();
        assert!(native_screen.suspended); // No compressed-video queue space needed.
        let Media::Video {
            data,
            visibility_serial,
            ..
        } = receive.try_recv().unwrap()
        else {
            panic!("expected compressed frame")
        };
        assert_eq!(data, [1, 2, 3]); // Still available to decode, despite invisibility.
        assert_ne!(visibility_serial, native_screen.serial);
        gate.lock().unwrap().publish_screen(7, 7, false).unwrap();
        // Submission obtains and applies the current state under the gate before
        // the first fresh keyframe is given to native decoding.
        let state = gate.lock().unwrap();
        native_screen = state.screen_for(7, 7).unwrap();
        assert_eq!(native_screen.serial, 2);
        assert!(!native_screen.suspended);
        assert_eq!(state.screen_for(7, 7).unwrap().serial, 2);
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
    fn synthetic_audio(index: u16) -> Media {
        Media::Audio {
            epoch: 1,
            serial: 11,
            pcm: vec![index as i16, -(index as i16)],
            pts: 100.0 + f64::from(index) / 1000.0,
        }
    }
    fn assert_synthetic_audio(message: Media, expected: u16) {
        let Media::Audio {
            epoch,
            serial,
            pcm,
            pts,
        } = message
        else {
            panic!("expected audio message");
        };
        assert_eq!((epoch, serial), (1, 11));
        assert_eq!(pcm, [expected as i16, -(expected as i16)]);
        assert_eq!(pts, 100.0 + f64::from(expected) / 1000.0);
    }
    #[test]
    fn bounded_audio_burst_preserves_every_packet_and_waits_for_native_space() {
        let (sender, receiver) = sync_channel(32);
        let producer = std::thread::spawn(move || {
            for index in 0..96 {
                send_bounded_audio(
                    &sender,
                    synthetic_audio(index),
                    || false,
                    || {},
                    Duration::from_secs(2),
                )
                .unwrap();
            }
        });
        let mut native = std::collections::VecDeque::with_capacity(32);
        let mut played = 0;
        let mut capacity_stops = 0;
        let started = Instant::now();
        while played < 96 {
            assert!(started.elapsed() < Duration::from_secs(5));
            let mut budget = DrainBudget::new(32);
            for _ in 0..budget.limit {
                if !audio_can_drain(1, 0, native.len() < 32) {
                    budget.block();
                    break;
                }
                let Ok(message) = receiver.try_recv() else {
                    break;
                };
                native.push_back(message);
                budget.consumed();
            }
            if !audio_can_drain(1, 0, native.len() < 32) {
                budget.block();
                capacity_stops += 1;
                // Even a retained pending-owner item cannot turn backpressure
                // into a zero-timeout UI loop. The next engine event resumes it.
                assert_eq!(pump_wait_ms(true, budget.backlog_possible(true)), 1000);
                for _ in 0..8 {
                    assert_synthetic_audio(native.pop_front().unwrap(), played);
                    played += 1;
                }
            } else if producer.is_finished() && budget.received == 0 {
                while let Some(message) = native.pop_front() {
                    assert_synthetic_audio(message, played);
                    played += 1;
                }
            } else {
                std::thread::sleep(Duration::from_millis(1));
            }
            assert!(native.len() <= 32);
        }
        producer.join().unwrap();
        assert!(capacity_stops > 0);
        assert!(receiver.try_recv().is_err());
    }
    #[test]
    fn full_audio_channel_retries_without_mutating_pcm_or_pts() {
        let (sender, receiver) = sync_channel(1);
        assert!(sender.try_send(synthetic_audio(0)).is_ok());
        let wake = Arc::new(AtomicUsize::new(0));
        let notification = wake.clone();
        let producer = std::thread::spawn(move || {
            send_bounded_audio(
                &sender,
                synthetic_audio(1),
                || false,
                || {
                    notification.fetch_add(1, Ordering::AcqRel);
                },
                Duration::from_secs(2),
            )
        });
        let started = Instant::now();
        while wake.load(Ordering::Acquire) == 0 {
            assert!(started.elapsed() < Duration::from_secs(1));
            std::thread::sleep(Duration::from_millis(1));
        }
        assert!(!producer.is_finished());
        assert_synthetic_audio(receiver.recv().unwrap(), 0);
        assert_synthetic_audio(receiver.recv_timeout(Duration::from_secs(1)).unwrap(), 1);
        producer.join().unwrap().unwrap();
        assert_eq!(wake.load(Ordering::Acquire), 2); // One full notification + insertion.
    }
    #[test]
    fn blocked_audio_producer_stops_on_transport_cancel_or_flush_without_ui_wake_flood() {
        for flush in [false, true] {
            let (sender, receiver) = sync_channel(1);
            assert!(sender.try_send(synthetic_audio(0)).is_ok());
            let stop = Arc::new(AtomicBool::new(false));
            let serial = Arc::new(AtomicU64::new(11));
            let wake = Arc::new(AtomicUsize::new(0));
            let (cancel, generation, notification) = (stop.clone(), serial.clone(), wake.clone());
            let producer = std::thread::spawn(move || {
                send_bounded_audio(
                    &sender,
                    synthetic_audio(1),
                    || cancel.load(Ordering::Acquire) || generation.load(Ordering::Acquire) != 11,
                    || {
                        notification.fetch_add(1, Ordering::AcqRel);
                    },
                    Duration::from_secs(2),
                )
            });
            let started = Instant::now();
            while wake.load(Ordering::Acquire) == 0 {
                assert!(started.elapsed() < Duration::from_secs(1));
                std::thread::sleep(Duration::from_millis(1));
            }
            std::thread::sleep(Duration::from_millis(20));
            assert_eq!(wake.load(Ordering::Acquire), 1);
            let canceled = Instant::now();
            if flush {
                serial.store(12, Ordering::Release);
            } else {
                stop.store(true, Ordering::Release);
            }
            producer.join().unwrap().unwrap();
            assert!(canceled.elapsed() < Duration::from_secs(1));
            assert_synthetic_audio(receiver.recv().unwrap(), 0);
            assert!(receiver.try_recv().is_err()); // Only the canceled pending packet was removed.
        }
    }
    #[test]
    fn failed_audio_epoch_cannot_retry_until_a_new_owner() {
        let owner = 7;
        let mut failed_owner = 0;
        let mut attempts = 0;
        for _ in 0..32 {
            if audio_can_drain(owner, failed_owner, true) {
                attempts += 1;
                failed_owner = owner;
            }
        }
        assert_eq!(attempts, 1);
        assert!(!audio_can_drain(owner, failed_owner, false));
        assert!(audio_can_drain(owner + 1, failed_owner, true));
        assert!(audio_can_drain(0, failed_owner, false));
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
