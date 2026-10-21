# Current state

Updated: 2026-10-21

## Runnable build
- Branch `feat/airplay-mvp`; local commits only. Run
  `dist/AirMirrorLite/AirMirrorLite.exe`. SHA-256:
  `A89C05D194657E7C73BB4E870C71F811DDA943B1E2B1B7074644E9172477C307`.
- Final phone check accepted: **responsive and correctly sized** after requested
  Photos scrolling, return to Settings and brief rotation. The app subsequently
  closed cleanly; no further phone interaction is pending for this correction.
- This local installation saves `994 2160` in
  `dist/AirMirrorLite/local/display-aspect.txt`. Normal launches load it and request
  phone-proportioned output; the final run needed no command-line override.
  Fresh installations without the file retain the normal widescreen request.

## Photo size and responsiveness
- Earlier logs proved Photos changed the transmitted source/display from
  994x2160 to 3840x2160. Keeping a portrait outline alone made the wide feed small.
  Requesting 994x2160 instead kept the source phone-proportioned, and the user
  confirmed that the full photo matched the phone. This is tested-phone evidence,
  not a compatibility claim for all senders. See RESEARCH.md for other receivers.
- The bounded, read-only saved aspect scales to 498x1080, 662x1440 or 994x2160.
  `--display-width` overrides it. Native codec/dimension limits are unchanged.
- The shell learns the first frame's normalized aspect. Matching ratios may
  rotate/resize; different-aspect media fits completely inside the established
  frame. Session-tagged messages reject stale formats/resets. Start from Home or
  Settings: physical orientation cannot be inferred from arbitrary media alone.
- Photos supplied presentation timestamps up to about 320 ms ahead of arrival.
  Screen-only sessions now present fresh decoded frames promptly. Audio SETUP
  restores shared-clock scheduling before PCM; audio teardown flushes old PCM
  before returning to interactive presentation. Mute and silence never switch
  modes. An audio transition can briefly hold the current picture to regain its
  deadline. Advertised latency remains an 80 ms hint, not an added local delay.
- Hardware decoding, bounded queues/IPC/logs, AppContainer, DPAPI and private-LAN
  scope remain. No new dependency, firewall or Defender change.

## Verification
- 56 Rust tests, cargo fmt, strict clippy and release build pass.
- Silent native suite: 100 decoder-bound H.264/HEVC GPU surfaces, two decoder
  instances, 125 presentations including synthetic checks, audio_samples=0.
  Synthetic pixel readback verifies full-image markers and letterboxing inside
  a retained portrait frame; no sender pixels or desktop capture are read back.
- Focused checks cover stable frame/default size, rotation, stale session
  messages, interactive-to-synchronized timing, future deadlines, paused redraw,
  resize/fullscreen and bounded ownership transitions. 360 AAC-ELD frames decode
  in the isolated worker with repeat hashes and sandbox checks; no PCM reaches
  WASAPI. Native codec/crypto sources are unchanged; prior ASan is historical.
- Real loopback control/ownership/negative-state/shutdown checks and all three
  saved-aspect /info profiles pass on this exact executable.
- Commands: cargo fmt --check; cargo test --locked --offline;
  cargo clippy --all-targets --locked --offline -- -D warnings;
  cargo build --release --locked --offline; scripts/Test-Media.ps1;
  scripts/Test-Control.ps1;
  python -O scripts/Test-VideoProfiles.py --display-aspect 994 2160;
  scripts/Package.ps1. Evidence: work/photo-final-*.log and
  local/{media-test,control-test,profile-test}/result.json.
- **Automated tests silent; no Computer Use.** Do not run --self-test or
  Test-Media.ps1 -Audio without a new explicit request. Normal Windows toolchain
  builds run outside the restricted sandbox; no protection was weakened.

## Physical evidence and remaining validation
- Final run: HEVC 994x2160 throughout, 1,156 decoder surfaces, 1,131 presentations,
  zero late drops, 26 interactive superseded-frame skips; clean joined shutdown.
  First picture was 177.9 ms after session claim, not phone-tap/display latency.
- The phone itself negotiated AAC-ELD during the user's interaction: logs show
  video_sync=0 -> 1, 147,068 audio samples, then audio teardown -> video_sync=0
  with screen mirroring retained. This verifies the live mode transition, not
  subjective sound quality or physical A/V sync. No sound playback was requested.
- The user confirmed lower delay after the earlier timestamp fix and responsive,
  correctly sized Photos on this final build. No numeric glass-to-glass latency
  is claimed. Logs did not independently show a landscape format during the
  final requested rotation check.
- Older H.264 picture/audio passed five minutes with user-confirmed sync and no
  freezes. Current physical A/V sync, 30-minute drift, lock/unlock and Wi-Fi-loss
  recovery remain unverified; do not substitute silent or loopback checks.
- Development package; distribution licensing remains unresolved. See
  VALIDATION.md and THIRD_PARTY.md. No keys, captures or private logs committed.
