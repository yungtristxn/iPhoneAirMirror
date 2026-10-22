# Current state

Updated: 2026-10-22

## Verified HAP screen works; startup improvement remains unproven
- User authorized a separate modern verified-pairing candidate. Active branch is
  codex/verified-pairing from cc8079c; implementation6438855. Original F92829... executable remains in
  dist/AirMirrorLite-ffmpeg-candidate/ and an additional exact copy is preserved in
  dist/AirMirrorLite-verified-legacy-fallback/. No working binary is overwritten.
- hap-pairing-experiment adds verified transient SRP M1-M4, authenticated control
  and event records, and authenticated screen/audio media ciphers. Service name
  AirMirrorLite HAP; feature48 on and legacy27 off. No unpaired failure fallback.
- 139 Rust tests in HAP/legacy modes, strict Clippy/formatting and release build
  pass. Invalid HAP audio datagrams are discarded before admission; key-bearing
  decrypted request/plist buffers are wiped. Independent network checks and silent
  GPU/native validation pass, with zero audio submitted. Device-tested HAP EXE SHA-256:
  `2FF3927A51B69C9D91875AF1D654352FB1EF1734F7B36A05CABCA0960E9C70A0`.
  Evidence: local/hap-pairing/20261022T202500438422Z/result.json and
  local/hap-verification/media-result.json. VERIFIED_PAIRING.md records pins, boundaries,
  commands and the public-setup-code versus identity-allowlist distinction.
- Additional independent encrypted AAC-ELD/LC SETUP, worker readiness, partial
  teardown/reconfiguration and wrong/missing-key rejection pass with no RTP/PCM
  sent and zero output counters: local/hap-pairing/20261022T203352735374Z/result.json.
- Runnable build: dist/AirMirrorLite-hap-candidate/AirMirrorLite.exe. Phone checks
  ran muted; DNS-SD target/listener/mute were verified and fallback hashes intact.
  User confirms HAP picture appears but selection delay feels similar. Log proves
  verified M1/M4, encrypted control/events and HEVC994x2160 presentation; accepted
  TCP to picture261 ms, pairing exchange41 ms. Live audio/sync remains untested.
  Prior legacy-pairing diagnosis is unconfirmed.
- Five reconnects with bounded DNS/TCP header metadata: SYN replies0.111-0.193 ms;
  accepted TCP -> first submitted frame177/178/211/270 ms, plus one1191 ms attempt
  that waited1009 ms for the separate video connection. User still reports~5s.
  Trace does not establish missing-AAAA/DNS causality or measure the phone tap/
  physical scanout. STARTUP_TRACE.md records scope, evidence and parser limitations.
- Small renderer correction attaches/commits the first successful video frame in
  the same dispatch before counting it. Native GPU tests pass190 surfaces with no
  audio. New build SHA-256:
  `C10FEA1AB69E58C1F2477062580CE652D2525878E5C0C461DC6B6EDB41D34E76`.
  This is not claimed to fix the full delay. The prior2FF3927... phone-checked build
  is preserved in work/hap-phone-verified-2ff3927/. Source0844d2d was deployed while
  the app was closed, and it remains closed. No additional phone check is requested.

## Remaining startup delay investigated
- User reports approximately five seconds after selection. Latest actual iPhone
  log shows 208 ms from accepted TCP to picture, decoder preparation 1.2 ms;
  discovery finished well before selection. Nine fresh silent loopback sessions
  confirm first-session negotiation median 6.700 ms. Three process launches reach
  the listener in 358.6-374.4 ms with existing OS caches.
- UxPlay ties a matching pre-connection delay to legacy pairing bit27. Its shortcut
  omits verified ECDH pairing and changes the media-key path. No smaller verified
  workaround was found; keep the accepted runtime/advertisements unchanged.
  PERFORMANCE_FOLLOWUP.md has sources, commands, evidence and the distinction
  between a likely sender-side cause and unmeasured phone tap timing.
- No new phone action, sound test, Computer Use or Windows setting change. A newer
  verified pairing implementation would be a separate protocol effort.

## Current candidate: smoother Photos handoff and screen-off blackout
- Branch codex/ffmpeg-aac-experiment; implementation 55ed35a. Executable:
  `dist/AirMirrorLite-ffmpeg-candidate/AirMirrorLite.exe`, features
  `ffmpeg-aac,display-update-experiment`, SHA-256:
  `F928294B01BB7F27ECC1571E4B8F4B301DF153081A696980FE5F7822C7BBDA40`.
- Photos remains photo-only, as requested. Ordinary decoder format changes keep
  the last successfully displayed private GPU frame and its aperture/color/angle
  until a fresh replacement presents. Compatible processors remain reusable;
  private textures stay bounded to two. Suspension/reset/failure clear the cache.
- Display changes settle for 100 ms instead of 500 ms, with the separate 500 ms
  send interval and one outstanding reply preserved. Scalar send timing now
  distinguishes receiver waiting from sender/ACK delay. Normal shutdown/stale
  owner cancels screen publication quietly; real invariant errors still fail.
