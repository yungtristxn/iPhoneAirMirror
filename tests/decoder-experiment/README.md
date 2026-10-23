# Silent decoder replacement experiment

This developer-only path compares the existing FDK decoder with the experimental
FFmpeg backend. It opens no window, WASAPI endpoint, discovery listener or media
capture. Do not point it at the preserved `dist` executable. Neither tool builds
or installs dependencies. Python analysis uses the existing development NumPy.

Root-level builds provide two separately provisioned experimental executables.
Use `--no-default-features --features decoder-lab` for the FDK control and `--no-default-features --features ffmpeg-aac,decoder-lab` for
the candidate, with separate target directories. Both lab builds reject normal
receiver startup, making the comparison independent of unrelated receiver code.
The normal default build uses FFmpeg; preserved FDK packages remain unchanged. See
`docs/DECODER_EXPERIMENT.md` for the measured raw LC offset, timing policy and build commands.
The runner is available on each with:

```powershell
<exe> --decoder-experiment --experiment-out <new-absolute-local-subdirectory> --experiment-sessions 10
```

The output parent must already exist beneath this repository's ignored `local/`.
The runner refuses an existing output directory and uses create-new output files.
Only four embedded synthetic fixtures can be decoded. First-pass little-endian
stereo i16 PCM and a bounded `report.json` are written; user input files cannot be
supplied. The ordinary receiver and working FDK package are not modified.

Run both binaries and compare, or analyze already generated reports:

```powershell
python scripts/Test-DecoderExperiment.py --reference <instrumented-fdk-exe> --candidate <ffmpeg-exe>
python scripts/Test-DecoderExperiment.py --reference-report <fdk-report.json> --candidate-report <ffmpeg-report.json>
python -B -m unittest discover -s tests/decoder-experiment -p "test_*.py"
```

The existing 120-packet ELD sine and LC stereo triangle fixtures remain unchanged.
Two new nonperiodic fixtures use distinct seeded, twice-low-pass-filtered integer
noise, 2048 initial silent samples, 4096 trailing silent samples, and irregular
997-sample envelope gaps. Generate them with the pinned development encoder:

```powershell
<fixture-generator> tests/fixtures/noise-44100-stereo-eld-experiment.frames eld experiment
<fixture-generator> tests/fixtures/noise-44100-stereo-lc-experiment.frames lc experiment
```

The experiment validates exact sample counts, both legacy FDK PCM golden hashes,
per-backend configure/reset/session determinism, low-level malformed input guards
and recovery, and the real restricted-worker path. AAC concealment may produce
bounded PCM from malformed data, so corruption is not assumed to require an error.
Empty/oversized payloads and unsupported configuration must be rejected. Every
guard is followed by a known-valid control; contract failures may need a new
restricted worker. Decode calls remain bounded by the existing IPC deadline.

Cross-decoder PCM need not be bit identical. The analyzer reports correlation,
normalized RMS error, gain ratio, maximum sample error and channel/source checks
after bounded alignment. Its declared engineering gates are correlation >=0.995,
normalized RMS error <=0.10, RMS ratio 0.95–1.05, and maximum error <= the larger
of 64 i16 units or 25% of reference peak. These are fixture-compatibility gates,
not listening-quality or perceptual-fidelity scores.

Steady tones have periodic lag ambiguity. Only the nonperiodic fixtures can
qualify temporal alignment: the correlation peak must be distinguishable from
other lags, agree with the coarse onset measurement, and differ by at most 44
samples (about 1 ms) for a drop-in replacement. A larger extra delay is reported
and fails even if the aligned waveform is otherwise similar. The tools do not
infer physical speaker/display synchronization from codec alignment.
Raw diagnostic delay remains explanatory; the two libraries' fields are not
interchangeable. Schema 2 also records the atomic additional-DSP-delay contract
and PCM normalized by the production integer timestamp mapper. Each generation
starts at known source sample 441,000. The analyzer checks metadata, trimmed
bytes, exact source intervals and repeated hashes, then compares their common
source interval without aligning the quality samples. The same temporal gate
applies; negative tests cover missing trim, wrong sign, stale delay/generation
and shifted PCM. `raw_adoption_gate` remains visible independently of
`normalized_adoption_gate`, which determines current qualification. Schema 1
reports cannot acquire normalized qualification retroactively.

Timing includes per-call IPC/decode wall time (p50/p95/max) and worker CPU from
`GetProcessTimes` before each process exits. The p95 call must fit the fixture's
audio-frame budget. First-packet and warm-packet distributions are separate;
the latter exclude packet zero of each newly configured pass. `GetProcessTimes`
uses 100-ns units, but short worker lifetimes can report zero or tick-quantized
CPU totals. These values do not establish zero CPU cost or precise per-packet CPU.
Memory results are worker-private-byte snapshots sampled
through repeated sessions, checked against the 256 MiB job bound and material
growth. They are not unsampled peaks, host-memory measurements, or proof of
indefinite stability. These results do not establish real-iPhone compatibility,
audible quality, A/V synchronization or end-to-end latency.

`scripts/Test-FFmpegBoundary.ps1` separately compiles the direct C-ABI harness
with MSVC /MD /O2 /W4 /WX and the verified static FFmpeg libraries. Output canaries,
invalid pointers/lengths/capacities, valid controls and malformed recovery are
checked without an endpoint, window or socket. Upstream public headers are
classified as external; our wrapper/harness remain strict. The script records
input/library/compiler/executable hashes under `local/decoder-boundary`. These
canaries do not replace whole-library memory instrumentation.
