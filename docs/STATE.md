# Current state

Updated: 2026-10-22

## Photos rotation event-channel correction awaiting phone check
- User confirms the optimized build's first picture appears sooner, then reports
  a critical mirroring failure while rotating in Photos. Exact F3B058... log shows
  first picture 294.7 ms after claim, transform 7/CW270 at 31056.241, then
  `missing event response CSeq` at 31056.800 and Ready at 31056.887. No GPU/decoder
  error appears; the app continued running and later shut down cleanly.
- The optional event reply parser required RTSP CSeq even for HTTP/1.1 replies.
  On parser error the accepted socket was dropped; absorbing the error afterward
  did not prevent the sender treating that TCP close as a lost session.
- Preserve the accepted event socket after any optional command failure, disable
  further commands, and release at session teardown. HTTP/1.1 replies may omit
  CSeq only with a definite bounded body length and one outstanding request;
  explicit sequence errors and RTSP omissions remain rejected. 102 Rust tests,
  strict Clippy, release build, silent native/GPU/codec checks and three lifecycle
  cycles pass. Tests cover omitted/mismatched/duplicate sequences, negative
  statuses, malformed replies, timeout/late ACK and stop-aware socket retention.
  Zero audio submitted; no new phone acceptance claimed.
- Original bounded scalar log and exact BUILD manifest preserved under
  `local/photos-event-failure/`. Windows event lookup found no matching recent
  AirMirrorLite crash event. The response's exact version/status was not logged
  by the failing build; new scalar reply diagnostics will distinguish it.
- Corrected EXE SHA-256:
  `1FD89573D59425C39267AF93BEBEE0D9EE6DEFB91B20C555AD19C2657D456E94`.
  Latest native evidence: `local/media-test/result.json`; lifecycle:
  `local/control-lifecycle/20261022T182030147988Z/result.json`.
- Fix committed as `2ba01a1`; corrected receiver is open with verified discovery.
  Launch and copied checks: `local/photos-event-fix/`; the prior candidate remains
  under `work/pre-event-response-fix-candidate/`. Updated source-included archive:
  `dist/AirMirrorLite-Windows-x64-photos-event-fix-2026-10-22.zip`.

## Connection performance follow-up
- User clarified the delay is after selecting AirMirrorLite, before first picture.
  Replaced 20 ms control/video accept sleeps with bounded readiness waits. Retain
  exactly one pristine preflight worker for the first valid encrypted SETUP;
  consume and destroy it, with fresh workers for later sessions.
- Fixed Windows log truncation so the 1 MiB cap works. Added scalar first-picture
  stage timings. No video-quality or audio-buffer changes.
- Five-process synthetic authenticated comparison: first claim-to-video-SETUP
  median 45.497 -> 6.909 ms; encrypted SETUP 23.379 -> 3.722 ms; video-ready to
  accepted TCP 21 ms -> below 1 ms log resolution. These include local synthetic
  sender work, not iPhone tap-to-picture. Later-session setup is broadly unchanged.
- 98 tests pass, strict Clippy/formatting and silent native/GPU/codec checks pass;
  three control lifecycle cycles and three video profiles pass. Zero audio output.
  Prepared worker costs about 4.9 MiB while waiting for the first connection.
- Candidate SHA `F3B058A6E3D5732703661FDFE6CDB5FC067C31AC60D2BE81D4C38F5AF02CC9E9`.
  Details, commands and evidence: PERFORMANCE_FOLLOWUP.md. User confirmed the
  quiet first picture appears sooner; the later Photos failure is tracked above.
  No sound tests or Computer Use.
- Implementation committed as `77717dc`; the candidate is open with verified
  listener/discovery readiness. Launch and copied validation evidence are under
  `local/connection-performance-phone-test/`. The prior candidate is preserved in
  `work/pre-connection-optimization-candidate/` and both FDK fallbacks are unchanged.
  Updated source-included ZIP passes full CRC/SHA and source checks:
  `dist/AirMirrorLite-Windows-x64-connection-fix-2026-10-22.zip`.

