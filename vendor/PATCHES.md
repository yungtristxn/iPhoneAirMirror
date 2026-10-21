# Local vendor patches

## PlayFair unsigned byte assembly (2026-10-21)

Base: UxPlay `15a6b87172eca38b0df4e615fbf1bba108734dde`,
`lib/playfair/modified_md5.c`.
Original SHA-256: `850c815fd99c9f8459e2ff323c25963e20873328b838cda33fff88400fba2667`.

The four input bytes are cast to `uint32_t` before shifting. This removes signed
`int` left-shift undefined behavior for a high first byte while preserving the
intended 32-bit word and wire results. No protocol or algorithm change is intended.
The current patched checksum is recorded in `vendor/SHA256SUMS`.

`scripts/Test-PlayfairUB.ps1` runs Clang's shift instrumentation in trap mode,
requires a deliberately invalid signed-shift control to fail, then checks 20
public expected keys and high-bit input bytes. It is separate from AddressSanitizer
and does not claim an exhaustive audit of upstream aliasing or arithmetic.
