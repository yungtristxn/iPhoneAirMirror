# Current state

Updated: 2026-10-22

## Screen-off blackout: automated checks passed
- User accepted the independent Photos-view orientation limitation and declined
  a manual layout control. New request: PC viewport goes black when the iPhone
  screen turns off/locks, then restores on resume. No idle/photo timeout heuristic.
- Sender configuration byte 6 values 0x56/0x5e mean suspended; 0x16/0x1e resume,
  matching pinned UxPlay handling and live options 0x011e -> 0x015e. A priority
  epoch/generation boundary hides video independently of queue capacity. Native
  composition shows opaque black, clears both paused caches and waits for a fresh
  matching frame on resume; audio and window shape are preserved.
- 111 Rust tests pass on FFmpeg/default backends; strict Clippy, formatting,
  release build, silent native GPU checks (190 GPU surfaces; zero audio samples)
  and three lifecycle cycles pass. Includes resize/fullscreen/restore, busy
  presentation, stale output and rapid lock/unlock. Read-only second review found
  no concrete correctness issue. No Computer Use or audible tests.
- Tested executable SHA-256:
  `7B65C9ACA58FC8C5C4EDB13B98DF198E032A575B491FA7112AD7033926FB1F31`.
  Baseline: `local/screen-off-baseline/`; checks: `local/screen-off-verification/`;
  lifecycle: `local/control-lifecycle/20261022T192816883327Z/result.json`.
- Physical lock/unlock and final desktop appearance on this build are pending.
  Next: connect from silent Settings, lock/wake/unlock twice. Verify black while
  suspended and fresh picture on resume. Independent Photos output without a
  suspension signal cannot reveal physical phone power state. SCREEN_OFF.md.

## Verified audio/display fixes
- RTSP event replies from this iPhone omit CSeq. The receiver now accepts that
  observed format on its serialized event connection; explicit wrong/duplicate
  sequences remain rejected. Optional failures retain the socket until teardown.
- Live 3A2CDD... evidence confirms repeated display acknowledgments for 2160x994
  and 994x2160. The user reports photo opening now updates correctly, but turning
  the phone while a photo remains open still does not. The user accepts that
  limitation for now; do not call all Photos rotation supported. See
  ROTATION_REGRESSION.md.
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
- The muted physical check kept the same photo open while turning sideways and
  upright. User reports
  no error; the log confirms 604,169 submitted frames while muted, native queue
  peak 30/32 and two acknowledged display updates. No audible sync/quality claim.
  Rotation while the photo stays open remains stuck. User now confirms the
  iPhone interface rotates while the PC displays only the photo. The latest log
  shows valid landscape/portrait acknowledgments but no changing transform or
  dimensions during the remaining photo view. This is consistent with Photos'
  independent external-display content; no reliable receiver-side physical-angle
  input was found. Evidence: `local/photos-external-feed-confirmed/`. Automatic
  in-photo orientation remains unresolved. Do not infer it from pixels/APStats.
- Prior implementation committed as `11a3185`. Its muted physical run confirmed
  muted=1 and audio_samples=0 at startup. Launch and
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
