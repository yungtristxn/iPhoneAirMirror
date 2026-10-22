# Sender suspension and black viewport

Requested 2026-10-22: make the PC view black when the iPhone screen turns off.
The feature uses an explicit sender suspension signal, not inactivity, black-bar
recognition, or a still-image timeout. Paused photos remain visible normally.

Pinned [UxPlay's mirror configuration handling](https://github.com/FDH2/UxPlay/blob/15a6b87172eca38b0df4e615fbf1bba108734dde/lib/raop_rtp_mirror.c#L580-L588)
identifies configuration-header byte 6 values 0x56/0x5e as suspended and 0x16/0x1e
as resumed. This phone's scalar logs contain matching 0x011e -> 0x015e changes
followed by stopped frame delivery. Only an exact 128-byte opcode-1 header can
carry this state; unknown option values retain the preceding state.

The implementation preserves the phone frame and quick controls, covers the
viewport with opaque black, and clears the retained presentation image. On resume,
the viewport stays black until a fresh frame succeeds. A transition generation
travels with each compressed input and its decoded output; old queued/decoded
frames are never made visible by a later resume. Compression reference state and
its bounded timestamp associations remain usable. Audio ownership and playback
are independent of screen visibility.

Validated 2026-10-22:
- 111 Rust tests pass for both the FFmpeg/display candidate and default backend.
  This covers exact/unknown header flags, idempotent and coalesced transitions,
  full compressed queues, owner replacement and preserved audio-flush state.
- Strict Clippy, formatting and the release build pass. A read-only second-agent
  review found no concrete correctness defect.
- Silent native tests on NVIDIA GeForce RTX 4070 SUPER validate opaque black,
  suspended paused frames, resize/fullscreen, minimized restore, busy presentation,
  old queued/decoded frames, resumed decoding and repeated/coalesced transitions.
  All 190 decoded surfaces were GPU surfaces; zero audio samples were submitted.
  The pixel readback is a generated constant 1x1 black surface, not phone content
  or a screenshot. This does not establish final desktop-compositor appearance.
- Three synthetic owner/reconnect cycles and clean process/listener shutdown pass.
  Lifecycle evidence: local/control-lifecycle/20261022T192816883327Z/result.json.
- Executable SHA-256:
  7B65C9ACA58FC8C5C4EDB13B98DF198E032A575B491FA7112AD7033926FB1F31.
  Saved build/test evidence: local/screen-off-verification/.

Physical check on this exact build: the user confirms "Black on lock; picture
restores." The scalar log records suspension at 35840.734 (generation 1), resume
at 35841.444 (generation 2), and successful generation-2 presentation afterward.
The receiver stayed muted. Evidence: physical-check-log.txt and validation.json
under local/screen-off-verification/. The app was closed afterward and all session
threads joined. A configuration racing that normal shutdown produced a spurious
teardown error. The Photos handoff follow-up now distinguishes normal cancellation
from live-owner failures, with a focused regression and clean lifecycle checks.
No audible A/V or end-to-end lock latency claim is made.

The subsequent F92829... Photos handoff build also passed the user's lock/unlock
check. Its log records suspend/resume generations 1/2 and fresh presentation,
muted with zero submitted audio samples. See PHOTOS_TRANSITION.md for that exact
build and local/photo-handoff-verification/ for its independent evidence.

Timing boundary: priority blanking starts after the suspension configuration is
parsed. Existing bounded video-reader backpressure or clock waits can delay
reaching a later wire record; this feature does not claim instant sensor-to-PC
screen-off timing. If a sender supplies independent external content without a
suspension signal, screen darkness must not be guessed from a stationary picture.
