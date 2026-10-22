//! Optional display updates on the legacy plaintext reverse RTSP event channel.
//! This module neither negotiates HAP nor falls back from encrypted input.
//! Wire references: omarroth/doubletake 5e2f1cbe3e1070bcefec8e549c7baf884727bc00,
//! internal/airplay/{event_channel,receiver_media}.go; the observed sender command
//! is a binary plist with type=updateDisplayInfo and one complete display dict.
use ::plist::Value;
use anyhow::{Result, bail, ensure};
use std::{
    collections::BTreeMap,
    io::{self, Read, Write},
    net::{IpAddr, TcpListener, TcpStream},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

const MAX_HEADER: usize = 8192;
const MAX_REPLY_BODY: usize = 16384;
const MAX_COMMAND_BODY: usize = 8192;
const MAX_REJECTED_PEERS: usize = 8;
const IO_POLL: Duration = Duration::from_millis(100);

#[derive(Clone, Copy)]
struct Update {
    revision: u64,
    width: u32,
    height: u32,
    changed_at: Instant,
}

/// One coalesced desired geometry, scoped to one already-authenticated session.
pub struct DisplayUpdates {
    initial: (u32, u32),
    desired: Mutex<Update>,
}
impl DisplayUpdates {
    pub fn new(width: u32, height: u32) -> Result<Self> {
        valid_geometry(width, height)?;
        Ok(Self {
            initial: (width, height),
            desired: Mutex::new(Update {
                revision: 0,
                width,
                height,
                changed_at: Instant::now(),
            }),
        })
    }

    /// Duplicate requests do not extend the quiet period or allocate queue data.
    pub fn request(&self, width: u32, height: u32) -> Result<bool> {
        self.request_at(width, height, Instant::now())
    }

    /// Latest validated display hint, including the baseline before any request.
    pub fn requested_size(&self) -> (u32, u32) {
        self.desired
            .lock()
            .map(|desired| (desired.width, desired.height))
            .unwrap_or(self.initial)
    }

    fn request_at(&self, width: u32, height: u32, now: Instant) -> Result<bool> {
        valid_geometry(width, height)?;
        let mut desired = self
            .desired
            .lock()
            .map_err(|_| anyhow::anyhow!("display-update state unavailable"))?;
        if (desired.width, desired.height) == (width, height) {
            return Ok(false);
        }
        let revision = desired
            .revision
            .checked_add(1)
            .ok_or_else(|| anyhow::anyhow!("display-update revision exhausted"))?;
        *desired = Update {
            revision,
            width,
            height,
            changed_at: now,
        };
        Ok(true)
    }

    fn snapshot(&self) -> Result<Update> {
        Ok(*self
            .desired
            .lock()
            .map_err(|_| anyhow::anyhow!("display-update state unavailable"))?)
    }
}

fn valid_geometry(width: u32, height: u32) -> Result<()> {
    ensure!(
        (48..=3840).contains(&width)
            && (48..=2160).contains(&height)
            && width.is_multiple_of(2)
            && height.is_multiple_of(2),
        "display-update geometry outside even native bounds"
    );
    Ok(())
}

#[derive(Clone, Copy)]
struct Policy {
    connect_timeout: Duration,
    command_timeout: Duration,
    quiet: Duration,
    poll: Duration,
}
impl Default for Policy {
    fn default() -> Self {
        Self {
            connect_timeout: Duration::from_secs(10),
            command_timeout: Duration::from_secs(2),
            quiet: Duration::from_millis(500),
            poll: Duration::from_millis(50),
        }
    }
}

struct Schedule {
    applied: (u32, u32),
    sent_at: Option<Instant>,
}
impl Schedule {
    fn ready(&self, update: Update, now: Instant, quiet: Duration) -> bool {
        update.revision != 0
            && self.applied != (update.width, update.height)
            && now.saturating_duration_since(update.changed_at) >= quiet
            && self
                .sent_at
                .is_none_or(|sent| now.saturating_duration_since(sent) >= quiet)
    }
}

/// Optional failures are logged once and absorbed. They never cancel media.
/// Only call for the existing legacy pairing mode: no encrypted-mode detection,
/// crypto negotiation, plaintext retry, unsolicited baseline or command pipeline.
pub fn run(
    listener: TcpListener,
    peer: IpAddr,
    stop: Arc<AtomicBool>,
    updates: Arc<DisplayUpdates>,
    base_display: Value,
    log: impl Fn(&str),
) -> Result<()> {
    run_with_policy(
        listener,
        peer,
        stop,
        updates,
        base_display,
        log,
        Policy::default(),
    )
}

fn run_with_policy(
    listener: TcpListener,
    peer: IpAddr,
    stop: Arc<AtomicBool>,
    updates: Arc<DisplayUpdates>,
    base_display: Value,
    log: impl Fn(&str),
    policy: Policy,
) -> Result<()> {
    let result = run_channel(listener, peer, &stop, &updates, &base_display, &log, policy);
    if let Err(error) = result
        && !stop.load(Ordering::Acquire)
    {
        // Parser errors never contain remote header/body text. Bound OS errors too.
        let reason: String = error.to_string().chars().take(192).collect();
        log(&format!("optional display updates disabled: {reason}"));
    }
    Ok(())
}

fn run_channel(
    listener: TcpListener,
    peer: IpAddr,
    stop: &AtomicBool,
    updates: &DisplayUpdates,
    base_display: &Value,
    log: &impl Fn(&str),
    policy: Policy,
) -> Result<()> {
    validate_display(base_display)?;
    listener.set_nonblocking(true)?;
    let accepted_by = Instant::now() + policy.connect_timeout;
    let mut rejected = 0;
    let mut stream = loop {
        if stop.load(Ordering::Acquire) {
            return Ok(());
        }
        ensure!(Instant::now() < accepted_by, "event connection deadline");
        match listener.accept() {
            Ok((stream, address)) if address.ip() == peer => break stream,
            Ok(_) => {
                rejected += 1;
                ensure!(
                    rejected <= MAX_REJECTED_PEERS,
                    "event peer-rejection budget"
                );
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(error.into()),
        }
        thread::sleep(policy.poll);
    };
    // Only the negotiated peer's one connection carries commands.
    drop(listener);
    let result = run_updates(&mut stream, stop, updates, base_display, log, policy);
    if let Err(error) = result
        && !stop.load(Ordering::Acquire)
    {
        let reason: String = error.to_string().chars().take(192).collect();
        log(&format!(
            "optional display updates disabled: {reason}; event connection retained"
        ));
        // A malformed reply, timeout or write failure disables this optional
        // feature permanently for the session. Do not send further commands or
        // retry an ambiguous transaction, and do not signal a fatal event-stream
        // disconnect to the phone by closing our accepted socket here.
        while !stop.load(Ordering::Acquire) {
            thread::sleep(policy.poll);
        }
    }
    drop(stream);
    Ok(())
}

fn run_updates(
    stream: &mut TcpStream,
    stop: &AtomicBool,
    updates: &DisplayUpdates,
    base_display: &Value,
    log: &impl Fn(&str),
    policy: Policy,
) -> Result<()> {
    // Restore blocking mode inherited from Winsock accept().
    stream.set_nonblocking(false)?;
    stream.set_read_timeout(Some(IO_POLL))?;
    stream.set_write_timeout(Some(IO_POLL))?;
    stream.set_nodelay(true)?;
    log("display-update event channel connected (legacy plaintext)");
    let mut schedule = Schedule {
        applied: updates.initial,
        sent_at: None,
    };
    let mut cseq = 0u32;
    while !stop.load(Ordering::Acquire) {
        let update = updates.snapshot()?;
        let now = Instant::now();
        if !schedule.ready(update, now, policy.quiet) {
            thread::sleep(policy.poll);
            continue;
        }
        cseq = cseq
            .checked_add(1)
            .ok_or_else(|| anyhow::anyhow!("event CSeq exhausted"))?;
        let command = command(base_display, update, cseq)?;
        let deadline = now + policy.command_timeout;
        schedule.sent_at = Some(now);
        if !write_command(stream, &command, stop, deadline)? {
            return Ok(());
        }
        match read_response(stream, cseq, stop, deadline, log)? {
            Reply::Accepted => {}
            Reply::Stopped => return Ok(()),
            Reply::Declined(status) => {
                bail!("sender declined command status={status}");
            }
        }
        schedule.applied = (update.width, update.height);
        log(&format!(
            "display update acknowledged revision={} geometry={}x{}",
            update.revision, update.width, update.height
        ));
    }
    Ok(())
}

struct LimitedBytes(Vec<u8>);
impl Write for LimitedBytes {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > MAX_COMMAND_BODY - self.0.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "event command budget",
            ));
        }
        self.0.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
