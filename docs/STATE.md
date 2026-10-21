# Current state

Updated: 2026-10-21

## Rotation disconnect fixed; Photos landscape follow-up open
- The manual FFmpeg phone test reached 994x2160 hardware mirroring, then rotation
  hit the Rust HEVC eight-picture DPB guard. The rejected count was not logged.
- Replaced the fixed guard with HEVC level/area bounds (16/12/8/6 pictures) while
  preserving resolution/packet/depth limits; added scalar inline SPS diagnostics.
  Fixed duplicated artwork behind error cards and bounded narrow card sizing.
- 82 Rust tests pass on both backends; Clippy/formatting pass. Silent native tests
  pass 162 production-parser access units, four DPB16 declarations, 160 GPU surfaces,
  controls/status/recovery/rotation, 480 AAC frames and sandbox checks; zero audio.
  Lifecycle tests pass three ownership/teardown cycles and pending-read shutdown;
  profile/aspect tests pass 1080/1440/2160 at both 30 and 60 fps.
- Corrected candidate SHA-256:
  `0F6B9E3EF7A41DA65E7557C5E86C4F935DE473068894D7D043E89BD2AACA14BF`.
  The user confirmed rotation remains connected and correctly sized, then clarified
  that Photos landscape still shows a small upright photo in a portrait PC frame.
  Live metadata proves Settings changes from 994x2160/DPB6 to 994x458/DPB12; Photos
  instead continues sending 994x2160. The user closed this candidate after testing.
  Both FDK fallback hashes remain unchanged. Rotation-capability advertisement
  and scalar decoder-orientation metadata are the next controlled investigation.
- The opt-in rotation-advertisement trial is now open at the candidate path,
  SHA `E74F70DD72E7F3B893216E8F28D05C44D6137A88F66ECF701AEBFBCE87CA4346`;
  it pairs display rotation=true with ScreenRotate bit 8, keeping the aspect hint.
  82 tests, strict Clippy, silent native checks and wire profiles pass. The user
  reported that it still fails; their screenshot shows the whole Photos interface
  sideways in the portrait frame. The trial produces source=994x2160 with a
  landscape display hint=994x458, while MF rotation is absent and both apertures
  remain full-frame. Normal packaging does not enable this feature.
  Previous FFmpeg binary: `work/rotation-verified-candidate/`; trial manifest and
  checks: `local/rotation-advertisement/`. See ROTATION_REGRESSION.md for limits.
- Further trial A761D568... passed 91 Rust tests and full silent GPU checks but
  still showed sideways Photos: this phone sends no rotation SEI. Research now
  identifies configuration byte5 as SymmetricTransform (0/3/4/7 map to
  0/180/90/270 clockwise). The unused SEI experiment has been removed.
- Current follow-up uses that actual transport metadata with verified per-frame
  GPU rotation, plus an opt-in legacy event channel for full updateDisplayInfo
  requests. Dynamic canvas requests follow only successfully presented frames of
  the established hint shape; no image cropping or destination-rectangle guesses.
  Built candidate SHA-256 is
  `AF3B9439AFB4BC220FF4F42ED4B03FD6B7359B549B46390B6DC1F1D175A1C183`.
  95 Rust tests pass with both default FDK and experimental FFmpeg builds;
  strict Clippy, silent native/GPU/worker checks and all three
  advertised profiles pass. GPU evidence includes all quarter-turns, exact PTS
  associations and busy-present cache preservation; 160 hardware surfaces,
  162 parser-validated access units and zero endpoint audio. Physical Photos
  convergence remains pending; normal packaging still disables both experiments.
  This exact build is open at the candidate path; discovery readiness is verified.
  Manifest, launch and copied test evidence: `local/transport-rotation-phone-test/`.
- Full bounded interaction matrix, commands, evidence and physical limitations:
  ROTATION_REGRESSION.md. Launch: `local/rotation-phone-test/launch.json`.

## Working receiver preserved
- Accepted source remains `feat/airplay-mvp` at `e06321c`.
- `dist/AirMirrorLite/AirMirrorLite.exe` and the extra
  `dist/AirMirrorLite-fallback-e06321c/AirMirrorLite.exe` remain byte-identical:
  SHA-256 `CCD1192625E6193A2CD2564621294CDEE810C8B2198B1639D40DA52E3755ED43`.
  Original identity/logs stay in place and are not copied into the candidate.
- Earlier quiet Settings/Photos/rotation/reconnect acceptance and five-minute
  H.264 picture/audio acceptance belong to that build. They are not FFmpeg evidence.

