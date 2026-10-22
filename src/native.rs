use anyhow::{Context, Result, bail, ensure};
use std::{
    ffi::{CStr, CString, c_char, c_void},
    ptr::NonNull,
};

unsafe extern "C" {
    fn am_error() -> *const c_char;
    fn am_lan_ipv4(
        requested: *const u8,
        address: *mut u8,
        index: *mut u32,
        prefix: *mut u32,
    ) -> i32;
    fn am_show_error(message: *const c_char);
    fn am_now() -> f64;
    fn am_wait_socket_readable(socket: usize, timeout_ms: u32) -> i32;
    fn am_open(hidden: i32) -> *mut c_void;
    fn am_video_probe(app: *mut c_void, validate: extern "C" fn(*const u8, u32, u32) -> i32)
    -> i32;
    fn am_ui_probe(app: *mut c_void, directory: *const c_char) -> i32;
    fn am_close(app: *mut c_void);
    fn am_wake_handle(app: *mut c_void) -> *mut c_void;
    fn am_signal(handle: *mut c_void);
    fn am_signal_close(handle: *mut c_void);
    fn am_flush_audio(app: *mut c_void);
    fn am_title(app: *mut c_void, title: *const c_char);
    fn am_pump(app: *mut c_void, wait_ms: u32) -> i32;
    fn am_wait(app: *mut c_void, wait_ms: u32) -> i32;
    fn am_dispatch(app: *mut c_void) -> i32;
    fn am_status(app: *mut c_void, kind: u32, detail: *const c_char);
    fn am_status_dismissed(app: *mut c_void) -> i32;
    fn am_prepare_video(app: *mut c_void, codec: u32, width: u32, height: u32, fps: u32) -> i32;
    fn am_presented(app: *mut c_void) -> u64;
    fn am_presented_size(app: *mut c_void) -> u64;
    fn am_discovery_status(discovery: *mut c_void) -> i32;
    fn am_hevc_available(app: *mut c_void) -> i32;
    fn am_video(
        app: *mut c_void,
        data: *const u8,
        len: u32,
        pts: f64,
        codec: u32,
        width: u32,
        height: u32,
        rotation_cw_degrees: u32,
    ) -> i32;
    fn am_video_ready(app: *mut c_void) -> i32;
    fn am_video_sync(app: *mut c_void, synchronized: i32);
    fn am_audio(app: *mut c_void, data: *const i16, samples: u32, pts: f64) -> i32;
    fn am_reset(app: *mut c_void) -> i32;
    fn am_stats(app: *mut c_void) -> *const c_char;
    fn am_discover(
        name: *const c_char,
        host: *const c_char,
        port: u16,
        address: *const u8,
        interface_index: u32,
        keys: *const *const c_char,
        values: *const *const c_char,
        count: u32,
    ) -> *mut c_void;
    fn am_undiscover(discovery: *mut c_void);
    fn am_protect(input: *const u8, len: u32, output: *mut u8, cap: u32, decrypt: i32) -> i32;
    fn am_is_sandboxed() -> i32;
    fn am_worker_start() -> *mut c_void;
    fn am_worker_provision() -> i32;
    fn am_worker_close(worker: *mut c_void);
    fn am_worker_metrics(worker: *mut c_void, private_bytes: *mut u64, cpu_ms: *mut f64) -> i32;
    fn am_worker_call(
        worker: *mut c_void,
        op: u32,
        input: *const u8,
        len: u32,
        output: *mut u8,
        cap: u32,
    ) -> i32;
    fn am_aac_open(codec: u32) -> *mut c_void;
    fn am_aac_backend() -> *const c_char;
    fn am_aac_last_error(decoder: *mut c_void) -> *const c_char;
    fn am_aac_delay(decoder: *mut c_void) -> i32;
    fn am_aac_close(decoder: *mut c_void);
    fn am_aac_decode_timed(
        decoder: *mut c_void,
        data: *const u8,
        len: u32,
        out: *mut i16,
        cap: u32,
        additional_delay: *mut u32,
    ) -> i32;
    fn playfair_decrypt(message: *const u8, key: *const u8, out: *mut u8);
}
pub fn error() -> anyhow::Error {
    anyhow::anyhow!(
        unsafe { CStr::from_ptr(am_error()) }
            .to_string_lossy()
            .into_owned()
    )
}
pub fn show_error(message: &str) {
    if let Ok(s) = CString::new(message) {
        unsafe { am_show_error(s.as_ptr()) }
    }
}
pub fn now() -> f64 {
    unsafe { am_now() }
}

