# Current state

Updated: 2026-10-21

## Runnable build
- Branch `feat/airplay-mvp`; local commits only. Run
  `dist/AirMirrorLite/AirMirrorLite.exe`. SHA-256:
  `DD0981778EB31D366386706E34E2936EE45C963B1DEC8C1437EF16AD25320E06`.
- The corrected package is open and advertising. Default remains native HEVC
  3840x2160/up-to-60 fps when supported; no software fallback. Native icon,
  frameless phone view, proportional resize/fullscreen, volume/mute and collapsible
  controls remain. C toggles controls; 0 resets size.
- --latency-ms is an advertised output-latency hint (80 ms by default, 60–250).
  It is no longer incorrectly added to the sender's presentation timestamps.
  Audio and video still use one mapped clock; this is not a physical latency claim.

## Current corrections
- User confirmed connection is faster, but scrolling delay remained below half a
  second. A 60 ms trial felt similarly delayed but smooth.
- Live timing showed sender PTS already 75–85 ms ahead of receipt. The receiver
  added another 60 ms in the trial (80 ms normally). This duplicate offset is now
  removed from both video and audio; relative timing is preserved mathematically.
- A per-instance high-resolution one-shot timer follows the next frame deadline;
  audio/UI/packet/close events interrupt it. Due video uses the DXGI slot event.
  Coalesced queue signals cannot strand a backlog. No global timer changes.
- Source shape is published when its frame is selected for drawing, instead of
  when future frames decode. Processor cache keys include source dimensions.
- User reported the PC staying landscape while the iPhone remained portrait when
  viewing photos. This exact physical symptom remains pending on the corrected
  build; decoded/presented/client shapes and validated header sizes now distinguish
  sender-format behavior from a window geometry failure. Header sizes do not
  control orientation and no pixels are rotated speculatively.
- H.264 low-latency property typing is corrected. HEVC MF_LOW_LATENCY reads back 1;
  this installed HEVC transform lacks ICodecAPI, explicitly logged as unavailable.
- Existing bounded queues/IPC, audio-gap recovery, zero-capability AppContainer,
  DPAPI, private LAN scope and 1 MiB log remain; log lines cap at 2,048 characters.
  No new dependencies, service, firewall, Defender or codec-install changes.

## Verification
- 49 Rust tests, cargo fmt and strict clippy pass; release builds cleanly.
- Silent native suite: 100 decoder-bound H.264/HEVC GPU surfaces, two cached
  decoder instances, 116 presentations including synthetic geometry tests, and
  audio_samples=0. Decoder/encoder synthetic timing is not phone latency.
- Tests cover idle sleeping, future deadlines/no early presentation, slot ownership,
  audio/close wakeups, queue backpressure, and confirmed paused redraws. GPU and UI
  tests cover queued opposite-aspect frames, rapid portrait/landscape changes,
  automatic/user-sized geometry, fullscreen return and normal DPI suggestions.
- 360 AAC-ELD frames decode in the isolated worker with repeat hashes and sandbox
  checks; PCM is never submitted to WASAPI. Earlier ASan evidence remains valid
  for unchanged native codec sources; it was not rerun in this follow-up.
- Real loopback control/ownership/negative-state/shutdown and three /info profiles
  pass with the same executable hash as the package. Hint remains 80 ms in /info.
- Commands: cargo test --locked --offline; cargo clippy --all-targets --locked
  --offline -- -D warnings; cargo build --release --locked --offline;
  scripts/Test-Media.ps1; scripts/Test-Control.ps1;
  python -O scripts/Test-VideoProfiles.py; scripts/Package.ps1.
  Evidence: work/timestamp-*.log and local/{media-test,control-test,profile-test}.
- **All tests silent; no Computer Use.** Do not run --self-test or Test-Media.ps1
  -Audio without a new explicit request. Builds used the installed Windows
  toolchain outside the restricted sandbox; no protection was weakened.

## Physical evidence and next step
- Quiet scrolling/photo check on this exact build is pending. Two questions ask
  whether scrolling delay is lower and whether the PC still switches to landscape
  while the iPhone stays portrait. Observe logs after the same photo is left open.
- Earlier ab77a95 session: 837 decoded/834 presented, three late drops,
  first picture 329.4 ms after session claim. Diagnostic 60 ms trial: 4,722 decoded/
  presented, zero drops, user-confirmed smooth, but similar perceived delay.
  Both were silent; the latter predates the duplicate-offset correction.
- Earlier HEVC picture was confirmed at 994x2160 versus 498x1080. Older H.264
  picture/audio passed five minutes with user-confirmed sync and no freezes.
  Current physical A/V sync, end-to-end latency, 30-minute drift, lock/unlock and
  Wi-Fi-loss recovery remain unverified. Do not substitute mathematical clock
  tests or local submission timings for those observations.
- Development package; distribution licensing remains unresolved. See
  VALIDATION.md and THIRD_PARTY.md. No keys, captures or private logs committed.
