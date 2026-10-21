$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$binary = Join-Path $root 'target/release/airmirror-lite.exe'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Build release first with scripts/Build.ps1 -Test' }
$dist = Join-Path $root 'dist/AirMirrorLite'
New-Item -ItemType Directory -Force -Path $dist,(Join-Path $dist 'licenses') | Out-Null
$destination = Join-Path $dist 'AirMirrorLite.exe'
# Permit metadata/document refresh while the identical verified binary is running.
if (-not (Test-Path -LiteralPath $destination) -or (Get-FileHash -LiteralPath $binary).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) {
    Copy-Item -LiteralPath $binary -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $root 'vendor/playfair/LICENSE.md') -Destination (Join-Path $dist 'licenses/PlayFair-GPLv3.md')
Copy-Item -LiteralPath (Join-Path $root 'vendor/fdk-aac/NOTICE') -Destination (Join-Path $dist 'licenses/FDK-AAC.txt')
Copy-Item -LiteralPath (Join-Path $root 'docs/THIRD_PARTY.md') -Destination (Join-Path $dist 'licenses/THIRD_PARTY.md')
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination (Join-Path $dist 'README.md')
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $dist 'AirMirrorLite.exe')
$commit = git -C $root rev-parse HEAD
@{
    executable = 'AirMirrorLite.exe'; sha256 = $hash.Hash; sourceBaseCommit = $commit
    sourceDirty = [bool](git -C $root status --porcelain)
    builtUtc = [DateTime]::UtcNow.ToString('o'); purpose = 'Local development / device acceptance only'
    target = 'x86_64-pc-windows-msvc'; rust = (rustc --version)
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dist 'BUILD.json') -Encoding utf8
Write-Output $hash
