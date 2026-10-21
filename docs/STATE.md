# Current state

Updated: 2026-10-21

## Runnable build
- Branch `feat/airplay-mvp`; local commits only. Run
  `dist/AirMirrorLite/AirMirrorLite.exe`. Current SHA-256:
  `CCD1192625E6193A2CD2564621294CDEE810C8B2198B1639D40DA52E3755ED43`.
- The user confirmed the quiet current-build phone check: **Everything works**
  after requested Settings/Photos/rotation and disconnect/reconnect actions.
  Saved `local/display-aspect.txt` remains `994 2160`; no launch override is needed.
- All twelve implementation findings from the supplied final review were checked
  against source and addressed. Details and limits are in REVIEW_FOLLOWUP.md.

## Main corrections
- Native waiting is separate from dispatch. Owner/audio/flush transitions are
  applied under one gate before dispatch; audio SETUP, FLUSH and teardown wait for
  that boundary. Full TEARDOWN also stops producers and clears the native session
  before acknowledging, without clearing a newer owner.
- FLUSH fences ordinary/retransmitted audio by validated sequence and RTP time,
  including wrapping counters. A new transport clears old fences. Active audio
  with missing/invalid RTP-Info gets 455 plus a reconnect instruction; screen-only
  FLUSH does not poison later audio. Phone pause/seek compatibility is unverified.
- Endpoint and graphics faults have bounded owner-thread recovery and persistent
  actionable status. Unrelated capture-device notifications do not disconnect a
  healthy output route. Failed Stop/Reset releases the old endpoint immediately.
- Successful Present establishes shape/readiness; busy/occluded candidates cannot
  establish a false first frame. Validated aperture origin, visible extent and
  BT.601/709 full/studio range survive queued and paused frames.
- WASAPI prefill precedes Start; chrome Present is nonblocking with dirty retry.
  Minimized output drains dependencies and retains a bounded fresh picture without
  unnecessary presentation. New bounded metrics expose copy cost and audio service.
- Persistent errors have a software panel independent of D3D. Rail placement stays
  within the work area; native accessibility covers controls, popup parenting,
  range/focus/actions and bounded cross-thread requests.
- Worker permissions are provisioned explicitly for the exact EXE, not rewritten
  on each launch. Read-only deployment has per-user data fallback. Explicit bind
  can select a second eligible private adapter. No isolation/Defender/firewall change.
- Minimal unsigned PlayFair assembly patch removes signed-shift UB. Package target,
  PE machine, toolchain/source provenance, dependency notices and linked docs agree.

## Verified checks
- 64 Rust tests, fmt, strict Clippy and release build pass.
- Silent native suite: 100 decoder-bound GPU surfaces, 131 presentations including
  redraws, one forced busy Present, endpoint and graphics recovery tests, and
  audio_samples=0. Four decoder instances span the injected graphics rebuild.
- Intrinsic tests cover transition/wait ordering, first-present geometry, aperture
  and color pixels, minimized restore, prefill, persistent status, rail/DPI edges,
  fullscreen, UIA behavior, timeout and detached providers. No Computer Use.
- Isolated worker: 480 ELD/LC frames with pinned PCM hashes and reset comparisons.
  Separate MSVC ASan: 120 ELD + 120 LC, pinned hashes and known signal controls,
  1,024 FairPlay mutations and 1,024 malformed AAC packets, canaries intact.
- Clang shift instrumentation: bad control traps, 20 public expected keys/high-bit
  input pass. This is targeted verification, not exhaustive native-code safety.
- Loopback control, worker owned-copy deployment/idempotent provisioning, and all
  three saved-aspect /info profiles pass with the current executable hash.
- Windows 11 Pro 10.0.26200 / RTX 4070 SUPER driver 32.0.16.1692. Synthetic input
  preparation averages 0.002567 ms, maximum 0.0781 ms; no pooling is justified by
  that workload. Startup medians: baseline 396.6 ms, revised 380.8 ms (three warm
  runs each). Both are about 0.4 s; no significant or end-to-end speedup is claimed.
- Commands: cargo fmt --check; cargo clippy --all-targets --locked --offline --
  -D warnings; cargo test --locked --offline; cargo build --release --locked
  --offline; scripts/Test-Media.ps1; scripts/Test-Control.ps1;
  scripts/Test-WorkerDeployment.ps1; python -O scripts/Test-VideoProfiles.py
  --display-aspect 994 2160; scripts/Test-PlayfairUB.ps1;
  cargo run --manifest-path tests/native-sanitizer/Cargo.toml --locked --offline
  (CARGO_TARGET_DIR=work/san-target); scripts/Package.ps1.
  Evidence: work/review-followup-*.log and local result JSON; see REVIEW_FOLLOWUP.md.

## Physical evidence and remaining boundaries
- The user accepted the quiet current-build display/reconnect check. Observed
  telemetry includes HEVC 994x2160, 1,386 decoded / 1,366 presented frames and
  zero audio samples; first picture was 223.0 ms after session claim. These are
  snapshots, not a measured physical latency or sustained-performance result.
  Native startup/listener readiness was 264.4/313.9 ms; discovery took 808.9 ms
  after its phase start. User-reported rotation/reconnect is distinct from logged
  portrait snapshots. See local/review-followup-phone/result.json.
- Prior accepted build `1ec851b`: responsive, correctly sized Photos, HEVC 994x2160,
  real audio transport activation/deactivation and clean shutdown. Those phone
  audio results do not automatically accept the new audio lifecycle/FLUSH changes.
- Older H.264 picture/audio passed five minutes with user-confirmed sync/no freezes.
  Current subjective HEVC A/V sync, 30-minute drift, physical hotplug/device loss,
  lock/unlock, Wi-Fi loss, screen-reader behavior and glass-to-glass latency remain
  unverified. Separate-admin-owner deployment and real multi-adapter choice are
  also distinct from owned-copy and synthetic selection tests.
- Automated tests remain silent. Do not run --self-test or Test-Media.ps1 -Audio
  without a new explicit request. No phone captures, private logs or keys in Git.
- R1 remains an external distribution-rights gate: the static GPL PlayFair/FDK
  combination has no established compatible grant. -ForDistribution fails;
  ordinary packaging is local development only. Notices do not resolve licensing.
