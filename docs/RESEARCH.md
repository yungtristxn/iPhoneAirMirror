# Architecture and implementation references

Current design, consolidated 2026-10-23. Scope is in SPEC.md, evidence in
VALIDATION.md/STATE.md and imported-source provenance in THIRD_PARTY.md.
Superseded experiments remain in Git history.

## Ownership and media path

The Rust host owns bounded request/plist/codec-record parsing, pairing, discovery,
the single sender and clocks. src/native.rs provides a narrow C ABI to a Windows
SDK C++ bridge, the implemented alternative to Rust Windows bindings.

Validated compressed video decodes through Media Foundation into D3D11 surfaces.
DirectComposition/DXGI renders the rounded phone window with a separate rail.
Reception performs no CPU readback or re-encoding. Tests can read generated
marker/black surfaces, not phone pixels.

AAC and retained PlayFair C code run in the executable's restricted-worker mode.
A zero-capability AppContainer and one-process/256 MiB kill-on-close job bound
it. Private bounded IPC shares a two-second deadline across request/reply.
Canceled overlapped I/O completes before buffers leave scope. Worker access is
provisioned explicitly, never weakened or replaced by an unrestricted fallback.

## Video and scheduling

H.264 SDR is the compatibility profile; native HEVC Main 8-bit 4:2:0 SDR is
capability-gated. Parsers bound coded/aligned dimensions, reference/decoded-picture
requirements and rotation. Unsupported profiles, depth, chroma, reflections and
metadata fail before submission.

Transforms are reused where reset permits; reference state, queues and owner
generations are cleared. Configuration prepares types before payload arrival.
Decoded queues preserve decoder-pool headroom. Successful presentation retains
a private GPU copy in two bounded cache slots, allowing paused redraw without
holding a decoder-owned surface. Ordinary format changes keep the old private
frame's aperture/color/rotation until a fresh replacement presents. Suspension,
owner reset and failure clear it.

Only successful Present commits readiness, shape and composition attachment.
Busy/occluded submissions retain the candidate. One-shot PTS timers, WASAPI,
media wakeups and conditional swap-chain readiness avoid fixed polling/global
timer changes. Advertised latency already appears in sender timestamps and is
not added again locally. Configured audio/video follow one RTP/NTP mapping;
screen-only mode can display fresh decoded frames promptly. Mute does not
change synchronization policy.

