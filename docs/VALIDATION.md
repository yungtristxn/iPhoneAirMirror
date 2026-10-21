# Validation boundaries

## Commands

The current review-remediation results and per-finding limits are in
[REVIEW_FOLLOWUP.md](REVIEW_FOLLOWUP.md). Sections below retain historical build
evidence; a successful earlier phone session is not current-build acceptance.

From the repository root, PowerShell (MSVC C++ tools and Rust installed):

```powershell
$env:CARGO_HOME = Join-Path $PWD 'work/cargo'
cargo fmt --check
cargo clippy --all-targets --locked --offline -- -D warnings
cargo test --locked --offline
cargo build --release --locked --offline
./scripts/Test-Media.ps1
./scripts/Test-Control.ps1 -Executable "$PWD/target/release/airmirror-lite.exe"
./scripts/Test-WorkerDeployment.ps1
python -O scripts/Test-VideoProfiles.py --display-aspect 994 2160
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

`scripts/Test-PlayfairUB.ps1` is the separate Clang shift-undefined-behavior check;
see `tests/native-sanitizer/README.md` for its portable development compiler,
mandatory failing control and published known-answer vectors. ASan alone cannot
establish absence of signed-shift UB. The current ASan harness covers both AAC
fixtures, pinned hashes and independent signal-content expectations; hashes do
not represent an independent decoder implementation.

## What each result establishes

| Check | Evidence | Does not establish |
|---|---|---|
| Rust tests | bounded framing/plist expansion, pairing proof/state/replay, FairPlay mode bounds, H.264 SPS/NAL budgets, RTP framing, shared-clock arithmetic, audio-mode/teardown parsing | iPhone compatibility |
| Native ASan | known-answer and deterministic malformed-input execution with memory instrumentation and output canaries | complete native-code safety or exhaustive fuzz coverage |
| Silent native probe (default) | synthetic H.264/HEVC decoded to `D3D11_BIND_DECODER` textures; GPU presentation, paused redraw, controls and isolated AAC/IPC in memory, with no PCM submitted to WASAPI | phone sound, physical A/V synchronization or glass-to-glass latency |
| Explicit audible probe (not run in this pass) | additionally sends generated AAC-ELD to WASAPI | phone sound or physical A/V synchronization |
| Worker probe | AppContainer token has zero capabilities; host canary read denied; worker UDP probe not delivered to a host listener | formal sandbox certification |
| Loopback control | actual requests, single owner, invalid framing/sequencing rejection, listener survival and timed shutdown | physical disconnect/lock/Wi-Fi behavior |
| Phone test | user's observations plus negotiated codec, decoder-surface and audio counters in the bounded local log | durations/scenarios not actually exercised |

## Resource bounds

RTSP headers 8 KiB, bodies 64 KiB, four control connections, connection/record
deadlines and per-connection request rates. Binary plists: 1,024 objects, depth 16,
4,096 expanded visits, 512 KiB expanded-data budget. Video: 2 MiB H.264 / 4 MiB
HEVC compressed record, eight queued compressed records plus one transition item,
a 500 ms backpressure deadline, at most eight H.264 / four HEVC decoded samples
and admission below six / three to preserve decoder-pool headroom.
SPS rejects unsupported profiles/bit depths/interlacing, oversized pictures and
more than eight references before native decode. Screen targets are H.264 SDR,
1920x1080-class (rotation allowed), or HEVC Main 8-bit SDR through 3840x2160,
up to 60 fps. The native HEVC/GPU capability gate determines advertisement.

Audio: stereo 44.1 kHz AAC-ELD (480 samples/frame) or AAC-LC (1,024), 8 KiB packet,
128-packet jitter window, 32 PCM blocks plus one transition item per host queue;
native worker has a 256 MiB process-memory limit and a two-second transaction
deadline covering request writes and response reads. Its job permits
one process and terminates on host close. Logs rotate at 1 MiB without media,
keys or raw negotiation bodies.

## Physical acceptance

Keep the source/build hash with each run. Note phone/iOS when known, GPU,
resolution, frame-counter deltas, workload and duration. `Measure-Receiver.ps1`
records host/codec CPU (percent of one core), working set, private bytes, handles
and threads. JSONL traces and the bounded receiver log stay under ignored local
directories. Working-set sums may include shared pages; private bytes are separate.

The shared NTP clock and default 80 ms advertised latency are implementation settings, not
measured end-to-end latency. Measure phone capture-to-PC display separately with
a synchronized visual counter/high-speed observation if a numeric latency claim
is needed. The user shortened the sustained test to five minutes; no 30-minute
or long-term drift claim should be made from it.

Current evidence and unresolved checks are in STATE.md. The final package is a
local development build; third-party distribution licensing is not resolved.

## Native UI regression (2026-10-21)

`--ui-test` exercises the hidden native shell: no caption/client inset,
portrait/landscape aspect locking, bottom-right hit testing, default-size reset,
rotation during fullscreen, hover slider, mute/restore, volume limits, owned
palette lifetime flags, stable speaker position when the hover slider opens at
the top screen edge in a compact landscape window, and close/disconnect events. The existing GPU probe then
checks landscape/portrait decode, paused redraw, resizing and fullscreen against
the DirectComposition path. The idle renderer checks that unmodified ticks do
not repaint the chrome.

`--ui-test --ui-test-dir local/qol-test/renders` saves the native idle/connecting
artwork and control palette as PNG diagnostics. This is internal rendering of
app artwork, not desktop automation, screen capture or phone recording. The
export is refused when a video surface is active. Inspect the diagnostics for
clipping, typography and alpha corners, including a compact landscape window.
They do not establish final desktop composition, mouse feel or physical phone
acceptance. The user requested no Computer Use for this validation.

Production video remains on D3D11 surfaces. Direct2D/DirectWrite paint idle chrome
and the small control palette only when invalidated. GDI+ serializes diagnostic
artwork PNGs only. These are Windows components. There is no web runtime,
UI framework package or new third-party source.
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

## Clarity, deliberate hover and collapsible controls

The final refinement uses `--ui-test` (also the default in Test-Media.ps1). This
runs native UI plus synthetic GPU video checks without calling the AAC/WASAPI
playback probe; `audio_samples=0` must remain in the output. The user stopped
audible testing: do not run `--self-test` or `Test-Media.ps1 -Audio` without a new
explicit request. Full earlier audio evidence remains historical evidence.

Focused native tests cover no immediate popup on pass-through, cancellation of
stale hover, slider drag/endpoints, labels not changing volume, Escape dismissal,
source-pixel default fit without enlargement, hide/show with mouse and keyboard,
collapse while volume is open, and stationary speaker / stable dock side at
monitor edges. Render diagnostics include 100%, 125% and 200% control artwork,
plus collapsed idle and hover states. These are offscreen artwork and in-process
handler tests, not physical mixed-DPI monitor or pointer-feel acceptance.

A development renderer initially hung on thread-local destruction after tests
completed. It now has an explicit UI-thread lifetime and is released before
thread teardown. Successful native-test exit and loopback timed shutdown verify
the fix. No sound test was rerun after the user's stop instruction.

## Actual higher-resolution video and embedded icon

Silent native probes cover H.264 landscape/portrait and HEVC 3840x2160 plus
992x2160. HEVC output requires decoder-bound DXGI surfaces, and the 4K probe
requires at least eight decoded hardware surfaces with exact 3840x2160 output.
Embedded icon tests load the PE resource at 16, 32, 48 and 256 pixels and verify
bitmap dimensions, plus both window icon handles. The ICO contains nine sizes.

The HEVC parser tests cover malformed/truncated box lengths and parameter arrays,
NAL lengths/layer IDs, unsupported depth/profile, coded dimensions, DPB budget,
bounded zero alignment padding and 8,192 deterministic mutations. H.264 now also
rejects over-budget coded area hidden by cropping. Real /info checks in
`python scripts/Test-VideoProfiles.py` verify 1080/60, 1440/60, 2160/60, and the
conditional high feature bit. All these checks are silent.

HEVC bounds: 4 MiB compressed access unit (including initial parameter sets),
64 KiB configuration, eight child boxes/arrays, eight parameter NALs per array,
16 KiB per parameter NAL, 4,096 NALs per access unit. Main profile, 8-bit 4:2:0,
single layer, level <= 5.1, at most eight decoded pictures in SPS, coded luma
<= 3840x2160 before cropping. Native output allows bounded padding through
3840x2176; queued decoded HEVC frames are limited to four, admission below three.
The compressed queue remains eight entries (up to 32 MiB for HEVC). GPU surfaces
use more memory at 2160; older 1080 memory/CPU measurements do not describe 4K.

Live silent phone acceptance on 2026-10-21: the first configuration was safely
rejected for a short container-alignment tail. After the zero-only padding fix,
the user confirmed picture appears. The receiver reported H.265, 994x2160,
131 then 263 then 351 decoded/GPU decoder surfaces, with zero audio samples and
no presentation error. Previous portrait was 498x1080: nearly four times as many
source pixels. This confirms actual higher-resolution phone delivery and GPU
presentation; it does not claim a new sound/A-V-sync or sustained-motion test.

The accepted live HEVC binary was
`89868E0383ED49968F99A7F4443F8004B92EBCBF67384ADC4D1DF550D1FBB955`.
Its session reached 1,876 decoder surfaces and 1,871 presentations, then the user
closed it cleanly; no receiver/worker or control listener remained. A passive
9.24-second quiet-screen sample had 126.63→126.73 MiB host private memory,
1001→1001 handles and 0.00–0.51% sampled CPU of one core. This excludes GPU memory
and is not a sustained-motion benchmark. The final package additionally guards
partially initialized decoders; a silent failure/recovery probe verifies that
state before the 4K decode test. No additional phone interaction was requested.

Final parser guards also reject embedded Annex-B delimiters inside length-delimited
NALs and invalid SPS emulation-prevention sequences, so a nested unvalidated
parameter set cannot bypass the dimension checks before native decoding. These
negative tests pass for both H.264 and HEVC. They were added after the live run.

## Connection startup and responsiveness (2026-10-21)

A fresh-process listener benchmark with existing OS caches measured old
1939.1 / 1904.4 / 1908.9 ms and new 357.3 / 392.8 / 397.5 ms. Median readiness
fell from 1908.9 to 392.8 ms. This is process launch to a successful local OPTIONS
response, not iPhone tap-to-picture or discovery visibility. The new independent
registration phase completed in 783.2 / 785.0 / 813.3 ms after the listener was
ready; all three registered and shut down successfully. No media was sent.

Run `python scripts/Measure-Startup.py --exe <absolute-exe> --label <name>
--discovery --address <private-PC-IPv4>` only when the normal receiver is closed.
It temporarily advertises the receiver during three hidden, silent runs.
A separate immediate-close run while registration was pending exited successfully
in about 398 ms; the final native regression also checks that readiness polling
cannot consume the manual-reset callback completion signal.

New regressions cover delayed initial clock synchronization, reordered and
replayed timing replies, clock-wait cancellation/timeout, idle video without
false partial-record timeout, strict partial/header-plus-payload deadlines,
truncated EOF, first-keyframe retention across a stale owner snapshot, and
common audio/video timing at 60/80/120/250 ms. Old-epoch/old-flush samples are
not retained indefinitely. Stream queues stay bounded; one pending item per
media channel handles the owner transition.

Native tests reuse only two decoder instances across H.264 and HEVC format
changes. Disconnect flushes and clears media types/surfaces; only the transform
objects remain cached. Configuration prepares the selected decoder ahead of the
first picture. If a transform refuses reset, that cached instance is discarded.
The 80 ms default removes 40 ms from the former configured playout target, and
all resolutions now request up to 60 fps. These are not numeric glass-to-glass
latency results. All tests remain silent; physical A/V sync is not re-tested.

## Performance and code-quality pass (2026-10-21)

Parallel source reviews covered native media/IPC ownership, Rust transport and
session handling, UI rendering/interactions, and build/measurement tooling.
The following actionable findings were fixed; this is a scoped engineering
review, not a claim that all possible defects have been eliminated.

| Finding | Change and focused evidence |
|---|---|
| Audio could remain stuck after more than 128 lost sequence numbers | Two consecutive forward packets resynchronize the bounded queue; tests cover loss, wrap, reordered/duplicate packets, outliers and retransmissions |
| Unmatched timing replies bypassed the liveness deadline | All replies reach the deadline check; a real loopback UDP flood cannot extend the session |
| Audio IPC allocated scratch vectors per packet | Reuse fixed host reply and worker input/output buffers; removes three explicit Rust allocations per packet while PCM keeps owned queue storage |
| Worker request writes were outside the reply deadline | Both overlapped directions share two seconds; silent blocked-pipe tests cover write/read/expired deadlines |
| Presentation could queue frames or block audio work | Waitable DXGI video chain limits queued presentation to one; nonblocking Present retains queued data and its reserved slot on backpressure |
| Shared event-driven audio requested a fixed 30 ms buffer | Follow the zero-duration API contract and log actual endpoint capacity/latency; physical A/V sync remains pending |
| Huge finite native timestamps could overflow integer conversion | Validate the signed MF clock range and clamp audio skip before conversion; rejection tests submit no PCM |
| Moving the window recreated/repainted control artwork | Reuse a bounded DC/bitmap/brush and cache artwork; 100 intrinsic moves add zero paints/allocations and no GDI objects |
| Volume flyout could stay open, stale timers could close it early, and capture loss left pressed artwork | Zone-based leave grace, monotonic timer deadline, capture cleanup and keyboard repeat tests |
| Partial text-resource initialization was mistaken for success | Retry each missing resource independently and invalidate cached artwork after failures |
| SETUP handling and unchanged status polling obscured state/allocated repeatedly | Extract explicit setup stages, preserve wire/state guards, and clone status only on change |
| Old binaries/tests could receive misleading provenance | Mandatory offline package freshness build; release defaults; artifact hashes; stale success removal; Python checks survive optimized mode |
| Resource sampling missed its endpoint or reused PID history | Final endpoint sample plus process-start identity and per-process timing; a 2-second passive probe ends at 2.056 seconds |
| ASan could reuse stale external native input or accept an empty fixture | Watch native/vendor inputs; empty fixture exits 5 instead of reporting success |

Final silent native suite: 100 decoder-bound GPU surfaces, two cached decoder
instances, 103 presentations including confirmed paused redraws, and zero audio
samples submitted. Every resize/fullscreen transition now requires an actual
presentation within one second. The isolated worker decoded 360 AAC-ELD frames
across three resets with identical PCM hashes, plus the FairPlay known answer.
File-read denial and absent UDP delivery were checked. A successful UDP send
return is logged separately because it does not establish delivery; no worker
capabilities or firewall exemptions were changed.

The ASan run passed 1,024 fixed-size valid-mode FairPlay mutations, 120 AAC-ELD
fixture frames and 1,024 bounded malformed AAC packets, with PCM canaries intact.
The separate empty-fixture negative test exited 5 as required. No audio endpoint
was fed by these checks.

New runtime metrics describe queue high-water marks and CPU time spent inside
MF submit/output and Present calls. The silent mixed-format GPU probe measured
about 0.50 ms mean submit and 0.59 ms mean Present on this RTX 4070 SUPER; maxima
were 17.54 and 8.39 ms. The endpoint reported capacity 1,036 frames (about 23.49 ms
at 44.1 kHz) and stream latency zero. These are local synthetic/initialization
observations, not sustained phone throughput or end-to-end latency measurements.

On the exact final executable (`411F3F2F3318712880725825CD1F1668C731915A53AA68873F4C5C99DCE613F6`),
44 Rust tests, strict clippy, silent media/control checks and all three /info
profiles passed. The loopback startup median was 383.9 ms (395.2/383.9/380.4),
versus 390.9 ms (395.2/385.3/390.9) before this pass. Both are roughly 0.4 seconds;
three samples do not support a significant additional startup-speed claim.
The reopened packaged receiver completed real discovery in 786.3 ms after
listener readiness and remained idle without errors. A 20.06-second passive
sample, excluding its first five seconds of startup, recorded 0–0.313% CPU of
one core and 77.83–77.89 MiB private memory; the final three samples held
1,037 handles. This is idle host evidence, not active mirroring or GPU memory.

Raw local evidence: `work/optimization-{tests,clippy,build,media,control,profiles,asan}.log`,
`local/{media-test,control-test,profile-test}/result.json`,
`local/startup-optimization-{before,after}/result.json`, and
`local/measure-20261021-165044.jsonl`. Packaging freshness was exercised before
launch; the package and all three test summaries have the same executable hash.

## Presentation deadlines and photo-window follow-up

On `ab77a95`, the user confirmed a faster connection but a noticeable delay
below half a second. The quiet HEVC session reached 837 decoded / 834 presented
frames with three late drops; first presentation was 329.4 ms after session claim.
This is not phone-tap-to-display timing. The later timer/60 ms diagnostic trial
(`55942EAF358B8485E3CC4805DE35379CEABFDA54330CD0472B1E187160C0E9B1`)
was smooth but felt similarly delayed. It reached 4,722 decoded/presented frames,
zero late drops and zero audio samples, and closed cleanly.

The trial exposed the reason to correct clock semantics: received frame PTS were
typically 75–85 ms in the future, before the receiver added its extra 60 ms.
MF residence averaged about 0.33 ms and presentation submission lateness about
0.37 ms, so fast decode calls were not proof that the receiver avoided waiting.
The correction follows the sender's presentation deadline without adding the
advertised latency again; video and audio lose the same common offset. Current
sound synchronization still requires separate physical acceptance.

The high-resolution one-shot wait uses frame deadlines, audio/UI/packet wakeups
and the DXGI slot event. Coalesced queue signals cannot strand a drain-budget
backlog. Tests cover future frames not shown early, due-frame delivery, slot
ownership, idle sleep despite a ready display, and audio/close interruption.
Per-session diagnostics distinguish arrival age, parsing/queue time, MF residence
and submission lateness; they do not capture media or measure display scanout.

The user also reported a persistent landscape PC window while the phone stayed
portrait during photo viewing. A confirmed defect published geometry at decode
time, ahead of the displayed frame. Geometry now follows the frame selected for
drawing. Video-processor cache keys include source dimensions. Silent synthetic
GPU and hidden UI tests cover future opposite-aspect frames, rapid format changes,
automatic/user-sized windows, fullscreen return and normal DPI suggestions.
Validated header dimensions and decoded/presented/window shapes are logged to
distinguish a remaining sender-format change from a window geometry problem.
These fields are diagnostic only; no physical-phone orientation is inferred
from an image aspect ratio or an undocumented header.

## Stable phone frame during Photos output

The user confirmed lower delay on `7ac11f7`, but the photo case still switched
the PC to landscape. The new diagnostics resolved the cause: the sender changed
both configuration dimension pairs from 994x2160 to 3840x2160, and the decoded
and presented dimensions agreed. The client window changed from 622x1344 to
2385x1344. This was an actual wider feed from the phone, not a receiver pixel
rotation or a stale window-size message.

The shell now learns an orientation-independent aspect from the first presented
frame of a session. Matching ratios within 1% may change resolution or orientation;
different-aspect media retains the established frame dimensions. The existing GPU
video processor fits the complete image with letterboxing. Default size continues
to use the established frame; session-tagged messages prevent delayed prior formats
or resets from corrupting the next session's choice. The reference does not drift
across successive format changes.

The receiver cannot identify physical phone orientation from a media feed alone.
Start from Home or Settings to establish the screen proportions. A session begun
directly in optimized media may establish that media's proportions instead; no
undocumented metadata or device-model guess is used to override it.

Focused hidden tests cover photo preservation in automatic/manual-sized windows,
same-aspect rotation, resolution tolerance, reset size, and old/reordered session
messages. The synthetic GPU test presents 16:9 content inside a retained portrait
viewport, verifies its letterbox destination and unchanged window, then checks
genuine reciprocal rotation. No screen capture or audio playback is involved.

## Accepted Photos sizing and interactive presentation (2026-10-21)

Keeping a portrait frame alone did not meet acceptance: the user reported a
small photo, consistent with fitting the entire 3840x2160 feed inside it. The
next experiment requested a phone-shaped 994x2160 external display. The user
confirmed the photo then matched the phone, and the source stayed 994x2160.
The saved local aspect now preserves this request across normal launches and
scales to the selected resolution; it is advisory and tested on this phone.

The user then reported increased delay while scrolling Photos. Logs showed
sender timestamps up to approximately 320 ms ahead of arrival, also present in
the earlier wide-feed session, while MF residence averaged about 0.27 ms.
Screen-only presentation now takes the freshest decoded frame without waiting
for that future deadline. Compressed decoding, timestamps and queue limits are
unchanged. Negotiated audio ownership restores shared-clock scheduling before
PCM delivery; explicit audio teardown flushes old PCM before interactive mode.
Mute and silent audio do not disable synchronization. Native guards additionally
restore synchronization on PCM admission and flush residual audio on disabling it.

On executable SHA-256
`A89C05D194657E7C73BB4E870C71F811DDA943B1E2B1B7074644E9172477C307`,
56 Rust tests, fmt, strict clippy, release build, loopback control and all three
saved-aspect profiles passed. The silent native suite produced 100 decoder-bound
GPU surfaces, two decoder instances and 125 presentations, with audio_samples=0.
The synthetic pixel check copies only a generated marker image before Present;
it verifies all corner markers, quadrant ordering and opaque letterbox margins.
Timing tests cover prompt screen-only display and restoration of future-frame
deadlines. The isolated worker decoded 360 AAC-ELD frames with repeat hashes and
sandbox checks, without submitting PCM to WASAPI. No Computer Use was used.

The final packaged build loaded `local/display-aspect.txt` with no command-line
width override. The user reported **responsive and correctly sized** after the
requested Photos scrolling, Settings return and brief rotation check. The log
shows HEVC 994x2160 throughout, 1,156 decoder surfaces, 1,131 presentations, zero
late drops and 26 interactive superseded-frame skips. It does not independently
show a landscape stream during that requested rotation check. First presentation
was 177.9 ms after session claim. Before audio activation, receive-to-submit
averaged 0.052 ms and MF residence 0.222 ms; these are local pipeline observations,
not glass-to-glass latency or display scanout measurements.

During this user interaction, the phone independently negotiated AAC-ELD. The log
shows video_sync=0 -> 1, 147,068 audio samples, then explicit audio teardown and
video_sync=0 while keeping screen mirroring. This is real transport/mode-transition
evidence, not a user-confirmed sound-quality or A/V-sync result. No sound playback
was requested, and all automated tests were silent. The app subsequently closed
with all session threads joined. Older five-minute A/V acceptance remains separate.

Evidence: `work/photo-final-{tests,clippy,build,media,profiles,control}.log`,
`local/{media-test,control-test,profile-test}/result.json` and the bounded ignored
package log. All test summaries match the final executable hash. No sender media,
keys or raw private logs are committed. Current physical A/V synchronization,
numeric end-to-end latency, extended drift and adverse-network recovery remain
outside this completed Photos check.
