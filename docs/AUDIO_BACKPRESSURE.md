# Photos audio-start burst correction

The 2026-10-22 phone run on 3A2CDD... successfully acknowledged repeated display
updates using RTSP replies without CSeq. The sender then negotiated AAC-ELD at
32258.980. At 32260.982 it began delivering queued audio; by 32260.985 the native
32-block queue was full while the WASAPI buffer still contained all 1,036 frames.
The existing admission path treated this temporary capacity condition as fatal.
Continuing the same drain loop repeated the error five times and filled the
connection card with internal timestamps. Evidence is retained privately under
`local/photos-audio-burst-failure/`.

The receiver now checks native audio capacity before removing or applying gain
to the next queued packet. PCM, timestamps and order remain intact in the existing
bounded channel or pending slot until capacity returns. A blocked native queue
does not force zero-time polling; normal WASAPI events drive progress. The native
and Rust queue limits remain 32 each, with no new playback buffer or clock offset.

The producer also propagates backpressure rather than immediately failing a full
Rust channel. Its retry is bounded to 500 ms and checks stream stop, audio-only
stop, shutdown, owner, failure and FLUSH generation. Canceled packets cannot leak
into a new generation. One wake on first blockage avoids repeatedly waking the
UI; insertion signals it again. A real native failure latches the failed owner,
flushes queued playback, logs the original cause once, and ends the drain burst.
The user-visible card contains only a concise reconnect instruction.

Silent tests cover a 96-block burst, a simulated full 1,036-frame endpoint,
future silence, drain/refill, exact stereo samples and timestamps, fixed queue
limits, canceled/FLUSHed sends, readiness recovery and failure latching. The
native simulation calls the production mixer only with an in-memory array; it
cannot call WASAPI GetBuffer/ReleaseBuffer/Start. Full native/worker checks also
pass with zero endpoint audio samples. 107 Rust tests and strict Clippy pass.

`--mute` starts normal reception muted using the existing local gain control.
It preserves audio decoding, buffering and A/V synchronization, and can be
toggled with M or the speaker button. It is used for the next phone check so
automatic Photos audio remains inaudible. A hidden loopback startup verifies
`muted=1`, zero submitted samples and clean exit; no audible sync acceptance is
claimed. Candidate executable SHA-256:
`8972816C665941D5C217CF1393ACCDE0BC5A7E2AC2B6B335DB8AD38304AE9E94`.

Evidence: `local/media-test/result.json`, `local/muted-startup-check/result.json`,
and `local/control-lifecycle/20261022T184724653263Z/result.json`.

The subsequent physical iPhone check remained connected without the audio error.
Its log records 604,169 submitted audio frames with local mute active throughout,
native queue high-water 30/32, 1,893 decoded pictures and two successful display
update acknowledgments. The Photos/audio-start workflow completed without error
in this run; the synthetic test covers the exact-full-buffer boundary. This does
not establish audible quality or A/V synchronization.
Evidence: `local/photos-audio-burst-fix/phone-check.json` and its scalar log.

Rotation while a photo remains open is still a separate investigation. Opening
a photo now updates the external canvas, and the live RTSP acknowledgments work.
The next useful check holds one still photo open while turning the phone and
compares received type-1 transform/configuration changes. Sender type-5 packets
are performance statistics, not a justified physical-orientation input; no
opaque metadata or pixel content is used to guess a rotation.