fn bounded_plist(value: &Value) -> Result<Vec<u8>> {
    let mut bytes = LimitedBytes(Vec::with_capacity(1024));
    value.to_writer_binary(&mut bytes)?;
    Ok(bytes.0)
}
fn validate_display(display: &Value) -> Result<()> {
    let dictionary = display
        .as_dictionary()
        .ok_or_else(|| anyhow::anyhow!("event base display must be a dictionary"))?;
    ensure!(dictionary.len() <= 32, "event display field budget");
    let uuid = dictionary.get("uuid").and_then(Value::as_string);
    ensure!(
        uuid.is_some_and(|s| !s.is_empty() && s.len() <= 128),
        "event display UUID"
    );
    ensure!(
        dictionary
            .get("rotation")
            .and_then(Value::as_boolean)
            .is_some(),
        "event display rotation must be a support boolean"
    );
    bounded_plist(display)?;
    Ok(())
}
fn command(base: &Value, update: Update, cseq: u32) -> Result<Vec<u8>> {
    valid_geometry(update.width, update.height)?;
    ensure!(cseq != 0, "event CSeq must be positive");
    let mut display = base
        .as_dictionary()
        .ok_or_else(|| anyhow::anyhow!("event base display dictionary"))?
        .clone();
    for (name, dimension) in [
        ("width", update.width),
        ("widthPixels", update.width),
        ("height", update.height),
        ("heightPixels", update.height),
    ] {
        display.insert(name.into(), Value::Integer(dimension.into()));
    }
    let request = super::plist::dict([
        ("type", Value::String("updateDisplayInfo".into())),
        ("displays", Value::Array(vec![Value::Dictionary(display)])),
    ]);
    let body = bounded_plist(&request)?;
    let mut wire = format!(
        "POST /command RTSP/1.0\r\nCSeq: {cseq}\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: {}\r\n\r\n",
        body.len()
    ).into_bytes();
    wire.extend_from_slice(&body);
    Ok(wire)
}

