# Current state

Updated: 2026-10-21

- Branch: `feat/airplay-mvp`, from clean bootstrap `9f21302`; local only.
- Implementation checkpoint committed as `e82528a`. Current running package:
  1,913,344 bytes, SHA-256
  `044982D48211D3926762455993A3466CD44D156F726D8CFCE0548F36B2B7FA8C`.
- Runnable package: `dist/AirMirrorLite/AirMirrorLite.exe`. Full Rust negotiation
  and H.264/AAC transport feed native MF/D3D11/WASAPI. Separate UI message thread;
  bounded queues/backpressure; DPAPI identity; zero-capability AppContainer codec
  worker with one-process/256 MiB job limits. No software/video-only fallback.
- Automated: `cargo test --offline` passed 23 tests, including 12,288 deterministic
  parser mutations; previous `cargo clippy --all-targets --offline -- -D warnings`
  passed. `scripts/Test-Control.ps1` passed real loopback framing/state, one-owner
  admission and timed-shutdown checks. Post-audio-mode clippy and 23 tests passed.
- Native synthetic: `scripts/Test-Media.ps1` passed on RTX 4070 SUPER: 71 H.264
  hardware-decoder surfaces across 1920x1080/1080x1920, 57,600 AAC-ELD samples sent
  to WASAPI, public FairPlay known answer, host-file denial and no delivery of the
  worker's loopback probe. Latest probe additionally passes paused-picture resize
  and fullscreen/restore in both orientations. This is separate from iPhone evidence.
- Live iPhone: discovery, pairing, FairPlay, real hardware-decoded video and
  negotiated AAC-ELD 44.1 kHz stereo reached playback. Latest live attempt delivered
  479 decoder surfaces and 47,233 audio samples before the audio-mode fix. After
  that fix the user confirmed both picture and sound stay working, including pause/resume.
- Corrected: unresolvable synthetic mDNS hostname; use the real computer hostname
  and private physical-interface IPv4. Corrected decoder-surface starvation and
  immediate queue-full disconnects. Audio-mode notification and audio-only teardown
  regression tests pass. A later automated resize exposed missing render-target
  bind flags on the cached NV12 texture; fixed and reproduced by the expanded
  synthetic regression. Its new build is awaiting live retry.
- Post-fix idle-window test also passed three resizes and fullscreen/restore;
  error dialogs are now excluded from that test by checking the window class.
  All 263 pinned vendor file hashes verified. Runtime DLL requirements are in README.
- An older stalled media process required force-stop after normal close timed out.
  Current synthetic/loopback shutdown checks pass; live close remains to be checked.
- Idle snapshot of an earlier release: 0.31% of one CPU core, 51.78 MiB working set
  over 5 seconds. Not a sustained performance or end-to-end latency measurement.
- Native AddressSanitizer: public FairPlay known answer + 1,024 fixed-size valid-mode
  mutations; 120 generated AAC-ELD frames + 1,024 malformed packets passed with PCM
  canaries intact. Command: `cargo run --manifest-path tests/native-sanitizer/Cargo.toml
  --offline` with CARGO_HOME=work/cargo and CARGO_TARGET_DIR=work/san-target.
- Four Windows automatic inbound app rules were narrowed and verified: Private,
  LocalSubnet, edge traversal Block, exact debug/package executable paths. No
  service, exclusions, or Defender/global-firewall changes.
- Build note: sandboxed MSVC librarian failed LNK1114/error 5; offline native builds
  outside the workspace sandbox pass with Rust 1.95.0 / MSVC 14.51.
- Dependencies/licenses: THIRD_PARTY.md. Local development only; distribution
  licensing unresolved. No keys/media/private logs committed.
- User requested a shorter run: use five minutes, not the original 30-minute
  acceptance duration. The first short run was interrupted by the resize bug and
  does not count. Next: retry the redraw fix, measure uninterrupted five-minute
  A/V/working set, then live close. Lock/network loss and end-to-end latency still
  lack physical evidence.

## Completed short live run and follow-up (2026-10-21)

- User confirmed the resumed run stayed in sync without freezes. Resource samples
  cover 11:50:12–11:55:11 UTC (299.5 seconds); the same session continued beyond it.
- RTX 4070 SUPER, driver 32.0.16.1692; Ryzen 7 7800X3D (16 logical CPUs).
  Windows per-process Video Decode counters measured 1.89–1.93% during iPhone video.
- Host private memory 89.81→89.91 MiB, working set 76.51–79.45 MiB, handles 711→708.
  Codec worker private memory 2.33→2.40 MiB and handles 153→153. No increasing
  process/handle count or disconnect appeared during the sampled run.
- The sample exposed ~200% of one CPU core in the host: Windows accepted TCP
  sockets inherited nonblocking mode and the control/video readers busy-polled.
  Both now explicitly use blocking reads with 100 ms cancellation/deadline checks.
- An intermittent `streamConnectionID` rejection was also corrected: this opaque
  identifier preserves all 64 bits even when the plist library represents it as
  signed. Ordinary numeric fields still reject negatives; bounded 16-byte plist
  encodings of u64 values are accepted.
- 26 tests, clippy with warnings denied, release build and loopback lifecycle
  checks pass after both fixes. The new package awaits a brief CPU verification.
- Live window close took 177 ms; no receiver/worker, TCP listener, UDP endpoint
  or mDNS service response remained. The reopened process handle did not retain
  an exit code; the receiver logged clean shutdown with empty stderr.
