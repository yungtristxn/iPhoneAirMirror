# Current state

Updated: 2026-10-21

## Runnable build
- Branch `feat/airplay-mvp`; local commits only. Run
  `dist/AirMirrorLite/AirMirrorLite.exe`. SHA-256:
  `0EA87953FEEC0C7376E013622C8BA1F45C2F326AF567551098419A00C82C6E47`.
- Higher-resolution native HEVC remains enabled: default request 3840x2160,
  now up to 60 fps. --resolution 1440/1080 and --fps 30 remain available.
  Native HEVC activation/GPU support gate advertisement; no software fallback.
- Shared video/audio playout now defaults to 80 ms (previously 120).
  --latency-ms accepts 60–250; 120 restores the former target. /info, RECORD
  latency and the shared clock agree. This is not measured glass-to-glass delay.
- Discovery registrations run concurrently without blocking the listener/media
  loop. Pending registration is cancellable; completion signals remain observable.
- Two cached decoder objects replace repeated activation. Disconnect clears stream
  state, media types and GPU surfaces; configuration prepares the next decoder
  before its first frame. A failed reset discards that cached instance.
- Initial video waits briefly for clock sync; delayed/reordered timing replies
  are accepted once each. A new epoch's first keyframe is retained across an owner
  snapshot race. Idle video is allowed; partial records retain one shared deadline.
- Frameless phone view, high-resolution app icon, proportional resize/fullscreen,
  volume/mute and collapsible controls remain. C toggles controls; 0 resets size.
  The audio/FairPlay AppContainer, bounded queues, DPAPI and Private-LAN scope remain.
  No service, firewall, Defender or codec-install changes.

## Verification
- 41 Rust tests and clippy with -D warnings pass. Added startup clock/ownership,
  NTP replay/reorder, idle/partial video, cancellation and common-delay regressions.
- Silent native UI/icon/H.264/HEVC probes pass: 100 decoder-bound GPU surfaces,
  only two decoder instances across format changes, audio_samples=0. Includes
  repeated discovery-completion polling and native decoder reset/recovery.
- Real loopback control/lifecycle and /info profile checks pass: 1080/1440/2160
  request 60 fps and report 80,000 microseconds output latency.
- Listener readiness from process launch: baseline 1939.1/1904.4/1908.9 ms;
  new 357.3/392.8/397.5 ms. Medians 1908.9→392.8 ms (about 79% lower).
  Fresh processes with existing OS caches; not a phone-tap measurement.
- Concurrent registration completes about 0.78–0.81 s after listener readiness.
  Three real discovery runs all completed and shut down cleanly. Early pending
  registration cancellation also passed; native completion-signal regression
  protects against an intermediate auto-reset-event mistake caught during tests.
- Commands: Build.ps1 -Test; cargo clippy --all-targets --locked --offline --
  -D warnings; Test-Media.ps1; Test-Control.ps1; Test-VideoProfiles.py;
  Measure-Startup.py --exe <exe> --label <name> --discovery --address <private IPv4>.
  Close the normal receiver before the discovery benchmark. Logs stay under local/.
- **Silent tests only; no Computer Use.** Do not run --self-test or Test-Media.ps1
  -Audio without a new explicit user request. No sound tests were run here.
- MSVC builds run outside the restricted sandbox because its librarian previously
  failed with LNK1114/error 5. No protection was weakened.

## Physical evidence and outstanding check
- User clarified the current issue is first connection and phone-to-window delay.
  This build is open for a quiet connection, scrolling and pause check. The user
  question is pending; perceived latency and current A/V sync are not yet verified.
- Earlier real iPhone HEVC picture was confirmed at 994x2160 versus 498x1080,
  almost four times the pixels, with decoder-bound GPU surfaces. Prior H.264
  picture/audio passed five minutes with user-confirmed sync and no freezes.
  These physical results predate the new 60-fps/80-ms scheduling settings.
- End-to-end latency, 30-minute drift, lock/unlock and Wi-Fi-loss recovery remain
  unmeasured/unverified. Keep those separate from local startup and GPU results.
- Development package; distribution licensing unresolved. See VALIDATION.md and
  THIRD_PARTY.md. No keys, captured media or private logs are committed.
