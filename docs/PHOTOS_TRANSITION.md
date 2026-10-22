# Photos feed handoff

Requested 2026-10-22: opening a photo feels like a small jump/stall. The user
explicitly prefers the photo-only PC view, with a smoother handoff; recreating
the phone toolbar or thumbnail strip is not requested.

## Evidence and boundary

The accepted screen-off build (7B65C9...) records the following transition in
local/screen-off-verification/physical-check-log.txt:
- 35851.562: 994x2160 source with CW270; display hint 994x458.
- 35852.190: landscape display update acknowledged.
- 35852.266: source still 994x2160/CW270; hint now 2158x994.
- 35853.063: photo source changes to 2160x994/CW0. Effective presentation remains
  2160x994, so this source-format handoff does not resize/recenter the phone shell.
- 35853.951: source returns to 994x2160/CW270 at the same effective display size.

Photos controls the independent content it sends. The app keeps that photo-only
content and its actual timestamps. Missing interface frames cannot be recovered
from this stream, and an abrupt sender-side cut cannot be reconstructed as the
phone's original animation. No image recognition, crop or recording is used.

## Receiver corrections

Normal Media Foundation format renegotiation previously cleared the private frame
that was last successfully presented. It now remains available for redraw until
its replacement successfully presents, with its own aperture, color,
rotation and screen-visibility generation. Old decoder-owned queued output can
still be released during renegotiation. Suspension and owner reset still clear
the private frame; no old content may return after lock/unlock.

This follows the existing stream-change protocol: negotiate a new output type,
then continue processing. It does not require throwing away a separately owned
renderer's last frame. Reference: Microsoft's
[Handling Stream Changes](https://learn.microsoft.com/en-us/windows/win32/medfound/handling-stream-changes).

The event scheduler also used the same 500 ms duration for both geometry settling
and minimum spacing between commands. The trace shows rotation-to-ACK delays of
628 ms and 577 ms, followed by the sender's new display geometry at 704 ms and
609 ms. A delayed canvas update can therefore plausibly create another size step
if a photo opens during that wait; the logs alone do not prove the visible cause.
Separate a 100 ms settling period from the retained 500 ms send interval. Keep
the 50 ms poll, one outstanding request, no ambiguous retry, and complete display
dictionary. This saves about 400 ms of deliberate scheduling wait for isolated
changes without increasing the two-per-second command ceiling. It is not a
measured phone-to-window latency reduction.

Normal shutdown can also race an explicit screen-state record. Such publication
must cancel quietly without mutating visibility or creating a playback error.
Live-owner invariant and serial-exhaustion errors remain failures.

## Verified automated checks

- 114 Rust tests and strict Clippy pass with both backends; formatting and the
  exact FFmpeg/display candidate release build pass. Tests cover isolated 100 ms
  settling, duplicates, rapid flips, the retained 500 ms send interval, withheld
  ACK/retention behavior and canceled versus invalid screen publications.
- Silent native checks on RTX 4070 SUPER pass four real HEVC format changes,
  preserving a deliberately non-default crop/color/rotation until successful
  replacement. The old private frame redraws while new output is delayed or busy.
  190 hardware surfaces, 192 parser-validated access units and zero audio samples;
  the existing lock/unlock, resize and lifecycle checks also pass.
- Three synthetic owner/reconnect cycles and clean process/listener shutdown pass:
  local/control-lifecycle/20261022T194408294268Z/result.json.
- An independent read-only review found no concrete correctness regression.
- Test evidence: local/photo-handoff-verification/. EXE SHA-256:
  F928294B01BB7F27ECC1571E4B8F4B301DF153081A696980FE5F7822C7BBDA40.

## Quiet phone comparison

The user reports "Smoother now" and confirms black-on-lock/picture restoration
still works on the exact F92829... build. The scalar log confirms 994x2160 hardware
presentation, muted=1, zero submitted audio samples and suspend/resume generations
1/2. Evidence: physical-check-log.txt and validation.json under the directory above.

The captured comparison segment retained the same coded format: its preserved
format-handoff counter stayed zero and no display-update command was needed. Thus
this is user-reported visual improvement and a real lock/unlock regression check,
not direct live evidence for the changed-format path or a measured 400 ms phone
latency gain. Those code paths have the separate native/scheduler evidence above.
The accepted 7B65C9... build remains in work/pre-photo-handoff-candidate/.
No sound playback, Computer Use or phone-screen capture was used.
