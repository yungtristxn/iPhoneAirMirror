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
established phone aspect can request a swap. Requests coalesce for 500 ms with
one outstanding reply, bounded framing, deadlines and a session/peer fence.
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
