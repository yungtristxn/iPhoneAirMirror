# FFmpeg native AAC experiment

Date: 2026-10-21. Branch: `codex/ffmpeg-aac-experiment`, based on `e06321c`.

## Decision

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
automatically. The standalone CLI uses only embedded synthetic fixtures and the
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
cargo build --release --locked --offline --features ffmpeg-aac --target-dir work/decoder-ffmpeg-target
./scripts/Test-FFmpegBoundary.ps1
python -B -m unittest discover -s tests/decoder-experiment -p "test_*.py"
python -B -O scripts/Test-DecoderExperiment.py --reference work/decoder-fdk-target/release/airmirror-lite.exe --candidate work/decoder-ffmpeg-target/release/airmirror-lite.exe
```

The final comparison currently exits **1** for the known LC timing mismatch;
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

## Next gate

Define the backend-neutral PCM/source-timestamp and limiting policy, associate any
decoder delay with configure/flush/reset generations, and validate with known
source-clock fixtures. Physical A/V calibration would require a separate explicit
authorization for sound. Keep the working receiver until those gates pass.
The native FFmpeg target avoids the known FDK/PlayFair link combination, but this
experiment is not a public release or a complete licensing/patent clearance.
