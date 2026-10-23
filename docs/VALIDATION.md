# Validation and release gates

Build, parser/simulated-sender, GPU, physical iPhone picture, audible A/V and
performance evidence are separate. An earlier pass does not qualify a new binary.
Current results and artifact identity are in STATE.md.

## Building

Requires Windows x64, Rust, Visual Studio C++ x64 tools/Windows SDK, PowerShell 7
and Python 3. FFmpeg's pinned tools and source are described in
[DECODER_EXPERIMENT.md](DECODER_EXPERIMENT.md). First build:

```powershell
./scripts/Build-FFmpeg.ps1 -Fetch
./scripts/Build.ps1 -Fetch -Test
./scripts/Package.ps1
```

Default builds use FFmpeg AAC, legacy pairing and display updates. `-Profile Hap`
is experimental; `-Profile FdkReference` is local-only and never release packaged.
Use fresh package destinations to preserve fallback builds. The source-complete
portable archive includes an offline REBUILD.txt recipe and dependency notices.

## Silent local checks

From PowerShell 7 at the repository root:

```powershell
$env:CARGO_HOME = Join-Path $PWD 'work/cargo'
cargo fmt --all --check
cargo test --locked --offline
cargo clippy --all-targets --locked --offline -- -D warnings
./scripts/Build.ps1
./scripts/Test-Media.ps1 -Executable target/release/airmirror-lite.exe
./scripts/Test-Control.ps1 -Executable target/release/airmirror-lite.exe
python -B scripts/Test-ControlLifecycle.py --exe target/release/airmirror-lite.exe
python -B scripts/Test-VideoProfiles.py --exe target/release/airmirror-lite.exe --display-aspect 994 2160 --expect-rotation
./scripts/Test-WorkerDeployment.ps1 -Executable target/release/airmirror-lite.exe
./scripts/Test-FFmpegBoundary.ps1
python -B -m unittest discover -s tests/connection-metadata -p 'test_*.py'
python -B -m unittest discover -s tests/decoder-experiment -p 'test_*.py'
python -B -m unittest discover -s tests/packaging -p 'test_*.py'
```

Use the exact executable when testing another profile. Build/test wrappers
explicitly provision its restricted-worker access. Tests use isolated data and
hidden/synthetic windows, not Computer Use. No command above requests phone audio.

For HAP: `./scripts/Build.ps1 -Profile Hap -Test`, then
`python -B scripts/Test-HapPairing.py --exe work/hap-receiver-target/release/airmirror-lite.exe`.
The independent sender verifies M4, encrypted control/events, AAC-ELD/LC SETUP,
worker readiness, teardown and invalid-key/authentication cases. It sends no RTP
or PCM. Synthetic native media remains separate.

Local FDK/PlayFair reference sanitizer checks:

```powershell
cargo run --manifest-path tests/native-sanitizer/Cargo.toml --locked --offline --target-dir work/san-target
./scripts/Test-PlayfairUB.ps1
```

MSVC ASan must be installed. The separate Clang shift-UB test requires its failing
control. Neither is full FFmpeg-library sanitizer coverage. Direct FFmpeg tests
use canaries and failure/reconfiguration contracts. Decoder-comparison commands
are in DECODER_EXPERIMENT.md.

`--ui-test` runs synthetic GPU/UI and isolated AAC without submitting audio.
`--ui-test-dir` renders generated idle/control artwork, never phone pixels.
`--self-test` / `Test-Media.ps1 -Audio` are audible and need a new explicit request.

## Artwork without an audio endpoint

`<exe> --ui-artwork <directory>` owns a fresh hidden App scene, invokes the same
native control/geometry/UIA checks and idle renderer, and exits before receiver
setup. Its explicit initializer omits endpoint setup; the normal am_open path
still requires audio. The exporter asserts no audio client, endpoint enumerator,
playback or submitted samples exist. Sender pixels cannot enter this scene.
It rejects extra receiver flags and remains disabled in decoder-lab builds.
This verifies UI/control artwork only, not media decode, playback or phone use.

## Evidence boundaries