/// Wait without taking ownership; the caller must keep the socket open and
/// check its cancellation flag between waits. Readiness may be EOF or accept.
pub fn wait_socket_readable(socket: usize, timeout_ms: u32) -> Result<bool> {
    ensure!(
        timeout_ms <= 1000,
        "socket readiness timeout exceeds 1000 ms"
    );
    match unsafe { am_wait_socket_readable(socket, timeout_ms) } {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(error()),
    }
}

// Test-only route: native encoders produce synthetic Annex-B pictures in memory.
// Validate those same NALs through the receiver's length-prefixed input parser
// before the native probe submits them to Media Foundation. No phone data enters
// this callback, and normal receiver submission does not pay for this conversion.
fn validate_probe_packet(bytes: &[u8], codec: u32) -> Result<()> {
    let budget = match codec {
        264 => crate::protocol::h264::MAX_FRAME,
        265 => crate::protocol::hevc::MAX_FRAME,
        _ => bail!("unsupported synthetic video codec"),
    };
    ensure!(
        !bytes.is_empty() && bytes.len() <= budget,
        "synthetic packet budget"
    );
    let marker = |at: usize| -> usize {
        if bytes.get(at..at + 4) == Some(&[0, 0, 0, 1]) {
            4
        } else if bytes.get(at..at + 3) == Some(&[0, 0, 1]) {
            3
        } else {
            0
        }
    };
    let mut framed = Vec::with_capacity(bytes.len());
    let mut at = 0;
    let mut count = 0;
    while at < bytes.len() {
        let prefix = marker(at);
        ensure!(prefix != 0, "synthetic Annex-B framing");
        let start = at + prefix;
        let mut end = start;
        while end < bytes.len() && marker(end) == 0 {
            end += 1;
        }
        ensure!(
            end > start && count < 4096 && framed.len() + 4 + end - start <= budget,
            "synthetic NAL budget"
        );
        framed.extend_from_slice(&((end - start) as u32).to_be_bytes());
        framed.extend_from_slice(&bytes[start..end]);
        at = end;
        count += 1;
    }
    if codec == 265 {
        crate::protocol::hevc::annex_b(&mut framed)?;
    } else {
        crate::protocol::h264::annex_b(&mut framed)?;
    }
    Ok(())
}

