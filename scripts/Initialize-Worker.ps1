param([Parameter(Mandatory=$true)][string]$Executable)
# Explicit development/package preparation. Normal receiver startup never edits
# an executable's ACL. This operation grants only the receiver's worker identity.
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$root = Split-Path -Parent $PSScriptRoot
$folder = Join-Path $root 'local/worker-provision'
New-Item -ItemType Directory -Force -Path $folder | Out-Null
$process = Start-Process -FilePath $Executable -ArgumentList '--provision-worker' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $folder 'stdout.txt') -RedirectStandardError (Join-Path $folder 'stderr.txt')
if (-not $process.WaitForExit(10000)) { $process.Kill(); throw 'Explicit worker provisioning timed out' }
if ($process.ExitCode -ne 0) { throw ('Explicit worker provisioning failed: ' + (Get-Content -Raw (Join-Path $folder 'stderr.txt'))) }
