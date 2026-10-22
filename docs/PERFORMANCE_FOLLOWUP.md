# Connection performance follow-up (2026-10-22)

The user identified the delay as selecting AirMirrorLite to seeing the first
picture. Tests remained silent, with no Computer Use or Windows setting changes.

## Findings and changes

- Control and video listeners slept for 20 ms between nonblocking accept calls.
  They now use bounded Winsock readiness waits, waking when a connection arrives.
  Idle checks fall from up to 50 to 10 per second while cancellation remains
  bounded. [WSAPoll's documented listener readiness](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsapoll)
  supports this without changing global timer resolution.
- Startup previously checked an isolated worker and both AAC modes, destroyed it,
  then synchronously started another worker for encrypted SETUP. Exactly one
  preflighted worker is now retained and consumed after valid pairing/FairPlay
  checks. It is destroyed after the key operation and never reused for another
  sender. Later sessions use fresh workers. Sole ownership moves across threads;
  no outstanding I/O, COM object, or shared mutable worker state is transferred.
- The 1 MiB log cap was ineffective on Windows: an append-only file handle could
  not truncate, and failed truncation was ignored. A writable handle with a
  mutex-owned cursor now truncates and rewinds correctly, with conservative
  accounting on failures. Tests cover an oversized old file, reopen/append,
  exact limit, wraparound and denied truncation without ACL changes.
- Added bounded scalar timings for worker acquisition/decryption/destruction,
  first video connection/configuration/payload and initial clock wait. No media,
  request bodies, phone identities or keys are added to diagnostics.

## Measured comparison

Before EXE: `32AFE7CFF278F8CC33235DEB142DCA0DD89258EFDCF9DEEFA4333E704BC2C02B`.
After EXE: `F3B058A6E3D5732703661FDFE6CDB5FC067C31AC60D2BE81D4C38F5AF02CC9E9`.

`Measure-Connection.py` runs five fresh receiver processes, each with three
authenticated loopback sessions. It uses freshly generated synthetic sender keys
and the repository's public FairPlay test vector, then NTP and a video TCP
connection. It sends no video payload or audio SETUP. The timings include Python
sender work and loopback transport; they are not isolated CPU or phone latency.

| Median | Before | After |
|---|---:|---:|
| First-session claim through video SETUP reply | 45.497 ms | 6.909 ms |
| First-session encrypted SETUP round trip | 23.379 ms | 3.722 ms |
| First-session video transport-ready through accepted TCP log | 21 ms | below 1 ms |
| All-session video transport-ready through accepted TCP log | 20 ms | below 1 ms |
| All-session claim through video SETUP reply | 36.011 ms | 35.722 ms |

The retained worker principally benefits the first connection. All 15 after-run
sessions confirm one prepared worker per process, followed by fresh workers.
Video transport timing includes SETUP reply delivery, client connect and socket
configuration; its log timestamps have 1 ms resolution. Zero is not zero latency.

Fresh-process listener readiness with existing OS caches (three launches) was
394.4 / 395.0 / 389.0 ms before and 368.4 / 362.1 / 389.2 ms after. That small
sample does not establish a separate cold-start improvement.

An eight-second idle observation measured about 77.2 MiB total private memory
before and 83.4 MiB after. The retained worker accounted for about 4.9 MiB of the
after result; the host also varied by about 1.3 MiB. The worker is consumed and
exits after the first valid encrypted SETUP, or is closed at app shutdown. CPU
samples rounded to zero for both builds; no CPU reduction is claimed from this
short, tick-quantized observation.

## Validation and limits

98 Rust tests pass on the FFmpeg experimental and default backends; strict Clippy
and formatting pass. Silent native checks cover 162 parser-validated synthetic
access units, 160 hardware surfaces, controls/rotation/recovery and 480 AAC
fixture frames with zero endpoint audio. Three ownership/lifecycle cycles and
all three advertised video profiles pass. Restricted worker checks also pass.

Historical iPhone logs showed decoder preparation around 1.5–1.8 ms. One slow
connection spent 1,013 ms between video transport readiness and TCP acceptance;
another spent 159 ms between accepted TCP and its first codec configuration.
Those waits cannot be attributed to decoder initialization. New stage timing
helps locate any remaining delay; no resolution reduction, frame dropping or
buffer reduction was introduced to chase a synthetic benchmark.

Physical first-picture speed, latest Photos behavior and FFmpeg audio sync remain
separate acceptance boundaries. A quiet Settings reconnect can check the first;
no sound playback is requested or authorized here.

Evidence under ignored local/:
- `connection-{before,after}-20261022/result.json`
- `startup-perf-startup-{before,after}-20261022/result.json`
- `performance-idle-comparison.json` and the two referenced JSONL files
- `media-test/result.json`, `profile-test/result.json`
- `control-lifecycle/20261022T173556592449Z/result.json`

Reproduce connection measurements with Python 3 plus `cryptography` installed:
```
python -B scripts/Measure-Connection.py --exe <receiver.exe> --label <new-label>
python -B scripts/Measure-Startup.py --exe <receiver.exe> --label <new-label>
```
Both use hidden loopback receivers without discovery. The connection benchmark
checks receiver signatures, CSeq, framing, clean teardown and zero audio output.
