# HAP startup follow-up: no demonstrated five-second fix

2026-10-22: the user confirms the HAP picture works but still estimates around
five seconds from selecting the receiver to seeing the PC picture. The working
legacy F92829... receiver remains preserved. Do not claim HAP removed that wait.

## Actual phone and connection metadata

HAP build2FF3927... completed verified transient M1/M4, encrypted control, an
authenticated event connection and actual HEVC994x2160 presentation. A bounded
90-second TShark observation then collected only scoped IPv4 mDNS metadata and
zero-data TCP7000 SYN/FIN/RST flags. No screen/audio/RTSP payload, TXT value, raw
capture file, system setting change or Computer Use was involved.

| Reconnect | SYN -> SYN/ACK | Accepted TCP -> first DXGI Present | Video SETUP -> accepted video TCP |
|---|---:|---:|---:|
| 1 | 0.185 ms | 1191 ms | 1009 ms |
| 2 | 0.193 ms | 270 ms | 5 ms |
| 3 | 0.124 ms | 211 ms | 6 ms |
| 4 | 0.144 ms | 177 ms | 15 ms |
| 5 | 0.111 ms | 178 ms | 5 ms |

HAP exchange intervals were44/139/43/39/39 ms. No multi-second control-SYN retry
is visible in this scope. The first attempt spent about one second waiting for
the phone's separate video connection; its SYN packets were outside this trace,
so do not label that interval network loss or a receiver polling delay.

Evidence: local/connection-metadata/20261022T205333254404Z/ contains sanitized
metadata.jsonl, summary.json, receiver-scalar-log.txt and analysis.json. The
wall/performance-counter offset changed by only0.00024 ms during the observation.
Neither capture nor receiver logs timestamp the user's tap or physical scanout.
First DXGI Present alone also does not prove that the frame already contained
visible phone UI. Therefore these figures cannot be subtracted from the user's
estimate to attribute the remainder to a specific iOS/DNS/UI component.

## DNS limits and diagnostic correction

The first trace contains cached known answers and no evidence establishing a
missing-AAAA timeout. It is IPv4-only and suppresses unrelated DNS names. Its
QR/QU/cache-flush Boolean fields were incorrectly treated as numbers; null in that
trace means unavailable. The old files were not reinterpreted. The utility now
parses TShark True/False values and seven pure fixture tests cover sanitization,
Boolean fields, scope validation and zero-payload TCP filtering. It requires
explicit RFC1918 PC/phone IPs, observed NPF interface and receiver hostname; no
user addresses or interface identifiers are embedded in tracked source.

The older ANY query from an ephemeral port is a legacy-unicast probe, not a test
of every normal mDNS address-resolution path. Native DnsServiceRegister's selected
interface does not by itself prove how Windows answers independent hostname
A/AAAA/NSEC questions. Those are investigation boundaries, not evidence of a
Windows bug. [RFC6762 responding/negative answers](https://www.rfc-editor.org/rfc/rfc6762.html#section-6)
and [Windows registration scope](https://learn.microsoft.com/en-us/windows/win32/api/windns/ns-windns-dns_service_register_request).

## Small presentation correction

Source review found that normal initial video Present was counted before attaching
its swap chain to DirectComposition. The next event-driven tick performed that
attachment. There is no five-second stats dependency: video readiness wakes the
loop immediately and chrome retries use a5 ms deadline.

Successful Present now attaches/commits video before incrementing the counter;
unchanged ongoing attachment is a no-op. Busy/failed Present still reveals no
candidate, and black-screen resume preserves its behavior. Focused intrinsic
tests assert attachment in that same draw/dispatch, before another tick. The
silent native suite passes190 GPU surfaces/192 validated units and zero audio
samples on the corrected build:
`C10FEA1AB69E58C1F2477062580CE652D2525878E5C0C461DC6B6EDB41D34E76`.
Evidence: local/hap-verification/first-frame-attachment/. The139 Rust/HAP/legacy
and independent wire checks passed before this graphics-only correction.

This removes one dispatch gap, not a demonstrated five-second delay. A successful
[DirectComposition commit](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-waitforcommitcompletion)
is also distinct from physical display completion. No new speedup claim or
speculative pairing/DNS downgrade is made. The larger reported delay remains
unresolved; actual HAP audio and audible A/V sync are still unverified.
