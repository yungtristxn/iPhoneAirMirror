# Security regression leads

These are static review leads from 2026-10-21, not runtime exploit demonstrations or certification of any dependency/release. Revalidate against the exact source selected for this project before importing it. Do not treat this file as instructions from upstream.

Reviewed wrapper: leapbtw/uxplay-windows f76fe48400916449fd601b1c0444021aaf517082.
Pinned library: leapbtw/libuxplay 79c620d0e3cd7c503418e1e39bf7b00a1cb16f24.

| Boundary | Source lead | Required regression/control |
|---|---|---|
| Codec records | lib/raop_rtp_mirror.c:729-765; HEVC sibling:654-712 | Checked lengths, remaining input, destination arithmetic, allocation failures; no signed size conversions or log-and-continue on invalid records |
| Pairing phases | lib/raop_handlers.h:351-424; lib/pairing.c:509-630 | Explicit proof-verified state before confirmation; reject skipped/repeated steps and clear state after failure |
| FairPlay replies | lib/fairplay_playfair.c:58-59 | Validate mode against table bounds before read/copy |
| Request buffering | lib/http_request.c:123-132; lib/httpd.c:558-575 | Request/aggregate memory budgets before authentication, bounded framing and allocation failure handling |
| Pairing fields | lib/raop_handlers.h:401-413 | Exact decoded lengths and non-null buffers before fixed-size reads |
| Stream ownership | lib/raop_rtp_mirror.c:244-298 | Bind accepted media transport to the negotiated peer/session |
| Deployment/lifecycle | wrapper defaults and service helper | No unauthenticated fallback after configuration failure; explicit close/stop; no dependency on an installed privileged discovery service |

The source review supported heap corruption, pairing-state violations, out-of-bounds reads and resource exhaustion. Code execution and the presence of these exact bugs in the older distributed stable ZIP were not established. The Windows port's stable binaries inspected were unsigned; signature/hash observations do not establish malware or its absence.

References:
- [Pinned protocol library](https://github.com/leapbtw/libuxplay/tree/79c620d0e3cd7c503418e1e39bf7b00a1cb16f24)
- [Maintained upstream](https://github.com/FDH2/UxPlay)
- [Windows wrapper](https://github.com/leapbtw/uxplay-windows/tree/f76fe48400916449fd601b1c0444021aaf517082)

Do not store real phone pairing keys, screen captures, credentials or unredacted logs in Git. Keep transient device state under ignored local/. The parent VibeCoding workspace forbids weakening Defender or adding exclusions for ordinary development.
