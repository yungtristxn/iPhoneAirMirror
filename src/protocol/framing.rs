use anyhow::{Result, bail, ensure};
use std::{
    collections::BTreeMap,
    io::Read,
    net::TcpStream,
    time::{Duration, Instant},
};
use zeroize::{Zeroize, Zeroizing};
pub const MAX_BODY: usize = 64 * 1024;
pub const MAX_HEADER: usize = 8192;
/// Winsock accept() inherits nonblocking mode from our polling listeners.
/// Restore blocking I/O so idle reads wait in the kernel; timeouts still let
/// shutdown and record deadlines be checked promptly.
pub fn configure_accepted_stream(stream: &TcpStream) -> Result<()> {
    stream.set_nonblocking(false)?;
    stream.set_read_timeout(Some(Duration::from_millis(100)))?;
    stream.set_write_timeout(Some(Duration::from_secs(2)))?;
    stream.set_nodelay(true)?;
    Ok(())
}
#[derive(Debug)]
pub struct Request {
    pub method: String,
    pub target: String,
    pub version: String,
    pub headers: BTreeMap<String, String>,
    pub body: Vec<u8>,
}
impl Drop for Request {
    fn drop(&mut self) {
        self.body.zeroize();
    }
}
pub fn parse_header(data: &[u8]) -> Result<(Request, usize)> {
    ensure!(
        data.len() <= MAX_HEADER && data.ends_with(b"\r\n\r\n"),
        "header length or terminator"
    );
    let text = std::str::from_utf8(data)?;
    let mut lines = text.split("\r\n");
    let (method, rest) = lines
        .next()
        .unwrap_or("")
        .split_once(' ')
        .ok_or_else(|| anyhow::anyhow!("request line"))?;
    let (target, version) = rest
        .split_once(' ')
        .ok_or_else(|| anyhow::anyhow!("request line"))?;
    ensure!(matches!(version, "RTSP/1.0" | "HTTP/1.1"), "request line");
    ensure!(
        !method.is_empty()
            && method.len() <= 32
            && method
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"!#$%&'*+-.^_`|~".contains(&b))
            && !target.is_empty()
            && target.len() <= 1024
            && !target.bytes().any(|b| b < 32 || b == 127),
        "request fields"
    );
    let mut headers = BTreeMap::new();
    for line in lines {
        if line.is_empty() {
            continue;
        }
        ensure!(
            headers.len() < 64 && !line.starts_with([' ', '\t']),
            "header count or folding"
        );
        let (k, v) = line
            .split_once(':')
            .ok_or_else(|| anyhow::anyhow!("header delimiter"))?;
        ensure!(
            !k.is_empty() && k.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-'),
            "header name"
        );
        ensure!(
            !v.bytes().any(|c| (c < 32 && c != b'\t') || c == 127),
            "header value"
        );
        ensure!(
            headers
                .insert(k.to_ascii_lowercase(), v.trim().to_string())
                .is_none(),
            "duplicate header"
        );
    }
    ensure!(
        !headers.contains_key("transfer-encoding"),
        "transfer encoding unsupported"
    );
    let len = if let Some(s) = headers.get("content-length") {
        ensure!(
            !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()),
            "content length syntax"
        );
        s.parse::<usize>()?
    } else {
        0
    };
    ensure!(len <= MAX_BODY, "body too large");
    if let Some(cseq) = headers.get("cseq") {
        ensure!(
            !cseq.is_empty() && cseq.len() <= 10 && cseq.bytes().all(|b| b.is_ascii_digit()),
            "CSeq syntax"
        );
    }
    Ok((
        Request {
            method: method.into(),
            target: target.into(),
            version: version.into(),
            headers,
            body: Vec::new(),
        },
        len,
    ))
}
pub fn read_request(
    stream: &mut impl Read,
    pending: &mut Vec<u8>,
    cancelled: impl Fn() -> bool,
    idle: Duration,
) -> Result<Option<Request>> {
    let mut start = None;
    let entered = Instant::now();
    loop {
        if cancelled() {
            return Ok(None);
        }
        ensure!(entered.elapsed() < idle, "control idle deadline");
        if let Some(end) = pending.windows(4).position(|w| w == b"\r\n\r\n") {
            let end = end + 4;
            let (mut req, len) = parse_header(&pending[..end])?;
            read_until(
                stream,
                pending,
                end + len,
                &cancelled,
                start.unwrap_or_else(Instant::now),
            )?;
            req.body = pending[end..end + len].to_vec();
            let remaining = compact_pending(pending, end + len)?;
            pending.truncate(remaining);
            return Ok(Some(req));
        }
        ensure!(pending.len() < MAX_HEADER, "header too large");
        let mut block = Zeroizing::new([0u8; 2048]);
        match stream.read(&mut block[..]) {
            Ok(0) if pending.is_empty() => return Ok(None),
            Ok(0) => bail!("truncated request"),
            Ok(n) => {
                start.get_or_insert_with(Instant::now);
                pending.extend_from_slice(&block[..n]);
            }
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) => {}
            Err(e) => return Err(e.into()),
        }
        if let Some(s) = start {
            ensure!(s.elapsed() < Duration::from_secs(5), "slow request");
        }
    }
}
/// Clear the consumed plaintext and the old tail before shortening the buffer.
/// The caller owns the remaining pipelined bytes and the returned request copy.
fn compact_pending(pending: &mut [u8], consumed: usize) -> Result<usize> {
    ensure!(consumed <= pending.len(), "request compaction boundary");
    let remaining = pending.len() - consumed;
    pending[..consumed].zeroize();
    pending.copy_within(consumed.., 0);
    pending[remaining..].zeroize();
    Ok(remaining)
}
fn read_until(
    stream: &mut impl Read,
    pending: &mut Vec<u8>,
    len: usize,
    cancelled: &impl Fn() -> bool,
    start: Instant,
) -> Result<()> {
    while pending.len() < len {
        ensure!(
            !cancelled() && start.elapsed() < Duration::from_secs(5),
            "body deadline"
        );
        let mut block = Zeroizing::new([0u8; 4096]);
        let max = block.len().min(len - pending.len());
        match stream.read(&mut block[..max]) {
            Ok(0) => bail!("truncated body"),
            Ok(n) => pending.extend_from_slice(&block[..n]),
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) => {}
            Err(e) => return Err(e.into()),
        }
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn accepted_stream_waits_instead_of_spinning() {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let client = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (mut accepted, _) = listener.accept().unwrap();
        accepted.set_nonblocking(true).unwrap();
        configure_accepted_stream(&accepted).unwrap();
        let start = Instant::now();
        let error = accepted.read(&mut [0; 1]).unwrap_err();
        assert!(matches!(
            error.kind(),
            std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
        ));
        assert!(
            start.elapsed() >= Duration::from_millis(50),
            "idle receive must block rather than busy-poll"
        );
        drop(client);
    }
    #[test]
    fn rejects_ambiguous_and_unbounded_framing() {
        for s in [
            "Content-Length: -1",
            "Content-Length: 65537",
            "Content-Length: 2\r\nContent-Length: 2",
            "Transfer-Encoding: chunked",
            "Content-Length: 18446744073709551616",
            "CSeq: 1\r\ninjected",
        ] {
            assert!(
                parse_header(format!("POST /info RTSP/1.0\r\n{s}\r\n\r\n").as_bytes()).is_err()
            );
        }
    }
    #[test]
    fn valid_empty_and_bounded_body() {
        assert_eq!(
            parse_header(b"POST /fp-setup RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 164\r\n\r\n")
                .unwrap()
                .1,
            164
        );
    }

    #[test]
    fn rejects_empty_fields_controls_and_overlong_sequences() {
        for line in [
            " /info RTSP/1.0",
            "POST  RTSP/1.0",
            "PO\tST /info RTSP/1.0",
            "POST /in\u{7f}fo RTSP/1.0",
            "POST /info RTSP/1.0 extra",
        ] {
            assert!(parse_header(format!("{line}\r\n\r\n").as_bytes()).is_err());
        }
        for field in ["CSeq:", "CSeq: 10000000000", "CSeq: -1", "X-Test: a\u{7f}b"] {
            assert!(
                parse_header(format!("POST /info RTSP/1.0\r\n{field}\r\n\r\n").as_bytes()).is_err()
            );
        }
        let (request, length) = parse_header(
            b"SET_PARAMETER rtsp://127.0.0.1/session RTSP/1.0\r\nCSeq: 9999999999\r\n\r\n",
        )
        .unwrap();
        assert_eq!(request.method, "SET_PARAMETER");
        assert_eq!(request.target, "rtsp://127.0.0.1/session");
        assert_eq!(length, 0);
    }

    #[test]
    fn compaction_preserves_overlap_and_wipes_every_retired_byte() {
        let original = b"consumed-body-and-next-request";
        for consumed in [0, 1, 4, 20, original.len()] {
            let mut pending = original.to_vec();
            let remaining = compact_pending(&mut pending, consumed).unwrap();
            assert_eq!(&pending[..remaining], &original[consumed..]);
            // Check the retired region before truncate makes it inaccessible.
            assert!(pending[remaining..].iter().all(|&byte| byte == 0));
        }
        let mut pending = original.to_vec();
        assert!(compact_pending(&mut pending, original.len() + 1).is_err());
        assert_eq!(pending, original);
    }

    #[test]
    fn pipelined_requests_keep_owned_bodies_after_pending_is_cleared() {
        let first = b"POST /pair-setup RTSP/1.0\r\nContent-Length: 6\r\n\r\nsecret";
        let second = b"POST /info RTSP/1.0\r\nContent-Length: 4\r\n\r\nnext";
        let mut input = std::io::Cursor::new([first.as_slice(), second.as_slice()].concat());
        let mut pending = Vec::new();
        let first = read_request(&mut input, &mut pending, || false, Duration::from_secs(1))
            .unwrap()
            .unwrap();
        assert_eq!(first.body, b"secret");
        assert_eq!(pending, second);
        let second = read_request(&mut input, &mut pending, || false, Duration::from_secs(1))
            .unwrap()
            .unwrap();
        assert!(pending.is_empty());
        assert_eq!(first.body, b"secret");
        assert_eq!(second.body, b"next");
        assert_eq!(second.target, "/info");
    }
}
