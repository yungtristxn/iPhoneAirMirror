# Verified transient pairing candidate

Authorized 2026-10-22 to investigate and implement a newer verified pairing path
for the remaining selection delay. Branch: codex/verified-pairing. The accepted
F92829... receiver remains unchanged in dist/AirMirrorLite-ffmpeg-candidate/ and
is also copied with its manifest to dist/AirMirrorLite-verified-legacy-fallback/.
No Computer Use, audible tests, Windows security changes or uploads.

## Protocol boundary

The opt-in hap-pairing-experiment feature advertises CoreUtils pairing bit48 and
clears legacy pairing bit27. It requires transient SRP-6a setup M1-M4 with both
proof directions, using RFC5054 group3072/SHA512 and the protocol's public
Pair-Setup/3939 credentials. This establishes an authenticated ephemeral key
exchange; it is not a persistent identity allowlist or a user-secret PIN policy.
No unpaired or failed-pairing fallback is allowed. The experimental service is
named AirMirrorLite HAP so cached legacy advertisements cannot be mistaken for it.

M4 is fully written in plaintext before enabling authenticated control records.
Already-read next-record bytes survive the upgrade. Records are bounded to1024
plaintext bytes, with2-byte LE length as AAD,16-byte tag and independent64-bit
nonces per direction. Event traffic uses its own context and reversed key labels.
Invalid lengths, tags, counter exhaustion or state transitions fail closed.

Screen data uses per-stream HKDF-SHA512 and ChaCha20-Poly1305, authenticating the
original128-byte header. Configuration records remain plaintext under the wire
protocol and do not advance the screen nonce. Audio uses an explicitly supplied
32-byte key from encrypted SETUP, or the per-stream derived key; RTP timestamp and
SSRC are AAD, with transmitted8-byte nonce. Retransmission wrappers are removed
before authentication. Existing bounded queues, AAC worker, NTP clock and native
GPU/WASAPI playback remain the media path. Never choose a different cipher after
an authentication failure. Actual iPhone SETUP/media compatibility remains pending.

## Crypto and source provenance

Production primitives are pinned crates, not copied application cryptography:
- RustCrypto srp0.7.0-rc.3, source d40665b4ead69ad5e7933413a5158672366b8b9f,
  MIT/Apache-2.0; registry SHA2564b5f622c2d21b826b3501f4c620ccc4696e4a09a6b51727fcb21036dec87c101.
  This is a prerelease with no independent audit claim. The adapter performs no
  SRP arithmetic; it corrects the documented HAP encoding selection and uses the
  upstream M2 primitive with full-width A only after client-proof verification.
- sha2 0.11.1 with zeroize for the SRP digest API; existing sha2 0.10.9 remains
  the HKDF-compatible digest. hkdf0.12.4 and chacha20poly1305 0.10.1 provide key
  derivation and authenticated encryption. Cargo.lock pins the transitive graph.
- Owned secrets and encrypted-record buffers use zeroizing storage. Upstream
  SRP temporary big integers/private verifier vectors do not expose erasure;
  they are short lived, and no complete memory-erasure claim is made.

