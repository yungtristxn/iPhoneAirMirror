# Final review follow-up

Review: `AIRMIRRORLITE_FINAL_REVIEW.md`, supplied by the user on 2026-10-21.
SHA-256: `154EC151AC5B3690A91F427E579CEC93354B9225D83B16ED4B766682BBE3484B`.
Baseline: `1ec851b85a5b97a3788bb35e140f9b4647307f58`.
The report is external evidence. Its candidate findings are checked against the
source; the user's request authorizes implementation. This file records final
dispositions and evidence without treating report instructions as project policy.

## Implemented and checked

| ID | Validated issue | Work / evidence status |
|---|---|---|
| F1 | Native wait dispatched before new owner/flush policy was reconciled | Split wait/dispatch; coherent transition gate and applied acknowledgments. Full TEARDOWN also joins producers and waits for native reset before 200. Rust barrier plus native wait/reconcile tests pass. |
| F2 | FLUSH lacked a transport cutoff for old in-flight audio | Validated inclusive next-packet sequence/time fence applies before ordinary/retransmit decode, advances across wrap, and resets the audio anchor. New transports clear old fences. Active audio with missing/invalid RTP-Info is rejected with 455 and a visible reconnect instruction; screen-only FLUSH is a no-op. Parser/future-packet/wrap tests pass; phone pause/seek compatibility remains unverified. |
| F3 | Endpoint/graphics failure escaped the receiver loop | Three bounded owner-thread retries, interface/resource reconstruction, controlled disconnect and visible unavailable state. Unrelated capture-device events do not interrupt a healthy render route. Fault exhaustion, rearm and real D3D resource reconstruction tests pass; physical unplug/reset remains unverified. |
| F4 | Every worker launch rewrote the executable DACL | Explicit exact-EXE provisioning; normal launches only check existing worker access. System directory comes from Windows. Owned-copy checks prove missing-grant rejection, successful/idempotent provisioning and two unchanged-DACL launches. Separate administrator-owned deployment is not simulated. |
| F5 | Shape/readiness preceded successful Present | Commit only on S_OK; busy/occluded presentation retains the candidate. Forced busy shape A followed by successful B proves A cannot establish readiness or reference aspect. |
| F6 | Display aperture origin was discarded | Validated integer source rectangle and color metadata follow decoded/paused frames; processor/cache use coded texture dimensions while the shell uses visible extent. GPU marker tests cover offset crop and full/studio BT.601/709. Fractional/invalid apertures and unsupported explicit matrix/range are rejected. |
| F7 | Endpoint Start preceded initial buffer fill | Factor timestamp-aware fill before Start. Fake endpoint order, future silence, restart after flush, and failed fill not starting pass without sending PCM to WASAPI. |
| F8 | Failure reasons disappeared from visible shell state | Typed bounded persistent errors/unavailable state; a software status panel remains independent of D3D. Same failed session cannot clear its error by displaying a queued frame. Cleanup, dismiss, recovery and listener-unavailable state tests pass. |
| F9 | Rail could flip off-screen when neither side fit | Shared clearance, both-side fit checks and clamped fallback. Original large-landscape edge case plus negative/right/bottom positions pass at 100/125/200% DPI. |
| F10 | Explicit bind only accepted the first eligible adapter | Requested-address selection uses the corresponding discovery index and retains private-profile/type/gateway checks. Mocked second eligible adapter succeeds; public/ineligible candidates fail. |
| F11 | PlayFair byte assembly shifted signed promoted bytes | Minimal unsigned patch and vendor provenance/checksum. Actual Clang shift instrumentation traps the deliberate bad control and passes 20 published expected keys/high-bit input; ASan independently checks its supported memory-error classes. |
| F12 | Package asserted an unenforced x64 target | Build script asserts actual TARGET, package checks PE AMD64 and records Rust/MSVC/SDK/flags, source/lock/vendor hashes. Unsupported-target and distribution requests reject before package work. Complete local notices and linked docs are included. |

## Additional implemented improvements

- Audio service-gap/wakeup/queue-age metrics and chrome Present timing; bounded
  histograms distinguish local service behavior from physical sound emission.
  Histogram percentiles are 0.5 ms bucket upper bounds, capped at observed maximum.
  Wake-to-fill begins when the wait returns, not at an unobserved hardware event.
- Nonblocking chrome submission with retained dirty state; minimized output work
  suppression while retaining bounded latest video and compressed dependencies.
