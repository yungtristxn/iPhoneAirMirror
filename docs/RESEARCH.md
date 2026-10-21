# Minimal Windows AirPlay receiver — research and recommended design
Research date: 2026-10-21

## Decision
Build a Windows-only, local-network AirPlay Screen Mirroring receiver with one active iPhone session and a single native display window. Prioritize low steady-state CPU use, bounded memory, low latency and minimal installed components. This is a research/design recommendation, not a built or benchmarked application.

Recommended application stack: Rust with Microsoft's windows crate; Win32 windowing; native Windows DNS-SD registration; Media Foundation H.264 decoding; Direct3D 11/DXGI presentation. Use maintained cryptographic implementations and narrowly scoped compatibility code. Do not implement cryptographic primitives or video decoders from scratch.

Current open-source implementations are interoperability references and potential sources of selected code, not trusted drop-in engines. Our earlier security findings apply to a particular uxplay-windows/libuxplay snapshot. A current upstream spot-check still shows relevant signed codec-length parsing patterns, so simply switching branch/version is not sufficient assurance.

## Product contract
- Open the app, select its receiver name in iPhone Control Center > Screen Mirroring.
- One receiver window, correct portrait/landscape aspect ratio, resize, fullscreen and disconnect.
- Closing the window stops listening and exits all child processes.
- One active sender; a second sender cannot silently replace it.
- Local-network operation, ordinary user privileges during normal use, no account.
- No recording, phone control, USB capture, DLNA, HLS URL playback, standalone music-player mode, Bluetooth discovery, autostart, background service or automatic updater.
- Start with H.264 SDR. HEVC/HDR/4K add compatibility and testing scope without helping the initial minimal product.
- Confirmed scope update: synchronized phone audio is REQUIRED. A video-only spike is only an intermediate milestone. Do not assume the stock Windows AAC decoder supports the negotiated format.

Capability advertisements must accurately describe the implemented subset. Dropping user-visible features does not imply all corresponding AirPlay negotiation endpoints or discovery records can be removed.

## Proposed data path
iPhone -> native discovery -> bounded AirPlay session/protocol worker -> decrypted compressed H.264 -> Media Foundation -> D3D11 GPU surfaces -> native window.

Discovery and UI live in the small host. The worker handles untrusted packet parsing, session state and any retained C compatibility code. A second process alone is not a sandbox: use an actual restricted worker/AppContainer boundary if the compatibility spike confirms the required LAN/IPC behavior. The same shipped executable can run host and worker roles. Transfer bounded compressed samples and validated metadata over private IPC; do not copy full decoded frames across that boundary.

The initial spike must verify this isolation approach. It should not quietly fall back to a fully privileged worker if restrictions fail. Media Foundation and GPU driver code remain dependencies and are not made memory-safe by writing the host in Rust.

