use anyhow::{Result, ensure};
pub const PLAYOUT_DELAY: f64 = 0.120;
const NTP_EPOCH: u64 = 2_208_988_800u64 << 32;
pub fn ntp(seconds: f64) -> u64 {
    (seconds * 4294967296.0) as u64
}
pub fn difference(a: u64, b: u64) -> f64 {
    a.wrapping_sub(b) as i64 as f64 / 4294967296.0
}
#[derive(Default)]
pub struct Clock {
    reference: Option<(u64, f64)>,
    audio: Option<(u32, u64)>,
    best_rtt: f64,
    pub updates: u64,
}
impl Clock {
    pub fn observe(&mut self, remote: u64, local: f64, rtt: f64) -> Result<()> {
        ensure!(
            rtt.is_finite() && (0.0..0.5).contains(&rtt),
            "invalid timing RTT"
        );
        if let Some((ref_ntp, ref_local)) = self.reference {
            let prediction = ref_local + difference(remote, ref_ntp);
            let error = local - prediction;
            ensure!(error.abs() < 2.0, "sender clock discontinuity");
            if rtt <= self.best_rtt * 1.5 + 0.001 {
                self.reference = Some((remote, prediction + error.clamp(-0.001, 0.001)));
            }
            self.best_rtt = (self.best_rtt * 1.01).min(rtt);
        } else {
            self.reference = Some((remote, local));
            self.best_rtt = rtt;
        }
        self.updates += 1;
        Ok(())
    }
    pub fn map(&self, remote: u64) -> Option<f64> {
        self.reference
            .map(|(r, l)| l + difference(remote, r) + PLAYOUT_DELAY)
    }
    pub fn video(&self, remote: u64) -> Option<f64> {
        self.map(remote.wrapping_add(NTP_EPOCH))
    }
    pub fn sync_audio(&mut self, rtp: u32, remote: u64) {
        self.audio = Some((rtp, remote));
    }
    pub fn clear_audio(&mut self) {
        self.audio = None;
    }
    pub fn audio(&self, rtp: u32) -> Option<f64> {
        let (r, n) = self.audio?;
        self.map(n)
            .map(|t| t + rtp.wrapping_sub(r) as i32 as f64 / 44100.0)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn video_audio_common_clock_and_rtp_wrap() {
        let mut c = Clock::default();
        let remote = NTP_EPOCH.wrapping_add(ntp(1000.0));
        c.observe(remote, 20.0, 0.002).unwrap();
        c.sync_audio(u32::MAX - 479, remote);
        assert!((c.video(ntp(1000.0)).unwrap() - 20.12).abs() < 1e-6);
        assert!((c.audio(0).unwrap() - 20.12 - 480.0 / 44100.0).abs() < 1e-6);
    }
    #[test]
    fn bounded_drift_correction() {
        let mut c = Clock::default();
        c.observe(ntp(100.0), 20.0, 0.01).unwrap();
        c.observe(ntp(101.0), 21.1, 0.01).unwrap();
        assert!((c.map(ntp(101.0)).unwrap() - 21.121).abs() < 0.00001);
        assert!(c.observe(ntp(102.0), 50.0, 0.01).is_err());
    }
}
