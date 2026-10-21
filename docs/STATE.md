# Current state

Updated: 2026-10-21

## Working receiver preserved
- Accepted source: `feat/airplay-mvp` at `e06321c`. Continue to run
  `dist/AirMirrorLite/AirMirrorLite.exe`; it has not been replaced.
- Additional fallback: `dist/AirMirrorLite-fallback-e06321c/AirMirrorLite.exe`.
  Both SHA-256 values remain
  `CCD1192625E6193A2CD2564621294CDEE810C8B2198B1639D40DA52E3755ED43`.
  The original identity/logs remain in place; they were not copied to the backup.
  Saved phone aspect `994 2160` and explicit worker execute permission are preserved.
- The user accepted the previous quiet Settings/Photos/rotation/reconnect check.
  Earlier H.264 picture/audio passed five minutes with user-confirmed sync.
  Current physical HEVC A/V drift and adverse-device recovery remain unverified.
  See REVIEW_FOLLOWUP.md and VALIDATION.md for those separate build histories.

## FFmpeg experiment completed; not promoted
- Active branch: `codex/ffmpeg-aac-experiment`. The `ffmpeg-aac` feature builds
  only pinned FFmpeg 9.0.2 native AAC plus avcodec/avutil, with no FDK linked.
  Source signature, SHA-256, config and library hashes are checked. Its isolated
  source-build recipe replayed successfully; byte-identical builds are not claimed.
- Both comparison builds enable `decoder-lab`: normal receiver startup is rejected
  before native device/discovery initialization. They only run synthetic decoding
  in the existing capability-free AppContainer/job/IPC path. No live receiver,
  video rendering, clock or audio-output policy was changed for this experiment.
- All four fixtures decode correctly. Repeated sessions/resets, sample counts,
  output bounds, malformed recovery and sandbox checks pass. Aligned waveform
  differences are at most one i16 value in the tested signals.
- ELD has zero relative sample offset. LC produces content **1,685 samples
  (38.2086 ms) earlier** than FDK, so the unchanged timing/adoption gate FAILS.
  Runtime metadata and source confirm FDK's 1024-sample concealment buffer plus
  661-sample limiter lookahead. No magic padding or tolerance relaxation was added.
- FFmpeg showed lower median decoder/IPC call time in this workload. Observed
  worker-private peaks: FDK 2.43 MiB, FFmpeg 4.08 MiB. These are synthetic local
  measurements, not physical A/V sync, unsampled peak memory or sustained phone
  performance. Full results and limitations: DECODER_EXPERIMENT.md.

## Verification
- 66 Rust tests, strict Clippy, Rust formatting and isolated release builds pass.
- Ten Python analyzer tests pass, including a delayed signal that must fail even
  when its aligned waveform matches. Nonperiodic signals remove tone ambiguity.
- Direct C-ABI boundary/canary tests pass for both codecs, including malformed
  input, reconfigure/recovery and latched contract failures. This is not full
  FFmpeg sanitizer coverage. Lab normal-start rejection and fallback hashes pass.
- Final comparison: `local/decoder-experiment/qualified-comparison/comparison.json`.
  Both per-backend runs pass; cross-backend adoption remains false for LC timing.
  FDK lab SHA: `A4C04DFD3EF2694BD42628B4C462873DE826C0B651D028010DDC2251C0ACFE51`.
  FFmpeg lab SHA: `9BAF829FF90535DC39298BEC77B049583CE4FE955AD271E7EE22FF385509B7FA`.
- Commands: scripts/Build-FFmpeg.ps1 -Fetch; cargo build --release --locked
  --offline --features decoder-lab --target-dir work/decoder-fdk-target;
  cargo build --release --locked --offline --features ffmpeg-aac --target-dir
  work/decoder-ffmpeg-target; scripts/Test-FFmpegBoundary.ps1;
  python -B -m unittest discover -s tests/decoder-experiment -p test_*.py;
  python -B -O scripts/Test-DecoderExperiment.py --reference
  work/decoder-fdk-target/release/airmirror-lite.exe --candidate
  work/decoder-ffmpeg-target/release/airmirror-lite.exe.
  The comparison correctly exits 1 for the documented timing mismatch.
- All tests were silent; no Computer Use, phone interaction, recording, global
  tool installation, publication, firewall change or Defender weakening occurred.

## Next step / release boundary
- Define backend-neutral PCM timestamp/delay and limiting behavior, validate it
  through configure/flush/reset, then obtain separately authorized physical A/V
  evidence before switching the receiver. The experiment does not establish that
  the old LC physical offset was correct or justify simply copying its delay.
- Open-source release is the chosen direction. The normal FDK build's known
  PlayFair/FDK distribution gate remains; the FFmpeg target avoids that link mix
  but still requires complete release/source/notices and remaining-rights review.
  Do not run audible probes without a new explicit request.
