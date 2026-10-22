pub mod clock;
pub mod events;
pub mod framing;
pub mod h264;
mod hap;
mod hap_srp;
pub mod hevc;
mod media_crypto;
pub mod orientation;
pub mod pairing;
pub mod plist;
pub mod secure;
mod streams;
use crate::runtime::{ReceiverStatus, Shared};
use ::plist::Value;
use anyhow::{Result, bail, ensure};
use plist::{bytes, dict, get, num, number};
use std::{
    io::Write,
    net::{IpAddr, TcpStream},
    sync::{Arc, atomic::Ordering},
    time::{Duration, Instant},
};
use zeroize::{Zeroize, Zeroizing};

// Authenticated SETUP may contain a plaintext media key (shk). Keep copies in
// the bounded plist tree scoped and wipe their byte storage on every return path.
struct SetupValue(Value);
impl std::ops::Deref for SetupValue {
    type Target = Value;
    fn deref(&self) -> &Value {
        &self.0
    }
}
impl Drop for SetupValue {
    fn drop(&mut self) {
        wipe_setup_data(&mut self.0);
    }
}
fn wipe_setup_data(value: &mut Value) {
    match value {
        Value::Data(bytes) => bytes.zeroize(),
        Value::Array(values) => values.iter_mut().for_each(wipe_setup_data),
        Value::Dictionary(values) => values.values_mut().for_each(wipe_setup_data),
        _ => {}
    }
}
// Screen mirroring, FairPlay, AAC, legacy pairing and unified /info. Excludes
// photo, HLS/URL playback, ALAC/PCM, metadata, remote-control and AP2 buffering.
// Bit 8 + displays.rotation is an opt-in interoperability experiment, not a
// proven Photos orientation fix. Pinned UxPlay records the same AppleTV pairing
// but deliberately leaves it disabled because its behavior is uncertain.
pub const ROTATABLE_DISPLAY: bool = cfg!(feature = "rotation-advertisement-experiment");
pub const HAP_ENABLED: bool = cfg!(feature = "hap-pairing-experiment");
pub const RECEIVER_NAME: &str = if HAP_ENABLED {
    "AirMirrorLite HAP"
} else {
    "AirMirrorLite"
};
pub const FEATURES: u64 = (1 << 2)
    | (1 << 6)
    | (1 << 7)
    | (1 << 9)
    | (1 << 11)
    | (1 << 12)
    | (1 << 14)
    | (1 << 20)
    | (1 << 21)
    | if HAP_ENABLED { 1u64 << 48 } else { 1 << 27 }
    | (1 << 28)
    | (1 << 30)
    | if ROTATABLE_DISPLAY { 1 << 8 } else { 0 };
