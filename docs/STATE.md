# Current state

Updated: 2026-10-21

## Runnable build
- Branch `feat/airplay-mvp`; local commits only. Run
  `dist/AirMirrorLite/AirMirrorLite.exe`. SHA-256:
  `411F3F2F3318712880725825CD1F1668C731915A53AA68873F4C5C99DCE613F6`.
- The verified package is open and advertising. Default remains capability-gated
  HEVC 3840x2160/up-to-60 fps, common 80 ms playout; `--resolution 1080/1440`,
  `--fps 30` and `--latency-ms 60..250` remain available. No software fallback.
- Frameless phone view, proportional resize/fullscreen, embedded icon,
  volume/mute and collapsible controls remain. C toggles controls; 0 resets size.

## Performance and quality changes
- Reviews covered native media/IPC, Rust transport/session state, UI and tooling.
  Confirmed actionable findings were fixed; details are in VALIDATION.md.
- Video presentation has a one-frame DXGI budget and does not block on a full
  display queue. The newest due decoded frame is retained; compressed
  dependencies and existing queue bounds remain intact.
- WASAPI uses the documented shared/event-driven initialization contract and
  logs actual capacity. Audio recovers from gaps beyond the jitter window;
  unmatched timing replies cannot suppress the liveness timeout.
- Codec IPC reuses bounded scratch buffers and puts both writes and reads under
  one two-second deadline. Timestamp conversions reject overflow. Unchanged
  status polling no longer allocates. SETUP stages are readable and explicit.
- Toolbar moves reuse existing artwork/resources. Volume leave grace, stale
  timers, capture loss, repeated key activation and partial painter init are fixed.
- Packaging proves release freshness before writing provenance. Tests identify
  the actual artifact and clear stale successes. Measurement includes its endpoint
  and detects PID reuse; ASan watches external native inputs and rejects empty fixtures.
- Native startup/discovery remain asynchronous with cached decoders from the
  preceding pass. The AppContainer, DPAPI, private LAN scope and 1 MiB log bound
  remain. No new dependency, service, firewall, Defender or codec-install changes.

## Verified evidence
- 44 Rust tests, cargo fmt and clippy with -D warnings pass; release builds cleanly.
- Silent native UI/icon/H.264/HEVC suite: 100 decoder-bound GPU surfaces,
  two decoder instances, 103 presentations, audio_samples=0. Every paused resize
  and fullscreen transition must actually present within one second.
- 100 intrinsic window moves caused no added palette paints/allocations or GDI
  objects. Blocked IPC and display-slot backpressure regressions pass.
- Isolated codec test decoded 360 AAC-ELD frames with identical hashes across
  resets, plus FairPlay and sandbox checks. It never submits PCM to WASAPI.
- ASan passed 1,024 FairPlay mutations, 120 AAC fixture frames and 1,024 malformed
  AAC inputs with intact canaries. Empty-fixture negative check exits 5.
- Real loopback control/ownership/malformed-state/shutdown and all three /info
  profile tests pass against the same executable hash as the package.
- Startup median 383.9 ms versus 390.9 ms before this pass: roughly unchanged at
  0.4 seconds. This is process-to-local-OPTIONS timing, not iPhone tap-to-picture.
  Packaged discovery completed in 786.3 ms after listener readiness.
- Passive 20-second idle sample: after startup, 0–0.313% of one core,
  77.83–77.89 MiB private memory; final three samples hold 1,037 handles.
  No active-phone throughput, GPU-memory or numeric end-to-end claim.
- Commands: Build.ps1 -Test; cargo clippy --all-targets --locked --offline --
  -D warnings; Test-Media.ps1; Test-Control.ps1; python -O scripts/Test-VideoProfiles.py;
  native-sanitizer cargo run; Measure-Startup.py; Measure-Receiver.ps1.
  Full paths, command details and raw local evidence are in VALIDATION.md.
- **Silent tests only; no Computer Use.** Do not run --self-test or Test-Media.ps1
  -Audio without a new explicit request. Native Windows builds used the normal
  installed toolchain outside the restricted sandbox; no protection was weakened.

## Physical evidence and next step
- Next: quiet iPhone reconnect and scroll/pause check on this build. First-picture
  speed and perceived phone-to-screen delay have not yet been accepted by the user.
- Earlier real iPhone HEVC picture was confirmed at 994x2160 versus 498x1080,
  nearly four times the pixels. Older H.264 picture/audio passed five minutes
  with user-confirmed sync and no freezes. These predate the current scheduling
  and WASAPI changes; current physical A/V sync is not re-tested.
- End-to-end latency, 30-minute drift, lock/unlock and Wi-Fi-loss recovery remain
  unmeasured/unverified. Synthetic recovery checks do not establish those results.
- Development package; distribution licensing remains unresolved. See
  VALIDATION.md and THIRD_PARTY.md. No keys, captured media or private logs committed.
