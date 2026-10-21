# Silent native regressions

Both checks consume only public/synthetic fixtures and never open WASAPI,
the network, or a window. Neither is whole-program or exhaustive fuzz coverage.

AddressSanitizer (installed MSVC ASan component required):

```powershell
$env:CARGO_HOME = Join-Path $PWD 'work/cargo'
$env:CARGO_TARGET_DIR = Join-Path $PWD 'work/san-target'
cargo run --manifest-path tests/native-sanitizer/Cargo.toml --locked --offline
Remove-Item Env:CARGO_TARGET_DIR
```

This checks FairPlay, AAC-ELD/AAC-LC decoding, malformed input, sample canaries,
both pinned PCM digests and independently specified sine/triangle frequency,
amplitude and channel controls. The digests use the same pinned decoder, not an
independent decoder implementation. The AAC encoder is development-only and is
not linked into the receiver.

Shift undefined behavior is a separate check; ASan does not establish it:

```powershell
./scripts/Test-PlayfairUB.ps1 -Compiler <absolute-path-to-clang.exe>
```

The runner requires a known invalid shift to terminate with the expected Windows
illegal-instruction trap, then checks 20 independently published expected keys
and explicit high-bit byte assembly under `-fsanitize=shift -fsanitize-trap=shift`.
The x64 MSVC headers/libraries must be available to Clang.

The verified local tool was portable Clang 21.1.8 from the official LLVM asset:
https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/LLVM-21.1.8-win64.exe
SHA-256, checked against the official GitHub release asset digest:
`7a5386c26497db1691f320121e5b113364dd0274b98e55f15f4dbc00c0450113`.
It was extracted with 7-Zip under ignored `work/llvm-21.1.8`; the installer was
not executed and no machine-level compiler or service was installed. The default
runner path refers to that optional local extraction, not a bundled dependency.
