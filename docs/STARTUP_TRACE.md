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

## Further research, 2026-10-23

No demonstrated small runtime fix was found. Keep both current candidates and
the fallback unchanged. The five-second estimate is not an established Apple
minimum, and the existing trace still does not locate the complete tap-to-visible
interval. Re-read the saved analysis and scalar log; no new capture or phone
interaction was needed.

The new lead is **persistent HAP pairing**, with a one-time code and remembered
controller keys. [shairplay-rust PR30](https://github.com/metaneutrons/shairplay-rust/pull/30)
reports iOS 26 buffered-audio startup improving from 5-11 seconds to about 0.5s
after several changes together: PTP reception, pairing-state flags, persistent
pairing and session handoff. Its merged implementation is
229c1beac8be1c990dae94960bae5ca465faf05b. This does not isolate the benefit of
pairing alone or prove a screen-mirroring improvement. Its
[status report](https://github.com/metaneutrons/shairplay-rust/blob/2fb72b30b658b33f106165a5b6fbd941919bdbe5/AP2-STATUS.md#L74-L167)
describes a different, unpaired legacy video path and unwired mirroring audio.

Applicability checked against current AirMirrorLite source:

| Lead | Finding / decision |
|---|---|
| Persistent HAP pairing | Plausible separate experiment. Current transient authentication is valid but does not remember trusted controller identities. Its observed exchange already takes 39-139 ms; any larger benefit would have to change sender behavior outside that exchange. |
| PTP port-unreachable delay | The other implementation uses PTP/buffered audio. We require NTP and do not advertise that mode. Adding a dummy PTP listener is not justified. |
| Pairing flags | Our HAP 0x004 agrees with their transient profile. Copying paired-device flags without the corresponding protocol/store would misrepresent support. |
| Stable identity and event endpoint | Already present: persisted receiver key, stable pi/deviceID, matching info fields and authenticated event connections in all five traced sessions. This is separate from persistent controller trust. |
| Previous-session overlap | Our session ownership and teardown already serialize active media. The trace supplies no corresponding startup failure. |

[UxPlay's documented shortcut](https://github.com/FDH2/UxPlay/blob/0477d471cc5899ea13382b5871cf70dbfe050ebd/README.md#L2184-L2194)
skips legacy pairing. Our HAP candidate already clears bit 27 while retaining
verified authentication, without a reported speedup. No authentication bypass or
unsupported capability change is warranted. The same
[README](https://github.com/FDH2/UxPlay/blob/0477d471cc5899ea13382b5871cf70dbfe050ebd/README.md#L2209-L2214)
reports operation with two advertised AppleTV versions, without establishing a
startup advantage from changing model/sourceVersion.

[Apple's deployment guide](https://support.apple.com/en-ae/guide/deployment/dep9151c4ace/web)
describes multiple discovery paths and preference for responsive/recently used
connections. It does not specify a mandatory five-second startup delay. Sender
path selection remains a hypothesis, not a diagnosis from our IPv4-only trace.
There is still no evidence justifying a Windows DNS replacement or a network
settings change.

Recommendation: retain the current behavior. If startup becomes a priority again,
evaluate persistent HAP pairing as a separate candidate, including real
pair-setup/pair-verify and protected, removable controller credentials. Compare
repeat tap-to-visible timings, not just handshake duration, before claiming a
benefit. Do not expand into BLE, PTP or remote control for this hypothesis.

Validation of this research checkpoint: source/primary-reference comparison,
saved-trace review, git diff --check and SHA256 comparison of the C10FEA... HAP,
2FF3927... phone-checked HAP, and both F92829... legacy copies. All executable
hashes remain as recorded above/in STATE.md. No implementation change, new build,
runtime test, sound output, Computer Use or system setting change.
