# HEVC rotation and interaction regression

The user reported a persistent "HEVC decoded picture budget" error while rotating
the FFmpeg candidate. The local log showed successful 994x2160 hardware mirroring
followed by this exact parser rejection. The rejected SPS bytes were not recorded,
so its precise buffer count cannot be reconstructed from that session.

## Cause and bounded correction

The error came from `sps_max_dec_pic_buffering_minus1 <= 7`, an unconditional
eight-picture decoded-picture-buffer (DPB) cap. HEVC Main permits larger DPBs for
pictures that occupy a smaller fraction of their declared level's maximum area.
The replacement follows H.265 Annex A: up to 16, 12, 8 or 6 pictures as that area
increases. It checks the declared level, coded area, per-layer ordering and reorder
count. Coded resolution, packet/NAL sizes, bit depth and chroma limits stay intact.
This permits a valid 16-picture declaration for phone-sized frames without
permitting sixteen full-size 4K pictures. Crop dimensions do not reduce the coded
area used for the budget.

Primary reference: [ITU-T H.265, Annex A.4.1 and Table A.1](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-H.265-201304-S!!PDF-E&lang=e&type=items).
The receiver admits only Main/8-bit/4:2:0 through level 5.1. Its existing Windows
decoder dimension limits remain a separate admission boundary. New logs expose
only scalar SPS dimensions, level, DPB count and reorder count, including inline
parameter changes; parameter sets and phone media are never saved.

The screenshot also exposed duplicate status rendering: idle artwork/text remained
behind the independent error card. Error/recovery text now belongs to that card
alone, with bounded sizing so a narrow phone window keeps its controls available.

## Regression matrix

The automated scope is deliberately finite. It covers supported interactions and
failure transitions; it is not proof of every possible device/input combination.

| Boundary | Checks |
|---|---|
| HEVC admission | Level/area DPB boundaries, both phone orientations, valid 16-picture declaration, excessive buffers/reordering, sublayer ordering, truncated/mutated records, hvcC and inline SPS paths |
| Parser to GPU | Native synthetic encoder output is routed through the production Rust NAL parser before Media Foundation; repeated resolution/orientation changes exercise the same decoder/session |
| GPU picture lifecycle | Paused redraw, resize/default size/fullscreen, successful-presentation geometry, aspect fit, aperture/color, minimization/restore, interactive/synchronized scheduling and bounded device recovery |
| Controls | Mute/volume values and hover/drag/capture behavior without PCM, rail collapse/expand, keyboard/accessibility, close/disconnect, narrow/wide status cards and DPI geometry |
| Protocol lifecycle | Persistent/fragmented/pipelined requests, sender ownership, teardown/reconnect, malformed/state rejection, timeout/shutdown, 30/60 fps and advertised profiles/aspect |
| Preserved audio contract | AAC fixture decoding, reset hashes, worker isolation, decoder-delay mapping and generation/FLUSH fences; no audible tests |

Synthetic 16-picture decoder tests declare room for sixteen decoded pictures;
they do not claim a stream actually references all sixteen simultaneously.
Intrinsic UI tests call this application's own handlers in hidden test windows.
They use no desktop automation, external input injection or phone capture.
Generated-pattern pixel checks cannot access mirrored phone frames.

## Evidence and physical follow-up

Verified candidate SHA-256:
`0F6B9E3EF7A41DA65E7557C5E86C4F935DE473068894D7D043E89BD2AACA14BF`.

- 82 Rust tests pass with both FDK and FFmpeg features; strict Clippy and Rust
  formatting pass. The pure parser tests include 8,192 deterministic malformed
  HEVC mutations as well as the focused DPB boundary cases.
- `scripts/Test-Media.ps1 -Executable <candidate>` passes silently in 14.63 s on
  the local RTX 4070 SUPER. It validates 162 synthetic access units through Rust,
  four DPB16 SPS declarations and 160 actual DXGI decoder surfaces, along with
  status pixels, controls, recovery and scheduling checks. AAC-ELD/LC golden hashes
  across 480 frames and AppContainer denial checks also pass; audio_samples=0.
- `python -B scripts/Test-ControlLifecycle.py --exe <candidate>` passes three
  ownership/teardown cycles and all framing/state/shutdown checks. A bounded native
  TCP-table query positively identifies the test listener before exit and proves
  its removal afterwards; a connect timeout is not treated as proof of closure.
  Evidence: `local/control-lifecycle/20261021T190138449023Z/result.json`.
- `python -B scripts/Test-VideoProfiles.py --exe <candidate> --display-aspect
  994 2160 --fps 60` and the same with `--fps 30` pass all three profiles. Saved
  evidence: `local/profile-test/rotation-fps60-result.json` and
  `rotation-fps30-result.json`. These check advertisements, not physical rotation.
