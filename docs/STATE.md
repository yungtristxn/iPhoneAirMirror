# Current state

Updated: 2026-10-22

## Current fixes and remaining Photos check
- RTSP event replies from this iPhone omit CSeq. The receiver now accepts that
  observed format on its serialized event connection; explicit wrong/duplicate
  sequences remain rejected. Optional failures retain the socket until teardown.
- Live 3A2CDD... evidence confirms repeated display acknowledgments for 2160x994
  and 994x2160. The user reports photo opening now updates correctly, but turning
  the phone while a photo remains open still does not. That case remains open;
  do not call all Photos rotation accepted. See ROTATION_REGRESSION.md.
- That run also hit an audio-start burst: 32 native PCM blocks queued while all
  1,036 WASAPI frames were still padded. Admission now waits for capacity in the
  existing bounded queues. Producer retries cancel on stop/owner/FLUSH changes.
  Genuine failures flush once, stop the burst and show a concise reconnect card.
- New --mute option starts normal reception muted for quiet physical checks;
  decoding and negotiated audio synchronization remain active. No audible tests
  or Computer Use. Details and exact evidence: AUDIO_BACKPRESSURE.md.
- Verified build SHA-256:
  `8972816C665941D5C217CF1393ACCDE0BC5A7E2AC2B6B335DB8AD38304AE9E94`.
  107 Rust tests, strict Clippy/formatting, release build and silent native tests
  pass. Native tests include a 96-block in-memory audio burst, 160 hardware video
  surfaces and codec/worker checks; zero audio submitted. Three lifecycle cycles
  and a muted loopback startup/clean shutdown pass.
- Next physical check: reconnect to the muted build, open one still photo and
  keep it open while turning sideways and upright; inspect configuration/angle
  diagnostics and confirm the audio error stays absent. No sound playback needed.
- Implementation committed as `11a3185`. This exact candidate is open with
  --mute; its live startup stats confirm muted=1 and audio_samples=0. Launch and
  copied checks: `local/photos-audio-burst-fix/`. Verified source-included ZIP:
  `dist/AirMirrorLite-Windows-x64-audio-burst-fix-2026-10-22.zip`.

## Evidence and source checkpoints
- Latest native: local/media-test/result.json; mute: local/muted-startup-check/.
- Lifecycle: local/control-lifecycle/20261022T184724653263Z/result.json.
- Original event failure: local/photos-event-failure/ (F3B058...);
  HTTP-only trial: local/photos-rtsp-ack-failure/ (1FD895...);
  successful RTSP ACK plus audio burst: local/photos-audio-burst-failure/ (3A2CDD...).
- Source checkpoints: 77717dc measured connection improvements; b98457b removed
  Windows Private-profile gate; 2ba01a1 retained event TCP on optional failure;
  80618bf accepted observed omitted-CSeq RTSP acknowledgments.
- User confirmed first picture appears sooner. Synthetic first-connection setup
  median fell 45.497 -> 6.909 ms; video TCP stage 21 ms -> below 1 ms log resolution.
  These are not phone tap-to-picture measurements. PERFORMANCE_FOLLOWUP.md has
  commands, memory tradeoff and evidence. The 1 MiB log cap now truncates correctly.

## Preserved fallback and packaging
- Branch: codex/ffmpeg-aac-experiment. Accepted FDK source stays feat/airplay-mvp
  at e06321c; dist/AirMirrorLite and dist/AirMirrorLite-fallback-e06321c EXEs retain
  SHA-256 `CCD1192625E6193A2CD2564621294CDEE810C8B2198B1639D40DA52E3755ED43`.
- Experimental FFmpeg receiver: dist/AirMirrorLite-ffmpeg-candidate; independent
  identity/logs and 994 2160 aspect hint. Prior candidates are preserved in work/.
- Friend ZIP uses only FFmpeg with complete matching application/dependency
  sources, GPLv3 license and notices, no pairing identity or previous logs.
  Package-Share.py verifies a fresh build matches the tested candidate. Default
  Package.ps1 still builds FDK; -FfmpegCandidate omits rotation/display experiments.
- Open-source sharing was authorized; no public upload. FDK redistribution remains
  blocked. See THIRD_PARTY.md and NOTICE.md. Physical FFmpeg A/V synchronization,
  long-run drift and near-clipping dynamics remain unverified; DECODER_EXPERIMENT.md
  records timing qualification and the limiter difference.

## Commands
Use CARGO_HOME=work/cargo and the explicit experimental features:
```
cargo test --locked --offline --features ffmpeg-aac,display-update-experiment --target-dir work/ffmpeg-receiver-target
cargo clippy --all-targets --locked --offline --features ffmpeg-aac,display-update-experiment --target-dir work/ffmpeg-receiver-target -- -D warnings
cargo build --release --locked --offline --features ffmpeg-aac,display-update-experiment --target-dir work/ffmpeg-receiver-target
./scripts/Test-Media.ps1 -Executable work/ffmpeg-receiver-target/release/airmirror-lite.exe
python -B scripts/Test-ControlLifecycle.py --exe work/ffmpeg-receiver-target/release/airmirror-lite.exe
```
Never add -Audio or run --self-test without a new explicit audible-test request.
