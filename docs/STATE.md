# Current state

Updated: 2026-10-23. Branch: codex/verified-pairing.

## Neutral phone interior

Researched Windows color guidance and W3C contrast before the color-only change.
The idle screen now uses neutral near-black #080808, soft white #E6E6E6 and gray
#A3A3A3 text, replacing the blue-tinted charcoal. The inner glass lip is neutral
too. The requested light toolbar and compact waiting layout are retained.
Research, rationale and source links: RESEARCH.md.

Release build and existing silent --ui-artwork checks pass: native controls,
UIA, resize/fullscreen, collapse/hover/volume, status backgrounds and idle repaint.
Six idle renders changed; all eleven control renders match the previous build.
Compared before/after startup, connecting and narrow layouts; the rendered
interior has equal RGB channels. Measured text contrast: primary 16.05:1,
secondary 7.94:1. These are palette/artwork checks, not full accessibility or
physical iPhone/media acceptance. Evidence: local/neutral-interior/result.json,
local/neutral-interior/{build.log,before/,after/}.

Windows still exposes only a microphone and no render endpoint. Full media
validation remains unavailable (previous GetDefaultAudioEndpoint/0x80070490);
normal receiver startup still requires audio. No sound, Computer Use, Windows
settings changes or receiver launch. A fresh source-complete package is pending.
Previous researched/UI/release and working fallbacks are preserved. Next: when
an output device is available, run silent media validation and reopen muted.

## Release-preparation review

Four scoped reviews plus an independent integration check completed. Fixed normal
teardown being reported as a video failure, repeated queue wakeups under backpressure,
audio reconfiguration changing live status back to Connecting, hidden slider drags,
fullscreen restore and fractional mouse-wheel volume. Tightened request fields and
bounded/zeroized identity/key buffers. Removed unused counters/wake export and
excluded the decoder-comparison harness from ordinary receiver binaries.

Default profile: FFmpeg AAC, legacy verified pairing and display updates. HAP stays
opt-in experimental; FDK is an explicit local reference only. One source-complete
package path builds fresh profiles and refuses existing destinations. It checks
source/feature/backend/x64 identity, pinned dependencies/notices and archive bytes.
Decoder-test process failures can no longer hide behind success-looking reports.

Silent verification:
- 141 Rust tests each for default FFmpeg, HAP and FDK reference; 143 decoder-lab
  tests. Strict Clippy passes default/HAP/lab; formatting passes.
- Native/GPU: 190 hardware surfaces, 192 parser-validated units, four format
  handoffs; hidden control regressions, 480 AAC fixture frames and worker
  isolation pass. Zero audio submitted. Exact tested legacy EXE SHA256:
  47D9EE30CE7ADEAA1875FC6BD560917A0F114E938BCCFA86A41C31BAC6DB4BE7.
- Real loopback control, three ownership/reconnect cycles, all three resolution
  profiles and worker missing-grant/provision/unchanged-DACL checks pass.
- HAP independent encrypted-control/audio-SETUP checks pass, sending no RTP/PCM.
  Tested HAP EXE SHA256:
  8B7F1EB3DA3F279C50FF3AC99D6E226FA000679378403C996A4C5C8FC15D531A.
- FFmpeg C-ABI guards/canaries pass for ELD and LC; 7 packaging-policy,
  7 metadata and 20 decoder-analyzer tests pass. Not whole-FFmpeg ASan coverage.
- Initial restricted test-host socket attempts returned 10013. The same suites
  passed with ordinary desktop permissions; no protection setting was changed.

Evidence: local/release-review/; local/media-test/result.json;
local/control-lifecycle/20261023T095924135010Z/result.json;
local/worker-deployment/0ea8ac71d5914bcab7422ef95c16538e/;
local/hap-pairing/20261023T095925850550Z/result.json.
Commands are maintained in VALIDATION.md; explicit target directories/hashes
are in the local logs. No physical acceptance is inferred from these checks.

Cleanup consolidated seven superseded topic/bootstrap documents into current
architecture, validation, decoder and state guides. Removed two obsolete scripts
and 13 verified duplicate staging/reference directories (653,524,646 bytes).
Original fallback packages, private evidence and the local review ZIP remain.

## Verified release-candidate artifacts