- The user confirms "Smoother now" and black-on-lock/picture restoration on this
  exact build. It was launched muted. The log confirms resumed generation-2
  presentation, muted=1 and zero submitted audio samples. This comparison segment
  had no coded-format transition or display command; do not present its subjective
  improvement as measured latency or live exercise of the new handoff path.
- Automated: 114 Rust tests on FFmpeg/default backends; strict Clippy, formatting
  and release build pass. Silent GPU tests validate four real HEVC format changes,
  delayed/busy replacement, non-default crop/color/rotation, and lock/unlock fences:
  190 GPU surfaces, 192 parser-validated access units, zero audio samples. Three
  lifecycle cycles and clean process/listener shutdown pass. Independent read-only
  review found no concrete regression. No Computer Use or audible tests.
- Evidence: `local/photo-handoff-verification/` and
  `local/control-lifecycle/20261022T194408294268Z/result.json`.
  PHOTOS_TRANSITION.md separates source behavior, inference, tests and phone evidence.
- Source-included friend ZIP:
  `dist/AirMirrorLite-Windows-x64-photo-handoff-2026-10-22.zip`.
  Package-Share.py verifies the tested executable against the current offline
  build, includes matching application/dependency source and checks every ZIP
  entry. The adjacent verification JSON records the final archive hash.

## Screen-off behavior and accepted boundaries
- a5e4cb4 introduced explicit sender suspension (header byte6 0x56/0x5e) and
  resume (0x16/0x1e), matching pinned UxPlay and phone logs. A priority generation
  boundary masks the viewport with opaque black, preserves controls/shape and
  clears both paused caches. Resume requires a fresh matching frame. No still-photo
  timeout or pixel heuristic; screen blanking does not alter independent audio.
- User accepted black-on-lock/picture restoration first on 7B65C9... and again on
  current F92829.... SCREEN_OFF.md and `local/screen-off-verification/` retain the
  first build's evidence. Independent Photos content without a suspension signal
  cannot reveal physical phone power state; no sensor-to-PC timing claim.
- Photos' independent view can omit the phone UI and may keep its orientation
  while the phone interface rotates. The user accepted this remaining limitation
  and declined a manual canvas control. Do not claim it is solved by the handoff
  work. ROTATION_REGRESSION.md has protocol findings and physical history.
- RTSP event replies with omitted CSeq work on this serialized connection. Explicit
  wrong sequences still fail; optional command failures retain the event socket.
- Bounded audio burst handling and --mute are retained. AUDIO_BACKPRESSURE.md has
  the prior quiet live check. Physical FFmpeg A/V sync, long-run drift and dynamics
  remain unverified; DECODER_EXPERIMENT.md records decoder-delay qualification.
- User previously confirmed quicker first picture. PERFORMANCE_FOLLOWUP.md records
  synthetic setup improvement (45.497 -> 6.909 ms), native socket waiting and the
  bounded preflight worker; it is not phone tap-to-picture timing.

## Preserved builds and sharing
- Accepted screen-off build 7B65C9...: `work/pre-photo-handoff-candidate/`.
  Earlier 8972816... audio build: `work/pre-screen-off-candidate/`.
- Original FDK source remains feat/airplay-mvp at e06321c. Both
  `dist/AirMirrorLite/AirMirrorLite.exe` and
  `dist/AirMirrorLite-fallback-e06321c/AirMirrorLite.exe` retain SHA-256
  `CCD1192625E6193A2CD2564621294CDEE810C8B2198B1639D40DA52E3755ED43`.
- Candidate has its own identity/logs and 994 2160 display preset. Never package
  identity/logs. Sharing uses FFmpeg plus complete matching source, GPLv3 and
  imported notices; no public upload. FDK redistribution remains blocked.
- Default Package.ps1 writes the FDK build; -FfmpegCandidate omits rotation/display
  flags. Use the explicit feature combination below and Package-Share.py for this
  tested candidate. See THIRD_PARTY.md and NOTICE.md for component licensing.

## Commands and next step
Use CARGO_HOME=work/cargo:
```
cargo test --locked --offline --features ffmpeg-aac,display-update-experiment --target-dir work/ffmpeg-receiver-target
cargo clippy --all-targets --locked --offline --features ffmpeg-aac,display-update-experiment --target-dir work/ffmpeg-receiver-target -- -D warnings
cargo build --release --locked --offline --features ffmpeg-aac,display-update-experiment --target-dir work/ffmpeg-receiver-target
./scripts/Test-Media.ps1 -Executable work/ffmpeg-receiver-target/release/airmirror-lite.exe
python -B scripts/Test-ControlLifecycle.py --exe work/ffmpeg-receiver-target/release/airmirror-lite.exe
```
The HAP screen and reconnect checks above passed; the larger reported delay is
unresolved. No further phone action is pending. Real HAP audio/A/V sync is untested.
Keep future tests silent and avoid Computer Use. Do not run --self-test or -Audio
without a new explicit user request for audible testing.