extern "C" fn validate_probe_video(data: *const u8, len: u32, codec: u32) -> i32 {
    if data.is_null() || len == 0 || len as usize > crate::protocol::hevc::MAX_FRAME {
        return 0;
    }
    // The native probe owns a locked IMFMediaBuffer for this synchronous call.
    let bytes = unsafe { std::slice::from_raw_parts(data, len as usize) };
    match std::panic::catch_unwind(|| validate_probe_packet(bytes, codec)) {
        Ok(Ok(())) => 1,
        Ok(Err(error)) => {
            use std::io::Write;
            let _ = writeln!(
                std::io::stderr().lock(),
                "Synthetic video parser: {error:#}"
            );
            0
        }
        Err(_) => 0,
    }
}
pub fn lan_ipv4(requested: Option<std::net::Ipv4Addr>) -> Result<(std::net::Ipv4Addr, u32, u32)> {
    let mut address = [0; 4];
    let (mut index, mut prefix) = (0, 0);
    let requested = requested.map(|value| value.octets());
    if unsafe {
        am_lan_ipv4(
            requested.as_ref().map_or(std::ptr::null(), |v| v.as_ptr()),
            address.as_mut_ptr(),
            &mut index,
            &mut prefix,
        )
    } == 0
    {
        return Err(error());
    }
    Ok((address.into(), index, prefix))
}
pub struct App(NonNull<c_void>);
pub struct Wake(NonNull<c_void>);
// A duplicated kernel event handle permits SetEvent concurrently; ownership closes it once.
unsafe impl Send for Wake {}
unsafe impl Sync for Wake {}
impl Wake {
    pub fn signal(&self) {
        unsafe { am_signal(self.0.as_ptr()) }
    }
}
impl Drop for Wake {
    fn drop(&mut self) {
        unsafe { am_signal_close(self.0.as_ptr()) }
    }
}
impl App {
    pub fn status(&self, kind: u32, detail: &str) {
        if let Ok(text) = CString::new(detail) {
            unsafe { am_status(self.0.as_ptr(), kind, text.as_ptr()) }
        }
    }
    pub fn take_status_dismissed(&self) -> bool {
        unsafe { am_status_dismissed(self.0.as_ptr()) != 0 }
    }
    pub fn wait(&self, timeout: u32) -> Result<i32> {
        let result = unsafe { am_wait(self.0.as_ptr(), timeout) };
        if result < 0 { Err(error()) } else { Ok(result) }
    }
    pub fn dispatch(&self) -> Result<i32> {
        let result = unsafe { am_dispatch(self.0.as_ptr()) };
        if result < 0 { Err(error()) } else { Ok(result) }
    }
    pub fn video_sync(&self, synchronized: bool) {
        unsafe { am_video_sync(self.0.as_ptr(), synchronized.into()) }
    }
    pub fn ui_probe(&self, directory: &str) -> Result<()> {
        let path = CString::new(directory)?;
        if unsafe { am_ui_probe(self.0.as_ptr(), path.as_ptr()) } == 0 {
            return Err(error());
        }
        Ok(())
    }
    pub fn prepare_video(&self, codec: u32, width: u32, height: u32, fps: u32) -> Result<()> {
        if unsafe { am_prepare_video(self.0.as_ptr(), codec, width, height, fps) } == 0 {
            return Err(error());
        }
        Ok(())
    }
    pub fn presented(&self) -> u64 {
        unsafe { am_presented(self.0.as_ptr()) }
    }
    pub fn presented_size(&self) -> (u32, u32) {
        let size = unsafe { am_presented_size(self.0.as_ptr()) };
        ((size >> 32) as u32, size as u32)
    }
    pub fn hevc_available(&self) -> bool {
        unsafe { am_hevc_available(self.0.as_ptr()) != 0 }
    }
    pub fn video_ready(&self) -> bool {
        unsafe { am_video_ready(self.0.as_ptr()) != 0 }
    }
    pub fn video_probe(&self) -> Result<()> {
        if unsafe { am_video_probe(self.0.as_ptr(), validate_probe_video) } == 0 {
            return Err(error());
        }
        Ok(())
    }
    pub fn waker(&self) -> Result<Wake> {
        NonNull::new(unsafe { am_wake_handle(self.0.as_ptr()) })
            .map(Wake)
            .ok_or_else(error)
    }
    pub fn flush_audio(&self) {
        unsafe { am_flush_audio(self.0.as_ptr()) }
    }
    pub fn open(hidden: bool) -> Result<Self> {
        NonNull::new(unsafe { am_open(hidden.into()) })
            .map(Self)
            .ok_or_else(error)
    }
    pub fn title(&self, title: &str) {
        if let Ok(s) = CString::new(title) {
            unsafe { am_title(self.0.as_ptr(), s.as_ptr()) }
        }
    }
    pub fn pump(&self, timeout: u32) -> Result<i32> {
        let r = unsafe { am_pump(self.0.as_ptr(), timeout) };
        if r < 0 { Err(error()) } else { Ok(r) }
    }
    pub fn video(
        &self,
        data: &[u8],
        pts: f64,
        codec: u32,
        width: u32,
        height: u32,
        rotation_cw_degrees: u32,
    ) -> Result<()> {
        ensure!(
            matches!(codec, 264 | 265)
                && !data.is_empty()
                && data.len()
                    <= if codec == 265 {
                        4 * 1024 * 1024
                    } else {
                        2 * 1024 * 1024
                    }
                && pts.is_finite()
                && matches!(rotation_cw_degrees, 0 | 90 | 180 | 270),
            "invalid video sample"
        );
        if unsafe {
            am_video(
                self.0.as_ptr(),
                data.as_ptr(),
                data.len() as u32,
                pts,
                codec,
                width,
                height,
                rotation_cw_degrees,
            )
        } == 0
        {
            return Err(error());
        }
        Ok(())
    }
    pub fn audio(&self, pcm: &[i16], pts: f64) -> Result<()> {
        ensure!(
            !pcm.is_empty() && pcm.len() <= 4096 && pcm.len().is_multiple_of(2) && pts.is_finite(),
            "invalid audio sample"
        );
        if unsafe { am_audio(self.0.as_ptr(), pcm.as_ptr(), pcm.len() as u32, pts) } == 0 {
            return Err(error());
        }
        Ok(())
    }
    pub fn reset(&self) -> Result<()> {
        if unsafe { am_reset(self.0.as_ptr()) } == 0 {
            return Err(error());
        }
        Ok(())
    }
    pub fn stats(&self) -> String {
        unsafe { CStr::from_ptr(am_stats(self.0.as_ptr())) }
            .to_string_lossy()
            .into_owned()
    }
}
impl Drop for App {
    fn drop(&mut self) {
        unsafe { am_close(self.0.as_ptr()) }
    }
}
pub struct Discovery(NonNull<c_void>);
impl Discovery {
    pub fn ready(&self) -> Result<bool> {
        match unsafe { am_discovery_status(self.0.as_ptr()) } {
            1 => Ok(true),
            0 => Ok(false),
            _ => Err(error()),
        }
    }
    pub fn register(
        name: &str,
        host: &str,
        port: u16,
        address: std::net::Ipv4Addr,
        interface_index: u32,
        txt: &[(&str, String)],
    ) -> Result<Self> {
        let name = CString::new(name)?;
        let host = CString::new(host)?;
        let keys: Vec<_> = txt
            .iter()
            .map(|(k, _)| CString::new(*k))
            .collect::<std::result::Result<_, _>>()?;
        let vals: Vec<_> = txt
            .iter()
            .map(|(_, v)| CString::new(v.as_str()))
            .collect::<std::result::Result<_, _>>()?;
        let kp: Vec<_> = keys.iter().map(|s| s.as_ptr()).collect();
        let vp: Vec<_> = vals.iter().map(|s| s.as_ptr()).collect();
        NonNull::new(unsafe {
            am_discover(
                name.as_ptr(),
                host.as_ptr(),
                port,
                address.octets().as_ptr(),
                interface_index,
                kp.as_ptr(),
                vp.as_ptr(),
                kp.len() as u32,
            )
        })
        .map(Self)
        .ok_or_else(error)
    }
}
impl Drop for Discovery {
    fn drop(&mut self) {
        unsafe { am_undiscover(self.0.as_ptr()) }
    }
}
pub fn protect(input: &[u8], decrypt: bool) -> Result<Vec<u8>> {
    ensure!(input.len() <= 4096, "identity file too large");
    let mut out = vec![0u8; 8192];
    let len = unsafe {
        am_protect(
            input.as_ptr(),
            input.len() as u32,
            out.as_mut_ptr(),
            out.len() as u32,
            decrypt.into(),
        )
    };
    ensure!(len >= 0, "Windows DPAPI identity operation failed");
    out.truncate(len as usize);
    Ok(out)
}
pub struct Worker {
    handle: NonNull<c_void>,
    reply: Box<[u8; 16384]>,
    generation: u64,
    samples_per_frame: usize,
}
// SAFETY: the native object owns only movable Win32 process/job/pipe/event
// handles, with no COM apartment or thread-bound state. Each call completes or
// cancels and joins its overlapped I/O before returning. Calls require &mut self,
// and the reply buffer moves with its sole owner. This does not implement Sync.
unsafe impl Send for Worker {}
pub struct DecodedAudio {
    pub pcm: Vec<i16>,
    pub additional_delay_samples: u32,
    pub generation: u64,
}