References:
- [D3D11/MF decode](https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation)
- [H.264](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder),
  [HEVC](https://learn.microsoft.com/en-us/windows/win32/medfound/h-265---hevc-video-decoder)
- [Low-latency API](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avlowlatencymode):
  H.264 uses VT_UI4; HEVC uses VT_BOOL.
- [Stream changes](https://learn.microsoft.com/en-us/windows/win32/medfound/handling-stream-changes)
- [DXGI latency](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains)
- [Waitable timers](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)

## Audio

FFmpeg supplies only native AAC and avcodec/avutil support. The worker returns
exact stereo PCM counts, a generation and additional DSP delay. The host maps
packet RTP minus that delay and trims only generation priming. DECODER_EXPERIMENT.md
records the independently checked FDK/FFmpeg comparison and limits.

Bounded PCM queues retain timestamps/order during bursts. Producers cancel on
stop, owner/FLUSH changes or failure; saturation has a bounded deadline. Native
playback waits for endpoint capacity instead of failing temporary fullness.
Event-driven shared WASAPI fills before Start. Device failures use bounded
owner-thread recovery; failure leaves actionable status and disconnects without
silently substituting another media path.

[WASAPI initialization](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize)
and [RTP timestamps](https://www.rfc-editor.org/rfc/rfc3640#section-2.6).

## Discovery and pairing

Native DNS-SD publishes AirPlay/RAOP on one eligible LAN interface using the
machine's actual .local hostname. Arbitrary service-target registration does
not establish its A/AAAA address record. Registration is asynchronous; callback
contexts outlive completion/cancel. At most two remain process-owned if Windows
fails to complete shutdown, avoiding a freed callback context.

Default pairing is verified legacy. HAP is explicitly experimental and uses
verified transient SRP/authenticated control, event and media records; it never
downgrades on failure. Neither profile is a persistent admission allowlist.
VERIFIED_PAIRING.md retains crypto pins/protocol boundaries; STARTUP_TRACE.md
records the unresolved selection delay.

- [DNS-SD](https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsserviceregister),
  [cancellation](https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsserviceregistercancel)
- [AppContainer](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer)
- [Pinned UxPlay reference](https://github.com/FDH2/UxPlay/tree/15a6b87172eca38b0df4e615fbf1bba108734dde)

## Photos, orientation and screen suspension

Configuration byte 4 is opcode; byte 5 is SymmetricTransform. Pure rotations
0/3/4/7 map to CW0/180/90/270; reflections/unknown values fail. Each compressed
input and decoded timestamp retains its angle. D3D11 swaps destination axes for
quarter-turns while source aperture coordinates remain unchanged.

Display updates send complete reverse-RTSP updateDisplayInfo dictionaries only
for presented screen shapes: 100 ms settling, 500 ms minimum send interval and
one outstanding reply. The observed iPhone omits CSeq on this serialized channel;
explicit mismatches fail. Unsupported commands disable updates while retaining
the connection. No pixel guesses or cropping.

Photos can send an independent photo-only view. Missing phone UI cannot be
reconstructed, and physical rotation cannot be inferred when the feed omits it.
That remaining behavior is accepted. Exact opcode 1 options 0x56/0x5e suspend;
0x16/0x1e resume. Unknown flags retain prior state. Visibility generations block
old queued/decoded/paused frames; resume reveals only a fresh successful Present.
Audio remains independent; still photos have no blackout timeout.

Wire facts only; no proprietary implementation imported:
- [Sender header writer](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/AirPlaySender.framework/AirPlaySender/AirPlaySender_19.mm)
- [Receiver commands](https://github.com/45clouds/WirelessCarPlay/blob/51145ef55f8dd9f1cbadd58353cacb5e0ca215e9/source/Sources/AirPlayReceiverSession.c)
- [Event mapping](https://github.com/omarroth/doubletake/blob/5e2f1cbe3e1070bcefec8e549c7baf884727bc00/internal/airplay/event_channel.go)
- [Suspend/resume](https://github.com/FDH2/UxPlay/blob/15a6b87172eca38b0df4e615fbf1bba108734dde/lib/raop_rtp_mirror.c#L580-L588)
- [Apple external content](https://developer.apple.com/library/archive/documentation/WindowsViews/Conceptual/WindowAndScreenGuide/UsingExternalDisplay/UsingExternalDisplay.html)

## Controls

Physical picture pixels and logical DPI-scaled UI geometry are separate.
DirectComposition clips and a narrow gradient bezel frame the video. Direct2D
paths draw the control icons, while DirectWrite handles labels; icon-font lookup
is no longer required. The light rail is shoulder-docked with the collapse tab
aligned to its first row. A shared control_layout.h grid supplies action centers,
hit targets, tooltip regions and UIA bounds. Reset and fullscreen have distinct
silhouettes; 44-DIP action targets and keyboard behavior remain. The shell
fits monitor work areas and keeps the rail accessible. A software status panel
survives D3D failure. UIA exposes names, roles, focus, invoke/toggle and volume
through bounded cross-thread requests with safe detach. Idle artwork is isolated
in idle_artwork.h, adapts to portrait/landscape/compact widths, and is skipped
for video, screen suspension and error cards. Its gradients and device diagram
are static native vectors: no new idle timer, bitmap or runtime dependency.

[Composition swap chain](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition),
[DPI](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-and-high-dpi),
[hover guidance](https://learn.microsoft.com/en-us/windows/win32/uxguide/ctrl-tooltips-and-infotips).

Implementation/source checks do not establish physical screen-reader behavior,
device-loss recovery, sustained performance or end-to-end latency.
