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
