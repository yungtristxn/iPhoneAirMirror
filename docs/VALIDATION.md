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

## Native UI regression (2026-10-21)

`--self-test` also exercises the hidden native shell: no caption/client inset,
portrait/landscape aspect locking, bottom-right hit testing, default-size reset,
rotation during fullscreen, hover slider, mute/restore, volume limits, owned
palette lifetime flags, stable speaker position when the hover slider opens at
the top screen edge in a compact landscape window, and close/disconnect events. The existing GPU probe then
checks landscape/portrait decode, paused redraw, resizing and fullscreen against
the DirectComposition path. The idle renderer checks that unmodified ticks do
not repaint the chrome.

`--self-test --ui-test-dir local/qol-test/renders` saves the native idle/connecting
artwork and control palette as PNG diagnostics. This is internal rendering of
app artwork, not desktop automation, screen capture or phone recording. The
export is refused when a video surface is active. Inspect the diagnostics for
clipping, typography and alpha corners, including a compact landscape window.
They do not establish final desktop composition, mouse feel or physical phone
acceptance. The user requested no Computer Use for this validation.

Production video remains on D3D11 surfaces. Direct2D/DirectWrite paint idle chrome
only when invalidated; GDI+ paints only the small control palette. Both are Windows
components. There is no web runtime, UI framework package or new third-party source.
Local gain is applied at the final WASAPI write so queued PCM responds to mute and
volume changes; sender gain and media timestamps remain independent.

## Historical physical performance before the phone-shell UI

The five-minute sample (shortened at the user's request) used 498x1080 video on
RTX 4070 SUPER driver 32.0.16.1692 / Ryzen 7 7800X3D: 59.30 decoded and 57.76
presented fps, 13,230,000 audio samples; GPU Video Decode counters 1.89–1.93%.
Host private memory was 89.81→89.91 MiB, working set 76.51–79.45 MiB, handles
711→708. Codec private memory was 2.33→2.40 MiB, handles 153→153. User reported
synchronized sound and no freezes. This preceded the socket-spin fix.

The subsequent real-phone CPU sample on 2026-10-21, 12:11:55–12:12:25 UTC,
covered 29.46 seconds with the blocking-read fix: host 1.871% plus codec 0.423%
of one logical core, approximately 30 video frames/s, no disconnect or stderr.
Host working set 76.15–76.86 MiB, private memory 82.87→83.54 MiB, handles 713→714.
Codec working set 11.32–11.38 MiB, private memory 2.39→2.31 MiB, handles 153→153.
Package SHA-256 was `345A6C9E448C732320B02911543A1BA41C44A2F568A4C68594469365EAB5746F`.
A separate live close took 177 ms with clean shutdown log and no remaining
receiver/worker, TCP listener, UDP endpoint or mDNS service response; the reopened
process handle did not provide an exit code. None of these measurements is a
physical acceptance result for the later frameless UI build.
