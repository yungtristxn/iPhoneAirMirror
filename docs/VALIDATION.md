# Validation boundaries

## Commands

From the repository root, PowerShell (MSVC C++ tools and Rust installed):

```powershell
$env:CARGO_HOME = Join-Path $PWD 'work/cargo'
cargo fmt --check
cargo clippy --all-targets --locked --offline -- -D warnings
cargo test --locked --offline
cargo build --release --locked --offline
./scripts/Test-Media.ps1
./scripts/Test-Control.ps1 -Executable "$PWD/target/release/airmirror-lite.exe"
```

Native ASan harness (no phone/network/UI):

```powershell
$env:CARGO_TARGET_DIR = Join-Path $PWD 'work/san-target'
cargo run --manifest-path tests/native-sanitizer/Cargo.toml --locked --offline
Remove-Item Env:CARGO_TARGET_DIR
```

The native harness requires the MSVC AddressSanitizer component. It compiles
the retained native sources with `/fsanitize=address`; no sanitizer runtime ships
in the receiver package. The AAC fixture generator is also development-only.

## What each result establishes

| Check | Evidence | Does not establish |
|---|---|---|
| Rust tests | bounded framing/plist expansion, pairing proof/state/replay, FairPlay mode bounds, H.264 SPS/NAL budgets, RTP framing, shared-clock arithmetic, audio-mode/teardown parsing | iPhone compatibility |
| Native ASan | known-answer and deterministic malformed-input execution with memory instrumentation and output canaries | complete native-code safety or exhaustive fuzz coverage |
| Native media probe | synthetic H.264 decoded to `D3D11_BIND_DECODER` textures on RTX 4070 SUPER; GPU presentation and paused redraw; generated AAC-ELD decoded in AppContainer and written to WASAPI | phone sound, physical A/V synchronization or glass-to-glass latency |
| Worker probe | AppContainer token has zero capabilities; host canary read denied; worker UDP probe not delivered to a host listener | formal sandbox certification |
| Loopback control | actual requests, single owner, invalid framing/sequencing rejection, listener survival and timed shutdown | physical disconnect/lock/Wi-Fi behavior |
| Phone test | user's observations plus negotiated codec, decoder-surface and audio counters in the bounded local log | durations/scenarios not actually exercised |

## Resource bounds

RTSP headers 8 KiB, bodies 64 KiB, four control connections, connection/record
deadlines and per-connection request rates. Binary plists: 1,024 objects, depth 16,
4,096 expanded visits, 512 KiB expanded-data budget. Video: 2 MiB compressed
record, eight queued compressed records, a 500 ms backpressure deadline, at most
eight decoded samples and admission below six to preserve decoder-pool headroom.
SPS rejects unsupported profiles/bit depths/interlacing, oversized pictures and
more than eight references before native decode. Screen target is H.264 SDR,
1920x1080-class (rotation allowed), up to 60 fps.

Audio: stereo 44.1 kHz AAC-ELD (480 samples/frame) or AAC-LC (1,024), 8 KiB packet,
128-packet jitter window, 32 PCM blocks per host queue; native worker has a
256 MiB process-memory limit and a two-second reply deadline. Its job permits
one process and terminates on host close. Logs rotate at 1 MiB without media,
keys or raw negotiation bodies.

## Physical acceptance

Keep the source/build hash with each run. Note phone/iOS when known, GPU,
resolution, frame-counter deltas, workload and duration. `Measure-Receiver.ps1`
records host/codec CPU (percent of one core), working set, private bytes, handles
and threads. JSONL traces and the bounded receiver log stay under ignored local
directories. Working-set sums may include shared pages; private bytes are separate.

The shared NTP clock and 120 ms playout target are implementation settings, not
measured end-to-end latency. Measure phone capture-to-PC display separately with
a synchronized visual counter/high-speed observation if a numeric latency claim
is needed. The user shortened the sustained test to five minutes; no 30-minute
or long-term drift claim should be made from it.

Current evidence and unresolved checks are in STATE.md. The final package is a
local development build; third-party distribution licensing is not resolved.