fn decoded_reply(out: &[u8], generation: u64, samples_per_frame: usize) -> Result<DecodedAudio> {
    ensure!(
        generation != 0 && matches!(samples_per_frame, 480 | 1024),
        "AAC decoder not configured"
    );
    ensure!(
        out.len() == 12 + samples_per_frame * 4,
        "AAC decode failed or invalid PCM count"
    );
    ensure!(
        u64::from_le_bytes(out[..8].try_into()?) == generation,
        "AAC stale decoder generation"
    );
    let delay = u32::from_le_bytes(out[8..12].try_into()?);
    ensure!(delay <= 4096, "AAC additional delay budget");
    Ok(DecodedAudio {
        pcm: out[12..]
            .chunks_exact(2)
            .map(|x| i16::from_le_bytes([x[0], x[1]]))
            .collect(),
        additional_delay_samples: delay,
        generation,
    })
}
pub fn provision_worker() -> Result<()> {
    if unsafe { am_worker_provision() } == 0 {
        return Err(error());
    }
    Ok(())
}
pub fn aac_backend() -> String {
    unsafe { CStr::from_ptr(am_aac_backend()) }
        .to_string_lossy()
        .into_owned()
}
impl Worker {
    pub fn start() -> Result<Self> {
        NonNull::new(unsafe { am_worker_start() })
            .map(|handle| Self {
                handle,
                reply: Box::new([0; 16384]),
                generation: 0,
                samples_per_frame: 0,
            })
            .ok_or_else(error)
    }
    pub fn call(&mut self, op: u32, input: &[u8]) -> Result<&[u8]> {
        ensure!(input.len() <= 8192, "worker request too large");
        let n = unsafe {
            am_worker_call(
                self.handle.as_ptr(),
                op,
                input.as_ptr(),
                input.len() as u32,
                self.reply.as_mut_ptr(),
                self.reply.len() as u32,
            )
        };
        ensure!(
            n >= 0 && n as usize <= self.reply.len(),
            "isolated codec worker failed or timed out"
        );
        Ok(&self.reply[..n as usize])
    }
    pub fn decrypt_key(&mut self, message: &[u8; 164], key: &[u8; 72]) -> Result<[u8; 16]> {
        ensure!(message[12] < 4, "invalid FairPlay key mode");
        let mut input = message.to_vec();
        input.extend_from_slice(key);
        self.call(1, &input)?
            .try_into()
            .map_err(|_| anyhow::anyhow!("invalid worker key reply"))
    }
    pub fn configure(&mut self, codec: u8) -> Result<()> {
        self.generation = 0;
        self.samples_per_frame = 0;
        let generation = u64::from_le_bytes(
            self.call(2, &[codec])?
                .try_into()
                .context("AAC decoder initialization failed")?,
        );
        ensure!(
            generation != 0 && matches!(codec, 4 | 8),
            "AAC configuration reply"
        );
        self.generation = generation;
        self.samples_per_frame = if codec == 8 { 480 } else { 1024 };
        Ok(())
    }
    pub fn generation(&self) -> u64 {
        self.generation
    }
    fn metrics(&self) -> Result<(u64, f64)> {
        let (mut bytes, mut cpu) = (0, 0.0);
        ensure!(
            unsafe { am_worker_metrics(self.handle.as_ptr(), &mut bytes, &mut cpu) } != 0,
            "could not sample worker metrics"
        );
        Ok((bytes, cpu))
    }
    pub fn private_bytes(&self) -> Result<u64> {
        Ok(self.metrics()?.0)
    }
    pub fn cpu_time_ms(&self) -> Result<f64> {
        Ok(self.metrics()?.1)
    }
    pub fn decoder_error(&mut self) -> Result<String> {
        let reply = self.call(6, &[])?;
        ensure!(reply.len() <= 256, "decoder diagnostic budget");
        Ok(std::str::from_utf8(reply)?.to_owned())
    }
    pub fn decoder_delay(&mut self) -> Result<Option<u32>> {
        let bytes: [u8; 4] = self
            .call(7, &[])?
            .try_into()
            .context("decoder delay diagnostic size")?;
        let delay = i32::from_le_bytes(bytes);
        ensure!(
            (-1..=88200).contains(&delay),
            "decoder delay diagnostic budget"
        );
        Ok((delay >= 0).then_some(delay as u32))
    }
    pub fn decode(&mut self, packet: &[u8]) -> Result<Vec<i16>> {
        Ok(self.decode_block(packet)?.pcm)
    }
    pub fn decode_block(&mut self, packet: &[u8]) -> Result<DecodedAudio> {
        let generation = self.generation;
        let samples_per_frame = self.samples_per_frame;
        ensure!(
            generation != 0 && matches!(samples_per_frame, 480 | 1024),
            "AAC decoder not configured"
        );
        let out = self.call(3, packet)?;
        decoded_reply(out, generation, samples_per_frame)
    }
}
impl Drop for Worker {
    fn drop(&mut self) {
        use zeroize::Zeroize;
        self.reply.zeroize();
        unsafe { am_worker_close(self.handle.as_ptr()) }
    }
}