| Check | Establishes | Does not establish |
|---|---|---|
| Rust | Input/state/proof/replay budgets, RTP wrap, cancellation/owner/FLUSH/visibility fences | Real sender interoperability |
| Native/GPU | Hardware surfaces, aperture/color/rotation, paused caches, busy Present, suspend/resume, injected recovery | Physical scanout/device loss |
| UI | Hidden controls/DPI/fullscreen/pointer/keyboard/accessibility providers | Screen-reader/operator usability |
| Worker | Token/file/UDP isolation checks, bounded IPC, AAC golden outputs/reset, key-copy erasure | Complete sandbox/dependency audit |
| Loopback | Listener, single owner, invalid input, FLUSH/teardown/reconnect/shutdown | Phone or Wi-Fi behavior |
| Package | Fresh matching source/binary/dependencies/notices, archive integrity, private-data exclusion | Stable release or legal/patent clearance |

Limits: RTSP headers 8 KiB/bodies 64 KiB/four controls; plist 1024 objects/depth 16/
4096 expanded visits/512 KiB data; H.264/HEVC 2/4 MiB records; eight compressed
records plus transition slot; 500 ms backpressure. Decoded queues preserve pool
headroom (at most eight H.264/four HEVC). Audio 44.1 kHz stereo ELD 480/LC 1024,
8KiB packets, 128-packet jitter window, 32-block PCM queues plus transition slots.
Worker: one process/256 MiB/two-second IPC. Log 1 MiB. Driver/OS allocations are
separate; these are not a total GPU/host-memory cap.

## Retained physical results

- Original FDK/H.264 498×1080: user-confirmed five-minute synchronized sound/picture,
  no freezes; 59.30 decoded/57.76 presented FPS, 13,230,000 audio samples,
  host private memory 89.81→89.91 MiB. The user shortened the planned 30-minute run.
  This predates FFmpeg/high-resolution work and proves neither for the candidate.
- F928294B... FFmpeg/legacy, source 55ed35a: quiet 994×2160 picture, Photos sizing/
  responsiveness, reconnect/rotation and black-on-lock/restoration. User reports
  smoother Photos handoff. The captured comparison did not change coded format;
  synthetic tests separately cover the cache path.
  Evidence: local/photo-handoff-verification/ and local/screen-off-verification/.
- Earlier 8972816... FFmpeg: muted Photos/audio-start burst stays connected,
  real audio counters and queue high-water 30/32. Not sound-quality/A/V acceptance.
  Evidence: local/photos-audio-burst-fix/phone-check.json.
- 2FF3927... HAP: verified pairing/control/events, 994×2160 screen and five quiet
  reconnects work. User reports ~5 s selection wait. Accepted TCP→Present 177–270ms
  in four cases and 1191 ms once. Phone tap and scanout were not measured.
  Evidence: local/hap-verification/phone-first/ and
  local/connection-metadata/20261022T205333254404Z/.
- C10FEA1... HAP: graphics-only first-frame attachment fix passes silent native
  tests; no new physical speedup/audio claim.

Exact fallback paths/hashes: STATE.md. Superseded detailed narratives: Git 5df8779.
Private evidence stays under ignored local/; never package identity, media or logs.

## Remaining release acceptance

The beta installer is compiled with `scripts/Package-Installer.py` from a verified,
clean, source-complete portable directory. It hashes every input and preserves
the exact receiver EXE. It does not install anything while compiling. Example:

```powershell
python -B scripts/Package-Installer.py --portable dist/iPhoneAirMirror-0.1.0-beta.1 --output dist/iPhoneAirMirror-0.1.0-beta.1-windows-x64-setup.exe
python -B tests/packaging/test_installer.py
```

An explicit `--test-install-root local/<fresh-name>` requires a separate output
under `work/` whose name includes `test-only`. That build has a different AppId,
fixed workspace app/data paths, no shortcuts and no uninstall registry entry.
Its install/upgrade/uninstall and negative-path results are separate from
production shortcut, Windows Apps registration and real per-user installation
acceptance. Production setup and receiver binaries are unsigned. The API version
floor is Windows 10 1703, not proof of testing across all admitted Windows/GPU
combinations. No installer test launches the receiver or plays audio.

Physical FFmpeg AAC-ELD/LC sound quality and A/V sync, pause/seek/FLUSH compatibility,
extended drift and adverse network/endpoint recovery remain open. HAP needs
its own encrypted-audio/A/V checks. Numeric latency requires actual phone-to-PC
display/speaker measurement. Screen-reader and other Windows/GPU/device coverage
are limited. This cleanup requests no phone or sound tests; keep the fallback
and release-candidate label until applicable physical evidence exists.
