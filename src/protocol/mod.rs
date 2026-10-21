pub mod clock;
pub mod framing;
pub mod h264;
pub mod hevc;
pub mod pairing;
pub mod plist;
mod streams;
use crate::{native, runtime::Shared};
use ::plist::Value;
use anyhow::{Result, bail, ensure};
use plist::{bytes, dict, get, num, number};
use std::{
    io::Write,
    net::TcpStream,
    sync::{Arc, atomic::Ordering},
    time::{Duration, Instant},
};
// Screen mirroring, FairPlay, AAC, legacy pairing and unified /info. Excludes
// photo, HLS/URL playback, ALAC/PCM, metadata, remote-control and AP2 buffering.
pub const FEATURES: u64 = (1 << 2)
    | (1 << 6)
    | (1 << 7)
    | (1 << 9)
    | (1 << 11)
    | (1 << 12)
    | (1 << 14)
    | (1 << 20)
    | (1 << 21)
    | (1 << 27)
    | (1 << 28)
    | (1 << 30);
#[derive(Clone, Copy, Debug)]
pub struct VideoProfile {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub hevc: bool,
}
impl VideoProfile {
    pub fn new(height: u32, hevc_available: bool) -> Result<Self> {
        let width = match height {
            1080 => 1920,
            1440 => 2560,
            2160 => 3840,
            _ => bail!("resolution must be 1080, 1440 or 2160"),
        };
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
    fp: pairing::FairPlay,
    claimed: bool,
}
impl Session {
    fn claim(&mut self) -> Result<()> {
        if self.claimed {
            return Ok(());
        }
        ensure!(
            self.shared
                .owner
                .compare_exchange(0, self.id, Ordering::AcqRel, Ordering::Acquire)
                .is_ok(),
            "receiver busy"
        );
        self.shared
            .owner_started
            .store(native::now().to_bits(), Ordering::Release);
        self.claimed = true;
        self.shared.status("Connecting iPhone");
        Ok(())
    }
}
impl Drop for Session {
    fn drop(&mut self) {
        self.streams.take();
        if self.claimed {
            self.shared
                .owner
                .compare_exchange(self.id, 0, Ordering::AcqRel, Ordering::Acquire)
                .ok();
            self.shared.status("Ready — select AirMirrorLite on iPhone");
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
        ("name", Value::String("AirMirrorLite".into())),
        ("model", Value::String("AppleTV3,2".into())),
        ("sourceVersion", Value::String("220.68".into())),
        ("features", number(s.video_profile.features())),
        ("statusFlags", number(68)),
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
        (
            "displays",
            Value::Array(vec![dict([
                ("uuid", Value::String(s.pi.clone())),
                ("width", number(s.video_profile.width.into())),
                ("height", number(s.video_profile.height.into())),
                ("widthPixels", number(s.video_profile.width.into())),
                ("heightPixels", number(s.video_profile.height.into())),
                ("widthPhysical", number(0)),
                ("heightPhysical", number(0)),
                ("rotation", Value::Boolean(false)),
                (
                    "refreshRate",
                    Value::Real(1.0 / f64::from(s.video_profile.fps)),
                ),
                ("maxFPS", number(s.video_profile.fps.into())),
                ("overscanned", Value::Boolean(false)),
                ("features", number(14)),
            ])]),
        ),
    ])
}
fn respond(
    stream: &mut TcpStream,
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
    Ok(())
}
pub fn serve(mut socket: TcpStream, shared: Arc<Shared>) -> Result<()> {
    framing::configure_accepted_stream(&socket)?;
    let peer = socket.peer_addr()?.ip();
    let local = socket.local_addr()?.ip();
    let id = shared.next.fetch_add(1, Ordering::AcqRel);
    let mut session = Session {
        id,
        shared: shared.clone(),
        streams: None,
        pair: Default::default(),
        fp: Default::default(),
        claimed: false,
    };
    let generation = shared.disconnect.load(Ordering::Acquire);
    let start = Instant::now();
    let mut pending = Vec::new();
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
        let result = (|| -> Result<(Vec<u8>, &str, String, bool)> {
            match (req.method.as_str(),target) {
                ("OPTIONS",_)=>Ok((Vec::new(),"text/plain","Public: OPTIONS, GET, POST, SETUP, RECORD, SET_PARAMETER, GET_PARAMETER, FLUSH, TEARDOWN\r\n".into(),false)),
                ("GET","/info")|("POST","/info")=>Ok((plist::encode(&info(&shared))?,"application/x-apple-binary-plist",String::new(),false)),
                ("POST","/pair-setup")=>{session.claim()?;shared.log("pair-setup");Ok((session.pair.setup(&req.body,&shared.identity)?,"application/octet-stream",String::new(),false))},
                ("POST","/pair-verify")=>{session.claim()?;shared.log("pair-verify");Ok((session.pair.verify(&req.body,&shared.identity)?,"application/octet-stream",String::new(),false))},
                ("POST","/fp-setup")=>{session.claim()?;session.pair.shared()?;shared.log("fp-setup");Ok((session.fp.exchange(&req.body)?,"application/octet-stream",String::new(),false))},
                ("POST","/feedback")=>Ok((Vec::new(),"application/octet-stream",String::new(),false)),
                ("POST","/audioMode")=>{ensure!(session.streams.is_some(),"audio mode before SETUP");validate_audio_mode(&req.body)?;Ok((Vec::new(),"application/octet-stream",String::new(),false))},
                ("SETUP",_)=>{
                    session.claim()?;let shared_key=*session.pair.shared()?;let v=plist::decode(&req.body)?;ensure!(v.as_dictionary().is_some(),"SETUP dictionary");
                    let mut response=::plist::Dictionary::new();
                    if get(&v,"ekey").is_ok(){
                        ensure!(session.streams.is_none(),"duplicate encrypted SETUP");let encrypted=bytes::<72>(&v,"ekey")?;let iv=bytes::<16>(&v,"eiv")?;
                        if let Ok(timing)=get(&v,"timingProtocol"){ensure!(timing.as_string()==Some("NTP"),"only NTP timing supported");}
                        let timing=u16::try_from(num(&v,"timingPort")?)?;
                        let mut worker=native::Worker::start()?;let key=worker.decrypt_key(session.fp.message()?,&encrypted)?;
                        let key=pairing::hash16(&key,&shared_key);
                        let streams=streams::Streams::new(peer,local,timing,key,iv,shared.clone(),id)?;
                        response.insert("timingPort".into(),number(streams.timing_port as u64));response.insert("eventPort".into(),number(0));session.streams=Some(streams);
                    }
                    if let Ok(streams)=get(&v,"streams"){
                        let streams=streams.as_array().ok_or_else(||anyhow::anyhow!("stream list"))?;ensure!(!streams.is_empty()&&streams.len()<=2,"stream count");
                        let media=session.streams.as_mut().ok_or_else(||anyhow::anyhow!("stream SETUP before keys"))?;let mut replies=Vec::new();
                        for s in streams {match num(s,"type")? {
                            110=>{let port=media.start_video(plist::stream_connection_id(s)?)?;replies.push(dict([("type",number(110)),("dataPort",number(port as u64))]));shared.log("video transport ready");}
                            96=>{let codec=u8::try_from(num(s,"ct")?)?;let spf=u16::try_from(num(s,"spf")?)?;let format=num(s,"audioFormat")?;
                                ensure!((codec==8&&format==1<<24)||(codec==4&&format==1<<22),"only stereo 44100Hz AAC-ELD/AAC-LC negotiated");
                                let cport=u16::try_from(num(s,"controlPort")?)?;let(dp,cp)=media.start_audio(codec,spf,cport)?;
                                replies.push(dict([("type",number(96)),("dataPort",number(dp as u64)),("controlPort",number(cp as u64))]));shared.log("audio transport ready");}
                            t=>bail!("unsupported stream type {t}")
                        }}response.insert("streams".into(),Value::Array(replies));
                    }
                    shared.status("Connected — waiting for picture and sound");Ok((plist::encode(&Value::Dictionary(response))?,"application/x-apple-binary-plist",format!("Session: {id}\r\n"),false))
                }
                ("RECORD",_)=>{ensure!(session.streams.is_some(),"RECORD before SETUP");shared.log("RECORD");Ok((Vec::new(),"text/plain",format!("Audio-Latency: {}\r\nSession: {id}\r\n",u64::from(shared.playout_ms)*44100/1000),false))},
                ("GET_PARAMETER",_)=>{ensure!(session.streams.is_some(),"parameter before SETUP");let db=f32::from_bits(shared.volume_db.load(Ordering::Relaxed));Ok((format!("volume: {db:.6}\r\n").into_bytes(),"text/parameters",String::new(),false))},
                ("SET_PARAMETER",_)=>{ensure!(session.streams.is_some(),"parameter before SETUP");ensure!(req.body.len()<=1024,"parameter budget");
                    if req.headers.get("content-type").is_some_and(|s|s=="text/parameters") {for line in std::str::from_utf8(&req.body)?.lines(){if let Some(v)=line.strip_prefix("volume:"){let db=v.trim().parse::<f32>()?;ensure!(db.is_finite()&&(-144.0..=0.0).contains(&db),"volume range");shared.volume_db.store(db.to_bits(),Ordering::Relaxed);}}}
                    Ok((Vec::new(),"text/parameters",String::new(),false))},
                ("FLUSH",_)=>{ensure!(session.streams.is_some(),"flush before SETUP");shared.log("FLUSH requested");shared.flush.fetch_add(1,Ordering::AcqRel);Ok((Vec::new(),"text/plain",String::new(),false))},
                ("TEARDOWN",_)=>{shared.log("TEARDOWN");let types=teardown_types(&req.body)?;let done=types.as_ref().is_none_or(|types|types.contains(&110));
                    if types.as_ref().is_some_and(|types|types.contains(&96)) && let Some(media)=session.streams.as_mut(){media.stop_audio();shared.log("audio stream stopped; screen session retained");}
                    Ok((Vec::new(),"text/plain",String::new(),done))},
                _=>bail!("unsupported endpoint")
            }
        })();
        match result {
            Ok((body, kind, extra, done)) => {
                respond(&mut socket, &req, 200, kind, &body, &extra)?;
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
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn high_resolution_advertisement_is_capability_gated() {
        assert!(VideoProfile::new(2160, false).is_err());
        assert!(VideoProfile::new(1440, false).is_err());
        assert!(VideoProfile::new(4320, true).is_err());
        let high = VideoProfile::new(2160, true).unwrap();
        assert_eq!((high.width, high.height, high.fps), (3840, 2160, 60));
        assert_eq!(high.features() >> 32, 0x400);
        assert!(high.features_text().ends_with(",0x400"));
        assert_eq!(high.with_fps(30).unwrap().fps, 30);
        assert!(high.with_fps(0).is_err());
        let legacy = VideoProfile::new(1080, true).unwrap();
        assert_eq!((legacy.width, legacy.height, legacy.fps), (1920, 1080, 60));
        assert_eq!(legacy.features(), FEATURES);
        assert!(legacy.features_text().ends_with(",0x0"));
        assert_eq!(VideoProfile::new(1440, true).unwrap().width, 2560);
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
    }
}