## Windows network profile restriction removed
- At the user's request, startup no longer queries Network List Manager or
  requires a Private profile. Public, Private and Domain labels have no effect
  on adapter selection. Still selects one active Ethernet/Wi-Fi RFC1918 IPv4
  interface with a gateway; explicit --bind and same-interface discovery remain.
- Missing-network guidance now describes the actual LAN requirement. User-facing
  docs no longer instruct changing Windows' network classification. No Windows
  profile, firewall or Defender settings were modified.
- 95 Rust tests, strict Clippy and release build pass. Silent native tests include
  adapter-selection rejection/selection cases, 160 GPU surfaces and 480 AAC
  fixture frames; zero audio submitted. Three loopback ownership/lifecycle cycles
  pass. Evidence: `local/media-test/result.json` and
  `local/control-lifecycle/20261022T171747212972Z/result.json`.
- Updated candidate SHA-256:
  `32AFE7CFF278F8CC33235DEB142DCA0DD89258EFDCF9DEEFA4333E704BC2C02B`.
  Updated share archive:
  `dist/AirMirrorLite-Windows-x64-network-fix-2026-10-22.zip`.
  Actual reception on the friend's Public-profile PC remains unverified; no
  physical phone/audio acceptance is inferred from the silent tests.

## Initial friend-test ZIP (superseded by network fix above)
- User authorized a local ZIP to share with a friend. Original project source
  and artwork are now GPL-3.0-only (LICENSE.md / NOTICE.md); imported notices stay.
- `scripts/Package-Share.py` packages only exact FFmpeg/rotation candidate
  AF3B9439... after an offline freshness build matches its hash. It includes full
  application, locked Rust and pinned FFmpeg sources plus build instructions.
  Default/FDK package guards and both working fallback binaries remain intact.
- The archive contains a newly generated 994:2160 aspect preset, no pairing
  identity, logs or captures. Launcher prepares the extracted executable's worker
  permission without elevation, downloads or firewall changes. Microsoft Visual
  C++ v14 x64 is a prerequisite; higher resolution still depends on native HEVC.
- Package CRC/SHA verification and offline source resolution with empty Cargo
  home pass. Launcher checks pass preparation, repeat idempotence, punctuation in
  extraction paths and failure handling. Exact sources reproduce AF3B9439...
  Evidence: `local/share-launcher-test/20261021T2317189447791Z/`.
- Fresh ZIP extraction also passes owner preparation, hidden no-discovery startup
  on an ephemeral loopback port and clean shutdown, with zero audio submitted.
  Evidence: `local/share-extraction-check.json`. Final archive location:
  `dist/AirMirrorLite-Windows-x64-test-2026-10-22.zip` plus verification JSON.
- No upload/publication, sound test or Computer Use. Other-PC compatibility and
  latest Photos/FFmpeg physical A/V acceptance remain unverified.

## Rotation disconnect fixed; Photos landscape follow-up open
- The manual FFmpeg phone test reached 994x2160 hardware mirroring, then rotation
  hit the Rust HEVC eight-picture DPB guard. The rejected count was not logged.
- Replaced the fixed guard with HEVC level/area bounds (16/12/8/6 pictures) while
  preserving resolution/packet/depth limits; added scalar inline SPS diagnostics.
  Fixed duplicated artwork behind error cards and bounded narrow card sizing.
- 82 Rust tests pass on both backends; Clippy/formatting pass. Silent native tests
  pass 162 production-parser access units, four DPB16 declarations, 160 GPU surfaces,
  controls/status/recovery/rotation, 480 AAC frames and sandbox checks; zero audio.
  Lifecycle tests pass three ownership/teardown cycles and pending-read shutdown;
  profile/aspect tests pass 1080/1440/2160 at both 30 and 60 fps.