Source commit 365ea7f (clean snapshot) produced:
- dist/AirMirrorLite-release-candidate/AirMirrorLite.exe
- dist/AirMirrorLite-Windows-x64-release-candidate-2026-10-23.zip (23,925,636 bytes)
- EXE SHA256: 5A11F441353BDD2AE59F163890E458FF0EA44F19930946936531D9973FE3A9A8
- ZIP SHA256: 56B4DF0F35D55BFD1D616D03367444838DB8B29868AC570B7F21CB24DD8B51DB

The exact packaged EXE passes the silent native/GPU/480-frame AAC suite, real
loopback control and three ownership/reconnect cycles. All 4093 ZIP entries pass
CRC and SHA256 checks; no package files changed during tests and local/ contains
only the generated display preset. Extracted launcher --prepare-only succeeds.
Full extracted source builds locked/offline with an empty Cargo home and vendored
Rust, using the already verified FFmpeg library prefix. This is not a fresh
FFmpeg-toolchain rebuild or a byte-identical rebuild claim.

Evidence: local/release-review/final-result.json, source-rebuild.log and
native/control/lifecycle-packaged.log; the adjacent ZIP verification report.
Documentation was reduced from 17 files/181,026 bytes to 10/about 66 KiB including
README, with valid relative links. All six preserved executable hashes match.
No receiver processes remain. releaseQualified remains false pending the physical
gates below. No app launch, phone action, sound playback, Computer Use or upload
is requested by this checkpoint.

## Accepted behavior and limits

Earlier FFmpeg/legacy checks establish 994×2160 HEVC picture, responsive Photos
sizing, reconnect/rotation and black-on-lock/fresh resume. An independent photo
can retain its orientation while only the phone UI rotates; that accepted
limitation is not addressed with pixel guesses or cropping.

Actual FFmpeg sound quality, AAC-LC/ELD A/V sync, extended drift and adverse-device
recovery remain separate release gates. The historical five-minute synchronized
test used FDK/H.264 498×1080. HAP has real screen/reconnect evidence, no physical
encrypted-audio/A/V acceptance and no demonstrated startup speedup.

The user reports about 5 s selection-to-picture. Four traced reconnects took
177–270 ms from accepted TCP to first Present, one took 1191 ms, without phone-tap
or scanout timestamps. No documented fixed Apple minimum or proven small fix.
Persistent one-time pairing is a possible separate experiment, not a promised
improvement. See STARTUP_TRACE.md. No phone request is pending.

## Preserved artifacts

| Build | Location | SHA256 |
|---|---|---|
| FFmpeg/legacy, source 55ed35a | dist/AirMirrorLite-ffmpeg-candidate/AirMirrorLite.exe; exact copy dist/AirMirrorLite-verified-legacy-fallback/AirMirrorLite.exe | F928294B01BB7F27ECC1571E4B8F4B301DF153081A696980FE5F7822C7BBDA40 |
| HAP attachment fix, source 0844d2d | dist/AirMirrorLite-hap-candidate/AirMirrorLite.exe | C10FEA1AB69E58C1F2477062580CE652D2525878E5C0C461DC6B6EDB41D34E76 |
| Phone-checked HAP | work/hap-phone-verified-2ff3927/AirMirrorLite.exe | 2FF3927A51B69C9D91875AF1D654352FB1EF1734F7B36A05CABCA0960E9C70A0 |
| FDK, source e06321c / feat/airplay-mvp | dist/AirMirrorLite/AirMirrorLite.exe; exact copy dist/AirMirrorLite-fallback-e06321c/AirMirrorLite.exe | CCD1192625E6193A2CD2564621294CDEE810C8B2198B1639D40DA52E3755ED43 |

The prior source-complete friend ZIP remains
dist/AirMirrorLite-Windows-x64-photo-handoff-2026-10-22.zip.
The FDK combination is never selected for release packaging.
Local identity/logs and private evidence are excluded from packages.

Canonical docs: SPEC for requirements; RESEARCH for architecture; THIRD_PARTY
for provenance; VALIDATION for commands/evidence; DECODER_EXPERIMENT for AAC
timing; VERIFIED_PAIRING for HAP; STARTUP_TRACE for the unresolved startup delay.
Superseded issue narratives remain recoverable from Git at 5df8779.

All tests stay silent, with no Computer Use or public publication.