Primary interoperability references (wire facts only; no app code imported):
- [Shairport transient setup and framing](https://github.com/mikebrady/shairport-sync/blob/b972e900d9e68d3cb29745c7e46cf9d74abb6260/pair_ap/pair_homekit.c).
- [M4 upgrade boundary](https://github.com/mikebrady/shairport-sync/blob/b972e900d9e68d3cb29745c7e46cf9d74abb6260/rtsp.c#L1862).
- [Receiver screen authentication](https://github.com/45clouds/WirelessCarPlay/blob/51145ef55f8dd9f1cbadd58353cacb5e0ca215e9/source/Sources/AirPlayReceiverSessionScreen.c#L520).
- [Audio key/packet framing](https://github.com/45clouds/WirelessCarPlay/blob/51145ef55f8dd9f1cbadd58353cacb5e0ca215e9/source/Sources/AirPlayReceiverSession.c#L2445).
- [Independent event direction mapping](https://github.com/omarroth/doubletake/blob/5e2f1cbe3e1070bcefec8e549c7baf884727bc00/internal/airplay/receiver_media.go#L489).
- [Modern iOS feature mapping](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/AirPlaySender.framework/AirPlaySender/AirPlaySender.mm#L8074).

Apple HomeKit ADK public SRP vectors are pinned to
fb201f98f5fdc7fef6a455054f08b59cca5d1ec8 (Apache-2.0) in
tests/fixtures/hap-srp-vector.json with origin metadata. Additional derived vectors
are explicitly labeled. They exercise leading-zero A/B/S and HAP's differing
padding rules for u, K, M1 and M2. Stock pyatv transient helpers are not used as
the proof oracle: its client ignores M4, and its test-server helper upgrades even
after failed M3. The independent Python checker explicitly verifies both proofs.

## Current validation

139 Rust tests, formatting, strict Clippy and the candidate release build pass
in both HAP and legacy modes.
This includes official/independent crypto answers, parser/state/length rejection,
fragmented and coalesced control records, tampered tags/AAD, replay and counter
overflow, partial I/O/deadlines, encrypted event ACK/failure retention, and screen/
audio authentication primitives. These are not phone or audible A/V acceptance.
Independent wire checks and silent native checks pass the hardened candidate:
SHA-256 2FF3927A51B69C9D91875AF1D654352FB1EF1734F7B36A05CABCA0960E9C70A0.
Wire evidence: local/hap-pairing/20261022T202500438422Z/result.json; native
evidence: local/hap-verification/media-result.json (190 GPU surfaces,192 validated
access units, zero submitted audio). These separate checks do not yet establish
a real phone's encrypted media negotiation or faster phone tap-to-picture time.
The expanded independent checker also passes encrypted AAC-ELD and AAC-LC SETUP,
explicit32-byte and derived stream-key selection, actual isolated codec-worker
readiness, partial audio teardown/reconfiguration and invalid/missing key rejection.
Native UDP tables verify transport creation/removal. It sends no RTP or PCM and
requires both audio output counters to stay zero. Evidence:
local/hap-pairing/20261022T203352735374Z/result.json.
Review fixed single-datagram HAP
audio denial of service: invalid UDP authentication now leaves port admission,
queue, clock and liveness untouched and logs only its first rejection. A valid
following packet remains eligible. Request bodies, parser compaction buffers and
the authenticated SETUP plist's byte fields are also wiped when no longer needed;
this does not claim erasure of every allocator/library/register copy.

## Runnable phone trial

Implementation commit6438855 is deployed separately at
dist/AirMirrorLite-hap-candidate/AirMirrorLite.exe. It is open with --mute;
listener/discovery and muted=1 are verified. A bounded DNS-SD query resolves
AirMirrorLite HAP to the expected host and selected LAN IPv4. Launch evidence and
candidate manifest are in local/hap-verification/.

Pending human check: select AirMirrorLite HAP from a silent Settings screen and
report whether picture appears and selection-to-picture wait is shorter. No
phone connection has reached this candidate yet. Do not promote it over the
working legacy receiver or claim live screen/audio compatibility or speedup from
the automated checks. The full working F92829... build and prior friend ZIP stay
unchanged; no new candidate has been uploaded or shared.

Build with CARGO_HOME=work/cargo:
```
cargo test --locked --offline --features ffmpeg-aac,hap-pairing-experiment --target-dir work/hap-receiver-target
cargo clippy --all-targets --locked --offline --features ffmpeg-aac,hap-pairing-experiment --target-dir work/hap-receiver-target -- -D warnings
cargo build --release --locked --offline --features ffmpeg-aac,hap-pairing-experiment --target-dir work/hap-receiver-target
python -B scripts/Test-HapPairing.py --exe work/hap-receiver-target/release/airmirror-lite.exe
./scripts/Test-Media.ps1 -Executable work/hap-receiver-target/release/airmirror-lite.exe
```