- Corrected candidate SHA-256:
  `0F6B9E3EF7A41DA65E7557C5E86C4F935DE473068894D7D043E89BD2AACA14BF`.
  The user confirmed rotation remains connected and correctly sized, then clarified
  that Photos landscape still shows a small upright photo in a portrait PC frame.
  Live metadata proves Settings changes from 994x2160/DPB6 to 994x458/DPB12; Photos
  instead continues sending 994x2160. The user closed this candidate after testing.
  Both FDK fallback hashes remain unchanged. Rotation-capability advertisement
  and scalar decoder-orientation metadata are the next controlled investigation.
- The opt-in rotation-advertisement trial is now open at the candidate path,
  SHA `E74F70DD72E7F3B893216E8F28D05C44D6137A88F66ECF701AEBFBCE87CA4346`;
  it pairs display rotation=true with ScreenRotate bit 8, keeping the aspect hint.
  82 tests, strict Clippy, silent native checks and wire profiles pass. The user
  reported that it still fails; their screenshot shows the whole Photos interface
  sideways in the portrait frame. The trial produces source=994x2160 with a
  landscape display hint=994x458, while MF rotation is absent and both apertures
  remain full-frame. Normal packaging does not enable this feature.
  Previous FFmpeg binary: `work/rotation-verified-candidate/`; trial manifest and
  checks: `local/rotation-advertisement/`. See ROTATION_REGRESSION.md for limits.
- Further trial A761D568... passed 91 Rust tests and full silent GPU checks but
  still showed sideways Photos: this phone sends no rotation SEI. Research now
  identifies configuration byte5 as SymmetricTransform (0/3/4/7 map to
  0/180/90/270 clockwise). The unused SEI experiment has been removed.
- Current follow-up uses that actual transport metadata with verified per-frame
  GPU rotation, plus an opt-in legacy event channel for full updateDisplayInfo
  requests. Dynamic canvas requests follow only successfully presented frames of
  the established hint shape; no image cropping or destination-rectangle guesses.
  Built candidate SHA-256 is
  `AF3B9439AFB4BC220FF4F42ED4B03FD6B7359B549B46390B6DC1F1D175A1C183`.
  95 Rust tests pass with both default FDK and experimental FFmpeg builds;
  strict Clippy, silent native/GPU/worker checks and all three
  advertised profiles pass. GPU evidence includes all quarter-turns, exact PTS
  associations and busy-present cache preservation; 160 hardware surfaces,
  162 parser-validated access units and zero endpoint audio. Physical Photos
  convergence remains pending; normal packaging still disables both experiments.
  This exact build is open at the candidate path; discovery readiness is verified.
  Manifest, launch and copied test evidence: `local/transport-rotation-phone-test/`.
  Exact-candidate lifecycle checks also pass three ownership/teardown cycles and
  baseline /info checks; separate event tests pass pending-ACK cancellation and
  coalescing. Evidence: `local/control-lifecycle/20261021T201559541218Z/`.
  Implementation checkpoint: `780c10c`; physical Photos result remains pending.
- Full bounded interaction matrix, commands, evidence and physical limitations:
  ROTATION_REGRESSION.md. Launch: `local/rotation-phone-test/launch.json`.

## Working receiver preserved
- Accepted source remains `feat/airplay-mvp` at `e06321c`.
- `dist/AirMirrorLite/AirMirrorLite.exe` and the extra
  `dist/AirMirrorLite-fallback-e06321c/AirMirrorLite.exe` remain byte-identical:
  SHA-256 `CCD1192625E6193A2CD2564621294CDEE810C8B2198B1639D40DA52E3755ED43`.
  Original identity/logs stay in place and are not copied into the candidate.
- Earlier quiet Settings/Photos/rotation/reconnect acceptance and five-minute
  H.264 picture/audio acceptance belong to that build. They are not FFmpeg evidence.

## Decoder delay implemented; separate FFmpeg receiver candidate
- Branch: `codex/ffmpeg-aac-experiment`. Native PCM replies now atomically include
  validated additional DSP delay and configuration generation. Integer RTP mapping
  subtracts that delay, trims pre-generation priming and adds no FIFO/padding.
- FDK LC delay is 1,685 samples (38.2086 ms); ELD and FFmpeg additional delay are
  zero. FFmpeg's raw context delay stays diagnostic, not a clock substitute.