// The worker verifies its OS token before invoking retained native routines. No fallback.
pub fn worker_main() -> Result<()> {
    use std::io::{Read, Write};
    ensure!(
        unsafe { am_is_sandboxed() } == 1,
        "codec worker requires capability-free AppContainer"
    );
    let mut input = std::io::stdin().lock();
    let mut output = std::io::stdout().lock();
    output.write_all(&0x414d5342u32.to_le_bytes())?;
    output.flush()?;
    struct Decoder(*mut c_void);
    impl Drop for Decoder {
        fn drop(&mut self) {
            unsafe { am_aac_close(self.0) }
        }
    }
    let mut decoder = Decoder(std::ptr::null_mut());
    let mut generation = 0u64;
    // The IPC contract has fixed maxima. Reuse storage instead of allocating and
    // clearing request/PCM response vectors for every 480-sample audio frame.
    let mut data = [0; 8192];
    let mut reply = [0; 8204];
    let mut pcm = [0i16; 4096];
    loop {
        let mut h = [0; 8];
        if input.read_exact(&mut h).is_err() {
            return Ok(());
        }
        let op = u32::from_le_bytes(h[..4].try_into()?);
        let len = u32::from_le_bytes(h[4..].try_into()?) as usize;
        ensure!(len <= 8192, "worker input size");
        input.read_exact(&mut data[..len])?;
        let reply_len = match op {
            1 => {
                ensure!(
                    len == 236
                        && &data[..4] == b"FPLY"
                        && data[4] == 3
                        && data[6] == 3
                        && data[12] < 4,
                    "worker FairPlay contract"
                );
                let mut key = [0; 16];
                unsafe { playfair_decrypt(data.as_ptr(), data[164..].as_ptr(), key.as_mut_ptr()) };
                reply[..16].copy_from_slice(&key);
                zeroize::Zeroize::zeroize(&mut key);
                16
            }
            2 => {
                ensure!(
                    len == 1 && matches!(data[0], 4 | 8),
                    "worker codec contract"
                );
                decoder = Decoder(unsafe { am_aac_open(data[0] as u32) });
                generation = generation
                    .checked_add(1)
                    .context("AAC generation exhausted")?;
                if decoder.0.is_null() {
                    0
                } else {
                    reply[..8].copy_from_slice(&generation.to_le_bytes());
                    8
                }
            }
            3 => {
                ensure!(!decoder.0.is_null() && len > 0, "worker decoder state");
                let mut delay = u32::MAX;
                let n = unsafe {
                    am_aac_decode_timed(
                        decoder.0,
                        data.as_ptr(),
                        len as u32,
                        pcm.as_mut_ptr(),
                        pcm.len() as u32,
                        &mut delay,
                    )
                };
                if n < 0 {
                    0
                } else {
                    ensure!(
                        n > 0 && n as usize <= pcm.len() && delay <= 4096,
                        "worker PCM contract"
                    );
                    reply[..8].copy_from_slice(&generation.to_le_bytes());
                    reply[8..12].copy_from_slice(&delay.to_le_bytes());
                    for (value, bytes) in pcm[..n as usize]
                        .iter()
                        .zip(reply[12..].chunks_exact_mut(2))
                    {
                        bytes.copy_from_slice(&value.to_le_bytes());
                    }
                    12 + n as usize * 2
                }
            }
            4 => {
                ensure!(len == 0, "worker probe length");
                reply[0] = 1;
                1
            }
            5 => {
                ensure!(len <= 2048, "sandbox probe size");
                let request = std::str::from_utf8(&data[..len])?;
                let (port, path) = request.split_once('\n').context("sandbox probe format")?;
                let file_denied = std::fs::read(path)
                    .is_err_and(|e| e.kind() == std::io::ErrorKind::PermissionDenied);
                let network_denied = (|| -> std::io::Result<()> {
                    let sock = std::net::UdpSocket::bind("127.0.0.1:0")?;
                    sock.send_to(b"probe", format!("127.0.0.1:{port}"))?;
                    Ok(())
                })()
                .is_err();
                reply[..2].copy_from_slice(&[file_denied.into(), network_denied.into()]);
                2
            }
            6 => {
                ensure!(len == 0, "decoder diagnostic request length");
                let text = unsafe { CStr::from_ptr(am_aac_last_error(decoder.0)) }.to_bytes();
                ensure!(text.len() <= 256, "decoder diagnostic length");
                reply[..text.len()].copy_from_slice(text);
                text.len()
            }
            7 => {
                ensure!(
                    len == 0 && !decoder.0.is_null(),
                    "decoder delay diagnostic state"
                );
                reply[..4].copy_from_slice(&unsafe { am_aac_delay(decoder.0) }.to_le_bytes());
                4
            }
            _ => bail!("unsupported worker operation"),
        };
        output.write_all(&(reply_len as u32).to_le_bytes())?;
        output.write_all(&reply[..reply_len])?;
        output.flush()?;
    }
}