fn transient(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted
    )
}
fn write_command(
    stream: &mut TcpStream,
    mut bytes: &[u8],
    stop: &AtomicBool,
    deadline: Instant,
) -> Result<bool> {
    while !bytes.is_empty() {
        if stop.load(Ordering::Acquire) {
            return Ok(false);
        }
        ensure!(Instant::now() < deadline, "event command write deadline");
        match stream.write(bytes) {
            Ok(0) => bail!("event connection closed during write"),
            Ok(count) => bytes = &bytes[count..],
            Err(error) if transient(&error) => {}
            Err(error) => return Err(error.into()),
        }
    }
    Ok(true)
}

struct ReplyHeader {
    length: usize,
    status: u16,
}
#[derive(Debug, PartialEq, Eq)]
enum Reply {
    Accepted,
    Stopped,
    // Zero denotes a nonzero binary-plist command status, otherwise HTTP status.
    Declined(u16),
}
fn response_header(header: &[u8], expected_cseq: u32, log: &impl Fn(&str)) -> Result<ReplyHeader> {
    ensure!(
        header.len() <= MAX_HEADER && header.ends_with(b"\r\n\r\n") && header.is_ascii(),
        "event response header budget or terminator"
    );
    let text = std::str::from_utf8(header)?;
    let mut lines = text[..text.len() - 4].split("\r\n");
    let status = lines.next().unwrap_or("");
    let mut parts = status.splitn(3, ' ');
    let protocol = parts.next().unwrap_or("");
    ensure!(
        matches!(protocol, "RTSP/1.0" | "HTTP/1.1"),
        "event response protocol"
    );
    let code = parts.next().unwrap_or("");
    ensure!(
        code.len() == 3 && code.bytes().all(|b| b.is_ascii_digit()),
        "event response status syntax"
    );
    let code: u16 = code.parse()?;
    ensure!(
        code == 200 || (400..=599).contains(&code),
        "event response status"
    );
    ensure!(
        !status.bytes().any(|b| b < 32 || b == 127),
        "event response status syntax"
    );
    let mut headers = BTreeMap::new();
    for line in lines {
        ensure!(
            headers.len() < 64 && !line.starts_with([' ', '\t']),
            "event response header count or folding"
        );
        let (name, value) = line
            .split_once(':')
            .ok_or_else(|| anyhow::anyhow!("event response header delimiter"))?;
        ensure!(
            !name.is_empty() && name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-'),
            "event response header name"
        );
        ensure!(
            !value.bytes().any(|b| (b < 32 && b != b'\t') || b == 127),
            "event response header value"
        );
        ensure!(
            headers
                .insert(name.to_ascii_lowercase(), value.trim())
                .is_none(),
            "duplicate event response header"
        );
    }
    ensure!(
        !headers.contains_key("transfer-encoding") && !headers.contains_key("content-encoding"),
        "encoded event response unsupported"
    );
    fn decimal(text: &str) -> Result<u32> {
        ensure!(
            !text.is_empty() && text.len() <= 10 && text.bytes().all(|b| b.is_ascii_digit()),
            "event response numeric field"
        );
        Ok(text.parse()?)
    }
    let length = match headers.get("content-length") {
        Some(value) => decimal(value)? as usize,
        None => 0,
    };
    ensure!(length <= MAX_REPLY_BODY, "event response body budget");
    log(&format!(
        "event response protocol={protocol} status={code} cseq_present={} body_bytes={length} length_present={}",
        headers.contains_key("cseq"),
        headers.contains_key("content-length")
    ));
    match headers.get("cseq") {
        Some(cseq) => ensure!(
            decimal(cseq)? == expected_cseq,
            "event response CSeq mismatch"
        ),
        None => {
            // Observed iPhone event replies use RTSP/1.0 200 without CSeq.
            // Correlate only this serialized event exchange: one outstanding
            // command, no pipeline, no retry after an ambiguous transaction.
            // RTSP without Content-Length has no body (RFC 2326 sections 4.4,
            // 12.14); HTTP still needs explicit framing on this connection.
            ensure!(
                protocol == "RTSP/1.0" || headers.contains_key("content-length"),
                "unframed HTTP event response"
            );
        }
    }
    if length > 0 && code == 200 {
        let kind = headers
            .get("content-type")
            .and_then(|v| v.split(';').next());
        ensure!(
            kind.is_some_and(|v| v
                .trim()
                .eq_ignore_ascii_case("application/x-apple-binary-plist")),
            "event response body type"
        );
    }
    Ok(ReplyHeader {
        length,
        status: code,
    })
}
fn response_body(body: &[u8]) -> Result<Reply> {
    if body.is_empty() {
        return Ok(Reply::Accepted);
    }
    let value = super::plist::decode(body)?;
    let dictionary = value
        .as_dictionary()
        .ok_or_else(|| anyhow::anyhow!("event response body dictionary"))?;
    if let Some(status) = dictionary.get("status") {
        // Apple's command statuses may be negative OSStatus values.
        let integer = status
            .as_signed_integer()
            .map(i128::from)
            .or_else(|| status.as_unsigned_integer().map(i128::from))
            .ok_or_else(|| anyhow::anyhow!("event response command status type"))?;
        if integer != 0 {
            return Ok(Reply::Declined(0));
        }
    }
    Ok(Reply::Accepted)
}
fn read_response(
    stream: &mut TcpStream,
    cseq: u32,
    stop: &AtomicBool,
    deadline: Instant,
    log: &impl Fn(&str),
) -> Result<Reply> {
    let mut received = Vec::with_capacity(MAX_HEADER);
    let mut target = None;
    let mut body_start = 0;
    let mut status = 0;
    loop {
        if stop.load(Ordering::Acquire) {
            return Ok(Reply::Stopped);
        }
        ensure!(Instant::now() < deadline, "event response deadline");
        if target.is_none() {
            if let Some(index) = received.windows(4).position(|w| w == b"\r\n\r\n") {
                body_start = index + 4;
                let header = response_header(&received[..body_start], cseq, log)?;
                target = Some(body_start + header.length);
                status = header.status;
            } else {
                ensure!(received.len() < MAX_HEADER, "event response header budget");
            }
        }
        if let Some(total) = target {
            ensure!(received.len() <= total, "unsolicited event response bytes");
            if received.len() == total {
                return if status == 200 {
                    response_body(&received[body_start..])
                } else {
                    Ok(Reply::Declined(status))
                };
            }
        }
        let mut block = [0u8; 1024];
        let limit = target.unwrap_or(MAX_HEADER) - received.len();
        let capacity = block.len().min(limit);
        match stream.read(&mut block[..capacity]) {
            Ok(0) => bail!("event response connection closed"),
            Ok(count) => received.extend_from_slice(&block[..count]),
            Err(error) if transient(&error) => {}
            Err(error) => return Err(error.into()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{net::SocketAddr, thread::JoinHandle};

    fn base() -> Value {
        super::super::plist::dict([
            ("uuid", Value::String("synthetic-display".into())),
            ("rotation", Value::Boolean(true)),
            ("features", Value::Integer(14.into())),
            ("maxFPS", Value::Integer(60.into())),
            ("width", Value::Integer(994.into())),
            ("height", Value::Integer(2160.into())),
            ("widthPixels", Value::Integer(994.into())),
            ("heightPixels", Value::Integer(2160.into())),
            ("heightPhysical", Value::Integer(0.into())),
        ])
    }
    struct Session {
        address: SocketAddr,
        updates: Arc<DisplayUpdates>,
        stop: Arc<AtomicBool>,
        logs: Arc<Mutex<Vec<String>>>,
        thread: Option<JoinHandle<Result<()>>>,
    }
    impl Session {
        fn start(peer: IpAddr, command_timeout: Duration) -> Self {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let address = listener.local_addr().unwrap();
            let updates = Arc::new(DisplayUpdates::new(994, 2160).unwrap());
            let stop = Arc::new(AtomicBool::new(false));
            let logs = Arc::new(Mutex::new(Vec::new()));
            let (u, s, l) = (updates.clone(), stop.clone(), logs.clone());
            let thread = thread::spawn(move || {
                run_with_policy(
                    listener,
                    peer,
                    s,
                    u,
                    base(),
                    |line| l.lock().unwrap().push(line.into()),
                    Policy {
                        connect_timeout: Duration::from_secs(1),
                        command_timeout,
                        quiet: Duration::from_millis(20),
                        poll: Duration::from_millis(5),
                    },
                )
            });
            Self {
                address,
                updates,
                stop,
                logs,
                thread: Some(thread),
            }
        }
        fn connect(&self) -> TcpStream {
            let stream = TcpStream::connect(self.address).unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(2)))
                .unwrap();
            stream
                .set_write_timeout(Some(Duration::from_secs(2)))
                .unwrap();
            stream
        }
        fn join(&mut self) {
            self.thread.take().unwrap().join().unwrap().unwrap();
        }
        fn wait_for_log(&self, message: &str) {
            let started = Instant::now();
            while !self
                .logs
                .lock()
                .unwrap()
                .iter()
                .any(|line| line.contains(message))
            {
                assert!(
                    started.elapsed() < Duration::from_secs(3),
                    "missing expected test log: {message}"
                );
                thread::sleep(Duration::from_millis(5));
            }
        }
        fn assert_retained(&self, peer: &mut TcpStream) {
            self.wait_for_log("disabled");
            peer.set_read_timeout(Some(Duration::from_millis(150)))
                .unwrap();
            assert!(transient(&peer.read(&mut [0u8; 1]).unwrap_err()));
            assert!(!self.stop.load(Ordering::Acquire));
            assert_eq!(
                self.logs
                    .lock()
                    .unwrap()
                    .iter()
                    .filter(|line| line.contains("disabled"))
                    .count(),
                1
            );
        }
    }
    impl Drop for Session {
        fn drop(&mut self) {
            self.stop.store(true, Ordering::Release);
            if let Some(thread) = self.thread.take() {
                thread.join().unwrap().unwrap();
            }
        }
    }
    fn request(stream: &mut TcpStream) -> super::super::framing::Request {
        super::super::framing::read_request(
            stream,
            &mut Vec::new(),
            || false,
            Duration::from_secs(2),
        )
        .unwrap()
        .unwrap()
    }
    fn acknowledge(stream: &mut TcpStream, cseq: u32) {
        write!(
            stream,
            "RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\nContent-Length: 0\r\n\r\n"
        )
        .unwrap();
    }

    #[test]
    fn coalescing_duplicates_quiet_period_and_rate_limit_are_deterministic() {
        let updates = DisplayUpdates::new(994, 2160).unwrap();
        let start = Instant::now();
        let mut schedule = Schedule {
            applied: updates.initial,
            sent_at: None,
        };
        let quiet = Duration::from_millis(500);
        assert!(!schedule.ready(updates.snapshot().unwrap(), start + quiet, quiet));
        assert!(!updates.request_at(994, 2160, start).unwrap());
        assert!(updates.request_at(2160, 994, start).unwrap());
        assert!(!updates.request_at(2160, 994, start + quiet).unwrap());
        let first = updates.snapshot().unwrap();
        assert_eq!(first.revision, 1);
        assert!(!schedule.ready(first, start + Duration::from_millis(499), quiet));
        assert!(schedule.ready(first, start + quiet, quiet));
        schedule.sent_at = Some(start + quiet);
        schedule.applied = (first.width, first.height);
        updates.request_at(1440, 664, start + quiet).unwrap();
        updates
            .request_at(994, 2160, start + Duration::from_millis(600))
            .unwrap();
        let newest = updates.snapshot().unwrap();
        assert_eq!(
            (newest.revision, newest.width, newest.height),
            (3, 994, 2160)
        );
        assert!(!schedule.ready(newest, start + Duration::from_millis(999), quiet));
        assert!(schedule.ready(newest, start + Duration::from_millis(1100), quiet));
        schedule.sent_at = Some(start + Duration::from_millis(1000));
        assert!(!schedule.ready(newest, start + Duration::from_millis(1100), quiet));
    }

    #[test]
    fn requested_size_tracks_validated_hint_without_an_ack() {
        let updates = DisplayUpdates::new(994, 2160).unwrap();
        assert_eq!(updates.requested_size(), (994, 2160));
        updates.request(2160, 994).unwrap();
        assert_eq!(updates.requested_size(), (2160, 994));
        assert!(updates.request(2160, 995).is_err());
        assert_eq!(updates.requested_size(), (2160, 994));
    }

    #[test]
    fn synthetic_peer_receives_full_coalesced_display_and_sequential_cseq() {
        let session = Session::start("127.0.0.1".parse().unwrap(), Duration::from_secs(2));
        let mut peer = session.connect();
        peer.set_read_timeout(Some(Duration::from_millis(120)))
            .unwrap();
        assert!(peer.read(&mut [0u8; 1]).is_err()); // No unsolicited baseline.
        peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        session.updates.request(2160, 994).unwrap();
        session.updates.request(1440, 664).unwrap();
        session.updates.request(2160, 994).unwrap();
        let first = request(&mut peer);
        assert_eq!(
            (&*first.method, &*first.target, &*first.version),
            ("POST", "/command", "RTSP/1.0")
        );
        assert_eq!(first.headers.get("cseq").map(String::as_str), Some("1"));
        let body = super::super::plist::decode(&first.body).unwrap();
        let dict = body.as_dictionary().unwrap();
        assert_eq!(
            dict.get("type").and_then(Value::as_string),
            Some("updateDisplayInfo")
        );
        let displays = dict.get("displays").unwrap().as_array().unwrap();
        assert_eq!(displays.len(), 1);
        let display = displays[0].as_dictionary().unwrap();
        let mut expected = base().into_dictionary().unwrap();
        for key in ["width", "widthPixels"] {
            expected.insert(key.into(), Value::Integer(2160.into()));
        }
        for key in ["height", "heightPixels"] {
            expected.insert(key.into(), Value::Integer(994.into()));
        }
        assert_eq!(display, &expected);
        // A newer request waits behind the first response, then becomes CSeq 2.
        session.updates.request(994, 2160).unwrap();
        peer.set_read_timeout(Some(Duration::from_millis(120)))
            .unwrap();
        assert!(peer.read(&mut [0u8; 1]).is_err());
        acknowledge(&mut peer, 1);
        peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        assert_eq!(
            request(&mut peer).headers.get("cseq").map(String::as_str),
            Some("2")
        );
        acknowledge(&mut peer, 2);
        assert!(!session.updates.request(994, 2160).unwrap());
    }

    #[test]
    fn http_reply_without_cseq_consumes_body_before_next_command() {
        let session = Session::start("127.0.0.1".parse().unwrap(), Duration::from_secs(2));
        let mut peer = session.connect();
        session.updates.request(2160, 994).unwrap();
        assert_eq!(
            request(&mut peer).headers.get("cseq").map(String::as_str),
            Some("1")
        );
        let body = bounded_plist(&super::super::plist::dict([(
            "status",
            Value::Integer(0.into()),
        )]))
        .unwrap();
        write!(peer,"HTTP/1.1 200 OK\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: {}\r\n\r\n",body.len()).unwrap();
        peer.write_all(&body).unwrap();
        session.updates.request(994, 2160).unwrap();
        assert_eq!(
            request(&mut peer).headers.get("cseq").map(String::as_str),
            Some("2")
        );
        peer.write_all(b"HTTP/1.1 200 OK\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n")
            .unwrap();
        session.wait_for_log("acknowledged revision=2");
        let logs = session.logs.lock().unwrap();
        assert!(
            logs.iter()
                .any(|line| line.contains("protocol=HTTP/1.1 status=200 cseq_present=false"))
        );
        assert!(
            logs.iter()
                .any(|line| line.contains("protocol=HTTP/1.1 status=200 cseq_present=true"))
        );
        assert!(!logs.iter().any(|line| line.contains("disabled")));
    }

    #[test]
    fn observed_rtsp_reply_without_cseq_allows_repeated_display_updates() {
        let session = Session::start("127.0.0.1".parse().unwrap(), Duration::from_secs(2));
        let mut peer = session.connect();
        for (index, (width, height)) in [(2160, 994), (994, 2160), (2160, 994), (994, 2160)]
            .into_iter()
            .enumerate()
        {
            assert!(session.updates.request(width, height).unwrap());
            let packet = request(&mut peer);
            assert_eq!(packet.headers["cseq"], (index + 1).to_string());
            let body = super::super::plist::decode(&packet.body).unwrap();
            let displays = body.as_dictionary().unwrap()["displays"]
                .as_array()
                .unwrap();
            let display = displays[0].as_dictionary().unwrap();
            assert_eq!(display["width"].as_unsigned_integer(), Some(width.into()));
            assert_eq!(display["height"].as_unsigned_integer(), Some(height.into()));
            // Both spellings delimit an empty RTSP reply, without an echoed CSeq.
            let reply = if index % 2 == 0 {
                b"RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n".as_slice()
            } else {
                b"RTSP/1.0 200 OK\r\n\r\n".as_slice()
            };
            peer.write_all(reply).unwrap();
            session.wait_for_log(&format!("acknowledged revision={}", index + 1));
        }
        let logs = session.logs.lock().unwrap();
        assert_eq!(
            logs.iter()
                .filter(|s| s.contains("display update acknowledged"))
                .count(),
            4
        );
        assert!(!logs.iter().any(|s| s.contains("disabled")));
    }

    #[test]
    fn explicit_bad_cseq_keeps_channel_but_disables_updates() {
        for reply in [
            b"RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n".as_slice(),
            b"RTSP/1.0 200 OK\r\nCSeq: invalid\r\nContent-Length: 0\r\n\r\n".as_slice(),
            b"RTSP/1.0 200 OK\r\nCSeq: 1\r\ncseq: 1\r\nContent-Length: 0\r\n\r\n".as_slice(),
            b"HTTP/1.1 200 OK\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n".as_slice(),
            b"HTTP/1.1 200 OK\r\nCSeq: invalid\r\nContent-Length: 0\r\n\r\n".as_slice(),
            b"HTTP/1.1 200 OK\r\nCSeq: 1\r\ncseq: 1\r\nContent-Length: 0\r\n\r\n".as_slice(),
        ] {
            let session = Session::start("127.0.0.1".parse().unwrap(), Duration::from_secs(2));
            let mut peer = session.connect();
            session.updates.request(2160, 994).unwrap();
            request(&mut peer);
            peer.write_all(reply).unwrap();
            session.updates.request(1440, 664).unwrap();
            session.assert_retained(&mut peer);
        }
    }

    #[test]
    fn http_decline_and_negative_command_status_retain_connection() {
        for negative_status in [false, true] {
            let session = Session::start("127.0.0.1".parse().unwrap(), Duration::from_secs(2));
            let mut peer = session.connect();
            session.updates.request(2160, 994).unwrap();
            request(&mut peer);
            if negative_status {
                let body = bounded_plist(&super::super::plist::dict([(
                    "status",
                    Value::Integer((-7116).into()),
                )]))
                .unwrap();
                write!(peer,"HTTP/1.1 200 OK\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: {}\r\n\r\n",body.len()).unwrap();
                peer.write_all(&body).unwrap();
            } else {
                peer.write_all(b"HTTP/1.1 501 Not Implemented\r\nContent-Length: 0\r\n\r\n")
                    .unwrap();
            }
            session.assert_retained(&mut peer);
            assert!(
                session
                    .logs
                    .lock()
                    .unwrap()
                    .iter()
                    .any(|line| line.contains("sender declined command"))
            );
        }
    }

    #[test]
    fn mismatched_cseq_disables_only_optional_channel_once() {
        let session = Session::start("127.0.0.1".parse().unwrap(), Duration::from_secs(2));
        let mut peer = session.connect();
        session.updates.request(2160, 994).unwrap();
        request(&mut peer);
        acknowledge(&mut peer, 9);
        session.assert_retained(&mut peer);
        let logs = session.logs.lock().unwrap();
        assert_eq!(logs.iter().filter(|s| s.contains("disabled")).count(), 1);
        assert!(logs.iter().any(|s| s.contains("CSeq mismatch")));
    }

    #[test]
    fn synthetic_stalled_reply_times_out_without_canceling_media() {
        let session = Session::start("127.0.0.1".parse().unwrap(), Duration::from_millis(250));
        let mut peer = session.connect();
        session.updates.request(2160, 994).unwrap();
        request(&mut peer);
        session.assert_retained(&mut peer);
        // A late reply never restarts commands or becomes the next update's ACK.
        acknowledge(&mut peer, 1);
        session.updates.request(1440, 664).unwrap();
        assert!(transient(&peer.read(&mut [0u8; 1]).unwrap_err()));
        assert!(
            session
                .logs
                .lock()
                .unwrap()
                .iter()
                .any(|s| s.contains("response deadline"))
        );
    }

    #[test]
    fn clean_decline_keeps_event_connection_open_and_stops_updates() {
        let session = Session::start("127.0.0.1".parse().unwrap(), Duration::from_secs(2));
        let mut peer = session.connect();
        session.updates.request(2160, 994).unwrap();
        request(&mut peer);
        peer.write_all(b"RTSP/1.0 501 Not Implemented\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n")
            .unwrap();
        session.updates.request(1440, 664).unwrap();
        session.assert_retained(&mut peer);
        let logs = session.logs.lock().unwrap();
        assert_eq!(
            logs.iter().filter(|line| line.contains("disabled")).count(),
            1
        );
        assert!(logs.iter().any(|line| line.contains("status=501")));
    }

    #[test]
    fn malformed_reply_retains_socket_until_explicit_session_stop() {
        let mut session = Session::start("127.0.0.1".parse().unwrap(), Duration::from_secs(2));
        let mut peer = session.connect();
        session.updates.request(2160, 994).unwrap();
        request(&mut peer);
        peer.write_all(b"RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 999999\r\n\r\n")
            .unwrap();
        session.assert_retained(&mut peer);
        session.updates.request(1440, 664).unwrap();
        let stopped = Instant::now();
        session.stop.store(true, Ordering::Release);
        session.join();
        assert!(stopped.elapsed() < Duration::from_secs(1));
        assert_eq!(peer.read(&mut [0u8; 1]).unwrap(), 0);
    }

    #[test]
    fn stop_interrupts_pending_reply_and_accept_without_failure_log() {
        for connected in [false, true] {
            let mut session = Session::start("127.0.0.1".parse().unwrap(), Duration::from_secs(2));
            let mut peer = connected.then(|| session.connect());
            if let Some(peer) = &mut peer {
                session.updates.request(2160, 994).unwrap();
                request(peer);
            }
            let started = Instant::now();
            session.stop.store(true, Ordering::Release);
            session.join();
            assert!(started.elapsed() < Duration::from_secs(1));
            assert!(
                !session
                    .logs
                    .lock()
                    .unwrap()
                    .iter()
                    .any(|s| s.contains("disabled"))
            );
        }
    }

    #[test]
    fn wrong_peer_is_closed_without_receiving_commands() {
        let session = Session::start("127.0.0.2".parse().unwrap(), Duration::from_secs(2));
        let mut foreign = session.connect();
        session.updates.request(2160, 994).unwrap();
        assert_eq!(foreign.read(&mut [0u8; 1]).unwrap(), 0);
        assert!(!session.stop.load(Ordering::Acquire));
    }

    #[test]
    fn response_framing_rejects_unbounded_ambiguous_and_unsolicited_fields() {
        let valid = b"RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
        assert_eq!(response_header(valid, 1, &|_| {}).unwrap().length, 0);
        assert!(
            response_header(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 1, &|_| {}).is_ok()
        );
        assert!(response_header(b"RTSP/1.0 200 OK\r\n\r\n", 1, &|_| {}).is_ok());
        for length in 0..valid.len() {
            assert!(response_header(&valid[..length], 1, &|_| {}).is_err());
        }
        for bad in [
            "HTTP/1.1 200 OK\r\n\r\n",
            "RTSP/1.0 302 Redirect\r\nCSeq: 1\r\n\r\n",
            "RTSP/1.0 200 OK\r\nCSeq: 2\r\n\r\n",
            "RTSP/1.0 200 OK\r\nCSeq: 1\r\ncseq: 1\r\n\r\n",
            "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: -1\r\n\r\n",
            "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 16385\r\n\r\n",
            "RTSP/1.0 200 OK\r\nCSeq: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
            "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Encoding: gzip\r\n\r\n",
            "RTSP/1.0 200 OK\r\n CSeq: 1\r\n\r\n",
            "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 1\r\n\r\n",
        ] {
            assert!(
                response_header(bad.as_bytes(), 1, &|_| {}).is_err(),
                "{bad}"
            );
        }
        assert!(response_header(&vec![b'a'; MAX_HEADER + 1], 1, &|_| {}).is_err());
        assert!(response_body(b"not a plist").is_err());
        for code in [0, 1, -7116] {
            let body = bounded_plist(&super::super::plist::dict([(
                "status",
                Value::Integer(code.into()),
            )]))
            .unwrap();
            assert_eq!(
                response_body(&body).unwrap(),
                if code == 0 {
                    Reply::Accepted
                } else {
                    Reply::Declined(0)
                }
            );
        }
    }

    #[test]
    fn geometry_and_base_display_budgets_preserve_state_on_rejection() {
        let updates = DisplayUpdates::new(994, 2160).unwrap();
        for (width, height) in [
            (0, 0),
            (47, 48),
            (995, 2160),
            (2160, 995),
            (3842, 2160),
            (2160, 2162),
            (u32::MAX, u32::MAX),
        ] {
            assert!(updates.request(width, height).is_err());
            assert_eq!(updates.snapshot().unwrap().revision, 0);
        }
        assert!(validate_display(&base()).is_ok());
        let mut huge = base().into_dictionary().unwrap();
        huge.insert("extra".into(), Value::String("x".repeat(MAX_COMMAND_BODY)));
        assert!(validate_display(&Value::Dictionary(huge)).is_err());
        let mut invalid = base().into_dictionary().unwrap();
        invalid.insert("rotation".into(), Value::Integer(90.into()));
        assert!(validate_display(&Value::Dictionary(invalid)).is_err());
        updates.desired.lock().unwrap().revision = u64::MAX;
        assert!(updates.request(2160, 994).is_err());
        assert_eq!(updates.snapshot().unwrap().width, 994);
    }
}
