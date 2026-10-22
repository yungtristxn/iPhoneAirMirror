# Imported components

Original project code and artwork are GPL-3.0-only; see LICENSE.md and NOTICE.md.
The user authorized a local ZIP for a friend on 2026-10-22. That test archive uses
only the FFmpeg candidate and includes matching application, FFmpeg and Rust
dependency sources, build instructions and notices. No public upload is involved.

The older/default executable statically combines GPL PlayFair with FDK AAC. The
[GNU license list](https://www.gnu.org/licenses/license-list.html#fdk) classifies
the FDK AAC license as incompatible with every GNU GPL version. Selecting a
project license or copying notices does not resolve that combination. No compatible
exception or alternative grant has been established for this build. Distribution
requires verified compatible permissions or a separately assessed component/build
arrangement and fulfillment of its obligations; the same executable's worker mode
does not separate the statically linked components. Local development packaging
is not a release authorization. FDK also supplies no patent-license grant; this
document does not determine patent rights or provide legal clearance.

| Component | Exact source | Use / license |
|---|---|---|
| PlayFair | https://github.com/FDH2/UxPlay/tree/15a6b87172eca38b0df4e615fbf1bba108734dde/lib/playfair | Five C files, headers and lookup tables only; GPLv3 text in `vendor/playfair/LICENSE.md`. No UxPlay server, HTTP parser, plist parser, renderer or app imported. |
| FairPlay reply table | `lib/fairplay_playfair.c` at the same UxPlay revision | 568 fixed reply bytes, extracted into `vendor/playfair/replies.bin`; source file header LGPL-2.1-or-later, copyright Juho Vaha-Herttua. Runtime bounds checked in Rust. |
| FDK AAC | https://github.com/mstorsjo/fdk-aac/tree/c41c3c7516ea8d0d78a53b96b30bed3a29745cc6 | Decoder and required common directories from `cpp/`; Fraunhofer FDK AAC license in `vendor/fdk-aac/NOTICE`. No encoder, executable, network library or general media framework in the receiver build. |
| FFmpeg (optional candidate) | https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz ; tag `n9.0.2`, commit `946fcce07b6dcd0331c8cc609192aeff5e1924f8` | Native AAC plus supporting avcodec/avutil, LGPL-2.1-or-later/per-file notices; no nonfree/GPL options, external codecs, network, media programs or video codecs. Source/tool hashes and signature fingerprint are in `scripts/ffmpeg-pins.json`. This target does not compile/link FDK. |
| RustCrypto AES/CBC/CTR/SHA-512 | aes 0.8.4, cbc 0.1.2, ctr 0.9.2, sha2 0.10.9 | Maintained cryptographic primitives; MIT/Apache-2.0. |
| RustCrypto HAP primitives | srp 0.7.0-rc.3, sha2 0.11.1, hkdf 0.12.4, chacha20poly1305 0.10.1 | Verified transient SRP/key derivation/AEAD; MIT/Apache-2.0. SRP is a prerelease with no independent audit claim. Exact source pin, HAP padding adapter and zeroization limits are recorded in VERIFIED_PAIRING.md. |
| Apple HomeKit ADK public test vectors | fb201f98f5fdc7fef6a455054f08b59cca5d1ec8, Tests/HAPCryptoTest.c | Public SRP known answers only; Apache-2.0. Source/license URLs and independently derived edge vectors are distinguished in tests/fixtures/hap-srp-vector.origin.json. No Apple implementation imported. |
| Dalek | ed25519-dalek 2.2.0, x25519-dalek 2.0.1 | Signatures/key agreement; BSD-3-Clause. |
| plist | 1.8.0 | Binary-plist reader/writer; MIT. A bounded graph validator precedes reads. |
| rand / zeroize / anyhow / cc | Versions pinned in Cargo.toml; full transitive graph in Cargo.lock | Random identity, key lifetime, error reporting, native compilation. See each crate's packaged license. |

Interoperability references: UxPlay files `raop_handlers.h`, `raop_ntp.c`,
`raop_rtp.c`, `raop_rtp_mirror.c`, `mirror_buffer.c`, `pairing.c` and
`renderers/audio_renderer.c` at the revision above. `rairplay` 1.0.0 was inspected
as a reference and was **not** imported as a dependency. Its permissive mode-4
index and unchecked native key-length boundary were unsuitable for this host.

Native Windows functions are implemented against the installed Windows SDK:
DnsServiceRegister, Win32 windows, Media Foundation, D3D11 video processing,
WASAPI, DPAPI and AppContainer process creation. No Bonjour service is installed.

Imported upstream sources retain their pinned origins. The single documented
local PlayFair byte-assembly correction is recorded in `vendor/PATCHES.md`;
`vendor/SHA256SUMS` records current source hashes. The extracted reply table and
its notice are separate files. Build selection is in the project's build.rs.
Never store device keys or screen/audio captures here.

The FFmpeg feature supports a receiver candidate; `decoder-lab` separately
enforces decode-only execution. The dedicated friend-test packaging path includes
the source and license materials rather than removing the default package guard.
The accepted FDK package and its distribution gate remain unchanged. The test
archive is not physical A/V acceptance, a stable release, or a determination of
patent rights. Remaining device checks are described in DECODER_EXPERIMENT.md.

## Implementation choices verified locally

The host uses a bounded C ABI to a Windows SDK C++ bridge for COM/media and OS
security APIs rather than the initially preferred Rust `windows` bindings.
Packet, plist, pairing-state and codec-record parsing remain in Rust. The bridge
uses native MF/D3D11/WASAPI, not a general media-player framework. The codec/key
worker is the same executable in a capability-free AppContainer; launch failure
has no unrestricted fallback. A kill-on-close job limits it to one process and
256 MiB. Host-file denial and nondelivery of a worker UDP probe were tested.

One GPU-to-GPU NV12 copy per presented frame retains the paused picture for resize
without holding a Media Foundation decoder-pool surface. An earlier retained
decoder-surface implementation stalled at rotation; the private GPU copy avoids
that ownership conflict. There is no CPU readback or video re-encoding in reception.

## High-resolution video and app icon

HEVC uses the already-installed Windows HEVC Video Extension through Media
Foundation, with D3D11 hardware decode. No HEVC codec binaries or upstream HEVC
parser sources are redistributed. The hvc1/hvcC layout was checked against the
pinned UxPlay interoperability reference above and implemented with explicit
bounds in Rust. The app icon is original project artwork; its multi-size ICO is
reproducible using scripts/Generate-Icon.py (Pillow is development-only).
