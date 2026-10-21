use anyhow::{Result, ensure};
pub const DEFAULT_PLAYOUT_MS: u32 = 80;
const NTP_EPOCH: u64 = 2_208_988_800u64 << 32;
pub fn ntp(seconds: f64) -> u64 {
    (seconds * 4294967296.0) as u64
}
pub fn difference(a: u64, b: u64) -> f64 {
    a.wrapping_sub(b) as i64 as f64 / 4294967296.0
}
pub struct Clock {
    reference: Option<(u64, f64)>,
    audio: Option<(u32, u64)>,
    best_rtt: f64,
    pub updates: u64,
}
impl Default for Clock {
    fn default() -> Self {
        Self::new(DEFAULT_PLAYOUT_MS).expect("default advertised latency bounds")
    }
}
impl Clock {
    pub fn new(advertised_latency_ms: u32) -> Result<Self> {
        // Validate the negotiation hint here without shifting the shared clock.
        // AirPlay video PTS and the audio RTP/NTP anchor already specify playback
        // time; adding the hint shifts both past the sender's deadlines.
        ensure!(
            (60..=250).contains(&advertised_latency_ms),
            "advertised latency must be 60..250 ms"
        );
        Ok(Self {
            reference: None,
            audio: None,
            best_rtt: 0.0,
            updates: 0,
        })
    }
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
        self.reference.map(|(r, l)| l + difference(remote, r))
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
        assert!((c.video(ntp(1000.0)).unwrap() - 20.0).abs() < 1e-6);
        assert!((c.audio(0).unwrap() - 20.0 - 480.0 / 44100.0).abs() < 1e-6);
    }
    #[test]
    fn bounded_drift_correction() {
        let mut c = Clock::default();
        c.observe(ntp(100.0), 20.0, 0.01).unwrap();
        c.observe(ntp(101.0), 21.1, 0.01).unwrap();
        assert!((c.map(ntp(101.0)).unwrap() - 21.001).abs() < 0.00001);
        assert!(c.observe(ntp(102.0), 50.0, 0.01).is_err());
    }
    #[test]
    fn advertised_latency_does_not_shift_presentation_deadlines() {
        for delay in [60, 80, 120, 250] {
            let mut c = Clock::new(delay).unwrap();
            let n = NTP_EPOCH.wrapping_add(ntp(1000.0));
            c.observe(n, 20.0, 0.002).unwrap();
            c.sync_audio(123, n);
            assert!((c.video(ntp(1000.0)).unwrap() - c.audio(123).unwrap()).abs() < 1e-6);
            assert!((c.audio(123).unwrap() - 20.0).abs() < 1e-6);
            assert!((c.video(ntp(1000.1)).unwrap() - 20.1).abs() < 1e-6);
            assert!((c.audio(123 + 4410).unwrap() - 20.1).abs() < 1e-6);
        }
        assert!(Clock::new(0).is_err());
        assert!(Clock::new(251).is_err());
    }
    #[test]
    fn clock_rewind_is_rejected_without_moving_audio_or_video() {
        let mut c = Clock::default();
        let remote = NTP_EPOCH.wrapping_add(ntp(1000.0));
        c.observe(remote, 20.0, 0.002).unwrap();
        c.sync_audio(123, remote);
        let prior_video = c.video(ntp(1000.1)).unwrap();
        let prior_audio = c.audio(123 + 4410).unwrap();
        let rewound = NTP_EPOCH.wrapping_add(ntp(990.0));
        assert!(c.observe(rewound, 21.0, 0.002).is_err());
        assert_eq!(c.updates, 1);
        assert_eq!(c.video(ntp(1000.1)), Some(prior_video));
        assert_eq!(c.audio(123 + 4410), Some(prior_audio));
        c.observe(remote.wrapping_add(ntp(1.0)), 21.0, 0.002)
            .unwrap();
        assert!((c.video(ntp(1001.0)).unwrap() - 21.0).abs() < 1e-6);
    }
}
