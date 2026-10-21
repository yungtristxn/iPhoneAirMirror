use anyhow::{Context, Result, bail, ensure};
use std::{
    ffi::{CStr, CString, c_char, c_void},
    ptr::NonNull,
};

unsafe extern "C" {
    fn am_error() -> *const c_char;
    fn am_lan_ipv4(address: *mut u8, index: *mut u32, prefix: *mut u32) -> i32;
    fn am_show_error(message: *const c_char);
    fn am_now() -> f64;
    fn am_open(hidden: i32) -> *mut c_void;
    fn am_video_probe(app: *mut c_void) -> i32;
    fn am_ui_probe(app: *mut c_void, directory: *const c_char) -> i32;
    fn am_close(app: *mut c_void);
    fn am_wake_handle(app: *mut c_void) -> *mut c_void;
    fn am_signal(handle: *mut c_void);
    fn am_signal_close(handle: *mut c_void);
    fn am_flush_audio(app: *mut c_void);
    fn am_title(app: *mut c_void, title: *const c_char);
    fn am_pump(app: *mut c_void, wait_ms: u32) -> i32;
    fn am_prepare_video(app: *mut c_void, codec: u32, width: u32, height: u32, fps: u32) -> i32;
    fn am_presented(app: *mut c_void) -> u64;
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
    ) -> i32;
    fn am_video_ready(app: *mut c_void) -> i32;
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
    fn am_worker_close(worker: *mut c_void);
    fn am_worker_call(
        worker: *mut c_void,
        op: u32,
        input: *const u8,
        len: u32,
        output: *mut u8,
        cap: u32,
    ) -> i32;
    fn am_aac_open(codec: u32) -> *mut c_void;
    fn am_aac_close(decoder: *mut c_void);
    fn am_aac_decode(
        decoder: *mut c_void,
        data: *const u8,
        len: u32,
        out: *mut i16,
        cap: u32,
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
pub fn lan_ipv4() -> Result<(std::net::Ipv4Addr, u32, u32)> {
    let mut address = [0; 4];
    let (mut index, mut prefix) = (0, 0);
    if unsafe { am_lan_ipv4(address.as_mut_ptr(), &mut index, &mut prefix) } == 0 {
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
    pub fn hevc_available(&self) -> bool {
        unsafe { am_hevc_available(self.0.as_ptr()) != 0 }
    }
    pub fn video_ready(&self) -> bool {
        unsafe { am_video_ready(self.0.as_ptr()) != 0 }
    }
    pub fn video_probe(&self) -> Result<()> {
        if unsafe { am_video_probe(self.0.as_ptr()) } == 0 {
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
    pub fn video(&self, data: &[u8], pts: f64, codec: u32, width: u32, height: u32) -> Result<()> {
        ensure!(
            matches!(codec, 264 | 265)
                && data.len()
                    <= if codec == 265 {
                        4 * 1024 * 1024
                    } else {
                        2 * 1024 * 1024
                    }
                && pts.is_finite(),
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
            )
        } == 0
        {
            return Err(error());
        }
        Ok(())
    }
    pub fn audio(&self, pcm: &[i16], pts: f64) -> Result<()> {
        ensure!(
            pcm.len() <= 4096 && pcm.len().is_multiple_of(2) && pts.is_finite(),
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
pub struct Worker(NonNull<c_void>);
impl Worker {
    pub fn start() -> Result<Self> {
        NonNull::new(unsafe { am_worker_start() })
            .map(Self)
            .ok_or_else(error)
    }
    pub fn call(&mut self, op: u32, input: &[u8]) -> Result<Vec<u8>> {
        ensure!(input.len() <= 8192, "worker request too large");
        let mut out = vec![0; 16384];
        let n = unsafe {
            am_worker_call(
                self.0.as_ptr(),
                op,
                input.as_ptr(),
                input.len() as u32,
                out.as_mut_ptr(),
                out.len() as u32,
            )
        };
        ensure!(n >= 0, "isolated codec worker failed or timed out");
        out.truncate(n as usize);
        Ok(out)
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
        ensure!(
            self.call(2, &[codec])? == [1],
            "AAC decoder initialization failed"
        );
        Ok(())
    }
    pub fn decode(&mut self, packet: &[u8]) -> Result<Vec<i16>> {
        let out = self.call(3, packet)?;
        ensure!(
            !out.is_empty() && out.len().is_multiple_of(4) && out.len() <= 8192,
            "AAC decode failed"
        );
        Ok(out
            .chunks_exact(2)
            .map(|x| i16::from_le_bytes([x[0], x[1]]))
            .collect())
    }
}
impl Drop for Worker {
    fn drop(&mut self) {
        unsafe { am_worker_close(self.0.as_ptr()) }
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
    loop {
        let mut h = [0; 8];
        if input.read_exact(&mut h).is_err() {
            return Ok(());
        }
        let op = u32::from_le_bytes(h[..4].try_into()?);
        let len = u32::from_le_bytes(h[4..].try_into()?) as usize;
        ensure!(len <= 8192, "worker input size");
        let mut data = vec![0; len];
        input.read_exact(&mut data)?;
        let reply = match op {
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
                key.to_vec()
            }
            2 => {
                ensure!(
                    len == 1 && matches!(data[0], 4 | 8),
                    "worker codec contract"
                );
                decoder = Decoder(unsafe { am_aac_open(data[0] as u32) });
                vec![u8::from(!decoder.0.is_null())]
            }
            3 => {
                ensure!(!decoder.0.is_null() && len > 0, "worker decoder state");
                let mut pcm = [0i16; 4096];
                let n = unsafe {
                    am_aac_decode(
                        decoder.0,
                        data.as_ptr(),
                        len as u32,
                        pcm.as_mut_ptr(),
                        pcm.len() as u32,
                    )
                };
                if n < 0 {
                    Vec::new()
                } else {
                    ensure!(n as usize <= pcm.len(), "worker PCM length");
                    pcm[..n as usize]
                        .iter()
                        .flat_map(|x| x.to_le_bytes())
                        .collect()
                }
            }
            4 => {
                ensure!(len == 0, "worker probe length");
                vec![1]
            }
            5 => {
                ensure!(len <= 2048, "sandbox probe size");
                let request = std::str::from_utf8(&data)?;
                let (port, path) = request.split_once('\n').context("sandbox probe format")?;
                let file_denied = std::fs::read(path)
                    .is_err_and(|e| e.kind() == std::io::ErrorKind::PermissionDenied);
                let network_denied = (|| -> std::io::Result<()> {
                    let sock = std::net::UdpSocket::bind("127.0.0.1:0")?;
                    sock.send_to(b"probe", format!("127.0.0.1:{port}"))?;
                    Ok(())
                })()
                .is_err();
                vec![file_denied.into(), network_denied.into()]
            }
            _ => bail!("unsupported worker operation"),
        };
        output.write_all(&(reply.len() as u32).to_le_bytes())?;
        output.write_all(&reply)?;
        output.flush()?;
    }
}

pub fn codec_probe(app: &App) -> Result<()> {
    use std::io::Read;
    let mut worker = Worker::start()?;
    let canary = std::env::current_exe()?
        .parent()
        .unwrap()
        .join(format!("sandbox-canary-{}.txt", std::process::id()));
    std::fs::write(&canary, b"Nonsecret sandbox test canary")?;
    let udp = std::net::UdpSocket::bind("127.0.0.1:0")?;
    let result = worker.call(
        5,
        format!("{}\n{}", udp.local_addr()?.port(), canary.display()).as_bytes(),
    );
    std::fs::remove_file(&canary)?;
    let result = result?;
    udp.set_read_timeout(Some(std::time::Duration::from_millis(250)))?;
    let mut packet = [0; 32];
    let delivered = udp.recv_from(&mut packet).is_ok();
    ensure!(
        result.first() == Some(&1) && !delivered,
        "AppContainer denial probe file/network={result:?}, datagram_delivered={delivered}"
    );
    let vector = include_bytes!("../tests/fixtures/fairplay-known-answer.bin");
    let key = worker.decrypt_key(vector[..164].try_into()?, vector[164..236].try_into()?)?;
    ensure!(key == vector[236..], "FairPlay known-answer mismatch");
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
