# Original AirMirrorLite bootstrap brief

Historical starting instructions, retained to explain the original scope. The
bootstrap has already been implemented. For current work, use SPEC.md and STATE.md;
do not recreate the branch or replace the current application with a scaffold.

Original prompt (repository root):

Build the first usable version of this minimal Windows AirPlay receiver: iPhone screen mirroring **with synchronized phone sound**, one active sender, one native resizable/fullscreen window, disconnect, and complete shutdown when the window closes.

Start by reading AGENTS.md, README.md, docs/SPEC.md and docs/STATE.md. Consult docs/RESEARCH.md for architecture and official API references, and docs/SECURITY_BASELINE.md before selecting protocol/native code. Inspect the actual scaffold and toolchain; the current executable is only a placeholder.

Implement the application, not just another plan or UI mockup. Create feat/airplay-mvp from the clean bootstrap and make reasonable implementation decisions autonomously. Keep the first architecture small. Prefer Rust + Win32, Windows DNS-SD, Media Foundation/D3D11 hardware video decode, and WASAPI audio. Prove the negotiated iPhone audio format; AAC-ELD may require a small decoder dependency. Do not mark video-only output complete.

Use current AirPlay implementations as interoperability references. Pin and record any imported source and licenses. Revalidate and fix relevant parser, pairing-state and buffer-limit problems before relying on reused code. Keep cryptography/codecs in maintained implementations, unsafe/FFI boundaries narrow, and resource usage bounded. Verify a real isolation boundary for any native worker; a child process alone is insufficient.

Deliver a vertical slice first: discovery -> real iPhone negotiation -> picture -> synchronized sound. Then finish rotation, resize/fullscreen, single-sender enforcement, reconnect and clean teardown. Keep video GPU-resident, avoid re-encoding and unnecessary copies, and bound queues while preserving compressed-frame dependencies. Optimize measured bottlenecks.

Stay within the feature exclusions in SPEC.md. Do not add a web UI, cloud service, recording, phone control, USB path, DLNA/HLS player, BLE, background service or updater. Do not weaken Defender or create exclusions.

Run focused build, protocol/state/length, lifecycle and media tests. Prepare a runnable build before asking me to perform phone-side actions; ask only for the exact physical step or genuinely blocking decision, and continue independent work meanwhile. Never claim live mirroring, hardware decoding, sound, A/V sync or performance from compilation or a simulated sender.

Keep docs/STATE.md concise with actual commands/results and the next unresolved step. Finish with launch instructions, what works, validation evidence and remaining limitations. Commit coherent verified changes locally; do not publish or push.
