# Imported components

Local development build; no publishing or redistribution has been performed.
The original project code is not yet assigned a distribution license. In particular,
the GPL PlayFair component and the FDK AAC license need a compatible distribution
arrangement before this combined executable is distributed. This checkpoint does
not claim distribution license compatibility.

| Component | Exact source | Use / license |
|---|---|---|
| PlayFair | https://github.com/FDH2/UxPlay/tree/15a6b87172eca38b0df4e615fbf1bba108734dde/lib/playfair | Five C files, headers and lookup tables only; GPLv3 text in `vendor/playfair/LICENSE.md`. No UxPlay server, HTTP parser, plist parser, renderer or app imported. |
| FairPlay reply table | `lib/fairplay_playfair.c` at the same UxPlay revision | 568 fixed reply bytes, extracted into `vendor/playfair/replies.bin`; source file header LGPL-2.1-or-later, copyright Juho Vaha-Herttua. Runtime bounds checked in Rust. |
| FDK AAC | https://github.com/mstorsjo/fdk-aac/tree/c41c3c7516ea8d0d78a53b96b30bed3a29745cc6 | Decoder and required common directories from `cpp/`; Fraunhofer FDK AAC license in `vendor/fdk-aac/NOTICE`. No encoder, executable, network library or general media framework in the receiver build. |
| RustCrypto AES/CBC/CTR/SHA-512 | aes 0.8.4, cbc 0.1.2, ctr 0.9.2, sha2 0.10.9 | Maintained cryptographic primitives; MIT/Apache-2.0. |
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

The imported upstream source files are unmodified. The extracted reply table and
its notice are separate files. Build selection is in the project's build.rs. Component source hashes are
recorded in vendor/SHA256SUMS. Never store device keys or screen/audio captures here.

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