## Decoder delay implemented; separate FFmpeg receiver candidate
- Branch: `codex/ffmpeg-aac-experiment`. Native PCM replies now atomically include
  validated additional DSP delay and configuration generation. Integer RTP mapping
  subtracts that delay, trims pre-generation priming and adds no FIFO/padding.
- FDK LC delay is 1,685 samples (38.2086 ms); ELD and FFmpeg additional delay are
  zero. FFmpeg's raw context delay stays diagnostic, not a clock substitute.
- Flush/loss/errors reconfigure decoder and mapper together. Native failures latch
  until configure. A transport rewind fence survives decoder resets; explicit
  FLUSH/new transport is required to rewind. Existing session/flush queue fencing
  remains. Encoder/filterbank priming is not guessed or recalibrated.
- `scripts/Package.ps1 -FfmpegCandidate` creates
  `dist/AirMirrorLite-ffmpeg-candidate/AirMirrorLite.exe`, linking pinned minimal
  FFmpeg AAC/avcodec/avutil with no FDK. Default packaging still selects FDK.
  The optional `decoder-lab` feature separately blocks receiver startup.
- The local candidate retains the known `994 2160` display-aspect preference.
  At the user's request, the accepted receiver was stopped and the candidate was
  opened for manual phone testing. Its log confirms listener/discovery readiness;
  phone connection and physical A/V acceptance remain pending. Both fallback
  hashes were verified unchanged. Launch evidence: `local/ffmpeg-phone-test/launch.json`.
  That initial candidate was built from `70dff1d`; the rotation correction above
  supersedes it. No sound tests or Computer Use were run.

## Earlier decoder-delay verification (70dff1d)
- 75 Rust tests pass on both backends; strict Clippy and formatting pass.
- 17 analyzer tests include wrong-sign/missing trim, shifted PCM, stale generation
  and delay, resource/report bounds and controls that must fail.
- Ten-session lab comparison: `local/decoder-experiment/delay-qualified-final/`.
  Four fixtures, 10,080 measured packets per backend plus guards. Raw LC offset
  remains -1,685; normalized nonperiodic ELD/LC both have zero sample offset and
  at most one i16-unit waveform difference. Raw adoption still fails; the explicit
  normalized source-clock qualification passes without relaxed tolerances.
- Actual candidate binary also passes its separate synthetic timing comparison:
  `local/decoder-experiment/receiver-candidate-delay-final/` (two sessions).
- Direct FFmpeg C-ABI bounds/PCM+metadata canaries and explicit reset checks pass.
  FDK/PlayFair ASan passes, including 1,024 freshly initialized malformed AAC cases.
  This is not whole-FFmpeg sanitizer coverage or exhaustive fuzzing.
- Candidate silent native/GPU and restricted-worker checks pass: 100 hardware
  decoder surfaces, AAC-ELD/LC golden hashes across resets, zero audio submitted.
  Loopback ownership/framing/state/shutdown checks pass. Evidence:
  `local/media-test/result.json`, `local/control-test/result.json`.
- No audible tests, Computer Use, phone capture, firewall/Defender changes or
  publication. Final executable/source identity is in the candidate's BUILD.json.

## Commands and remaining boundary
- `cargo test --locked --offline [--features ffmpeg-aac]`; strict Clippy; cargo fmt.
- `scripts/Test-FFmpegBoundary.ps1`; `cargo run --manifest-path
  tests/native-sanitizer/Cargo.toml --locked --offline` (CARGO_TARGET_DIR=work/san-target).
- `python -B -m unittest discover -s tests/decoder-experiment -p test_*.py`;
  `python -B -O scripts/Test-DecoderExperiment.py --reference <FDK-lab-exe>
  --candidate <FFmpeg-exe> --sessions 10 --output <new local directory>`.
- `scripts/Package.ps1 -FfmpegCandidate`; `scripts/Test-Media.ps1 -Executable
  <candidate>` and `scripts/Test-Control.ps1 -Executable <candidate>`; no -Audio.
- Physical FFmpeg A/V sync, long-run drift and audible near-clipping behavior
  remain unverified. FFmpeg saturates immediately; it does not reproduce FDK's
  lookahead limiter. See DECODER_EXPERIMENT.md for the contract and limits.
- Open source is the chosen direction, but no public release is authorized or
  prepared. The FDK distribution gate remains; candidate source/notices and
  remaining release-rights review are still required. No audible tests without
  a new explicit request.
