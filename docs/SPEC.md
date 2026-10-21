# Required scope and acceptance

Confirmed 2026-10-21: the user wants their own minimal, efficient Windows AirPlay receiver and **phone sound is required**.

## Scope
- Native iPhone Control Center > Screen Mirroring over the same local network.
- H.264 SDR screen video with correct aspect ratio and portrait/landscape changes.
- Synchronized phone audio through Windows speakers using WASAPI.
- One active sender; a second sender must not silently replace the current one.
- One resizable window, fullscreen, disconnect and minimal connection status.
- Ordinary-user runtime; close fully exits, deregisters discovery and closes sockets.
- No timer, watermark, subscription, account or cloud relay.

Exclude recording, remote phone input, USB capture, DLNA, direct URL/HLS playback, standalone music-player features, BLE discovery, startup service, autostart and automatic updating. Necessary AirPlay negotiation/audio transport is still in scope.

## Preferred design
Rust and Microsoft's windows bindings. Native DnsServiceRegister for discovery; Media Foundation H.264 decoder with D3D11/DXGI presentation. Keep decoded frames GPU-resident. Use a narrowly scoped screen-audio decoder if the negotiated format requires one; Media Foundation AAC-ELD support must not be assumed.

Use bounded protocol/session code and an actual restricted worker for retained risky native parsing if feasible. One executable may provide host and worker roles. Prototype OS/codec/isolation compatibility before committing to large abstractions. Document an evidence-backed deviation, rather than silently dropping sound, hardware decoding or protection.

## Acceptance
1. iPhone discovers the receiver and establishes a real mirroring session.
2. Moving video and phone audio work together; no growing A/V drift over at least 30 minutes.
3. Portrait/landscape, resize and fullscreen retain correct image/audio behavior.
4. Disconnect/reconnect, lock/unlock and network loss recover cleanly.
5. One active sender is enforced; invalid/unexpected protocol sequences are rejected.
6. Malformed lengths, truncated records, invalid pairing transitions and excessive bodies do not crash the app or cause unbounded memory growth.
7. Hardware decode is confirmed on compatible hardware; any fallback is explicitly reported.
8. CPU/GPU/working-set/frame-rate measurements state device, resolution and workload. 1080p-class/up-to-60-fps is an initial capability target, not a guarantee.
9. Closing the window leaves no receiver processes, listeners or discovery registration.
10. Build/package/release identity and imported component licenses are recorded.

A passing build, synthetic media or simulated sender is not equivalent to items 1-4. When physical interaction is needed, provide the exact next phone action after preparing a runnable build, and continue independent work.

## Implementation order
1. Verify toolchain and minimal Windows discovery/decoder/audio feasibility; establish module boundaries.
2. Deliver a real vertical slice: iPhone discovery -> negotiation -> decoded picture -> sound.
3. Complete lifecycle, A/V timing, one-sender behavior and display controls.
4. Harden input/state/native boundaries, then run focused negative tests and real-device acceptance.
5. Measure and optimize observed bottlenecks; do not expand product scope.

## Dependency policy
Record source URL, pinned revision/version, purpose and license for each imported component. Existing UxPlay-family code is an interoperability reference and may contain the previously identified unsafe paths. A different branch or wrapper is not evidence those paths are fixed. Project distribution licensing remains to be selected when actual dependencies are chosen.
