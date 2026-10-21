# Current state

Updated: 2026-10-21

## Working implementation
- Branch `feat/airplay-mvp`; implementation `e82528a`, connection/CPU fixes `c14d37f`.
  Local commits only; no push or publication. Working tree was clean at this checkpoint.
- Run `dist/AirMirrorLite/AirMirrorLite.exe`. Current executable SHA-256:
  `345A6C9E448C732320B02911543A1BA41C44A2F568A4C68594469365EAB5746F`.
- Real iPhone discovery, legacy pairing/FairPlay, H.264 hardware video and AAC-ELD
  44.1 kHz stereo audio work. User confirmed picture/sound, pause/resume, then
  a five-minute run with synchronized sound and no freezes. User explicitly
  shortened the original 30-minute target; do not claim 30-minute acceptance.
- Native DNS-SD on the private physical IPv4 LAN, MF/D3D11 display, WASAPI audio,
  separate UI thread, bounded transport/queues, persistent DPAPI identity.
  F11 fullscreen, D disconnect, close exits. No software/video-only fallback.
- AAC/FairPlay run in a zero-capability AppContainer with one-process/256 MiB job
  limits and bounded private IPC. Four app firewall rules are verified Private /
  LocalSubnet / edge traversal Block. No services, exclusions or Defender changes.

## Verification
- 26 Rust tests, `cargo clippy --all-targets --locked --offline -- -D warnings`,
  release build and real loopback control/single-owner/shutdown checks pass.
- Native synthetic regression covers landscape/portrait hardware decoding,
  paused-picture redraw, resize/fullscreen and AAC-ELD through WASAPI. AppContainer
  host-file read denied; worker UDP probe not delivered. These are separate from
  physical iPhone evidence.
- Native ASan: FairPlay known answer + 1,024 fixed-size valid-mode mutations;
  120 AAC-ELD fixture frames + 1,024 malformed packets; PCM canaries intact.
  All 263 pinned vendor file hashes verified. See VALIDATION.md / THIRD_PARTY.md.
- Live five-minute sample: 498x1080 video, 59.30 decoded / 57.76 presented fps,
  13,230,000 audio samples. RTX 4070 SUPER, driver 32.0.16.1692; Ryzen 7 7800X3D.
  Per-process Windows Video Decode engine counters measured 1.89–1.93%.
- Host private memory 89.81→89.91 MiB; working set 76.51–79.45 MiB; handles 711→708.
  Codec private memory 2.33→2.40 MiB and handles 153→153. No disconnect in the sample.
- That sample exposed two busy CPU cores from inherited nonblocking Winsock
  connections. The current package explicitly restores blocking reads with 100 ms
  cancellation/deadline checks. Regression passes; a real idle control connection
  measured 0.0% of one core over ten seconds. Post-fix live CPU check awaits reconnect.
- Current package also preserves signed/unsigned encodings of all 64 stream-ID
  bits, fixing intermittent negotiation rejection. Numeric lengths/ports stay strict.
- Live close completed in 177 ms with no receiver/worker, TCP listener, UDP endpoint
  or mDNS service response remaining; clean shutdown log and empty stderr. Exit code
  was unavailable from the reopened process handle. The receiver was restarted.

## Remaining evidence / limits
- Brief live CPU measurement of the newest package is pending; the five-minute
  media run used the preceding package before the socket/stream-ID corrections.
- Physical lock/unlock and Wi-Fi-loss recovery, 30-minute drift, and measured
  glass-to-glass latency remain unverified. The 120 ms playout setting is not an
  end-to-end latency measurement.
- Local development build; distribution licensing is unresolved. MSVC x64 runtime
  is required. No phone keys, captured media or private logs are committed.
- Build commands: scripts/Build.ps1 -Test, Test-Media.ps1, Test-Control.ps1.
  Sandbox MSVC archive update failed LNK1114/error 5; offline builds outside that
  sandbox passed. The native ASan command and detailed bounds are in VALIDATION.md.
