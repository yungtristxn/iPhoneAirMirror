# AirMirrorLite

A minimal Windows receiver for iPhone AirPlay **screen mirroring with synchronized sound** over the local network.

**Status:** working local implementation on `feat/airplay-mvp`. Real iPhone HEVC mirroring is confirmed at **994×2160**, up from 498×1080. A previous H.264 session passed five minutes with user-confirmed synchronized audio and no freezes. Current high-resolution checks are silent; HEVC A/V sync has not been re-tested. See docs/STATE.md for evidence and remaining checks.

## Run

Build with `./scripts/Build.ps1 -Fetch -Test`, then `./scripts/Package.ps1`.
Launch `dist/AirMirrorLite/AirMirrorLite.exe`, select **AirMirrorLite** in iPhone
Control Center → Screen Mirroring, and play a local non-DRM video with sound.
Both devices must be on the same trusted private LAN. Normal operation requires
no administrator rights, Bonjour service, account or cloud connection.

- Frameless, rounded phone view that follows the mirrored video’s aspect ratio.
  Drag the picture to move; drag the curved bottom-right grip to resize. Default
  size preserves source pixels when they fit the monitor, avoiding unnecessary
  shrinking or enlargement. The separate HEVC negotiation now requests a higher
  transmitted resolution when native support is available.
- Side controls: speaker, default size, fullscreen and close. Pause over the
  speaker for a horizontal volume slider; click to mute/unmute. Buttons stay
  fixed as the flyout opens. Volume changes only PC playback.
- Click the small chevron beside the controls to hide them. Hover at that edge
  of the phone to reveal the expand chevron. `C` also hides/shows the controls.
- Keyboard: `0` default size; `M` mute; Up/Down volume; `F11` fullscreen;
  `D` disconnect; Escape leaves fullscreen or disconnects. Double-click the
  picture to toggle fullscreen. Tab then Enter/Space operates the side controls;
  Left/Right adjusts volume in 1% steps when the speaker has keyboard focus.
  Escape dismisses the volume flyout before leaving fullscreen or disconnecting.
- Close the receiver window to stop discovery, sockets and codec workers.
- Identity is random and protected by Windows DPAPI in `local/identity.dpapi`
  next to the executable. `local/receiver.log` is bounded to 1 MiB and contains
  protocol stage names and counters, not media or pairing secrets.
- The receiver has no PIN/admission whitelist: any reachable permitted LAN peer
  can request the single available session. Pair-verify proves key possession;
  it is not an allowlist of trusted people.
- Firewall access should be limited to this exact executable, **Private** profile
  and **LocalSubnet**. Do not allow public-network access or disable protection.
- `--ui-test` runs **silent** native controls, embedded-icon and synthetic
  H.264/HEVC hardware-video checks.
  `scripts/Test-Media.ps1` uses this mode by default; it submits no audio samples.
- `--ui-test --ui-test-dir local/ui-renders` also renders idle/control artwork at
  100%, 125% and 200% for inspection. It cannot export a mirrored phone frame.
  `--ui-preview` shows synthetic video without discovery or network listeners.
- The full `--self-test` / `scripts/Test-Media.ps1 -Audio` probe includes an audible
  AAC test tone. Do not run it unless the user explicitly requests audible testing.

By default, a compatible native HEVC decoder/GPU requests **3840×2160 at up to
30 fps**. iPhone chooses the actual dimensions for its screen aspect ratio; the
verified portrait result is 994×2160. To choose a different request when starting
AirMirrorLite, use `--resolution 1440` (2560×1440 / 30 fps) or
`--resolution 1080` (H.264 compatibility / 60 fps). A new mirroring connection is
needed after changing the request. Hosts without native HEVC support default to
1080; explicitly selecting a higher mode requires native HEVC and GPU support.

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

## Start
- [Implementation prompt](docs/START_HERE.md)
- [Required scope and acceptance](docs/SPEC.md)
- [Architecture research and sources](docs/RESEARCH.md)
- [Security regression leads](docs/SECURITY_BASELINE.md), when implementing protocol/native code
- [Current state](docs/STATE.md)

## Intended experience
Open the app, select AirMirrorLite in iPhone Control Center > Screen Mirroring, and see/hear the phone. One active sender; resize, rotation, fullscreen, disconnect. Closing the window fully stops the receiver.

Preferred stack: Rust + Win32, native Windows DNS-SD, Media Foundation H.264 + Direct3D 11, and a narrowly selected audio decoder feeding WASAPI. Architecture assumptions must be verified on a real iPhone and the target GPU.

## Verification
```powershell
./scripts/Build.ps1 -Test
./scripts/Test-Media.ps1
./scripts/Test-Control.ps1
```

The Rust tests cover request/body budgets, plist graphs, pairing proof/replay,
FairPlay modes, NAL/SPS lengths and dimensions, RTP framing and shared-clock math.
The loopback check covers the real control listener and shutdown. The media probe
uses generated test media; UI diagnostics exercise a hidden native test window. See [STATE](docs/STATE.md) for the current evidence and
the still-open physical acceptance checks.

The kickoff prompt uses focused context, explicit boundaries and observable acceptance criteria, informed by [OpenAI's workflow guidance](https://developers.openai.com/cookbook/examples/codex/iterating-development-workflows-with-codex#build-phase-files). It does not add routine approval gates between implementation steps.
