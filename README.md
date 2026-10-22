# AirMirrorLite

A minimal Windows receiver for iPhone AirPlay **screen mirroring with synchronized sound** over the local network.

**Status:** working local implementation on `feat/airplay-mvp`. Real iPhone HEVC mirroring works at **994×2160**, up from 498×1080. The current review fixes pass silent automated checks, and the user confirmed the quiet Photos/rotation/reconnect check. An older H.264 session passed five minutes with synchronized audio and no freezes; current HEVC A/V sync and adverse-device recovery have not been physically re-tested. See docs/STATE.md and docs/REVIEW_FOLLOWUP.md for evidence and remaining checks.

## Run

The opt-in FFmpeg receiver candidate and silent decoder checks are documented in
[DECODER_EXPERIMENT.md](docs/DECODER_EXPERIMENT.md). Build/package it with
`./scripts/Package.ps1 -FfmpegCandidate`; its executable is
`dist/AirMirrorLite-ffmpeg-candidate/AirMirrorLite.exe`. It uses explicit decoder
delay timestamps and remains separate from the accepted FDK receiver below.
Physical A/V acceptance of this candidate is still outstanding. Do not run both
receivers at once. Adding the `decoder-lab` build feature disables reception.

For the separately authorized friend-test archive, use
`python -B scripts/Package-Share.py --output dist/AirMirrorLite-Windows-x64-audio-burst-fix-2026-10-22.zip`.
This preserves the tested FFmpeg/rotation candidate, verifies its hash against a
fresh offline build, and includes matching source/dependencies and GPLv3 notices.
It excludes pairing identities and logs. Extract the ZIP and use
`Start-AirMirrorLite.cmd`; it prepares that copy's restricted worker before launch.
The included README lists the Microsoft Visual C++ x64 runtime prerequisite.
This test package does not qualify the older FDK binary for redistribution.
The current first-connection improvements and measured limits are recorded in
[PERFORMANCE_FOLLOWUP.md](docs/PERFORMANCE_FOLLOWUP.md).

Build with `./scripts/Build.ps1 -Fetch -Test`, then `./scripts/Package.ps1`.
Launch `dist/AirMirrorLite/AirMirrorLite.exe`, select **AirMirrorLite** in iPhone
Control Center → Screen Mirroring, and play a local non-DRM video with sound.
Both devices must be on the same trusted private LAN. Normal operation requires
no administrator rights, Bonjour service, account or cloud connection.

Build/package scripts provision the restricted worker's read/execute permission
on the exact executable. If building or deploying it manually, its owner runs
`AirMirrorLite.exe --provision-worker` once. Ordinary launches only read that
permission; they do not rewrite the executable's access rules or use an
unrestricted worker if permission is missing.

- Frameless, rounded phone view that learns the screen's proportions from the
  first mirrored frame. Matching screen rotations and resolution changes resize
  it; different-aspect photo/video output fits inside the established frame.
  Drag the picture to move; drag the curved bottom-right grip to resize. Default
  size preserves source pixels when they fit the monitor, avoiding unnecessary
  shrinking or enlargement. The separate HEVC negotiation now requests a higher
  transmitted resolution when native support is available.
- Start mirroring from the Home screen or Settings to establish the phone frame.
  If a session starts directly with an optimized media feed, its initial shape
  is used because the receiver cannot infer the phone's physical orientation.
- Side controls: speaker, default size, fullscreen and close. Pause over the
  speaker for a horizontal volume slider; click to mute/unmute. Buttons stay
  fixed as the flyout opens. Volume changes only PC playback.
- Click the small chevron beside the controls to hide them. Hover at that edge
  of the phone to reveal the expand chevron. `C` also hides/shows the controls.
- Keyboard: `0` default size; `M` mute; Up/Down volume; `F11` fullscreen;
  `D` disconnect; Escape leaves fullscreen or disconnects. Double-click the
  picture to toggle fullscreen. Tab then Enter/Space operates the side controls;
  Left/Right adjusts volume in 1% steps when the speaker has keyboard focus.
  Escape dismisses the volume flyout or a dismissible connection error before
  leaving fullscreen or disconnecting. Unavailable-device/listener messages remain
  until recovery; their instructions explain when a reconnect or restart is needed.
- Launch the prepared executable with `--mute` to start muted; the speaker button
  or `M` restores sound. Decoding and A/V synchronization stay active while muted.
- Custom controls expose native accessibility names, roles, focus, invoke/toggle
  actions and volume range values. Keyboard volume also supports Home/End and
  Page Up/Down when the slider has focus.
- Close the receiver window to stop discovery, sockets and codec workers.
- Identity is random and protected by Windows DPAPI in `local/identity.dpapi`
  next to a writable executable directory. Read-only deployments use
  `%LOCALAPPDATA%/AirMirrorLite`; `--data-dir` explicitly chooses another directory.
  `receiver.log` in that data directory is bounded to 1 MiB and contains protocol
  stage names and counters, not media or pairing secrets.
- The receiver has no PIN/admission whitelist: any reachable permitted LAN peer
  can request the single available session. Pair-verify proves key possession;
  it is not an allowlist of trusted people.
- Windows may ask to allow this exact executable through the firewall on the
  active network profile. The app accepts Public, Private and Domain profiles;
  changing the network's classification is not required. It binds to one local
  LAN address. Scope manually managed rules to that executable and LocalSubnet.
