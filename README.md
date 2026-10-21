# AirMirrorLite

A minimal Windows receiver for iPhone AirPlay **screen mirroring with synchronized sound** over the local network.

**Status:** runnable implementation on `feat/airplay-mvp`; real iPhone acceptance is pending. Native discovery, protocol/state checks, synthetic H.264 hardware decode and AAC-ELD playback have been exercised. These checks do not establish iPhone compatibility or 30-minute A/V synchronization.

## Run

Build with `./scripts/Build.ps1 -Fetch -Test`, then `./scripts/Package.ps1`.
Launch `dist/AirMirrorLite/AirMirrorLite.exe`, select **AirMirrorLite** in iPhone
Control Center → Screen Mirroring, and play a local non-DRM video with sound.
Both devices must be on the same trusted private LAN. Normal operation requires
no administrator rights, Bonjour service, account or cloud connection.

- F11: fullscreen; Escape: leave fullscreen or disconnect; D: disconnect.
- Close the receiver window to stop discovery, sockets and codec workers.
- Identity is random and protected by Windows DPAPI in `local/identity.dpapi`
  next to the executable. `local/receiver.log` is bounded to 1 MiB and contains
  protocol stage names and counters, not media or pairing secrets.
- The receiver has no PIN/admission whitelist: any reachable permitted LAN peer
  can request the single available session. Pair-verify proves key possession;
  it is not an allowlist of trusted people.
- Firewall access should be limited to this exact executable, **Private** profile
  and **LocalSubnet**. Do not allow public-network access or disable protection.
- `--self-test` generates synthetic landscape/portrait bars and a brief quiet
  tone, checks hardware decoder surfaces, FairPlay known-answer data, AAC-ELD,
  AppContainer file denial and network isolation. It does not access a phone.

Runtime requirements: Windows 10/11 x64 with the Microsoft Visual C++ x64 runtime
(`MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`), Media Foundation, a D3D11 H.264
decoder/video processor, and a working default audio endpoint. Initialization
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
uses generated test media. See [STATE](docs/STATE.md) for the current evidence and
the still-open physical acceptance checks.

The kickoff prompt uses focused context, explicit boundaries and observable acceptance criteria, informed by [OpenAI's workflow guidance](https://developers.openai.com/cookbook/examples/codex/iterating-development-workflows-with-codex#build-phase-files). It does not add routine approval gates between implementation steps.
