//! Integer sample-clock mapping for additional decoder DSP delay.
//!
//! A decoded block belongs to the packet's RTP position minus the decoder's
//! reported additional delay. Leading samples before this decoder generation's
//! first input packet are priming and must not reach playback. This module stores
//! no PCM and adds no output FIFO or independent wall-clock offset.
use anyhow::{Result, ensure};

pub const MAX_ADDITIONAL_DELAY_SAMPLES: u32 = 4096;
const MAX_FRAMES_PER_BLOCK: usize = 2048;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BlockTiming {
    /// Remove this many *frames*, not interleaved samples, from the decoded block.
    pub skip_frames: usize,
    /// RTP position of the first retained sample frame; use only for nonempty output.
    pub output_rtp: u32,
    pub output_frames: usize,
}

#[derive(Debug)]
pub struct AudioTiming {
    generation: u64,
    next_input_rtp: Option<u32>,
    delay_samples: Option<u32>,
    frames_per_block: Option<usize>,
    input_frames: u64,
}

impl AudioTiming {
    /// Create a fresh mapper after every successful decoder configuration.
    /// Generation IDs come from the configured decoder, not from packet contents.
    pub fn new(generation: u64) -> Result<Self> {
        ensure!(
            generation != 0,
            "audio timing requires a configured decoder generation"
        );
        Ok(Self {
            generation,
            next_input_rtp: None,
            delay_samples: None,
            frames_per_block: None,
            input_frames: 0,
        })
    }

    pub fn expected_next_input(&self) -> Option<u32> {
        self.next_input_rtp
    }

    /// Check before decoding. A gap, duplicate or rewind requires resetting the
    /// decoder itself and creating a fresh mapper, not relabeling buffered PCM.
    pub fn needs_reset(&self, packet_rtp: u32) -> bool {
        self.expected_next_input()
            .is_some_and(|expected| packet_rtp != expected)
    }