pub(crate) fn checked_worker() -> Result<Worker> {
    use std::io::Write;
    struct Canary(std::path::PathBuf);
    impl Drop for Canary {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.0);
        }
    }
    let mut worker = Worker::start()?;
    let path = std::env::current_exe()?
        .parent()
        .unwrap()
        .join(format!("sandbox-canary-{}.txt", std::process::id()));
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)?;
    let canary = Canary(path);
    let written = file.write_all(b"Nonsecret sandbox test canary");
    drop(file);
    written?;
    let udp = std::net::UdpSocket::bind("127.0.0.1:0")?;
    let result = worker.call(
        5,
        format!("{}\n{}", udp.local_addr()?.port(), canary.0.display()).as_bytes(),
    );
    std::fs::remove_file(&canary.0)?;
    let result = result?;
    udp.set_read_timeout(Some(std::time::Duration::from_millis(250)))?;
    let mut packet = [0; 32];
    let delivered = match udp.recv_from(&mut packet) {
        Ok(_) => true,
        Err(error)
            if matches!(
                error.kind(),
                std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
            ) =>
        {
            false
        }
        Err(error) => return Err(error).context("sandbox probe receive failed"),
    };
    // Winsock sendto success confirms submission, not delivery. AppContainer
    // filtering may reject the datagram after send_to has returned successfully.
    // Keep the token/file checks and observe the receiving socket independently.
    ensure!(
        matches!(result, [1, 0 | 1]) && !delivered,
        "AppContainer denial probe file_denied/send_error={result:?}, datagram_delivered={delivered}"
    );
    println!(
        "PASS: AppContainer file read denied; UDP datagram not received (send_error={}).",
        result[1] != 0
    );
    let vector = include_bytes!("../tests/fixtures/fairplay-known-answer.bin");
    let key = worker.decrypt_key(vector[..164].try_into()?, vector[164..236].try_into()?)?;
    ensure!(key == vector[236..], "FairPlay known-answer mismatch");
    Ok(worker)
}

