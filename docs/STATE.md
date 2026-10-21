# Current state

Updated: 2026-10-21

## Runnable build
- Branch `feat/airplay-mvp`; local commits only, no push or publication.
- Run `dist/AirMirrorLite/AirMirrorLite.exe`. Executable SHA-256:
  `3750CD0E204BD7C8D79C8AE62C8848F2D94A22EA161CE85BB4921851750C5DB0`.
- Frameless rounded phone view; native DirectComposition video and a thin rim.
  Actual video aspect drives the window, including rotation. Drag the picture
  to move, bottom-right curved grip to resize, reset button / `0` for default size.
- Side controls: hover speaker for volume slider, click to mute/restore, default
  size, fullscreen, close. `M` mute, Up/Down volume, F11/double-click fullscreen,
  Tab/Enter controls, D disconnect. PC-local gain leaves phone volume independent.
- Native DNS-SD, MF/D3D11 hardware H.264, synchronized WASAPI audio, separate UI
  thread, bounded transport/queues and DPAPI identity remain in place. AAC/FairPlay
  retain the zero-capability AppContainer and one-process/256 MiB job.
  No new third-party source, services, firewall changes or Defender changes.

## Current verification
- 26 Rust tests; clippy with `-D warnings`; offline release build: pass.
- Hidden native UI tests: captionless client geometry, portrait/landscape ratio,
  bottom-right resize, default reset, rotation during fullscreen, hover slider,
  volume clamps, mute restoration, palette ownership, close/disconnect events.
- Native GPU/audio probe: 71 hardware decoder surfaces, over 60 presentations and
  57,600 AAC-ELD PCM frames through WASAPI; landscape/portrait, paused redraw,
  resize/fullscreen and isolated worker checks pass on RTX 4070 SUPER.
- Real loopback control/single-owner/invalid-input checks and timed shutdown pass.
- Native-rendered idle, connecting, portrait, landscape, compact and control PNGs
  inspected under ignored `local/qol-test/renders`. Final validation used internal
  tests/artwork diagnostics, with no further Computer Use after the user's request.
  These are not screenshots of the final desktop composition or a physical phone.
- Hidden idle sample before the final palette-position adjustment, 10.01 seconds: 0.156% of one logical core, private memory
  60.32→60.35 MiB, handles 784→786; clean timed exit, empty stderr. Not playback data.
- Commands: `scripts/Build.ps1 -Test`; `cargo clippy --all-targets --locked --offline
  -- -D warnings`; `scripts/Test-Media.ps1 -UiRenderDirectory local/qol-test/renders`;
  `scripts/Test-Control.ps1 -Executable target/release/airmirror-lite.exe`.
  MSVC builds use the installed toolchain outside the restricted sandbox because
  its librarian previously failed with LNK1114/error 5. No protection was weakened.

## Prior physical evidence and remaining checks
- Before this UI change, real iPhone discovery, pairing/FairPlay, H.264 picture and
  44.1 kHz stereo AAC-ELD worked. The user confirmed pause/resume and a five-minute
  synchronized run without freezes, explicitly shortening the original 30 minutes.
- Prior package `345A6C9E448C732320B02911543A1BA41C44A2F568A4C68594469365EAB5746F`
  passed a live playback CPU sample: host 1.871% + codec 0.423% of one core.
  Earlier GPU Video Decode counters measured 1.89–1.93%; prior live close took
  177 ms and left no receiver/worker/listeners/mDNS reply. These are historical
  measurements, not measurements of this UI build. Details are in VALIDATION.md.
- The polished shell still needs a fresh physical iPhone check of picture, sound,
  controls and final desktop appearance. No new physical acceptance is claimed.
- Lock/unlock, Wi-Fi-loss recovery, 30-minute drift and glass-to-glass latency remain
  unverified. The 120 ms playout setting is not an end-to-end latency measurement.
- Local development build; distribution licensing remains unresolved. MSVC x64
  runtime is required. No phone keys, captured media or private logs are committed.
- Next: use the packaged build for a short physical phone/controls check. Source,
  automated validation and packaging are complete; no additional phone action is
  required to reproduce the native regression tests.
