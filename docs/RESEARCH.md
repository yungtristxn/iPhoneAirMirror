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