// Decode-only regression: no App reference and no path to am_audio/WASAPI.
// Reconfiguration and repeat hashes cover reuse of the fixed IPC buffers.
pub fn codec_probe_silent() -> Result<()> {
    use sha2::{Digest, Sha256};
    use std::io::Read;
    let mut worker = checked_worker()?;
    let eld = include_bytes!("../tests/fixtures/tone-44100-stereo-eld.frames");
    let lc = include_bytes!("../tests/fixtures/triangle-44100-stereo-lc.frames");
    let mut references = [None, None];
    let mut total_frames = 0;
    // Exercise a real format transition, same-format FLUSH/reset, then restoration.
    for codec in [8, 4, 4, 8] {
        worker.configure(codec)?;
        let fixture: &[u8] = if codec == 8 { eld } else { lc };
        let expected_samples = if codec == 8 { 960 } else { 2048 };
        let mut input = std::io::Cursor::new(fixture);
        let mut hash = Sha256::new();
        let mut frames = 0;
        let mut samples = 0;
        let mut energy = 0f64;
        while (input.position() as usize) < fixture.len() {
            let mut len = [0; 4];
            input.read_exact(&mut len)?;
            let len = u32::from_le_bytes(len) as usize;
            ensure!(len <= 8192, "fixture length");
            let start = input.position() as usize;
            let end = start.checked_add(len).context("fixture length overflow")?;
            let packet = fixture.get(start..end).context("truncated AAC fixture")?;
            input.set_position(end as u64);
            let pcm = worker.decode(packet)?;
            ensure!(pcm.len() == expected_samples, "AAC sample-count contract");
            samples += pcm.len();
            for sample in pcm {
                hash.update(sample.to_le_bytes());
                energy += f64::from(sample).powi(2);
            }
            frames += 1;
        }
        ensure!(
            frames >= 100 && energy > 1e6,
            "AAC fixture decoded no signal"
        );
        let digest = hash.finalize();
        if codec == 4 {
            ensure!(frames == 120 && samples == 245760, "AAC-LC fixture count");
            ensure!(
                format!("{digest:x}")
                    == if cfg!(feature = "ffmpeg-aac") {
                        "514b421e11ea7b9896e4b2d83a24d13bfd0c1cc93aaa3465b85ee16373055aa3"
                    } else {
                        "ec0e0ecf994a43665ab2ef72c245ca49385d5927353a531e97dbe84707b59ae9"
                    },
                "AAC-LC pinned PCM golden hash mismatch"
            );
        } else {
            ensure!(frames == 120 && samples == 115200, "AAC-ELD fixture count");
            ensure!(
                format!("{digest:x}")
                    == if cfg!(feature = "ffmpeg-aac") {
                        "d2291893f6096a492b657211cd02c3ddd8f1ecf6c4440a6338cb11c94d1ffece"
                    } else {
                        "c5679c95ef23189c71327d0e78e26ffd94c596f40902ccdfb7f5feaf256f8f6f"
                    },
                "AAC-ELD pinned PCM golden hash mismatch"
            );
        }
        let reference = &mut references[usize::from(codec == 4)];
        if let Some(previous) = reference {
            ensure!(
                digest == *previous,
                "AAC reset/reuse changed decoded samples"
            );
        }
        *reference = Some(digest);
        total_frames += frames;
    }
    println!(
        "PASS: silent isolated codec regression: FairPlay known-answer, sandbox checks, {total_frames} AAC-ELD/AAC-LC frames, both pinned golden hashes and identical hashes after reset. No audio samples submitted."
    );
    Ok(())
}