Microsoft documents both native service registration and creating AppContainer processes:
- [DnsServiceRegister](https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsserviceregister)
- [Launching an AppContainer](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer)
- [Rust Media Foundation bindings](https://microsoft.github.io/windows-docs-rs/doc/windows/Win32/Media/MediaFoundation/index.html)

## Components
| Component | Proposed implementation | Reason |
|---|---|---|
| Discovery | DnsServiceRegister / DnsServiceDeRegister | Uses Windows' existing DNS-SD support; avoids installing Bonjour. Registration is tied to process lifetime. |
| Network/session | Rust, bounded buffers and explicit per-connection state | Avoids copying the unsafe C input/state handling identified earlier. |
| AirPlay interoperability | Current UxPlay code and observed real-device behavior as references; selected narrowly bounded compatibility routines | Reverse-engineered interoperability is the difficult part. Porting protocol behavior is more realistic than inventing it. |
| Binary plist | An established parser with input/depth/allocation limits | Binary plist support is part of negotiation; JSON cannot replace it. |
| Cryptography | Maintained libraries plus required FairPlay-compatible mirroring handshake code | Use verified implementations; keep native/FFI input contracts small and fixed-size where possible. |
| Video | Windows H.264 Media Foundation transform with D3D11 device manager | Hardware decode and OS-provided fallback; no bundled general video framework required. |
| Display | Win32 HWND + D3D11/DXGI | Small native UI and GPU color conversion/presentation. |
| Required phone audio | Negotiated codec decoder + WASAPI | Native Windows AAC support does not document AAC-ELD; select only the necessary decoder after a real-device probe. |
| Settings | One small per-user configuration file and receiver identity | No database; no environment-variable command configuration or arbitrary media-pipeline expressions. |

UxPlay exposes separate video_process/audio_process callbacks, which demonstrates a useful boundary between AirPlay reception and media rendering. Replacing GStreamer still requires adaptation of initialization, DNS-SD, timing, shutdown and build dependencies.
- [UxPlay callback interface](https://github.com/FDH2/UxPlay/blob/master/lib/raop.h)
- [UxPlay build structure](https://github.com/FDH2/UxPlay/blob/master/CMakeLists.txt)

## Video efficiency
The important savings come from the media path, rather than choosing Rust instead of C++:

1. Decode the received compressed stream once. Do not re-encode it for display.
2. Enable hardware decoding when the actual device/driver supports it.
3. Keep decoded NV12 surfaces on the GPU and perform scaling/color conversion there.
4. Reuse decoder surfaces and render resources.
5. Keep the presentation queue short; present the freshest decoded frame when display falls behind.
6. Do not arbitrarily drop compressed interdependent frames: maintain decoder references or use a deliberate keyframe-resynchronization policy.
7. Use event-driven networking and frame-ready wakeups. Avoid periodic idle rendering and polling.
8. Enable low-latency codec/pipeline options, then measure behavior. They cannot remove capture or network delay on the iPhone.

Microsoft's decoder expects Annex-B H.264 with start codes. The receiver must correctly normalize the received NAL representation and handle SPS/PPS and resolution changes.
- [H.264 decoder requirements](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder)
- [D3D11 / Media Foundation device sharing](https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation)
- [Codec low-latency setting](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avlowlatencymode)
- [Media Foundation low-latency pipeline](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-low-latency)

Targets to validate, not performance promises: near-idle CPU while waiting, hardware-decode confirmation during motion, bounded working set over a long session, and a 1080p-class/60-fps capability target subject to the sender's actual negotiated stream. No credible CPU percentage, binary-size or end-to-end latency figure can be given before measurement on the user's GPU/network.

## Audio caveat
Current UxPlay explicitly handles AAC-ELD for mirroring and ALAC for other audio modes. Microsoft's AAC decoder documentation lists AAC-LC and HE-AAC, not AAC-ELD. Therefore a completely Windows-codec-only audio design is not established.

Sound is required. Test the actual negotiated format first and add a small decoder-only build for that format; a restricted libavcodec/libavutil build is one practical candidate. Avoid adding a general media player, muxer, encoder, URL fetcher or complete GStreamer distribution solely to decode screen audio. Keep A/V timing and jitter buffers bounded; "no buffering" would create glitches.
- [UxPlay audio formats](https://github.com/FDH2/UxPlay/blob/master/renderers/audio_renderer.c)
- [Microsoft AAC decoder profiles](https://learn.microsoft.com/en-us/windows/win32/medfound/aac-decoder)

## What existing projects establish
- FDH2/UxPlay is the main compatibility reference. Its whole application includes more features and dependencies than this product needs. The code is GPLv3 at project level; component licenses must be retained and checked before distributing derived builds.
- moieric11/AirPlay-Windows is a useful Windows integration reference: its current source uses native DnsServiceRegister, and its README documents D3D11 GPU-resident decoding/display. It still uses FFmpeg/SDL and includes recording/HLS/other scope, so copying the full app would miss the minimality goal. Its own local pipeline latency estimate explicitly excludes sender capture, network transit and display scan-out.
- Old unofficial AirPlay protocol notes are useful background, but not a complete current iOS contract. Some project protocol documents are visibly stale relative to implementation. Current code and real iPhone acceptance tests must resolve disagreements.

References:
- [UxPlay](https://github.com/FDH2/UxPlay)
- [Windows native DNS-SD implementation reference](https://github.com/moieric11/AirPlay-Windows/blob/main/src/mdns/mdns_service.cpp)
- [AirPlay-Windows current architecture and limits](https://github.com/moieric11/AirPlay-Windows/blob/main/README.md)
- [Unofficial protocol documentation](https://github.com/openairplay/airplay-spec)

## Required security work
Reducing features and dependencies does not itself fix the AirPlay parser or pairing bugs:
- Enforce message/frame/body limits before allocation or decoding; use checked arithmetic.
- Enforce pairing stages and proof success; validate every cryptographic field length.
- Reject invalid FairPlay indices before table access.
- Associate stream connections with the negotiated session; do not accept an unrelated peer simply because it reaches a media port.
- Use PIN/password admission only after its implementation has been validated. Authentication cannot protect code that must parse preauthentication requests.
- Allow only intended private-network connectivity and restrict firewall rules to the executable and local subnet. Multicast discovery/interface details must be tested.
- Generate a random persistent receiver identity; protect stored secrets with Windows facilities.
- Add focused malformed-input, reconnect/shutdown and resource-limit tests, plus fuzzing/sanitizers for the remaining native boundary.
- Keep logs bounded and avoid raw screen content, pairing secrets or credentials.
- If distributing the app, sign and publish reproducible/traceable release artifacts. A personal build can still trigger reputation or antivirus checks; custom source does not guarantee Defender acceptance.

## Build sequence
1. Compatibility spike: native DNS-SD + minimal handshake + one real iPhone H.264 stream; verify hardware Media Foundation decode and portrait/landscape changes.
2. Minimal app: single window, fullscreen, disconnect, correct exit, deterministic cleanup, one active sender.
3. Hardening: replace/harden the known unsafe parser/state paths, enforce limits and worker isolation; validate negative cases.
4. Required audio: add the actual negotiated screen-audio codec and WASAPI with synchronization; video-only is not accepted as the completed product.
5. Acceptance: sustained session, repeated connect/disconnect, lock/unlock, rotation, network interruption, sleep/resume, no increasing memory/handle count, no leftover listeners/processes after close.
6. Measure actual CPU, GPU decode activity, private working set, frame pacing and glass-to-glass latency separately. Local decoder timing is not end-to-end latency.

No receiver implementation, installation, service creation or firewall modification was performed for this research. A separate project bootstrap has since been prepared.

## Native phone shell (2026-10-21)

The UI uses DirectComposition with a GPU video child and a separately invalidated
Direct2D/DirectWrite frame. The owned layered palette draws controls only, using Direct2D/DirectWrite.
GDI+ is retained only for development PNG serialization.
Microsoft API references:
- [Composition swap chain](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition)
- [HWND composition target](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-createtargetforhwnd)
- [Rounded composition clip](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nn-dcomp-idcompositionrectangleclip)

### Clarity and interaction refinement
- [Microsoft icon guidance](https://learn.microsoft.com/en-us/windows/apps/design/iconography/segoe-fluent-icons-font): use system Segoe Fluent Icons at standard 20-DIP control / 16-DIP chevron sizes; fall back to installed Segoe MDL2 Assets on Windows 10. No font is copied or redistributed.
- [Direct2D high-DPI guidance](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-and-high-dpi): separate physical video pixels from logical UI coordinates. Default video fit is capped at 1:1; native vector/text controls are rendered at monitor DPI, with panel strokes aligned to physical pixels.
- [Hover timing and placement guidance](https://learn.microsoft.com/en-us/windows/win32/uxguide/ctrl-tooltips-and-infotips): use system hover timing to avoid accidental opening and keep the initiating control unobstructed. The requested interactive volume flyout uses a stable adjacent card, a traversable hover region, leave grace, Escape dismissal and keyboard alternatives; it is not an informational tooltip.
- [Slider guidance](https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/slider): provide enough travel and immediate feedback. The horizontal slider has a 32-DIP vertical hit area, a separate percentage label and 1% keyboard steps.

The observed iPhone portrait stream was 498x1080. Display fitting can avoid loss
from unnecessary scaling but cannot reconstruct detail absent from the stream.
Negotiated capture capabilities and decoder resource bounds remain unchanged.

## Actual high-resolution mirroring and native application icon (2026-10-21)

The previous 498x1080 portrait limit came from the 1920x1080 H.264 advertisement,
not the window size. [UxPlay's maintained protocol reference](https://github.com/FDH2/UxPlay/blob/15a6b87172eca38b0df4e615fbf1bba108734dde/README.md)
documents feature bit 42 (ScreenMultiCodec) and higher-height HEVC requests.
The implementation independently parses bounded hvc1/hvcC configuration records;
no upstream parser or renderer was imported. Short zero alignment tails are
accepted outside hvcC, with nonzero/truncated boxes still rejected.

[Windows HEVC decoder documentation](https://learn.microsoft.com/en-us/windows/win32/medfound/h-265---hevc-video-decoder)
describes Annex-B input and D3D acceleration. MFTEnumEx activation, D3D11 awareness
and a 3840x2160 NV12 hardware configuration gate advertisement. Validated SPS
sizes are supplied to Media Foundation before output-type selection. Every output
sample must expose a decoder-bound DXGI surface; no software decode fallback.
Synthetic 4K landscape and 992x2160 portrait, then live iPhone 994x2160, passed on
the local RTX 4070 SUPER. Windows HEVC Video Extension 2.5.33.0 was already installed.

The [Windows icon resource guidance](https://learn.microsoft.com/en-us/windows/win32/menurc/about-icons)
informs the embedded multi-image ICO. The original design and generator are in
assets/app-icon.svg and scripts/Generate-Icon.py. Windows SDK rc.exe embeds the
icon/version resource, and the UI loads DPI-appropriate small/large icons.
Native tests load 16, 32, 48 and 256-pixel resource variants and check dimensions.
No third-party artwork, font, runtime library or icon installer was added.

## Connection startup and scheduling

[DnsServiceRegister](https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsserviceregister)
is asynchronous. Start the two registrations without serial waits, retain their
request data, and allow the listener/media loop to run while registration
completes. A manual-reset completion event remains observable across polls.
[Cancellation](https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsserviceregistercancel)
keeps callback context alive until completion; on an OS timeout at shutdown, at
most two contexts remain process-owned rather than risking a freed callback.

[Media Foundation stream messages](https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-processmessage)
permit flushing/ending a stream while retaining the transform object. Clear the
media types and surfaces between sessions, reuse the initialized H.264/HEVC
objects, and prepare types on the codec-configuration packet. Keep a fresh-object
fallback if a transform refuses reset. Never retain sender reference frames.

Receiver playout is now configurable at 60–250 ms, default 80 ms. NTP mapping,
/info latency and RECORD audio-latency samples use the same target. Initial video
waits briefly for a valid clock rather than failing a first-reply race. An idle
video socket is not a partially received record: the ten-second deadline starts
with the first byte and covers header plus payload; control/timing still own
connection liveness.

## Performance and lifecycle review

[Microsoft's DXGI latency guidance](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains)
supports a per-swap-chain presentation budget of one. The receiver checks the
waitable slot only when a frame/redraw is due and uses nonblocking Present,
retaining the newest due decoded frame when the display is busy. It does not
wait on a continuously ready display event while idle. Compressed dependencies
are preserved; the existing decoded queue bounds remain in force.

[IAudioClient::Initialize](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize)
requires zero duration and periodicity for shared event-driven audio. The actual
endpoint capacity and reported stream latency are logged rather than assumed.
Changing the buffer contract still needs physical synchronization acceptance.

Worker request writes and response reads now share one two-second deadline.
Overlapped operations are cancelled and completed before their buffers leave
scope. Fixed IPC scratch storage avoids repeated allocations; returned PCM
still owns its bounded queue entry. None of these local changes establishes
numeric phone-to-screen latency.

### Frame-deadline scheduling

[Windows high-resolution waitable timers](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
are available from Windows 10 version 1803. Use a per-instance one-shot timer
for the next frame's PTS, alongside audio, media-arrival and UI wake events.
The display-slot event is included only when useful; consuming it reserves that
slot for the ensuing draw. Older systems can use a regular waitable timer.
This avoids fixed polling and does not change global timer resolution.

[The codec low-latency API](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avlowlatencymode)
has an H.264 decoder exception: its VARIANT is VT_UI4; HEVC uses VT_BOOL.
Read back the codec and MF attribute settings rather than equating a fast submit
call with low pipeline latency. Track timestamp age at receipt, host queue time,
output readiness and presentation submission lateness separately. These are
receiver-stage measurements; they do not measure physical capture/display delay.

### Presentation timestamps already include the sender's scheduling

The [reverse-engineered stream format](https://openairplay.github.io/airplay-spec/screen_mirroring/stream_packets.html)
identifies the video NTP field as a presentation timestamp. The pinned UxPlay
implementation converts remote to local clock time and submits it directly to
the synchronized renderer; audio uses its RTP/NTP anchor on the same clock.
It does not add the advertised receiver latency a second time.

The live 60 ms trial showed timestamps about 75–85 ms ahead of receipt, followed
by another 60 ms of receiver delay. Removing that unconditional common offset
from both audio and video preserves their relative timing while avoiding the
duplicate wait. The --latency-ms option remains an advertisement to the sender;
normal deadline scheduling and audio endpoint compensation remain. This corrects
the earlier interpretation of the setting as an extra local playout buffer.
