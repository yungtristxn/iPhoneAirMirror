# AAC timing and silent decoder qualification

FFmpeg is the default release-candidate backend. FDK remains an explicit local
reference; accepted fallback executables are preserved. Automated qualification
does not establish current physical sound or A/V acceptance.

## Timing contract

A decode returns exact stereo PCM, additional decoder DSP delay and a generation
atomically. The host validates frame size, generation and bounded delay, maps
PCM to packet RTP minus that delay and trims only generation priming.
FLUSH, reconfigure, discontinuity/loss and failure restart decoder/mapper
together. Old generations, changing delay and unflushed transport rewinds fail.
No extra FIFO, guessed encoder priming or teardown tail drain is introduced.

FDK reports additional outputDelay 0 for ELD and 1685 LC samples: 1024 from its
concealment policy and 661 from limiter lookahead. FFmpeg's wrapper adds no
such DSP stages and reports 0. AVCodecContext::delay is a distinct codec warm-up
diagnostic. FFmpeg saturates finite float samples into i16 without a lookahead
limiter; near-clipping dynamics need separate acceptance.

Sources: [RTP presentation timestamps](https://www.rfc-editor.org/rfc/rfc3640#section-2.6),
[FFmpeg context delay](https://ffmpeg.org/doxygen/trunk/structAVCodecContext.html),
and pinned FDK aacdecoder_lib.h/conceal.cpp/limiter.cpp in vendor/fdk-aac.

## Qualified synthetic comparison

Four 120-packet fixtures include known steady signals and nonperiodic stereo
content with silent boundaries. Raw comparison found LC content 1685 samples
earlier in FFmpeg, ELD 0. Blindly padding FFmpeg to reproduce old scheduling was
rejected.

Schema 2 uses the actual receiver's integer timestamp mapper and independently
checks metadata, trim, source intervals and repeated hashes. The common source
interval is compared without waveform realignment. Recorded normalized LC/ELD
offset is 0, maximum sample error one i16 unit; the raw LC difference remains
visible. Wrong-sign, missing-trim, stale-generation/delay and shifted controls
fail. This measures relative decoder scheduling, not phone/speaker timing.

Historical evidence:
- local/decoder-experiment/delay-qualified-final/: ten sessions per backend,
  10,080 measured packets each plus guards.
- local/decoder-experiment/receiver-candidate-delay-final/: two-session historical
  receiver-candidate comparison.
- local/decoder-boundary/result.json: C-ABI guards/canaries, failure latching and
  reconfiguration. Not whole-FFmpeg sanitizer coverage.
- work/ffmpeg-aac-msvc/airmirror-ffmpeg.json: library/configuration provenance.

Historical nonperiodic IPC-plus-decode p50/p95: FDK ELD 0.0386/0.06671ms,
FFmpeg ELD 0.0269/0.0507ms; FDK LC 0.0679/0.1127ms, FFmpeg LC 0.0363/0.064205ms.
Worker-private snapshots peaked around 2.43/4.08 MiB respectively. Synthetic burst
timing, sampled memory and tick-quantized CPU are not general performance claims.
Current validation belongs in STATE.md.

## Dependency and reproduction

FFmpeg 9.0.2, n9.0.2 commit 946fcce07b6dcd0331c8cc609192aeff5e1924f8 is pinned in
scripts/ffmpeg-pins.json. Archive SHA256:
8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e.
Recorded signature fingerprint: FCF986EA15E6E293A5644F10B4322F04D67658D8.

Only native AAC, avcodec and avutil are enabled: no other codecs, network,
formats, programs, external/nonfree/GPL options. The x64/MD recipe uses pinned
local Clang-CL/NASM tooling. Fresh archive hashes can differ; byte-identical
reproducibility is not claimed. Manifests record actual hashes. FFmpeg builds
do not compile/link FDK.

```powershell
$env:CARGO_HOME = Join-Path $PWD 'work/cargo'
./scripts/Build-FFmpeg.ps1 -Fetch
cargo build --release --locked --offline --no-default-features --features decoder-lab --target-dir work/decoder-fdk-target
cargo build --release --locked --offline --no-default-features --features ffmpeg-aac,decoder-lab --target-dir work/decoder-ffmpeg-target
./scripts/Initialize-Worker.ps1 -Executable work/decoder-fdk-target/release/airmirror-lite.exe
./scripts/Initialize-Worker.ps1 -Executable work/decoder-ffmpeg-target/release/airmirror-lite.exe
cargo test --locked --offline --features decoder-lab
./scripts/Test-FFmpegBoundary.ps1
python -B -m unittest discover -s tests/decoder-experiment -p 'test_*.py'
python -B -O scripts/Test-DecoderExperiment.py --reference work/decoder-fdk-target/release/airmirror-lite.exe --candidate work/decoder-ffmpeg-target/release/airmirror-lite.exe
```

Both lab builds reject receiver startup. The comparison harness/nonperiodic
fixtures are compiled only with decoder-lab; ordinary receivers reject
--decoder-experiment. Only embedded fixtures and new output directories beneath
local/ are accepted. No endpoint, UI or discovery opens. Python analysis needs
NumPy; neither test tool installs dependencies.

Schema 1 intentionally failed the LC timing gate and cannot acquire schema 2
qualification retroactively. Analyzer contracts: tests/decoder-experiment/README.md;
fixture provenance: tests/fixtures/README.md. All checks remain silent.