#[derive(Clone, Copy, Debug)]
pub struct VideoProfile {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub hevc: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AudioCutoff {
    pub sequence: u16,
    pub timestamp: u32,
}
impl VideoProfile {
    fn default_width(height: u32) -> Result<u32> {
        Ok(match height {
            1080 => 1920,
            1440 => 2560,
            2160 => 3840,
            _ => bail!("resolution must be 1080, 1440 or 2160"),
        })
    }
    pub fn new(height: u32, hevc_available: bool) -> Result<Self> {
        let width = Self::default_width(height)?;
        ensure!(
            height == 1080 || hevc_available,
            "higher-resolution mirroring requires a native HEVC decoder and GPU support; use --resolution 1080 for compatibility"
        );
        Ok(Self {
            width,
            height,
            fps: 60,
            hevc: height > 1080,
        })
    }
    pub fn with_fps(mut self, fps: u32) -> Result<Self> {
        ensure!(matches!(fps, 30 | 60), "fps must be 30 or 60");
        self.fps = fps;
        Ok(self)
    }
    pub fn with_display_width(mut self, width: u32) -> Result<Self> {
        let maximum = Self::default_width(self.height)?;
        ensure!(
            (48..=maximum).contains(&width) && width.is_multiple_of(2),
            "display width must be an even number from 48 to {maximum} for this resolution"
        );
        // Advisory display geometry only: no cropping, inferred orientation or
        // change to codec admission limits. The sender chooses its actual stream.
        self.width = width;
        Ok(self)
    }
    pub fn features(self) -> u64 {
        FEATURES | if self.hevc { 1u64 << 42 } else { 0 }
    }
    pub fn features_text(self) -> String {
        let bits = self.features();
        format!("0x{:X},0x{:X}", bits as u32, bits >> 32)
    }
}
struct Session {
    id: u64,
    shared: Arc<Shared>,
    streams: Option<streams::Streams>,
    pair: pairing::Pairing,
    hap: hap::Pairing,
    fp: pairing::FairPlay,
    claimed: bool,
}
impl Session {
    fn teardown_all(&mut self) -> Result<()> {
        // Join producers before releasing ownership, then wait for the media
        // thread's applied reset boundary before acknowledging full TEARDOWN.
        drop(self.streams.take());
        if std::mem::take(&mut self.claimed) {
            self.shared.release_applied(self.id)?;
            self.shared.status(ReceiverStatus::Ready);
        }
        Ok(())
    }
    fn claim(&mut self) -> Result<()> {
        if self.claimed {
            return Ok(());
        }
        self.shared.claim(self.id)?;
        self.claimed = true;
        self.shared
            .status(ReceiverStatus::Connecting("Connecting iPhone"));
        Ok(())
    }
    fn setup(&mut self, body: &[u8], peer: IpAddr, local: IpAddr) -> Result<Vec<u8>> {
        self.claim()?;
        self.require_verified()?;
        let value = SetupValue(plist::decode(body)?);
        ensure!(value.as_dictionary().is_some(), "SETUP dictionary");
        let mut response = ::plist::Dictionary::new();

        if get(&value, "ekey").is_ok() || (HAP_ENABLED && self.streams.is_none()) {
            ensure!(self.streams.is_none(), "duplicate encrypted SETUP");
            if let Ok(timing) = get(&value, "timingProtocol") {
                ensure!(
                    timing.as_string() == Some("NTP"),
                    "only NTP timing supported"
                );
            }
            let timing = u16::try_from(num(&value, "timingPort")?)?;
            ensure!(timing > 0, "NTP timing port required");
            // Complete pairing/FairPlay and encrypted-field checks before taking
            // the pristine startup worker reserved for the first session.
            let security = if HAP_ENABLED {
                // Screen/audio authentication keys derive from the mutually
                // verified HAP session or an explicit authenticated stream key.
                // A supplied FairPlay setup remains validated, never a fallback.
                if get(&value, "ekey").is_ok() {
                    let encrypted = bytes::<72>(&value, "ekey")?;
                    let _iv = bytes::<16>(&value, "eiv")?;
                    let fairplay = self.fp.message()?;
                    let (mut worker, _) = self.shared.take_key_worker()?;
                    let _key = zeroize::Zeroizing::new(worker.decrypt_key(fairplay, &encrypted)?);
                }
                media_crypto::Security::Hap(self.hap.verified()?.clone())
            } else {
                let encrypted = bytes::<72>(&value, "ekey")?;
                let iv = bytes::<16>(&value, "eiv")?;
                let fairplay = self.fp.message()?;
                let worker_started = Instant::now();
                let (mut worker, prepared) = self.shared.take_key_worker()?;
                let worker_acquire_ms = worker_started.elapsed().as_secs_f64() * 1000.0;
                let decrypt_started = Instant::now();
                let raw_key = zeroize::Zeroizing::new(worker.decrypt_key(fairplay, &encrypted)?);
                let decrypt_ms = decrypt_started.elapsed().as_secs_f64() * 1000.0;
                let key = pairing::hash16(raw_key.as_ref(), self.pair.shared()?);
                let shutdown_started = Instant::now();
                drop(worker);
                self.shared.log(&format!("SETUP key worker prepared={prepared} acquire_ms={worker_acquire_ms:.1} decrypt_ms={decrypt_ms:.1} shutdown_ms={:.1}", shutdown_started.elapsed().as_secs_f64() * 1000.0));
                media_crypto::Security::legacy(key, iv)
            };
            let streams =
                streams::Streams::new(peer, local, timing, security, self.shared.clone(), self.id)?;
            response.insert("timingPort".into(), number(streams.timing_port.into()));
            response.insert("eventPort".into(), number(streams.event_port.into()));
            self.streams = Some(streams);
        }

        if let Ok(requested) = get(&value, "streams") {
            let requested = requested
                .as_array()
                .ok_or_else(|| anyhow::anyhow!("stream list"))?;
            ensure!(
                !requested.is_empty() && requested.len() <= 2,
                "stream count"
            );
            let media = self
                .streams
                .as_mut()
                .ok_or_else(|| anyhow::anyhow!("stream SETUP before keys"))?;
            let mut replies = Vec::with_capacity(requested.len());
            for stream in requested {
                match num(stream, "type")? {
                    110 => {
                        let port = media.start_video(plist::stream_connection_id(stream)?)?;
                        replies.push(dict([
                            ("type", number(110)),
                            ("dataPort", number(port.into())),
                        ]));
                        self.shared.log("video transport ready");
                    }
                    96 => {
                        let codec = u8::try_from(num(stream, "ct")?)?;
                        let spf = u16::try_from(num(stream, "spf")?)?;
                        let format = num(stream, "audioFormat")?;
                        ensure!(
                            (codec == 8 && format == 1 << 24) || (codec == 4 && format == 1 << 22),
                            "only stereo 44100Hz AAC-ELD/AAC-LC negotiated"
                        );
                        let control = u16::try_from(num(stream, "controlPort")?)?;
                        let stream_id = if get(stream, "streamConnectionID").is_ok() {
                            Some(plist::stream_connection_id(stream)?)
                        } else {
                            None
                        };
                        let direct_key = if get(stream, "shk").is_ok() {
                            Some(bytes::<32>(stream, "shk")?)
                        } else {
                            None
                        };
                        let (data_port, control_port) =
                            media.start_audio(codec, spf, control, stream_id, direct_key)?;
                        replies.push(dict([
                            ("type", number(96)),
                            ("dataPort", number(data_port.into())),
                            ("controlPort", number(control_port.into())),
                        ]));
                        self.shared.log("audio transport ready");
                    }
                    kind => bail!("unsupported stream type {kind}"),
                }
            }
            response.insert("streams".into(), Value::Array(replies));
        }
        self.shared.status(ReceiverStatus::Connecting(
            "Connected — waiting for picture and sound",
        ));
        plist::encode(&Value::Dictionary(response))
    }
    fn require_verified(&self) -> Result<()> {
        if HAP_ENABLED {
            self.hap.verified()?;
        } else {
            self.pair.shared()?;
        }
        Ok(())
    }
}
impl Drop for Session {
    fn drop(&mut self) {
        self.streams.take();
        if self.claimed {
            self.shared.release(self.id);
            self.shared.status(ReceiverStatus::Ready);
        }
    }
}
fn info(s: &Shared) -> Value {
    let formats = (1 << 22) | (1 << 24);
    let aud = |kind| {
        dict([
            ("type", number(kind)),
            ("audioInputFormats", number(formats)),
            ("audioOutputFormats", number(formats)),
        ])
    };
    let latency = |kind| {
        dict([
            ("type", number(kind)),
            ("audioType", Value::String("default".into())),
            ("inputLatencyMicros", number(0)),
            (
                "outputLatencyMicros",
                number(u64::from(s.playout_ms) * 1000),
            ),
        ])
    };
    dict([
        ("deviceID", Value::String(s.device.clone())),
        ("macAddress", Value::String(s.device.clone())),
        ("name", Value::String(RECEIVER_NAME.into())),
        ("model", Value::String("AppleTV3,2".into())),
        ("sourceVersion", Value::String("220.68".into())),
        ("features", number(s.video_profile.features())),
        ("statusFlags", number(if HAP_ENABLED { 4 } else { 68 })),
        (
            "pk",
            Value::Data(s.identity.verifying_key().as_bytes().to_vec()),
        ),
        ("pi", Value::String(s.pi.clone())),
        ("vv", number(2)),
        ("keepAliveLowPower", Value::Boolean(true)),
        ("keepAliveSendStatsAsBody", Value::Boolean(true)),
        ("initialVolume", Value::Real(0.0)),
        ("audioFormats", Value::Array(vec![aud(100), aud(101)])),
        (
            "audioLatencies",
            Value::Array(vec![latency(100), latency(101)]),
        ),
        ("displays", Value::Array(vec![display_description(s)])),
    ])
}

pub(crate) fn display_description(s: &Shared) -> Value {
    let (width, height) = s.display_geometry();
    dict([
        ("uuid", Value::String(s.pi.clone())),
        ("width", number(width.into())),
        ("height", number(height.into())),
        ("widthPixels", number(width.into())),
        ("heightPixels", number(height.into())),
        ("widthPhysical", number(0)),
        ("heightPhysical", number(0)),
        ("rotation", Value::Boolean(ROTATABLE_DISPLAY)),
        (
            "refreshRate",
            Value::Real(1.0 / f64::from(s.video_profile.fps)),
        ),
        ("maxFPS", number(s.video_profile.fps.into())),
        ("overscanned", Value::Boolean(false)),
        ("features", number(14)),
    ])
}
fn respond(
    stream: &mut impl Write,
    req: &framing::Request,
    code: u16,
    content_type: &str,
    body: &[u8],
    extra: &str,
) -> Result<()> {
    let reason = match code {
        200 => "OK",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        453 => "Not Enough Bandwidth",
        455 => "Method Not Valid in This State",
        _ => "Error",
    };
    let cseq = req
        .headers
        .get("cseq")
        .map(|s| format!("CSeq: {s}\r\n"))
        .unwrap_or_default();
    let header = format!(
        "{} {code} {reason}\r\n{cseq}Server: AirTunes/220.68\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\n{extra}\r\n",
        req.version,
        body.len()
    );
    stream.write_all(header.as_bytes())?;
    stream.write_all(body)?;
    stream.flush()?;
    Ok(())
}
pub fn serve(socket: TcpStream, shared: Arc<Shared>) -> Result<()> {
    framing::configure_accepted_stream(&socket)?;
    let peer = socket.peer_addr()?.ip();
    let local = socket.local_addr()?.ip();
    let mut socket = secure::ControlStream::new(socket);
    let id = shared.next.fetch_add(1, Ordering::AcqRel);
    let mut session = Session {
        id,
        shared: shared.clone(),
        streams: None,
        pair: Default::default(),
        hap: Default::default(),
        fp: Default::default(),
        claimed: false,
    };
    let generation = shared.disconnect.load(Ordering::Acquire);
    let start = Instant::now();
    let mut pending = Zeroizing::new(Vec::new());
    let mut request_count = 0;
    let mut rate = Instant::now();
    loop {
        if shared.shutdown.load(Ordering::Acquire)
            || generation != shared.disconnect.load(Ordering::Acquire)
            || session
                .streams
                .as_ref()
                .is_some_and(|s| s.stop.load(Ordering::Acquire))
        {
            break;
        }
        ensure!(
            session.streams.is_some() || start.elapsed() < Duration::from_secs(30),
            "negotiation deadline"
        );
        let Some(req) = framing::read_request(
            &mut socket,
            &mut pending,
            || {
                shared.shutdown.load(Ordering::Acquire)
                    || generation != shared.disconnect.load(Ordering::Acquire)
                    || session
                        .streams
                        .as_ref()
                        .is_some_and(|s| s.stop.load(Ordering::Acquire))
            },
            Duration::from_secs(if session.streams.is_some() { 90 } else { 30 }),
        )?
        else {
            break;
        };
        if rate.elapsed() >= Duration::from_secs(1) {
            request_count = 0;
            rate = Instant::now();
        }
        request_count += 1;
        ensure!(request_count <= 64, "control rate limit");
        // Log endpoint names from an allowlist only. Never log supplied names, plists or secrets.
        let target = req.target.split('?').next().unwrap_or("");
        let label = match (req.method.as_str(), target) {
            ("GET", "/info") | ("POST", "/info") => "info",
            ("OPTIONS", _) => "options",
            ("POST", "/pair-setup") => "pair-setup",
            ("POST", "/pair-verify") => "pair-verify",
            ("POST", "/fp-setup") => "fp-setup",
            ("POST", "/feedback") => "feedback",
            ("POST", "/audioMode") => "audio-mode",
            ("SETUP", _) => "setup",
            ("RECORD", _) => "record",
            ("SET_PARAMETER", _) => "set-parameter",
            ("GET_PARAMETER", _) => "get-parameter",
            ("FLUSH", _) => "flush",
            ("TEARDOWN", _) => "teardown",
            _ => "unsupported",
        };
        shared.log(&format!("request {label} body_bytes={}", req.body.len()));
        if label == "unsupported" {
            respond(
                &mut socket,
                &req,
                404,
                "text/plain",
                b"Unsupported endpoint\n",
                "",
            )?;
            continue;
        }
        let mut upgrade = None;
        let result = (|| -> Result<(Vec<u8>, &str, String, bool)> {
            match (req.method.as_str(), target) {
                ("OPTIONS", _) => Ok((
                    Vec::new(),
                    "text/plain",
                    "Public: OPTIONS, GET, POST, SETUP, RECORD, SET_PARAMETER, GET_PARAMETER, FLUSH, TEARDOWN\r\n".into(),
                    false,
                )),
                ("GET", "/info") | ("POST", "/info") => Ok((
                    plist::encode(&info(&shared))?,
                    "application/x-apple-binary-plist",
                    String::new(),
                    false,
                )),
                ("POST", "/pair-setup") => {
                    session.claim()?;
                    shared.log("pair-setup");
                    if HAP_ENABLED {
                        ensure!(req.headers.get("x-apple-hkp").is_none_or(|kind| kind == "4"), "unsupported HAP pairing type");
                        let started = Instant::now();
                        let (response, secret) = session.hap.setup(&req.body)?;
                        if let Some(secret) = secret { upgrade = Some(secure::RecordKeys::control_receiver(secret.bytes())?); }
                        shared.log(&format!("HAP setup proof_verified={} elapsed_ms={:.1}", upgrade.is_some(), started.elapsed().as_secs_f64() * 1000.0));
                        return Ok((response, "application/pairing+tlv8", String::new(), false));
                    }
                    Ok((
                        session.pair.setup(&req.body, &shared.identity)?,
                        "application/octet-stream", String::new(), false,
                    ))
                }
                ("POST", "/pair-verify") => {
                    ensure!(!HAP_ENABLED, "legacy pairing is disabled in the HAP candidate");
                    session.claim()?;
                    shared.log("pair-verify");
                    Ok((
                        session.pair.verify(&req.body, &shared.identity)?,
                        "application/octet-stream", String::new(), false,
                    ))
                }
                ("POST", "/fp-setup") => {
                    session.claim()?;
                    session.require_verified()?;
                    shared.log("fp-setup");
                    Ok((session.fp.exchange(&req.body)?, "application/octet-stream", String::new(), false))
                }
                ("POST", "/feedback") => Ok((Vec::new(), "application/octet-stream", String::new(), false)),
                ("POST", "/audioMode") => {
                    ensure!(session.streams.is_some(), "audio mode before SETUP");
                    validate_audio_mode(&req.body)?;
                    Ok((Vec::new(), "application/octet-stream", String::new(), false))
                }
                ("SETUP", _) => Ok((
                    session.setup(&req.body, peer, local)?,
                    "application/x-apple-binary-plist",
                    format!("Session: {id}\r\n"),
                    false,
                )),
                ("RECORD", _) => {
                    ensure!(session.streams.is_some(), "RECORD before SETUP");
                    shared.log("RECORD");
                    let latency = u64::from(shared.playout_ms) * 44100 / 1000;
                    Ok((Vec::new(), "text/plain", format!("Audio-Latency: {latency}\r\nSession: {id}\r\n"), false))
                }
                ("GET_PARAMETER", _) => {
                    ensure!(session.streams.is_some(), "parameter before SETUP");
                    let db = f32::from_bits(shared.volume_db.load(Ordering::Relaxed));
                    Ok((format!("volume: {db:.6}\r\n").into_bytes(), "text/parameters", String::new(), false))
                }
                ("SET_PARAMETER", _) => {
                    ensure!(session.streams.is_some(), "parameter before SETUP");
                    ensure!(req.body.len() <= 1024, "parameter budget");
                    if req.headers.get("content-type").is_some_and(|s| s == "text/parameters") {
                        for line in std::str::from_utf8(&req.body)?.lines() {
                            if let Some(value) = line.strip_prefix("volume:") {
                                let db = value.trim().parse::<f32>()?;
                                ensure!(db.is_finite() && (-144.0..=0.0).contains(&db), "volume range");
                                shared.volume_db.store(db.to_bits(), Ordering::Relaxed);
                            }
                        }
                    }
                    Ok((Vec::new(), "text/parameters", String::new(), false))
                }
                ("FLUSH", _) => {
                    ensure!(session.streams.is_some(), "flush before SETUP");
                    shared.log("FLUSH requested");
                    if session.streams.as_ref().is_some_and(|streams| streams.audio_ports.is_none()) {
                        // No audio transport exists to flush or fence. Do not let
                        // a screen-only notification poison a later audio SETUP.
                        return Ok((Vec::new(), "text/plain", String::new(), false));
                    }
                    let cutoff = parse_flush_cutoff(req.headers.get("rtp-info").map(String::as_str));
                    let cutoff = match cutoff {
                        Ok(cutoff) => cutoff,
                        Err(error) => {
                            shared.failure("Audio flush lacked a valid RTP-Info boundary. Reconnect mirroring to resume sound.");
                            return Err(error);
                        }
                    };
                    shared.request_flush(id, cutoff)?;
                    Ok((Vec::new(), "text/plain", String::new(), false))
                }
                ("TEARDOWN", _) => {
                    shared.log("TEARDOWN");
                    let plan = teardown_plan(&req.body)?;
                    match plan {
                        TeardownPlan::All => session.teardown_all()?,
                        TeardownPlan::Audio => {
                            if let Some(media) = session.streams.as_mut() {
                                media.stop_audio()?;
                                shared.log("audio stream stopped; screen session retained");
                            }
                        }
                    }
                    Ok((Vec::new(), "text/plain", String::new(), plan == TeardownPlan::All))
                }
                _ => bail!("unsupported endpoint"),
            }
        })();
        match result {
            Ok((body, kind, extra, done)) => {
                respond(&mut socket, &req, 200, kind, &body, &extra)?;
                if let Some(keys) = upgrade {
                    socket.upgrade(keys, std::mem::take(&mut *pending))?;
                    shared.log("HAP control encryption active; verified transient session");
                }
                if done {
                    break;
                }
            }
            Err(e) => {
                let busy = shared.owner.load(Ordering::Acquire) != 0 && !session.claimed;
                respond(
                    &mut socket,
                    &req,
                    if busy { 453 } else { 455 },
                    "text/plain",
                    b"Request rejected\n",
                    "",
                )?;
                return Err(e);
            }
        }
    }
    Ok(())
}
fn validate_audio_mode(body: &[u8]) -> Result<()> {
    ensure!(body.len() <= 512, "audio mode budget");
    let v = plist::decode(body)?;
    ensure!(
        get(&v, "audioMode")?.as_string() == Some("default"),
        "unsupported audio mode"
    );
    Ok(())
}
fn parse_flush_cutoff(value: Option<&str>) -> Result<AudioCutoff> {
    let value =
        value.ok_or_else(|| anyhow::anyhow!("FLUSH requires RTP-Info; reconnect mirroring"))?;
    ensure!(
        !value.is_empty() && value.len() <= 96,
        "FLUSH RTP-Info budget"
    );
    let (mut sequence, mut timestamp) = (None, None);
    for field in value.split(';') {
        let (name, value) = field
            .trim()
            .split_once('=')
            .ok_or_else(|| anyhow::anyhow!("FLUSH RTP-Info field"))?;
        let value = value.trim();
        ensure!(
            !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit()),
            "FLUSH RTP-Info number"
        );
        match name.trim() {
            "seq" => {
                ensure!(sequence.is_none(), "duplicate FLUSH sequence");
                sequence = Some(value.parse::<u16>()?);
            }
            "rtptime" => {
                ensure!(timestamp.is_none(), "duplicate FLUSH timestamp");
                timestamp = Some(value.parse::<u32>()?);
            }
            _ => bail!("unsupported FLUSH RTP-Info field"),
        }
    }
    Ok(AudioCutoff {
        sequence: sequence.ok_or_else(|| anyhow::anyhow!("FLUSH sequence missing"))?,
        timestamp: timestamp.ok_or_else(|| anyhow::anyhow!("FLUSH timestamp missing"))?,
    })
}
fn teardown_types(body: &[u8]) -> Result<Option<Vec<u64>>> {
    if body.is_empty() {
        return Ok(None);
    }
    let v = plist::decode(body)?;
    let Ok(streams) = get(&v, "streams") else {
        return Ok(None);
    };
    let streams = streams
        .as_array()
        .ok_or_else(|| anyhow::anyhow!("teardown streams"))?;
    ensure!(!streams.is_empty() && streams.len() <= 2, "teardown count");
    let mut types = Vec::new();
    for s in streams {
        let t = num(s, "type")?;
        ensure!(
            matches!(t, 96 | 110) && !types.contains(&t),
            "teardown type"
        );
        types.push(t);
    }
    Ok(Some(types))
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TeardownPlan {
    All,
    Audio,
}
fn teardown_plan(body: &[u8]) -> Result<TeardownPlan> {
    let types = teardown_types(body)?;
    Ok(if types.as_ref().is_none_or(|types| types.contains(&110)) {
        TeardownPlan::All
    } else {
        TeardownPlan::Audio
    })
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn flush_requires_two_bounded_unambiguous_rtp_cutoffs() {
        let maximum = AudioCutoff {
            sequence: u16::MAX,
            timestamp: u32::MAX,
        };
        assert_eq!(
            parse_flush_cutoff(Some("seq=65535;rtptime=4294967295")).unwrap(),
            maximum
        );
        assert_eq!(
            parse_flush_cutoff(Some("rtptime=4294967295; seq=65535")).unwrap(),
            maximum
        );
        assert_eq!(
            parse_flush_cutoff(Some("seq=0;rtptime=0")).unwrap(),
            AudioCutoff {
                sequence: 0,
                timestamp: 0
            }
        );
        assert!(parse_flush_cutoff(None).is_err());
        for value in [
            "",
            "seq=1",
            "rtptime=1",
            "seq=1;rtptime=1;seq=2",
            "seq=1;rtptime=1;url=x",
            "seq=-1;rtptime=1",
            "seq=65536;rtptime=1",
            "seq=1;rtptime=4294967296",
            "seq=+1;rtptime=1",
            "seq=1;rtptime=1;",
        ] {
            assert!(parse_flush_cutoff(Some(value)).is_err(), "{value}");
        }
        assert!(parse_flush_cutoff(Some(&"0".repeat(97))).is_err());
    }
    #[test]
    fn high_resolution_advertisement_is_capability_gated() {
        assert!(VideoProfile::new(2160, false).is_err());
        assert!(VideoProfile::new(1440, false).is_err());
        assert!(VideoProfile::new(4320, true).is_err());
        let high = VideoProfile::new(2160, true).unwrap();
        assert_eq!((high.width, high.height, high.fps), (3840, 2160, 60));
        let pairing_upper = if HAP_ENABLED { 0x10000 } else { 0 };
        assert_eq!(high.features() >> 32, pairing_upper | 0x400);
        assert!(
            high.features_text()
                .ends_with(&format!(",0x{:X}", pairing_upper | 0x400))
        );
        assert_eq!(high.with_fps(30).unwrap().fps, 30);
        assert!(high.with_fps(0).is_err());
        let legacy = VideoProfile::new(1080, true).unwrap();
        assert_eq!((legacy.width, legacy.height, legacy.fps), (1920, 1080, 60));
        assert_eq!(legacy.features(), FEATURES);
        assert!(
            legacy
                .features_text()
                .ends_with(&format!(",0x{pairing_upper:X}"))
        );
        assert_eq!(VideoProfile::new(1440, true).unwrap().width, 2560);
    }
    #[test]
    fn optional_display_width_preserves_profile_and_native_limits() {
        let high = VideoProfile::new(2160, true).unwrap().with_fps(30).unwrap();
        let portrait = high.with_display_width(994).unwrap();
        assert_eq!(
            (portrait.width, portrait.height, portrait.fps),
            (994, 2160, 30)
        );
        assert!(portrait.hevc);
        assert_eq!(portrait.features(), high.features());
        assert_eq!(high.width, 3840);
        for (height, maximum) in [(1080, 1920), (1440, 2560), (2160, 3840)] {
            let profile = VideoProfile::new(height, true).unwrap();
            for width in [48, 994, maximum] {
                let narrowed = profile.with_display_width(width).unwrap();
                assert_eq!(narrowed.width, width);
                assert_eq!(narrowed.height, profile.height);
                assert_eq!(narrowed.fps, profile.fps);
                assert_eq!(narrowed.hevc, profile.hevc);
            }
            for width in [0, 46, 47, 49, maximum + 2, u32::MAX] {
                assert!(profile.with_display_width(width).is_err());
            }
            // Replacing the hint is still bounded by this resolution's original
            // default width, rather than the previously selected advisory width.
            assert_eq!(
                profile
                    .with_display_width(994)
                    .unwrap()
                    .with_display_width(maximum)
                    .unwrap()
                    .width,
                maximum
            );
        }
    }
    #[test]
    fn default_audio_mode_notification() {
        let body = plist::encode(&dict([("audioMode", Value::String("default".into()))])).unwrap();
        assert_eq!(body.len(), 64);
        validate_audio_mode(&body).unwrap();
        assert!(validate_audio_mode(&body[..63]).is_err());
    }
    #[test]
    fn partial_audio_teardown() {
        let body = plist::encode(&dict([(
            "streams",
            Value::Array(vec![dict([("type", number(96))])]),
        )]))
        .unwrap();
        assert_eq!(teardown_types(&body).unwrap(), Some(vec![96]));
        assert_eq!(teardown_types(&[]).unwrap(), None);
        assert_eq!(teardown_plan(&body).unwrap(), TeardownPlan::Audio);
    }
    #[test]
    fn full_teardown_always_requires_applied_session_release_before_ack() {
        assert_eq!(teardown_plan(&[]).unwrap(), TeardownPlan::All);
        for types in [vec![110], vec![96, 110], vec![110, 96]] {
            let body = plist::encode(&dict([(
                "streams",
                Value::Array(
                    types
                        .into_iter()
                        .map(|kind| dict([("type", number(kind))]))
                        .collect(),
                ),
            )]))
            .unwrap();
            assert_eq!(teardown_plan(&body).unwrap(), TeardownPlan::All);
        }
    }
}