pub fn codec_probe(app: &App) -> Result<()> {
    use std::io::Read;
    let mut worker = checked_worker()?;
    worker.configure(8)?;
    let mut input = std::io::Cursor::new(include_bytes!(
        "../tests/fixtures/tone-44100-stereo-eld.frames"
    ));
    let start = now() + 0.12;
    let mut frames = 0;
    let mut energy = 0f64;
    while (input.position() as usize) < input.get_ref().len() {
        let mut len = [0; 4];
        input.read_exact(&mut len)?;
        let len = u32::from_le_bytes(len) as usize;
        ensure!(len <= 8192, "fixture length");
        let mut data = vec![0; len];
        input.read_exact(&mut data)?;
        let pcm = worker.decode(&data)?;
        ensure!(pcm.len() == 960, "AAC-ELD 480-frame contract");
        energy += pcm.iter().map(|s| f64::from(*s).powi(2)).sum::<f64>();
        app.audio(&pcm, start + frames as f64 * 480.0 / 44100.0)?;
        frames += 1;
        app.pump(0)?;
        while now() < start - 0.12 + frames as f64 * 480.0 / 44100.0 {
            app.pump(2)?;
        }
    }
    let until = now() + 0.25;
    while now() < until {
        app.pump(2)?;
    }
    ensure!(
        frames >= 100 && energy > 1e6,
        "AAC fixture decoded no signal"
    );
    app.flush_audio();
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{decoded_reply, validate_probe_packet, wait_socket_readable};

    #[test]
    fn socket_wait_handles_timeout_accept_data_eof_and_bounds() {
        use std::{
            io::{Read, Write},
            net::{TcpListener, TcpStream},
            os::windows::io::AsRawSocket,
            time::{Duration, Instant},
        };
        let listener = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0)).unwrap();
        listener.set_nonblocking(true).unwrap();
        let socket = listener.as_raw_socket() as usize;
        assert!(!wait_socket_readable(socket, 0).unwrap());
        let began = Instant::now();
        assert!(!wait_socket_readable(socket, 25).unwrap());
        assert!(began.elapsed() >= Duration::from_millis(10));

        let address = listener.local_addr().unwrap();
        let connecting = std::thread::spawn(move || TcpStream::connect(address).unwrap());
        assert!(wait_socket_readable(socket, 1000).unwrap());
        let (mut server, _) = listener.accept().unwrap();
        let mut client = connecting.join().unwrap();
        server.set_nonblocking(true).unwrap();
        let connected = server.as_raw_socket() as usize;
        assert!(!wait_socket_readable(connected, 0).unwrap());
        client.write_all(b"x").unwrap();
        assert!(wait_socket_readable(connected, 1000).unwrap());
        let mut byte = [0];
        assert_eq!(server.read(&mut byte).unwrap(), 1);
        assert_eq!(byte, *b"x");
        drop(client);
        assert!(wait_socket_readable(connected, 1000).unwrap());
        assert_eq!(server.read(&mut byte).unwrap(), 0);
        assert!(wait_socket_readable(socket, 1001).is_err());
        assert_eq!(unsafe { super::am_wait_socket_readable(socket, 1001) }, -1);
        assert!(wait_socket_readable(usize::MAX, 0).is_err());
    }

    #[test]
    fn synthetic_decoder_probe_uses_production_video_admission() {
        assert!(validate_probe_packet(&[0, 0, 0, 1, 0x26, 1, 0x80], 265).is_ok());
        assert!(
            validate_probe_packet(&[0, 0, 1, 0x26, 1, 0x80, 0, 0, 0, 1, 0x26, 1, 0x80], 265)
                .is_ok()
        );
        for bad in [
            &[][..],
            &[0x26, 1, 0x80],
            &[0, 0, 1],
            &[0, 0, 1, 0xa6, 1],
            &[0, 0, 1, 0x27, 1],
            &[0, 0, 1, 0x42, 1],
        ] {
            assert!(validate_probe_packet(bad, 265).is_err());
        }
        assert!(validate_probe_packet(&[0, 0, 1, 0x65, 0x80], 264).is_ok());
        assert!(validate_probe_packet(&[0, 0, 1, 0xe5, 0x80], 264).is_err());
        assert!(validate_probe_packet(&[0, 0, 1, 0x26, 1, 0x80], 0).is_err());
    }

    #[test]
    fn pcm_metadata_is_atomic_and_bounded() {
        let mut reply = vec![0u8; 12 + 1024 * 4];
        reply[..8].copy_from_slice(&17u64.to_le_bytes());
        reply[8..12].copy_from_slice(&1685u32.to_le_bytes());
        let block = decoded_reply(&reply, 17, 1024).unwrap();
        assert_eq!(block.generation, 17);
        assert_eq!(block.additional_delay_samples, 1685);
        assert_eq!(block.pcm.len(), 2048);
        assert!(decoded_reply(&reply, 18, 1024).is_err());
        assert!(decoded_reply(&reply, 0, 1024).is_err());
        assert!(decoded_reply(&reply, 17, 480).is_err());
        assert!(decoded_reply(&reply[..reply.len() - 4], 17, 1024).is_err());
        assert!(decoded_reply(&[], 17, 1024).is_err());
        reply[8..12].copy_from_slice(&4097u32.to_le_bytes());
        assert!(decoded_reply(&reply, 17, 1024).is_err());
    }
}
