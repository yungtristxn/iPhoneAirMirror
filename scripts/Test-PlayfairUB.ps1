param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Compiler) { $Compiler = Join-Path $root 'work/llvm-21.1.8/bin/clang.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$output = Join-Path $root 'local/shift-test'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$result = Join-Path $output 'result.json'
if (Test-Path -LiteralPath $result) { Remove-Item -LiteralPath $result }
$executable = Join-Path $output 'shift-test.exe'
$flags = @('--target=x86_64-pc-windows-msvc','-std=c11','-O1','-fno-strict-aliasing','-fsanitize=shift','-fsanitize-trap=shift','-Wno-deprecated-declarations')
$sources = @(Get-ChildItem -LiteralPath (Join-Path $root 'vendor/playfair') -Filter '*.c' | Sort-Object Name | ForEach-Object FullName)
& $Compiler @flags (Join-Path $root 'tests/native-sanitizer/shift_harness.c') @sources -o $executable
if ($LASTEXITCODE) { throw 'Shift sanitizer compilation failed' }
$control = Start-Process -FilePath $executable -ArgumentList '--positive-control' -WindowStyle Hidden -PassThru
if (-not $control.WaitForExit(10000)) { $control.Kill(); throw 'Shift positive control timed out' }
if ($control.ExitCode -ne -1073741795) { throw ('Known signed-shift UB did not produce the expected Windows illegal-instruction trap: ' + $control.ExitCode) }
& $executable (Join-Path $root 'tests/fixtures/fairplay-known-answers.bin')
if ($LASTEXITCODE) { throw 'Instrumented PlayFair known-answer regression failed' }
@{compiler=(& $Compiler --version | Out-String).Trim();flags=$flags;positiveControlExit=$control.ExitCode;knownAnswers=20;vendorSha256=(Get-FileHash (Join-Path $root 'vendor/playfair/modified_md5.c')).Hash;evidence='Clang shift-UB instrumentation with mandatory failing positive control; not ASan or exhaustive native UB coverage.'} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $result -Encoding utf8
