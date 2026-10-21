# AirMirrorLite

A planned minimal Windows app for iPhone AirPlay **screen mirroring with synchronized sound** over the local network.

**Status:** Git/Rust project bootstrap only. The executable currently reports this status; it does not advertise, listen, mirror, decode, play sound, install services or change the firewall.

## Start
- [Implementation prompt](docs/START_HERE.md)
- [Required scope and acceptance](docs/SPEC.md)
- [Architecture research and sources](docs/RESEARCH.md)
- [Security regression leads](docs/SECURITY_BASELINE.md), when implementing protocol/native code
- [Current state](docs/STATE.md)

## Intended experience
Open the app, select AirMirrorLite in iPhone Control Center > Screen Mirroring, and see/hear the phone. One active sender; resize, rotation, fullscreen, disconnect. Closing the window fully stops the receiver.

Preferred stack: Rust + Win32, native Windows DNS-SD, Media Foundation H.264 + Direct3D 11, and a narrowly selected audio decoder feeding WASAPI. Architecture assumptions must be verified on a real iPhone and the target GPU.

## Current scaffold checks
```powershell
cargo fmt --check
cargo check --offline
cargo build --release --offline
```

These check only the bootstrap. They do not establish AirPlay or hardware compatibility. Cargo.lock is committed because this is an application. No upstream implementation or third-party dependency has been imported yet; record origin, exact revision and license when adding one.

The kickoff prompt uses focused context, explicit boundaries and observable acceptance criteria, informed by [OpenAI's workflow guidance](https://developers.openai.com/cookbook/examples/codex/iterating-development-workflows-with-codex#build-phase-files). It does not add routine approval gates between implementation steps.