- `--ui-test` runs **silent** native controls, embedded-icon, synthetic
  H.264/HEVC hardware-video and isolated codec/IPC checks. AAC test data is
  decoded in memory only; it is never sent to an audio endpoint.
  `scripts/Test-Media.ps1` uses this mode by default; it submits no audio samples.
- `--ui-test --ui-test-dir local/ui-renders` also renders idle/control artwork at
  100%, 125% and 200% for inspection. It cannot export a mirrored phone frame.
  `--ui-preview` shows synthetic video without discovery or network listeners.
- The full `--self-test` / `scripts/Test-Media.ps1 -Audio` probe includes an audible
  AAC test tone. Do not run it unless the user explicitly requests audible testing.

By default, a compatible native HEVC decoder/GPU requests **3840×2160 at up to
60 fps**. iPhone chooses the actual dimensions for its screen aspect ratio; the
verified portrait result is 994×2160. To choose a different request when starting
AirMirrorLite, use `--resolution 1440` (2560×1440 / up to 60 fps) or
`--resolution 1080` (H.264 compatibility / 60 fps). A new mirroring connection is
needed after changing the request. `--fps 30` restores the lower-bandwidth frame
rate. `--latency-ms` sets the output-latency hint advertised to the sender
(default 80 ms; allowed 60–250). Both playback paths follow the sender's
presentation timestamps on one synchronized clock when an audio stream is
configured; this hint is not added again as a local delay. Screen-only sessions
present fresh decoded frames promptly. Mute and silent passages do not disable
synchronization in a configured audio session. The hint is not measured
phone-to-screen latency.
Hosts without native HEVC support default to
1080; explicitly selecting a higher mode requires native HEVC and GPU support.
`--bind` can select any active Ethernet/Wi-Fi adapter with an RFC1918 IPv4 address
and a gateway, including one that is not enumerated first; discovery uses the
same selected interface. Windows network profile labels are not consulted.

To keep Photos proportional to the phone, the local data directory can contain
`display-aspect.txt` with a reference width and height, for example `994 2160`.
The receiver scales this ratio to the selected resolution and requests that
display geometry from the sender. The validated local installation uses this
setting. `--display-width N` overrides it for one launch; without either setting,
the usual widescreen request is used. Native decoder limits remain unchanged.

Active-audio FLUSH requests must include valid RTP sequence and timestamp
boundaries. This prevents canceled packets still in flight from entering the new
playback interval. A sender that omits the boundary receives a rejection and a
visible reconnect instruction. Screen-only FLUSH is harmless and does not affect
a later audio connection. Compatibility with this stricter boundary still needs
real-phone seek/pause evidence on the updated build.

The executable includes its own multi-size icon for Explorer, the taskbar and
Alt-Tab; no separate icon installation is needed.

Runtime requirements: Windows 10/11 x64 with the Microsoft Visual C++ x64 runtime
(`MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`), Media Foundation, a D3D11 H.264
decoder/video processor, and a working default audio endpoint. Higher modes use
the installed Windows HEVC Video Extension (when available) and an HEVC-capable
GPU; the application does not install or redistribute that codec. Initialization
fails explicitly if these or the AppContainer worker are unavailable.
No silent software-video or video-only fallback is enabled.

This package is for local development/device acceptance. See
[component licenses and distribution limits](docs/THIRD_PARTY.md) before redistribution.
`scripts/Package.ps1 -ForDistribution` refuses while the current static component
combination lacks an established compatible distribution grant.

## Start
- [Implementation prompt](docs/START_HERE.md)
- [Required scope and acceptance](docs/SPEC.md)
- [Architecture research and sources](docs/RESEARCH.md)
- [Security regression leads](docs/SECURITY_BASELINE.md), when implementing protocol/native code
- [Current state](docs/STATE.md)

## Intended experience
Open the app, select AirMirrorLite in iPhone Control Center > Screen Mirroring, and see/hear the phone. One active sender; resize, rotation, fullscreen, disconnect. Closing the window fully stops the receiver. Discovery registration runs asynchronously; decoder objects are retained across connections while stream state and surfaces are cleared.

Preferred stack: Rust + Win32, native Windows DNS-SD, Media Foundation H.264 + Direct3D 11, and a narrowly selected audio decoder feeding WASAPI. Architecture assumptions must be verified on a real iPhone and the target GPU.

## Verification
```powershell
./scripts/Build.ps1 -Test
./scripts/Test-Media.ps1
./scripts/Test-Control.ps1
python scripts/Test-VideoProfiles.py
```

The control and media wrappers both default to the release executable and record
its hash. Packaging runs a locked, offline release freshness build before copying
the executable and writing source provenance to `BUILD.json`.

The Rust tests cover request/body budgets, plist graphs, pairing proof/replay,
FairPlay modes, NAL/SPS lengths and dimensions, RTP framing and shared-clock math.
The loopback check covers the real control listener and shutdown. The media probe
uses generated test media; UI diagnostics exercise a hidden native test window. See [STATE](docs/STATE.md) for the current evidence and
the still-open physical acceptance checks.

The kickoff prompt uses focused context, explicit boundaries and observable acceptance criteria, informed by [OpenAI's workflow guidance](https://developers.openai.com/cookbook/examples/codex/iterating-development-workflows-with-codex#build-phase-files). It does not add routine approval gates between implementation steps.