- Native evidence: `work/rotation-native-final-media.log` and
  `local/media-test/result.json`. Its tested work-directory EXE and the packaged
  candidate have the same hash. No sound playback, Computer Use or phone capture.

The parser-to-GPU test exposed a previously hidden fixture issue: the local
Windows encoder emitted reserved level_idc=15 without an explicit level request.
The synthetic encoder now requests `MF_MT_MPEG2_LEVEL=eAVEncH265VLevel5_1` (153),
and the production parser continues to reject reserved levels. The test stage
label also now preserves the first failing stage instead of masking it with a
later rotation label. No receiver decoder limit was relaxed for this fixture.

The corrected candidate was reopened for a quiet manual Settings/Photos rotation
test; discovery readiness is recorded in `local/rotation-phone-test/launch.json`
and the candidate's local log. The user confirmed stable, correctly sized rotation,
then clarified that Photos landscape still remains a portrait PC frame containing
a small upright letterboxed photo. Settings demonstrably alternates coded
1008x2160/display 994x2160 at level 4.1/DPB6 and coded 1008x464/display 994x458
at level 3.1/DPB12; the old fixed cap rejects that valid twelve-picture request.
Photos instead continues sending display 994x2160 with identical transport-header
dimensions. That separate external-canvas behavior is not fixed by DPB admission.
The candidate was subsequently closed by the user. Rotation capability and decoder
metadata remain under investigation; do not crop or rotate upright photos by guess.
Physical lock/unlock, network loss and A/V synchronization remain separate evidence. Both accepted FDK
executables retain SHA-256
`CCD1192625E6193A2CD2564621294CDEE810C8B2198B1639D40DA52E3755ED43`.

## Photos external-canvas investigation

