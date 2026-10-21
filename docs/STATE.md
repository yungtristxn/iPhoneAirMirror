# Current state

Updated: 2026-10-21

## Runnable build
- Branch `feat/airplay-mvp`; local commits only, no push/publication.
- Run `dist/AirMirrorLite/AirMirrorLite.exe`. Executable SHA-256:
  `281B692CBB468F8BF2D829F4CC1B4AB8F57D8E74170621B3717A3ABC47E4B2AE`.
- Frameless rounded phone view with a quiet, pixel-aligned graphite rim.
  Default/reset size uses the negotiated source pixels when they fit the monitor,
  with no default enlargement. UI DPI is separate from physical video pixels.
  The last observed portrait stream was 498x1080; this change does not increase
  the phone's transmitted resolution or alter negotiation/decoder budgets.
- Native DirectWrite Segoe Fluent Icons (installed MDL2 fallback), standard icon
  sizes, a horizontal volume flyout after deliberate hover, stationary buttons,
  wider slider target, safe pointer travel and Escape dismissal. Click speaker
  to mute/restore; phone volume and media timestamps remain independent.
- Small side chevron hides the controls; hover beside the phone to reveal the
  expand chevron. `C` toggles them; Tab/Enter also works while collapsed.
  `0` reset, M mute, Up/Down volume, Left/Right fine volume when speaker focused,
  F11/double-click fullscreen, D disconnect. Drag picture to move; corner to resize.
- Native DNS-SD, MF/D3D11 video, WASAPI audio, bounded queues and DPAPI identity
  remain. AAC/FairPlay retain the zero-capability AppContainer / 256 MiB job.
  No imported sources/fonts, services, firewall or Defender changes.

## Verification and test preference
- **User stopped audible testing.** Do not run `--self-test` or
  `scripts/Test-Media.ps1 -Audio` without a new explicit request.
  `--ui-test` and Test-Media.ps1 now default to silent UI/GPU validation.
- 26 Rust tests, clippy `-D warnings`, offline release build: pass.
- Final silent native test: 71 hardware decoder surfaces, 68 presentations,
  `audio_samples=0`, exit 0. Portrait/landscape, resize/fullscreen and paused
  redraw pass on RTX 4070 SUPER. No audio was submitted by the final tests.
- UI tests cover delayed/stale hover, horizontal drag/endpoints, mute restore,
  Escape dismissal, exact source-pixel default sizing, collapse/expand including
  keyboard access, and stable speaker/dock positions near monitor edges.
- Native artwork inspected at 100%, 125%, 200%, plus collapsed/hover states under
  ignored `local/refinement-renders`. These are internal artwork renders, not
  desktop screenshots or physical mixed-DPI monitor acceptance. No Computer Use.
- An intermediate renderer teardown hang was fixed by explicit UI-thread resource
  lifetime. Native test exit and real loopback listener/admission/negative-input/
  timed-shutdown checks pass. See VALIDATION.md for detailed evidence boundaries.
- Commands: `scripts/Build.ps1 -Test`; `cargo clippy --all-targets --locked --offline
  -- -D warnings`; `scripts/Test-Media.ps1 -UiRenderDirectory local/refinement-renders`;
  `scripts/Test-Control.ps1 -Executable target/release/airmirror-lite.exe`.
  MSVC uses the installed toolchain outside the restricted sandbox due to its
  previous LNK1114/error 5. No protection was weakened.

## Physical evidence / next step
- Earlier real iPhone picture/audio, pause/resume and five-minute synchronized
  playback with no freezes were user-confirmed; the user shortened 30 minutes.
  Historical live CPU was about 2.3% of one core combined, GPU Video Decode about
  1.9%, and clean live close 177 ms. These measurements predate this UI refinement.
- User exercised the preceding frameless build and reported softness, border/icon
  and hover issues. This revision still needs their visual/phone check; automated
  results do not establish improved perceived picture quality or current A/V sync.
- Lock/unlock, Wi-Fi-loss recovery, 30-minute drift and glass-to-glass latency remain
  unverified. A 120 ms playout setting is not an end-to-end latency measurement.
- Local development build; distribution licensing unresolved; MSVC x64 runtime
  required. No phone keys, captured media or private logs are committed.
- Next: use the packaged revision for a visual/controls check. Keep further tests
  silent unless the user explicitly changes the preference above.
