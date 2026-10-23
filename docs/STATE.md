# Current state

Updated: 2026-10-23. Branch: codex/verified-pairing.

## Public beta publication

After the muted beta launch, the user reported "works great" and explicitly
requested repository creation, source push and publication. This is a positive
user report, not a measured audio/sync or sustained acceptance result.
Target: yungtristxn/iPhoneAirMirror; both exact-name searches and the account's
repository API found no existing match. The README is now user-facing and short,
with generated native waiting-window/control images and installation/use steps.
Build instructions remain in VALIDATION.md. No phone content is published.

The tested installer/portable hashes below are retained. The release tag targets
their build source 45b397a; main also includes the later documentation updates.
Publication verification is pending. No runnable code or release bytes changed.

## GitHub beta preparation

Display name: iPhone Air Mirror; package version 0.1.0-beta.1. The user selected
icon 1 and requested a descriptive iPhone/Air/Mirror name. The selected paired
screen mark now has reproducible SVG/PNG and nine native ICO sizes. Human-facing
UI, discovery, helpers and package names match. Existing internal Cargo/window/
AppContainer identifiers stay stable; installed data has its own named folder.

Release build, 141 Rust tests, strict Clippy, formatting, seven package-policy
and seven metadata tests pass. Silent native artwork/control checks pass;
startup/narrow layout with the longer name and icon-size renders were inspected.
Source review findings (README encoding and legacy discovery-trace default) fixed.
Evidence: local/release-branding/. No sound playback or Computer Use.

A per-user installer wraps the same source-complete portable payload, preserving
user data. Eight installer policy tests pass. Missing-DLL preflight fails before
app/data writes; a post-copy preparation failure returns exit 20 with repair
guidance and a working uninstaller. The bounded hidden helper is reviewed against
pinned Inno 6.7.1/i386. Path override, junction, denied-listing and ownership
fixtures pass. Final workspace install/upgrade/uninstall validates 4105 files,
the worker ACL and retention of changed settings/data and an unrelated file.
The production installer was compiled/structurally verified; only the separate
workspace variant was installed (no production shortcuts or Apps registration).

The audio outputs subsequently returned. The exact final receiver now passes
silent native/GPU/codec checks: 190 hardware surfaces, 192 validated access units,
four retained-frame handoffs and 480 AAC-ELD/LC fixture frames; zero audio samples
submitted. Loopback control, three ownership cycles, listener shutdown and
owned-copy worker deployment also pass. No physical phone/audio acceptance is
inferred. Ten earlier fallback executable hashes remain unchanged.

Built from clean source 45b397a. Final local beta assets:
- dist/iPhoneAirMirror-0.1.0-beta.1-windows-x64-portable.zip (4100 verified entries)
  SHA256: 8D167D597DF96E4E0F09C439C82BEE539E6C9909971D734CDCAD8D7A730155D3.
- dist/iPhoneAirMirror-0.1.0-beta.1-windows-x64-setup.exe
  SHA256: 5936C82E315B3773980E0D3F4CEF672A8788ECC5610F92588307E6F7AF62F19E.
- Both use receiver SHA256:
  2708C3D9508A71CFC3AE118FBB1E3C8DC8F25E140D7AD8AE3FB4B9AE43BB0AD2.
Adjacent versioned SHA256SUMS/release-notes files are ready. Actual executable
and installer signing status: NotSigned. Earlier unpublished drafts are preserved
under work/release-draft-*; working fallback folders were not moved.

Final receipt: local/release-branding/final-result.json. Installer evidence:
local/installer-final-lifecycle-evidence-20261023/result.json and the missing-
runtime/post-copy-failure/negative fixture folders. Native/control evidence:
local/release-branding/final-{native-result.json,media.log,control.log,worker.log}.
No GitHub remote exists and nothing was uploaded. Next: select the target repo
and review the local beta assets/notes before any GitHub draft or publication.

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

At this earlier color checkpoint Windows exposed only a microphone and no render
endpoint, blocking full media validation (GetDefaultAudioEndpoint/0x80070490).
The later beta check above supersedes that environmental limitation. No sound,
Computer Use, Windows settings changes or interactive receiver launch occurred.

Packaged clean source e681e95. Exact packaged EXE passes the silent UI checks;
all 17 PNGs match the inspected build. All 4096 ZIP entries verified.
Candidate: dist/AirMirrorLite-neutral-interior-candidate/AirMirrorLite.exe
SHA256: A83487B668FE2CCC29534CF71E70FB6ACB5A148C8EB21F9ABF028C8FF36C81FE.
ZIP: dist/AirMirrorLite-Windows-x64-neutral-interior-2026-10-23.zip
SHA256: A26E24B80439E2FE9D81F9CAAB9581EB3C3E6CC6F22E3B60704ED2A5672FFCE0.
Nine prior researched/UI/release/working fallback EXEs retain their hashes.
Next: when an output device is available, run silent media validation and reopen
muted. No new physical iPhone/audio acceptance is claimed.

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
