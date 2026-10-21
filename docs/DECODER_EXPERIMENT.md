# FFmpeg native AAC experiment

Date: 2026-10-21. Branch: `codex/ffmpeg-aac-experiment`, based on `e06321c`.

## Current timing contract and candidate

The original raw comparison below remains evidence of a 1,685-sample difference.
The follow-up implements a backend-neutral **additional decoder DSP delay**
contract, rather than padding FFmpeg to copy FDK's old scheduling behavior:

- Every successful native decode returns PCM and its additional delay atomically.
  The worker reply includes the configuration generation. The host checks the
  generation, exact negotiated frame size and bounded delay before using PCM.
- FDK reports and validates `CStreamInfo.outputDelay`: zero for ELD, 1,685 for
  the pinned LC concealment/limiter policy. FFmpeg's wrapper adds no such DSP
  stages and explicitly reports zero. `AVCodecContext::delay` stays diagnostic;
  it is not substituted for the FDK field.
- The returned block starts at `packet_rtp - additional_delay` in sample units.
  Trim only the part before this generation's first packet. The first LC FDK
  block is entirely priming; the next loses its first 661 sample frames.
  Thereafter blocks remain contiguous without a new FIFO or accumulated floating
  point clock. The existing shared RTP/NTP clock maps the corrected timestamp.
- FLUSH, reconfiguration, packet loss/discontinuous RTP and decode errors restart
  decoder state and the mapper together. Native failures latch until explicit
  configure; there is no hidden reopen that could silently restart priming.
  Old-generation replies and changing delay/frame size are rejected. Session and
  FLUSH fences still reject queued stale output before native playback.
  A separate transport RTP fence survives decoder errors and sequence-loss
  recovery, rejecting unflushed rewinds that could overlap already queued PCM.
- This removes **additional decoder** delay only. AAC transform/encoder priming
  and the sender's timestamp convention are not guessed from silence or fixture
  content. RTP wrap is handled with integer arithmetic. No tail drain is invented
  at FLUSH/teardown; old-generation delayed content is discarded.

The synthetic fixture clock starts at sample 441,000 and advances by each packet's
480 or 1,024 frames. Reports preserve raw PCM/lag and separately store normalized
PCM produced by the actual receiver mapper. The analyzer independently checks
that normalized bytes equal raw PCM after the declared priming, verifies exact
source intervals and reset metadata, then compares the common interval without
shifting it to improve the waveform score. Wrong-sign, omitted trim, stale
generation/delay and shifted-output controls must fail. This establishes decoder
relative scheduling, not the phone's capture clock or physical speaker timing.

FFmpeg conversion saturates finite float samples into i16 without adding a
lookahead limiter. FDK retains its existing limiter. Low-level fixture agreement
does not prove identical near-clipping dynamics; this is an explicit candidate
behavior difference, not an unreported limiter replacement.

`ffmpeg-aac` is now independent of `decoder-lab`. The latter still rejects normal
startup. `scripts/Package.ps1 -FfmpegCandidate` creates a runnable local receiver
in `dist/AirMirrorLite-ffmpeg-candidate` with FFmpeg provenance/notices and no FDK
linked. Default packaging still selects FDK; the accepted package and additional
fallback are preserved. Candidate physical A/V acceptance remains outstanding;
no audible testing, public distribution or default-package replacement is implied.