- Flush/loss/errors reconfigure decoder and mapper together. Native failures latch
  until configure. A transport rewind fence survives decoder resets; explicit
  FLUSH/new transport is required to rewind. Existing session/flush queue fencing
  remains. Encoder/filterbank priming is not guessed or recalibrated.
- `scripts/Package.ps1 -FfmpegCandidate` creates
  `dist/AirMirrorLite-ffmpeg-candidate/AirMirrorLite.exe`, linking pinned minimal
  FFmpeg AAC/avcodec/avutil with no FDK. Default packaging still selects FDK.
  The optional `decoder-lab` feature separately blocks receiver startup.
- The local candidate retains the known `994 2160` display-aspect preference.
  At the user's request, the accepted receiver was stopped and the candidate was
  opened for manual phone testing. Its log confirms listener/discovery readiness;
  phone connection and physical A/V acceptance remain pending. Both fallback
  hashes were verified unchanged. Launch evidence: `local/ffmpeg-phone-test/launch.json`.
  That initial candidate was built from `70dff1d`; the rotation correction above
  supersedes it. No sound tests or Computer Use were run.

## Earlier decoder-delay verification (70dff1d)
- 75 Rust tests pass on both backends; strict Clippy and formatting pass.
- 17 analyzer tests include wrong-sign/missing trim, shifted PCM, stale generation
  and delay, resource/report bounds and controls that must fail.
- Ten-session lab comparison: `local/decoder-experiment/delay-qualified-final/`.
  Four fixtures, 10,080 measured packets per backend plus guards. Raw LC offset
  remains -1,685; normalized nonperiodic ELD/LC both have zero sample offset and
  at most one i16-unit waveform difference. Raw adoption still fails; the explicit
  normalized source-clock qualification passes without relaxed tolerances.
- Actual candidate binary also passes its separate synthetic timing comparison:
  `local/decoder-experiment/receiver-candidate-delay-final/` (two sessions).
- Direct FFmpeg C-ABI bounds/PCM+metadata canaries and explicit reset checks pass.
  FDK/PlayFair ASan passes, including 1,024 freshly initialized malformed AAC cases.
  This is not whole-FFmpeg sanitizer coverage or exhaustive fuzzing.
- Candidate silent native/GPU and restricted-worker checks pass: 100 hardware
  decoder surfaces, AAC-ELD/LC golden hashes across resets, zero audio submitted.
  Loopback ownership/framing/state/shutdown checks pass. Evidence:
  `local/media-test/result.json`, `local/control-test/result.json`.
- No audible tests, Computer Use, phone capture, firewall/Defender changes or
  publication. Final executable/source identity is in the candidate's BUILD.json.

## Commands and remaining boundary
- `cargo test --locked --offline [--features ffmpeg-aac]`; strict Clippy; cargo fmt.
- `scripts/Test-FFmpegBoundary.ps1`; `cargo run --manifest-path
  tests/native-sanitizer/Cargo.toml --locked --offline` (CARGO_TARGET_DIR=work/san-target).
- `python -B -m unittest discover -s tests/decoder-experiment -p test_*.py`;
  `python -B -O scripts/Test-DecoderExperiment.py --reference <FDK-lab-exe>
  --candidate <FFmpeg-exe> --sessions 10 --output <new local directory>`.
- `scripts/Package.ps1 -FfmpegCandidate`; `scripts/Test-Media.ps1 -Executable
  <candidate>` and `scripts/Test-Control.ps1 -Executable <candidate>`; no -Audio.
- Physical FFmpeg A/V sync, long-run drift and audible near-clipping behavior
  remain unverified. FFmpeg saturates immediately; it does not reproduce FDK's
  lookahead limiter. See DECODER_EXPERIMENT.md for the contract and limits.
- The user authorized the source-included friend-test ZIP above, with no public
  upload. The FDK distribution gate remains. A stable release and physical A/V
  acceptance remain separate work. No audible tests without a new explicit request.