The upright, letterboxed photo is inside the received portrait canvas; simply
turning the PC frame would turn the photo sideways or require guessed cropping.
Pinned UxPlay advertises `displays.rotation=false` and leaves ScreenRotate bit 8
off, while noting the paired true/bit-8 advertisement on Apple TV 3 and uncertainty
about its meaning. The [reverse-engineered feature table](https://openairplay.github.io/airplay-spec/features.html)
names the bit but provides no Photos-specific guarantee.

An opt-in `rotation-advertisement-experiment` Cargo feature now pairs that display
flag with bit 8, keeping the 994x2160 hint and display features 14 unchanged. It is
not enabled by normal packaging. The trial passed 82 Rust tests, strict Clippy,
silent parser/GPU/UI/codec checks and all three wire profiles; this verifies the
trial implementation, not its effect on Photos.

Trial EXE SHA-256:
`E74F70DD72E7F3B893216E8F28D05C44D6137A88F66ECF701AEBFBCE87CA4346`.
Evidence/manifest: `local/rotation-advertisement/`. The previous verified FFmpeg
candidate remains under `work/rotation-verified-candidate/`; both FDK fallbacks
are unchanged. The trial is temporarily open at the existing candidate path so
the phone can retest without requiring a different application firewall path.
Its BUILD.json explicitly marks it experimental and dirty against `8753080`.

Non-fatal native diagnostics now query the decoder's current output type for
rotation, minimum/geometric aperture and pixel aspect. They record only bounded
scalar values and distinguish absence/error; they do not change rendering or
capture media. The user reported that the manual Photos trial still fails and
provided a screenshot showing the full Photos interface rotated sideways inside
the portrait frame. With this trial, headers now distinguish source=994x2160 from
display=994x458 in landscape. MF reports rotation absent, minimum/geometric
apertures both 0,0:994x2160 and pixel aspect 1:1. Thus the header hint alone does
not establish the direction or justify cropping; scalar SEI/header diagnostics
and explicit per-picture GPU rotation handling are the next step.

The subsequent SEI-based trial (`A761D568...`) also failed on the phone: this
sender supplies no orientation SEI. That parser and its persistence rules were
removed. The following correction uses the actual transport field instead.

## Researched transport correction and display-update candidate

The configuration header contains an opcode at byte 4 and a separate
SymmetricTransform at byte 5. Treating those as one u16 opcode and masking its
high byte discarded orientation. The observed `0x0701` is opcode 1, transform 7.
The transform combines a quarter-turn bit (4) and axis-flip bits (2 and 1): pure
rotations 0/3/4/7 map to clockwise 0/180/90/270 degrees. Reflection and unknown
values fail closed. This is not EXIF numbering and `displays.rotation` is a
support Boolean, not an angle. The observed 851 vertical placement is half the
difference between 2160 and 458; it does not justify cropping the source image.

Protocol research references (facts only; no code imported from these sources):
- [Sender configuration-header writer](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/AirPlaySender.framework/AirPlaySender/AirPlaySender_19.mm)
  (`sbpd_createConfigBBuf`) and the corresponding MediaToolbox SymmetricTransform
  implementation establish the byte and transform composition.
- [Doubletake event channel](https://github.com/omarroth/doubletake/blob/5e2f1cbe3e1070bcefec8e549c7baf884727bc00/internal/airplay/event_channel.go)
  documents the legacy plaintext versus paired encrypted channel split.
- [Receiver command transport](https://github.com/45clouds/WirelessCarPlay/blob/51145ef55f8dd9f1cbadd58353cacb5e0ca215e9/source/Sources/AirPlayReceiverSession.c)
  and the sender's `Control.mm` command handler establish reverse RTSP
  `POST /command`, binary plist and replacement of the complete display dictionary.
- [Apple external-screen mirroring](https://developer.apple.com/documentation/uikit/uiscreen/mirrored)
  explains why an app can present its own external-screen content. No public
  receiver flag forcing literal Photos mirroring was found. Display input-device
  feature bits are not treated as a speculative mirror-only switch.

Every encoded frame carries the configuration's resolved angle through the Rust
queue, native input and exact decoded presentation timestamp. D3D11 performs the
rotation while source aperture coordinates remain unchanged. Destination axes
swap for quarter-turns. Shell geometry commits only after successful Present.
Two bounded GPU cache slots preserve both pixels and angle if Present is busy;
there is no production CPU readback. The extra cache is at most one NV12 texture
(about 12.53 MB at the maximum aligned dimensions).

The opt-in `display-update-experiment` adds the legacy eventPort listener and
implies rotation advertisement. A full `updateDisplayInfo` dictionary changes
width/height and widthPixels/heightPixels together while preserving UUID, refresh
rate and other capabilities. Only successfully presented frames matching the
established phone aspect can request a swap. The initial 500 ms coalescing period
is now 100 ms, with a separate 500 ms minimum send interval (PHOTOS_TRANSITION.md).
One outstanding reply, bounded framing, deadlines and a session/peer fence remain.
Unsupported commands disable updates while retaining the connection. There is
no HAP downgrade, baseline command, pixel heuristic or image crop. Normal
packaging leaves these experiments disabled until physical acceptance.

Candidate SHA-256:
`AF3B9439AFB4BC220FF4F42ED4B03FD6B7359B549B46390B6DC1F1D175A1C183`.
95 Rust tests and strict Clippy pass. Silent GPU checks pass all four rotations,
future-frame changes, busy-present cache preservation, decoded timestamp
association, 162 parser-validated access units and 160 hardware surfaces.
An intentional cache-overwrite negative control failed the pixel check as
expected; that hook was removed. AAC fixtures and worker isolation also pass,
with zero endpoint audio samples. All three advertisement profiles pass with
rotation capability enabled. These checks do not establish Photos convergence
on an actual iPhone; that focused quiet test remains pending.

The exact candidate also passes three silent control ownership/teardown cycles
with baseline `/info` checked after partial and full teardown. Separately, real
loopback event tests pass withheld-ACK cancellation and coalesced command/CSeq
handling. Evidence: `local/control-lifecycle/20261021T201559541218Z/`.
These are separate boundary tests, not an authenticated presented-frame plus
pending-event-ACK to new-owner integration. A restricted-token run could not
initialize AppContainer/socket access; the same checks passed with ordinary
desktop process permissions, without changing any protection policy.

Reproduce with CARGO_HOME=work/cargo:
```
cargo test --locked --offline --features ffmpeg-aac,display-update-experiment --target-dir work/ffmpeg-receiver-target
cargo clippy --all-targets --locked --offline --features ffmpeg-aac,display-update-experiment --target-dir work/ffmpeg-receiver-target -- -D warnings
cargo build --release --locked --offline --features ffmpeg-aac,display-update-experiment --target-dir work/ffmpeg-receiver-target
./scripts/Test-Media.ps1 -Executable work/ffmpeg-receiver-target/release/airmirror-lite.exe
python -B scripts/Test-VideoProfiles.py --exe work/ffmpeg-receiver-target/release/airmirror-lite.exe --display-aspect 994 2160 --expect-rotation
```

## Live event-response failure and correction (2026-10-22)

The user confirmed faster first-picture startup on F3B058..., then reported a
critical failure when rotating Photos. The preserved scalar log shows valid
transform 7/CW270 at 31056.241, `missing event response CSeq` at 31056.800, and
return to Ready at 31056.887. The app continued running for about nineteen more
seconds and shut down cleanly. No decoder/GPU failure is logged, and the recent
Windows event lookup found no matching application crash. The evidence points
to a dropped mirroring session after the event socket was closed, not a native
process crash. The peer's exact protocol/status was not recorded by that build.

The parser required CSeq for both RTSP and HTTP responses. Its error escaped the
scope owning the accepted event socket, so the socket closed before the optional
error was absorbed. Keeping the Rust media stop flag clear did not isolate that
observable disconnect from the sender.

The correction accepts omitted CSeq only for HTTP/1.1 with explicit bounded
Content-Length. Exactly one command remains outstanding, with no pipelining or
retry after an ambiguous response. Supplied CSeq values still require an exact
match; duplicate, malformed and missing RTSP CSeq remain errors. This follows
[HTTP/1.1 ordered response association](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.2).
The inspected pinned sender
[APTransport event handler](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/APTransport.framework/APTransport/APTransport.mm#L6895)
builds generic HTTP responses; it does not explicitly copy a CSeq in that handler.
This supports the compatibility path but does not prove the old phone reply's
unrecorded version. No upstream code was imported.

Any optional failure after accept now disables further display commands while
retaining that one socket until session teardown. Memory/framing/deadline limits
remain, late replies cannot restart commands, and shutdown remains stop-aware.
Scalar protocol/status/CSeq-presence/body-length diagnostics provide the missing
evidence without logging header values or body contents.

102 Rust tests and strict Clippy pass, including real loopback retention checks,
HTTP replies with and without CSeq, complete body consumption before another
command, explicit sequence errors, unsupported/negative statuses, malformed
responses, timeout/late reply and teardown EOF. The full silent native suite and
three control lifecycle cycles also pass with zero audio submitted. Corrected
EXE: `1FD89573D59425C39267AF93BEBEE0D9EE6DEFB91B20C555AD19C2657D456E94`.
Original evidence: `local/photos-event-failure/`; new physical rotation acceptance
is pending.

## Observed RTSP acknowledgment follow-up

The next phone check confirmed that socket retention prevents the disconnect.
It also supplied the missing protocol evidence: `RTSP/1.0 status=200`, no CSeq,
zero response body. The HTTP-only compatibility exception therefore still
disabled later display commands, explaining why subsequent photo opening could
keep the wrong display geometry. The user's initial correctly-sized report was
later narrowed to connection stability; photo-opening orientation remains open.

The compatibility exception now covers missing CSeq on this one serialized
legacy event connection for RTSP as well. This is an observed AirPlay deviation
from normal RTSP sequencing, not a general relaxation of RTSP control parsing.
Any supplied CSeq still must match exactly. There is one outstanding command,
no pipelining, no retry after a timeout, and no continuation after an ambiguous
reply. RTSP's absent Content-Length means an empty body under
[RFC 2326 sections 4.4 and 12.14](https://www.rfc-editor.org/rfc/rfc2326.html#section-4.4);
HTTP still requires explicit bounded length. New diagnostics distinguish an
omitted Content-Length from an explicitly empty response.

A real loopback regression alternates four portrait/landscape display commands
with omitted-CSeq RTSP acknowledgments and verifies every geometry and command
sequence. Empty replies with and without Content-Length are covered; explicit
wrong, malformed and duplicate sequences remain rejected for both protocols.
103 Rust tests and strict Clippy pass. Physical repeated-photo acceptance remains
pending. Prior log/manifest: `local/photos-rtsp-ack-failure/`.

## Remaining open-photo orientation boundary

On the muted audio-fixed build 897281..., the user confirms that the iPhone's
Photos interface changes orientation while the PC receives only the photo. Live
configuration/ACK logs verify landscape 2160x994 and portrait 994x2160 updates,
then fixed dimensions and transform zero during the reported unchanged photo
view. There is no new advertised angle for the receiver to apply in that view.
Evidence: `local/photos-external-feed-confirmed/`.

This matches Photos' separate external-content path. Apple's
[external-display documentation](https://developer.apple.com/documentation/uikit/uiscreen/mirrored)
describes apps replacing mirroring with independent content. Inspection of the
pinned sender found that transform changes produce type-1 configuration records;
type-5 APStats carries performance/system statistics, not a justified orientation
signal. The optional 25,000-byte stats trailer is generated random data in that
sender version. No payload was recorded or imported for this investigation.

The inspected Photos debug setting for ignoring mirrored/recording screens is
an on-device setting, with its recording-screen default gated by internal UI;
it is not a discovered receiver wire option. No speculative feature flag,
image-based rotation guess or source crop is added. A manual portrait/landscape
canvas control could offer a receiver-side workaround, but automatic physical
rotation while this independent photo view remains open is not resolved.
