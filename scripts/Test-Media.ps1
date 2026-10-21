param([string]$Executable = '', [string]$UiRenderDirectory = '', [switch]$Audio)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $root 'target/release/airmirror-lite.exe' }
$data = Join-Path $root 'local/media-test'
New-Item -ItemType Directory -Force -Path $data | Out-Null
$arguments = @($(if ($Audio) { '--self-test' } else { '--ui-test' }))
if ($UiRenderDirectory) { $arguments += @('--ui-test-dir', ('"' + $UiRenderDirectory + '"')) }
$process = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $data 'stdout.txt') -RedirectStandardError (Join-Path $data 'stderr.txt')
if (-not $process.WaitForExit(45000)) { Stop-Process -Id $process.Id; throw 'Media test exceeded 45 seconds' }
Get-Content (Join-Path $data 'stdout.txt'),(Join-Path $data 'stderr.txt')
if ($process.ExitCode -ne 0) { throw ('Media probe failed: ' + $process.ExitCode) }
@{timeUtc=[DateTime]::UtcNow.ToString('o');executable=$Executable;sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $Executable).Hash;exitCode=$process.ExitCode;audioTest=[bool]$Audio;evidence=$(if($Audio){'Synthetic H.264/AAC + native GPU/WASAPI + isolated worker. No iPhone acceptance.'}else{'Silent native UI and synthetic H.264/GPU checks; no PCM submitted. No phone or sound acceptance.'})} | ConvertTo-Json | Set-Content (Join-Path $data 'result.json')