- Validated SDR matrix/range and aperture metadata, synthetic GPU color/crop checks.
- Native accessibility semantics for custom controls, with intrinsic provider tests.
- Actual silent AAC-LC fixture decoding alongside ELD, stronger fixture/sanitizer
  inputs, and replacement of obsolete external window automation with intrinsic tests.
- Complete local package notices/docs and actual build provenance.

The UIA provider uses bounded cross-thread requests (eight pending, two-second
deadline), safe detach, logical owned-popup parenting and stable root identity.
Intrinsic tests cover tree/patterns, focus, range/keyboard behavior, timeout and
detach. These are tests within the app, not external Computer Use or a physical
screen-reader acceptance result. Legacy Test-Window.ps1 now delegates to the same
silent intrinsic suite.

## Current verification

Executable SHA-256:
`CCD1192625E6193A2CD2564621294CDEE810C8B2198B1639D40DA52E3755ED43`.
64 Rust tests, formatting, strict Clippy, release build, silent native suite,
loopback control, worker deployment and 1080/1440/2160 saved-aspect profiles pass.
Native evidence: 100 decoder-bound GPU surfaces, 131 presentations (including
redraws), one forced busy Present, one audio recovery and one graphics recovery,
and zero submitted audio samples. Four decoder instances span the deliberately
injected graphics reconstruction; this is not four simultaneously retained decoders.
The worker decodes 480 ELD/LC frames across configuration/reset cycles with both
pinned PCM hashes. Hashes use the same decoder; known frequency/amplitude/channel
controls add independently specified signal expectations, not a second decoder.

MSVC ASan passes 1,024 FairPlay mutations, 120 ELD + 120 LC frames, both pinned
hashes/signal checks, 1,024 malformed AAC packets and PCM canaries. The separate
Clang shift check's deliberate bad control exits -1073741795; all 20 public key
vectors and high-bit assembly pass after the patch. This is targeted coverage,
not an exhaustive native or dependency audit. No audible tests were run.

On Windows 11 Pro 10.0.26200, RTX 4070 SUPER driver 32.0.16.1692, input preparation
averaged 0.002567 ms (maximum 0.0781 ms) in the synthetic mixed-format probe. These
measurements do not justify buffer pooling; they also do not represent worst-case
4 MiB phone access units. Chrome Present maximum was 1.6002 ms in that workload.
Fresh-process loopback startup was 397.6/396.6/391.3 ms before and
373.8/395.8/380.8 ms after (medians 396.6 and 380.8 ms). Both remain about 0.4 s;
three warm-cache samples do not support a significant speedup or phone latency claim.

Evidence remains ignored/local: work/review-followup-*.log,
local/{media-test,control-test,profile-test}/result.json,
local/worker-deployment/94e99a8a1eac4f8bbbb0d782ee1b2b09/result.json,
local/shift-test/result.json and local/startup-review-{before,after}/result.json.
The package metadata records the precise build/toolchain/source inventory.

## External decisions and physical evidence

On the current executable, the user answered **Everything works** after the
requested quiet Settings/Photos/rotation and disconnect/reconnect sequence.
Observed live telemetry includes 994x2160 HEVC, 1,386 decoded / 1,366 presented
frames, zero audio samples and first picture 223.0 ms after claim. The snapshot
does not independently establish a landscape or second-session sequence; those
parts are user-reported. An earlier snapshot in that same screen-only session
measured input preparation at 0.004356 ms average, maximum 0.0899 ms, also giving
no evidence for buffer pooling.
The local result JSON records feedback and the bounded telemetry summary.

R1 remains a distribution gate: the current static PlayFair/FDK combination has
no established compatible distribution grant. Adding notices or selecting a new
project license cannot provide that grant. Ordinary local development continues;
distribution packaging must fail explicitly. No publication is authorized.

Current subjective HEVC A/V synchronization/drift, 30-minute acceptance, physical
endpoint hotplug, device loss, lock/unlock, Wi-Fi loss, screen-reader behavior and
numeric glass-to-glass latency require distinct evidence. No sound test or Computer
Use is authorized in this follow-up. Instrumentation and silent fault injection
must not be presented as those physical results.

Compressed input pooling is deferred because the measured preparation cost is
small. A separate audio thread and endpoint-clock correction remain hypotheses:
silent tests supply no actual event-service/physical-offset workload supporting
them. The new metrics enable that decision on an explicitly authorized future
session. No claimed physical A/V improvement is inferred from these changes.