Reference semantics: [RFC 3640 section 2.6](https://www.rfc-editor.org/rfc/rfc3640#section-2.6)
describes compressed-unit presentation timestamps; the pinned FDK header documents
its additional `outputDelay`. [FFmpeg's context delay documentation](https://ffmpeg.org/doxygen/trunk/structAVCodecContext.html)
uses a distinct codec warm-up definition. Neither substitutes for measurement of
this AirPlay sender's end-to-end A/V synchronization.

## Follow-up verification

The schema-2 comparison passes normalized source-clock qualification for all four
fixtures. The raw LC gate remains failed at -1,685 samples; normalized nonperiodic
LC and ELD have zero residual sample offset and at most one i16-unit sample
difference. Both lab backends run ten sessions (10,080 measured packets each,
plus guards). A separate two-session comparison uses the exact receiver-candidate
binary. Evidence is under `local/decoder-experiment/delay-qualified-final` and
`local/decoder-experiment/receiver-candidate-delay-final`; each report records
the binaries' hashes. No post-hoc waveform alignment is applied to normalized
quality measurements. The LC common source interval is samples 441,000..562,195
(exclusive end); FFmpeg's unmatched 1,685-frame tail is explicitly excluded.

75 Rust tests pass on both backends, including generation/reset/wrap/model and
transport rewind controls. Strict Clippy and formatting pass. Seventeen analyzer
tests pass. The direct FFmpeg C ABI harness validates eight argument guards,
three latched decoder failures and three explicit reconfigurations per codec,
with unchanged PCM/metadata canaries on failure. The FDK/PlayFair ASan harness
passes and now reopens for each of 1,024 malformed AAC inputs so a previous error
latch cannot turn later cases into no-op parser checks. This is not whole-FFmpeg
sanitizer coverage.

The separately packaged receiver passes silent native UI/GPU tests on the local
RTX 4070 SUPER: 100 hardware decoder surfaces and 480 ELD/LC worker fixture frames,
with zero audio submitted. Silent loopback checks cover ownership, malformed
requests, state rejection and shutdown. These do not establish phone acceptance,
audible quality or physical A/V sync. Final candidate identity and clean source
provenance are in its `BUILD.json`; the accepted fallback hashes below are unchanged.

## Original experiment decision (before delay handling)

The native FFmpeg decoder is a viable candidate, but **not a drop-in replacement
under the current audio timing contract**. Both AAC-ELD and AAC-LC decode the
required fixtures with the correct frame counts and nearly identical aligned
PCM. ELD has zero relative sample offset. LC produces corresponding content
1,685 samples (38.2086 ms) earlier than FDK and fails the unchanged timing gate.
No compensation, tolerance relaxation or receiver migration was applied.

The working receiver remains at `dist/AirMirrorLite/AirMirrorLite.exe`, with an
additional copy at `dist/AirMirrorLite-fallback-e06321c/AirMirrorLite.exe`.
Both retain SHA-256
`CCD1192625E6193A2CD2564621294CDEE810C8B2198B1639D40DA52E3755ED43`.
The source fallback remains on `feat/airplay-mvp` at `e06321c`.
Identity/private logs were not copied into the extra fallback; the original
package and its data remain intact. The saved display aspect is preserved.

## Scope and dependency

The opt-in `ffmpeg-aac` feature replaces only `native/codec.cpp` with
`native/codec_ffmpeg.cpp`. It does not compile/link FDK. The default build remains
FDK. Both measured executables enable `decoder-lab`, which rejects normal receiver
startup before device/discovery initialization. FFmpeg enables that guard
automatically in that original experiment. Current comparison commands explicitly
select both features. The standalone CLI uses only embedded synthetic fixtures and the
existing zero-capability AppContainer/job/IPC path; no audio endpoint, window,
phone input, capture or public advertisement is used.

FFmpeg **9.0.2**, tag `n9.0.2`, commit
`946fcce07b6dcd0331c8cc609192aeff5e1924f8`, is pinned in
`scripts/ffmpeg-pins.json`. Archive SHA-256:
`8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`.
The official archive signature was checked against fingerprint
`FCF986EA15E6E293A5644F10B4322F04D67658D8`.

Only native `aac` (floating-point), `avcodec` and `avutil` are enabled. There are
no other decoders, encoders, parsers, demuxers, muxers, filters, network protocols,
programs, external codecs or nonfree options. The build uses x64 /MD, Clang-CL
21.1.8, MSVC 14.51.36231, SDK 10.0.26100.0, NASM 3.02 and runtime CPU dispatch.
Build tooling is extracted locally; no global installation or protection change.
The checked-in build recipe replayed successfully from fresh scratch directories.
Archive hashes differ between builds, so byte-identical reproducibility is not
claimed. The generated manifest records the actual library/configuration hashes.

## Observed results

Final comparison: `local/decoder-experiment/qualified-comparison/comparison.json`.
Each backend decoded four 120-packet fixtures, then ten fresh worker sessions with
eight configure/decode passes each: 10,080 measured packets per backend, plus
guard/control decoding. Steady sine/triangle fixtures verify signal content;
seeded nonperiodic stereo fixtures with silent boundaries establish temporal
alignment without periodic phase ambiguity.

| Nonperiodic fixture | FDK call p50 / p95 | FFmpeg call p50 / p95 | Relative content offset | Aligned max sample error |
|---|---|---|---|---|
| AAC-ELD, 480 samples/channel | 0.0386 / 0.06671 ms | 0.0269 / 0.0507 ms | 0 samples | 1 i16 unit |
| AAC-LC, 1024 samples/channel | 0.0679 / 0.1127 ms | 0.0363 / 0.064205 ms | -1685 samples | 1 i16 unit |

These are IPC-plus-decode wall times in a synthetic burst workload on the current
machine, not physical latency or general throughput guarantees. First-packet and
warm-call distributions are kept separately in the JSON. Observed worker-private
peaks were 2,551,808 bytes (FDK) and 4,276,224 bytes (FFmpeg), about 2.43/4.08 MiB.
Both memory/growth gates passed. Snapshots do not establish unsampled peak memory
or an indefinite leak test. Worker CPU totals were tick-quantized; their medians
were 46.875 ms and 31.25 ms per session and must not be treated as precise per-call
CPU measurements. No host/GPU cost or speaker emission time is measured here.

Both restricted-worker runs passed, including exact sample counts, per-backend
reset/session hashes, pinned legacy FDK hashes and all sixteen guard identities.
The **cross-backend adoption gate failed only on LC timing**, as intended; a
matching waveform after alignment does not hide that difference. Correlation of
the nonperiodic signals exceeded 0.9999999 and agreed with the onset measurement.

## Why LC differs

Runtime diagnostics report FDK additional `outputDelay=1685` for LC and zero for
ELD; FFmpeg reports `AVCodecContext::delay=0` for both. These fields have different
documented meanings and are explanatory metadata, not interchangeable clock APIs.
Source inspection agrees with the measured offset:

- FDK default energy-interpolation concealment adds one 1024-sample LC frame:
  `vendor/fdk-aac/libAACdec/src/conceal.cpp` (`CConcealment_GetDelay`) and
  `aacdecoder.cpp` add that delay to `CStreamInfo`.
- Non-low-delay profiles enable the default limiter; its 15 ms lookahead adds
  `floor(15 * 44100 / 1000) = 661` samples. See `aacdecoder_lib.cpp`,
  `libPCMutils/include/limiter.h` and `libPCMutils/src/limiter.cpp`.
- ELD selects a concealment mode without the extra frame and disables that
  limiter by default. Its zero measured offset is consistent with those choices.

The test does not prove that the previous LC physical A/V offset was correct.
Blindly padding FFmpeg by 1685 samples would preserve an existing behavior without
establishing the intended sender/PCM clock convention. The fixtures also do not
qualify replacement of FDK's limiter on near-clipping content.

## Verification and reproduction

66 Rust tests, strict Clippy and the experimental release build passed. Ten Python
analyzer tests reject shifted, swapped, silent, DC, gain-changed, spiked, corrupt
or incomplete data. Direct C-ABI checks cover null/size/capacity rejection, output
canaries, bounded malformed-input recovery and latched contract violations.
Those checks are not whole-FFmpeg sanitizer coverage. Both lab builds explicitly
reject normal receiver startup. All tests are silent; no Computer Use was used.

```powershell
$env:CARGO_HOME = Join-Path $PWD 'work/cargo'
./scripts/Build-FFmpeg.ps1 -Fetch
cargo build --release --locked --offline --features decoder-lab --target-dir work/decoder-fdk-target
cargo build --release --locked --offline --features ffmpeg-aac,decoder-lab --target-dir work/decoder-ffmpeg-target
./scripts/Test-FFmpegBoundary.ps1
python -B -m unittest discover -s tests/decoder-experiment -p "test_*.py"
python -B -O scripts/Test-DecoderExperiment.py --reference work/decoder-fdk-target/release/airmirror-lite.exe --candidate work/decoder-ffmpeg-target/release/airmirror-lite.exe
```

The original schema-1 comparison exits **1** for the known LC timing mismatch;
individual decoder runs exit 0. Do not turn this into a passing adoption result.
Development prerequisites: Rust, x64 MSVC/Windows SDK, PowerShell 7, Git Bash,
7-Zip and Python/NumPy. The recipe verifies/downloads optional local tools under
ignored `work/`; it never installs a service or changes global PATH.

Measured FDK lab EXE SHA-256:
`A4C04DFD3EF2694BD42628B4C462873DE826C0B651D028010DDC2251C0ACFE51`.
Measured FFmpeg lab EXE SHA-256:
`9BAF829FF90535DC39298BEC77B049583CE4FE955AD271E7EE22FF385509B7FA`.
Other evidence: `work/ffmpeg-*.log`, `local/decoder-boundary/result.json`,
`work/ffmpeg-aac-msvc/airmirror-ffmpeg.json`. These are local development artifacts.

## Remaining physical/release gate

Physical A/V calibration would require a separate explicit authorization for
sound. Keep the accepted working receiver as a fallback until that gate passes.
The native FFmpeg target avoids the known FDK/PlayFair link combination, but this
experiment is not a public release or a complete licensing/patent clearance.
