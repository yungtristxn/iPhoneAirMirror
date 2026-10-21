# Current state

Updated: 2026-10-21

## Runnable build
- Branch `feat/airplay-mvp`; local commits only, no push/publication.
- Run `dist/AirMirrorLite/AirMirrorLite.exe`. Executable SHA-256:
  `B59F3D30066642E875822ACF11CAB6F1052A399A119D5A5F2A675C61D8F218E4`.
- Actual higher-resolution AirPlay is implemented: native HEVC Main/8-bit SDR,
  requested 3840x2160 at up to 30 fps when the installed decoder and GPU support it.
  `--resolution 1440` requests 2560x1440/30; `--resolution 1080` selects H.264/60.
  Hosts without native HEVC support default to H.264 1080. No software fallback.
- New original multi-size app icon is embedded in the EXE and loaded for the
  taskbar/Alt-Tab at window DPI. ICO/SVG/generator are under assets/ and scripts/.
- Frameless phone view, source-pixel fit, proportional corner resize, fullscreen,
  native vector icons, deliberate-hover volume flyout, click mute, and collapsible
  controls remain. C toggles controls; hover at the hidden edge reveals expansion.
- DNS-SD, persistent DPAPI identity, synchronization clocks and WASAPI audio remain.
  AAC/FairPlay retain the zero-capability AppContainer / 256 MiB job. No service,
  new firewall rule, Defender change, external codec binary or font was installed.

## Verification
- 34 Rust tests including HEVC container/NAL/SPS bounds, zero-padding regression,
  8,192 HEVC mutations, H.264 coded-area limits and high feature-bit gating: pass.
  Clippy with `-D warnings`, offline release build and real control/lifecycle pass.
- Silent native probes cover H.264 landscape/portrait, HEVC 3840x2160 and 992x2160,
  paused redraw, resize/fullscreen, partial decoder-initialization recovery, UI
  controls, and embedded 16/32/48/256 icon dimensions. 100 decoder-bound GPU surfaces
  in the full run; audio_samples=0. This is synthetic GPU evidence, not phone audio.
- `python scripts/Test-VideoProfiles.py` verifies real /info replies for 1080/60,
  1440/30 and 2160/30, conditional HEVC bit 42 and clean silent process exits.
- The final decoder-readiness and NAL-framing guards were added after live
  acceptance; their recovery/negative-input probes pass. Live evidence below identifies the preceding binary explicitly.
- **Keep tests silent and do not use Computer Use.** Test-Media.ps1 defaults to
  --ui-test. Do not run --self-test / Test-Media.ps1 -Audio without a new explicit
  request. No sound tests were run during this resolution/icon work.
- MSVC builds use the installed toolchain outside the restricted sandbox due to
  its previous LNK1114/error 5; no protection was weakened.

## Real iPhone evidence
- User confirmed picture appears. Received **994x2160 HEVC**, up from 498x1080:
  nearly four times the source pixels. The log reached 1,876 decoded/decoder-bound
  GPU frames and 1,871 presentations, with zero audio samples and empty stderr.
- First attempt rejected an AirPlay alignment tail safely. The bounded zero-only
  tail fix passed regression tests and allowed the real session to connect.
- Accepted binary SHA-256:
  `89868E0383ED49968F99A7F4443F8004B92EBCBF67384ADC4D1DF550D1FBB955`.
- Passive connected-screen sample (9.24 seconds): host private memory
  126.63→126.73 MiB, handles 1001→1001; sampled CPU 0.00–0.51% of one core.
  This was a quiet text-screen sample, not sustained motion or GPU-memory usage.
- User closed the receiver afterwards. Clean shutdown log, no remaining receiver/
  worker process or port-7000 listener. Leave it closed unless asked to launch.
- Earlier H.264 picture/audio, pause/resume and five-minute synchronized playback
  without freezes were user-confirmed. HEVC sound/A-V sync was not re-tested.
- Lock/unlock, Wi-Fi-loss recovery, 30-minute drift and glass-to-glass latency remain
  unverified. The 120 ms playout setting is not an end-to-end latency measurement.
- Local development build; distribution licensing unresolved. See VALIDATION.md
  and THIRD_PARTY.md. No phone keys, captured media or private logs are committed.
