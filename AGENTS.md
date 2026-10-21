# AirMirrorLite

## Product
Minimal Windows AirPlay screen mirroring **with synchronized phone audio**.
One local-network sender; native resizable/fullscreen window; close means exit.
Audio is required. A video-only spike is an intermediate result.

## Context
Read README.md, docs/SPEC.md and docs/STATE.md first.
Use docs/RESEARCH.md for architecture/API references.
Read docs/SECURITY_BASELINE.md before importing or changing AirPlay/native code.
Keep these sources canonical; do not duplicate the research into this file.

## Engineering
Prefer Rust, Win32, native DNS-SD, Media Foundation/D3D11 and WASAPI.
Keep dependencies and runtime work small; verify negotiated audio codec support.
Reuse maintained crypto/codecs; do not invent cryptography or copy an entire app.
Bound network input, queues, allocations, sessions and logs.
Keep unsafe/FFI code narrow; a second process alone is not a sandbox.
Pin imported source and record origin/license; revalidate prior security leads.
No recording, remote control, USB, DLNA/HLS player, BLE, cloud, updater or service.

## Evidence
Never label a scaffold or simulated sender as real-device acceptance.
Separate build, parser tests, GPU checks and live iPhone/audio evidence.
Measure latency end to end separately from local decode timing.
Maintain a short docs/STATE.md checkpoint with commands, results and next step.
Respect the parent workspace's Defender guidance. No exclusions or bypasses.
