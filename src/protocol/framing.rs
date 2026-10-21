use anyhow::{Result, bail, ensure};
use std::{
    collections::BTreeMap,
    io::Read,
    net::TcpStream,
    time::{Duration, Instant},
};
pub const MAX_BODY: usize = 64 * 1024;
pub const MAX_HEADER: usize = 8192;
#[derive(Debug)]
pub struct Request {
    pub method: String,
    pub target: String,
    pub version: String,
    pub headers: BTreeMap<String, String>,
    pub body: Vec<u8>,
}
pub fn parse_header(data: &[u8]) -> Result<(Request, usize)> {
    ensure!(
        data.len() <= MAX_HEADER && data.ends_with(b"\r\n\r\n"),
        "header length or terminator"
    );
    let text = std::str::from_utf8(data)?;
    let mut lines = text.split("\r\n");
    let parts: Vec<_> = lines.next().unwrap_or("").split(' ').collect();
    ensure!(
        parts.len() == 3 && matches!(parts[2], "RTSP/1.0" | "HTTP/1.1"),
        "request line"
    );
    ensure!(
        parts[0].len() <= 32 && parts[1].len() <= 1024 && !parts[1].bytes().any(|b| b < 32),
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
        ensure!(!v.bytes().any(|c| c < 32 && c != b'\t'), "header value");
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
            cseq.len() <= 10 && cseq.bytes().all(|b| b.is_ascii_digit()),
            "CSeq syntax"
        );
    }
    Ok((
        Request {
            method: parts[0].into(),
            target: parts[1].into(),
            version: parts[2].into(),
            headers,
            body: Vec::new(),
        },
        len,
    ))
}
pub fn read_request(
    stream: &mut TcpStream,
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
            pending.drain(..end + len);
            return Ok(Some(req));
        }
        ensure!(pending.len() < MAX_HEADER, "header too large");
        let mut block = [0u8; 2048];
        match stream.read(&mut block) {
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
fn read_until(
    stream: &mut TcpStream,
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
        let mut block = [0u8; 4096];
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
}
