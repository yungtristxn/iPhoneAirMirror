# AirMirrorLite

A minimal Windows receiver for iPhone AirPlay screen mirroring and synchronized
phone audio. Native Win32 controls, Media Foundation/D3D11 video, WASAPI audio and
an isolated FFmpeg AAC worker. No account, recording, service or cloud.

**Release candidate, not a qualified stable release.** Earlier builds have real
iPhone HEVC picture, Photos, rotation, reconnect and lock/unlock evidence.
Current FFmpeg audio timing passes silent fixtures; physical sound quality and
A/V sync remain separate acceptance checks. The older five-minute synchronized
test used FDK and does not qualify FFmpeg or HAP. See [state](docs/STATE.md) and
[validation](docs/VALIDATION.md).

## Build and package

Development requirements: Windows x64, Rust, Visual Studio C++ x64 tools/Windows
SDK, PowerShell 7 and Python 3. FFmpeg uses additional pinned local tools; see
[decoder notes](docs/DECODER_EXPERIMENT.md).

```powershell
./scripts/Build-FFmpeg.ps1 -Fetch
./scripts/Build.ps1 -Fetch -Test
./scripts/Package.ps1
```

Default: **FFmpeg AAC, legacy verified pairing and display updates**.
Packaging creates a source-complete `dist/AirMirrorLite-release-candidate`
directory without replacing preserved fallback folders. Every output must be
new. For a local ZIP:

```powershell
python -B scripts/Package-Share.py --output dist/AirMirrorLite-Windows-x64-candidate.zip
```

The archive includes matching application/dependency source, build instructions
and notices. Extract it and run `Start-AirMirrorLite.cmd`; retain the source and
license materials when sharing. Nothing is published automatically.
[Component provenance](docs/THIRD_PARTY.md).

HAP is separately experimental: `./scripts/Build.ps1 -Profile Hap -Test` and
`./scripts/Package.ps1 -Profile hap`. It advertises **AirMirrorLite HAP**; faster
startup and physical encrypted-audio/A/V acceptance are unproven.
[HAP details](docs/VERIFIED_PAIRING.md).
`-Profile FdkReference` is local reference testing only, never release packaging.

## Run

Run the extracted launcher, then select **AirMirrorLite** in iPhone Control
Center → Screen Mirroring. Keep both devices on the same trusted LAN; run only
one receiver at a time.

Runtime requires Windows 10/11 x64, Microsoft Visual C++ x64 runtime, Media
Foundation, hardware D3D11 H.264 decoding/video processing and a default audio
endpoint. Higher resolutions need native HEVC support, normally the installed
Windows HEVC Video Extension plus a compatible GPU. The app does not install
codecs or silently substitute software video/video-only operation.

Windows may ask to allow the executable through its firewall. Public, Private
and Domain classifications work without changing Windows settings. The receiver
selects one active Ethernet/Wi-Fi interface with an RFC1918 IPv4 address and
gateway. Use `--bind IP` to select another eligible adapter. No network setting
or firewall rule is changed automatically; manually managed rules should be
scoped to the executable and local subnet.

The launcher provisions restricted-worker read/execute access on that exact
executable. Manual deployment requires `AirMirrorLite.exe --provision-worker`
once. Normal launches only check the grant and never use an unrestricted fallback.

## Controls and picture

A rounded titanium edge and inset glass lip frame the picture. The light control
rail docks near the phone's upper edge (centered on short landscape windows),
with consistent vector icons, a connected collapse tab and a hover volume card.
Artwork follows monitor DPI; fullscreen removes the bezel.

- Drag the picture to move; bottom-right grip resizes. Double-click/F11 toggles
  fullscreen; `0` restores default size.
- Speaker/M toggles mute; hover opens the volume slider. Up/Down changes volume.
  Tab and Enter/Space operate controls; focused volume also supports Left/Right,
  Home/End and Page Up/Down.
- Chevron/C hides the rail. Hover the collapsed edge to reveal its expand arrow.
  D disconnects. Escape dismisses a flyout/error, exits fullscreen or disconnects.
  Closing exits and stops discovery, sockets and workers.
- Start mirroring from Settings/Home to establish the phone frame. Photos may
  send an independent photo-only view without toolbars/thumbnails and can retain
  its orientation while the phone interface rotates.
- Format changes retain the last picture until a replacement presents. Explicit
  sender suspension makes the viewport black; resume waits for a fresh frame.
  Still photos do not time out. Physical orientation/power cannot be inferred
  from independent content alone.

## Options and data

Use `--help` for all options. Common controls:

| Option | Purpose |
|---|---|
| `--mute` | Start muted; decoding and synchronization remain active. |
| `--resolution 1080\|1440\|2160` | Request H.264 compatibility or native HEVC. |
| `--fps 30\|60` | Maximum requested rate; default 60. |
| `--latency-ms 60..250` | Sender latency hint; default 80 ms, not measured latency. |
| `--display-width N` | Explicit even-pixel display-width hint. |
| `--data-dir PATH` | Private writable data location. |

With HEVC support, default request is 3840×2160/up-to-60 fps; the sender chooses
actual dimensions (994×2160 observed). Otherwise default is H.264/1080. Reconnect
after changing requests.

Data defaults to `local/` beside a writable executable, otherwise
`%LOCALAPPDATA%/AirMirrorLite`. Identity is random and DPAPI-protected.
The 1 MiB bounded log excludes media, keys and raw negotiation bodies.
Optional `display-aspect.txt` holds two dimensions, e.g. `994 2160`; it is an
advisory canvas ratio, not a crop or orientation sensor. There is no user
PIN/admission allowlist: a reachable LAN peer can request the available session.
Cryptographic pairing does not authorize a person.

## Development references

- [Product contract](docs/SPEC.md), [current state](docs/STATE.md)
- [Architecture/references](docs/RESEARCH.md), [security boundaries](docs/SECURITY_BASELINE.md)
- [Validation commands/evidence](docs/VALIDATION.md), [component licenses](docs/THIRD_PARTY.md)
- [AAC qualification](docs/DECODER_EXPERIMENT.md), [HAP](docs/VERIFIED_PAIRING.md)
- [Startup investigation](docs/STARTUP_TRACE.md)

Tests default to silent synthetic fixtures. `--self-test` / `Test-Media.ps1 -Audio`
require an explicit request for sound. `--ui-test-dir` renders generated control
art only; it cannot export phone frames. The comparison harness is compiled only
with `decoder-lab`, whose executable cannot start a receiver.
