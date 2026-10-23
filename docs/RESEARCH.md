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
DirectComposition clips and a narrow gradient bezel frame the video. DirectWrite
renders installed Segoe Fluent Icons at recommended sizes, with Segoe MDL2 Assets
fallback on Windows10; fonts are not bundled. Direct2D paints functional control
surfaces. The light rail has a separate whole-rail disclosure button, not a sound
submenu. Shared control_layout.h metrics supply action centers, hit targets,
tooltips and UIA bounds. The shell fits work areas and preserves keyboard access.
A software status panel survives D3D failure. UIA uses bounded cross-thread
requests and safe detach. Idle artwork is a compact neutral waiting state and is
skipped during video, suspension and error display. It has no animation timer.

[Composition swap chain](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition),
[DPI](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-and-high-dpi),
[hover guidance](https://learn.microsoft.com/en-us/windows/win32/uxguide/ctrl-tooltips-and-infotips).

Implementation/source checks do not establish physical screen-reader behavior,
device-loss recovery, sustained performance or end-to-end latency.

The installer/API floor is Windows 10 1703 (build 15063): the native shell directly
imports [SetProcessDpiAwarenessContext](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setprocessdpiawarenesscontext).
This version gate prevents loader failure on earlier Windows 10 builds; it is
not physical validation across every Windows version admitted by the installer.

The per-user installer uses Inno Setup 6.7.1. Its file callbacks catch script
exceptions, so raising from AfterInstall is not a rollback/failure contract.
Preflight extracts the same EXE to temporary storage and prepares it before
application writes. A later installed-file preparation failure sets a nonzero
custom exit code and repair state. Inno Exec resets child error mode; a small
bounded CreateProcessW bridge preserves the mode needed for silent loader errors.
The compiler engine and loader must be i386 to match its reviewed record layouts.
[Inno events](https://jrsoftware.org/ishelp/topic_scriptevents.htm),
[pinned callback implementation](https://github.com/jrsoftware/issrc/blob/7d30127fdb2ca8b7c0617e8d616e8625bf6ecd99/Projects/Src/Setup.MainFunc.pas),
[error mode inheritance](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-seterrormode).

## UI correction researched before implementation (2026-10-23)

The user's supplied screenshot is evidence for this bounded visual review, not a
live usability/accessibility audit. No Computer Use or app capture was performed.
1. Idle view: oversized decorative devices, duplicate readiness information,
   promotional copy and a tutorial card compete with the receiver's simple task.
   Correct with one quiet waiting state and concise connection instructions.
2. Commands: bespoke symbols vary optically; attaching disclosure to sound makes
   it resemble a volume dropdown. Use installed Windows symbols and a separate
   whole-rail disclosure control. Keep hit targets, tooltips and UIA in one grid.

The user explicitly chose to keep the light toolbar. Palette preference is not
prescribed by platform guidelines. The phone frame and media behavior are retained.

Primary references reviewed before any source edit:
- [Microsoft icons](https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/icons):
  recognizable, simple symbols that harmonize with the interface.
- [Segoe Fluent Icons](https://learn.microsoft.com/en-us/windows/apps/design/iconography/segoe-fluent-icons-font):
  standard 16/20/24/32-DIP sizes; Windows11 supplies the font. Use the installed
  Segoe MDL2 Assets fallback on Windows10, without redistributing fonts.
- [Windows typography](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/typography):
  use a restrained type hierarchy; this view uses 20/28 subtitle and 14/20 body.
- [Apple toolbar guidance](https://developer.apple.com/design/human-interface-guidelines/toolbars?changes=_2):
  deliberate commands, stable grouping, and reliably restorable hidden controls.
  These are interaction principles, not an instruction to copy Apple materials.
- [Reflector's name/control visibility](https://help.airsquirrels.com/reflector/232-show-or-hide-the-device-name-and-record-button-in-the-reflector-window):
  secondary interface information may be hidden or revealed on hover. This is
  documented behavior, not evidence from a live review of that product.

Keyboard behavior, contrast over arbitrary desktop backgrounds and physical
screen-reader use cannot be established from the supplied screenshot alone.
Native intrinsic regressions and generated artwork are checked separately.

The subsequent interior-color correction removes the residual blue cast from
the idle canvas, text and glass lip. Microsoft recommends neutral surfaces and
sparing accent color; its Windows best-practices guidance favors softened dark
and light tones over pure black/white UI. For this phone-shaped media surface,
the chosen fixed idle palette is #080808, #E6E6E6 primary, #A3A3A3 secondary and
#8C8C8C icon. These exact shades are a design choice, not prescribed platform
tokens. The light toolbar retains its chosen colors. Media pixels and the true
black suspension surface are unaffected.
[Windows color](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/color),
[Windows visual style](https://learn.microsoft.com/en-us/windows/apps/get-started/best-practices).
Primary/secondary text contrast is 16.05:1 / 7.94:1 against the idle background,
using the [W3C contrast calculation](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html).
This is a palette measurement, not full accessibility or physical-display
certification. Generated native artwork verifies neutral interior RGB values.