    /// Describe a successfully decoded packet without changing its samples.
    /// Map `output_rtp` through the existing common RTP/NTP clock. The caller must
    /// discard `skip_frames * channels` samples and skip zero-frame submissions.
    /// Any rejected call leaves this mapper unchanged.
    pub fn map_block(
        &mut self,
        generation: u64,
        packet_rtp: u32,
        frames: usize,
        additional_delay: u32,
    ) -> Result<BlockTiming> {
        ensure!(
            generation == self.generation,
            "stale or unconfigured decoder generation"
        );
        ensure!(
            (1..=MAX_FRAMES_PER_BLOCK).contains(&frames),
            "decoded audio frame-count budget"
        );
        ensure!(
            additional_delay <= MAX_ADDITIONAL_DELAY_SAMPLES,
            "additional decoder delay budget"
        );
        ensure!(
            !self.needs_reset(packet_rtp),
            "RTP discontinuity requires decoder reconfiguration"
        );
        ensure!(
            self.delay_samples
                .is_none_or(|delay| delay == additional_delay),
            "decoder delay changed within a generation"
        );
        ensure!(
            self.frames_per_block
                .is_none_or(|previous| previous == frames),
            "decoded frame count changed within a generation"
        );
        let next_input_frames = self
            .input_frames
            .checked_add(frames as u64)
            .ok_or_else(|| anyhow::anyhow!("audio timing sample counter overflow"))?;
        let remaining_priming = u64::from(additional_delay).saturating_sub(self.input_frames);
        let skip_frames = frames.min(remaining_priming as usize);
        let timing = BlockTiming {
            skip_frames,
            output_rtp: packet_rtp
                .wrapping_sub(additional_delay)
                .wrapping_add(skip_frames as u32),
            output_frames: frames - skip_frames,
        };
        self.next_input_rtp = Some(packet_rtp.wrapping_add(frames as u32));
        self.delay_samples = Some(additional_delay);
        self.frames_per_block = Some(frames);
        self.input_frames = next_input_frames;
        Ok(timing)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    // Nonperiodic, deterministic source labels. The source index is known
    // independently of decoder timestamps, unlike a periodic phase comparison.
    fn source(index: usize) -> i16 {
        let mut value = (index as u32)
            .wrapping_mul(1664525)
            .wrapping_add(1013904223);
        value ^= value >> 13;
        value = value.wrapping_mul(0x85ebca6b);
        ((value >> 16) as i32 - 32768) as i16
    }

    fn modeled_decode(packet_offset: usize, frames: usize, delay: u32) -> Vec<i16> {
        (0..frames)
            .map(|frame| {
                let source_index = (packet_offset + frame) as i64 - i64::from(delay);
                if source_index < 0 {
                    0
                } else {
                    source(source_index as usize)
                }
            })
            .collect()
    }

    fn scheduled_source(
        origin: u32,
        generation: u64,
        delay: u32,
        frames: usize,
        blocks: usize,
    ) -> BTreeMap<u32, i16> {
        let mut timing = AudioTiming::new(generation).unwrap();
        let mut result = BTreeMap::new();
        for block in 0..blocks {
            let offset = block * frames;
            let packet_rtp = origin.wrapping_add(offset as u32);
            assert!(!timing.needs_reset(packet_rtp));
            let pcm = modeled_decode(offset, frames, delay);
            let mapped = timing
                .map_block(generation, packet_rtp, frames, delay)
                .unwrap();
            assert_eq!(mapped.skip_frames + mapped.output_frames, frames);
            for (index, value) in pcm[mapped.skip_frames..].iter().enumerate() {
                let rtp = mapped.output_rtp.wrapping_add(index as u32);
                assert!(
                    result.insert(rtp, *value).is_none(),
                    "duplicate sample-clock position"
                );
                let known_source_index = rtp.wrapping_sub(origin) as usize;
                assert_eq!(
                    *value,
                    source(known_source_index),
                    "PCM labeled with the wrong source time"
                );
            }
        }
        result
    }

    #[test]
    fn zero_and_multi_packet_decoder_delay_align_to_the_known_source_clock() {
        let origin = 7_654_321;
        let ffmpeg = scheduled_source(origin, 1, 0, 1024, 12);
        let fdk = scheduled_source(origin, 2, 1685, 1024, 12);
        assert_eq!(ffmpeg.len(), 12 * 1024);
        assert_eq!(fdk.len(), 12 * 1024 - 1685);
        assert_eq!(fdk.get(&origin), Some(&source(0)));
        for (rtp, value) in &fdk {
            assert_eq!(ffmpeg.get(rtp), Some(value));
        }
        // Equal source positions therefore map to equal common-clock deadlines.
        for offset in [0u32, 362, 363, 4096, 10_000] {
            let deadline = 20.0 + f64::from(offset) / 44100.0;
            let rtp = origin.wrapping_add(offset);
            assert_eq!(ffmpeg.get(&rtp), fdk.get(&rtp));
            assert_eq!(
                20.0 + f64::from(rtp.wrapping_sub(origin)) / 44100.0,
                deadline
            );
        }
    }

    #[test]
    fn priming_larger_than_one_packet_is_trimmed_without_a_fifo() {
        let mut timing = AudioTiming::new(1).unwrap();
        let first = timing.map_block(1, 100, 1024, 1685).unwrap();
        assert_eq!((first.skip_frames, first.output_frames), (1024, 0));
        let second = timing.map_block(1, 1124, 1024, 1685).unwrap();
        assert_eq!(
            (second.skip_frames, second.output_frames, second.output_rtp),
            (661, 363, 100)
        );
        let third = timing.map_block(1, 2148, 1024, 1685).unwrap();
        assert_eq!(
            (third.skip_frames, third.output_frames, third.output_rtp),
            (0, 1024, 463)
        );
        assert!(
            std::mem::size_of::<AudioTiming>() <= 64,
            "timing must not buffer decoded audio"
        );
    }

    #[test]
    fn flush_and_codec_reconfigure_start_fresh_priming_generations() {
        let mut before = AudioTiming::new(10).unwrap();
        before.map_block(10, 1000, 1024, 1685).unwrap();
        before.map_block(10, 2024, 1024, 1685).unwrap();
        let mut after = AudioTiming::new(11).unwrap();
        assert!(after.map_block(10, 50_000, 1024, 1685).is_err());
        assert_eq!(after.expected_next_input(), None);
        let first = after.map_block(11, 50_000, 1024, 1685).unwrap();
        assert_eq!(
            first.output_frames, 0,
            "old generation progress cannot bypass new priming"
        );
        let second = after.map_block(11, 51_024, 1024, 1685).unwrap();
        assert_eq!(second.output_rtp, 50_000);
        let mut eld = AudioTiming::new(12).unwrap();
        let eld_block = eld.map_block(12, 70_000, 480, 0).unwrap();
        assert_eq!(
            eld_block,
            BlockTiming {
                skip_frames: 0,
                output_rtp: 70_000,
                output_frames: 480
            }
        );
    }

    #[test]
    fn gap_duplicate_and_rewind_require_reset_before_decode() {
        let mut timing = AudioTiming::new(1).unwrap();
        timing.map_block(1, 1000, 480, 0).unwrap();
        assert_eq!(timing.expected_next_input(), Some(1480));
        for packet in [1000, 999, 1960, 500_000] {
            assert!(timing.needs_reset(packet));
            assert!(timing.map_block(1, packet, 480, 0).is_err());
            assert_eq!(timing.expected_next_input(), Some(1480));
        }
        let mut recovered = AudioTiming::new(2).unwrap();
        assert!(!recovered.needs_reset(1960));
        assert_eq!(
            recovered.map_block(2, 1960, 480, 0).unwrap().output_rtp,
            1960
        );
    }

    #[test]
    fn wrap_and_large_integer_progress_do_not_accumulate_clock_drift() {
        let origin = u32::MAX - 499;
        let ffmpeg = scheduled_source(origin, 3, 0, 1024, 8);
        let fdk = scheduled_source(origin, 4, 1685, 1024, 8);
        for (rtp, value) in &fdk {
            assert_eq!(ffmpeg.get(rtp), Some(value));
        }
        let mut timing = AudioTiming::new(5).unwrap();
        let mut rtp = origin;
        // Start near the u32 half-range in integer progress without millions of
        // iterations; mapping depends only on exact packet RTP and stable delay.
        timing.input_frames = u64::from(u32::MAX) + 10_000;
        for _ in 0..10_000 {
            let mapped = timing.map_block(5, rtp, 1024, 1685).unwrap();
            assert_eq!(mapped.skip_frames, 0);
            assert_eq!(mapped.output_rtp, rtp.wrapping_sub(1685));
            rtp = rtp.wrapping_add(1024);
        }
        assert_eq!(timing.expected_next_input(), Some(rtp));
    }

    #[test]
    fn invalid_or_changing_metadata_never_advances_state() {
        assert!(AudioTiming::new(0).is_err());
        let mut timing = AudioTiming::new(1).unwrap();
        for (generation, frames, delay) in [
            (0, 1024, 0),
            (2, 1024, 0),
            (1, 0, 0),
            (1, 2049, 0),
            (1, 1024, 4097),
        ] {
            assert!(timing.map_block(generation, 1000, frames, delay).is_err());
            assert_eq!(timing.expected_next_input(), None);
        }
        timing.map_block(1, 1000, 1024, 1685).unwrap();
        assert!(timing.map_block(1, 2024, 1024, 0).is_err());
        assert!(timing.map_block(1, 2024, 480, 1685).is_err());
        assert_eq!(timing.expected_next_input(), Some(2024));
        timing.input_frames = u64::MAX;
        assert!(timing.map_block(1, 2024, 1024, 1685).is_err());
        assert_eq!(timing.expected_next_input(), Some(2024));
    }

    #[test]
    fn known_source_negative_controls_detect_wrong_sign_and_stale_origin() {
        let delay = 1685u32;
        let packet_offset = 3 * 1024;
        let pcm = modeled_decode(packet_offset, 1024, delay);
        let correct_source_index = packet_offset - delay as usize;
        let wrong_sign_index = packet_offset + delay as usize;
        assert_eq!(pcm[0], source(correct_source_index));
        assert_ne!(
            pcm[0],
            source(wrong_sign_index),
            "wrong sign must not pass the source-clock control"
        );
        let mut stale = AudioTiming::new(1).unwrap();
        stale.map_block(1, 1000, 1024, delay).unwrap();
        stale.map_block(1, 2024, 1024, delay).unwrap();
        assert!(
            stale.map_block(2, 3048, 1024, delay).is_err(),
            "new generation cannot inherit old priming progress"
        );
    }
}
