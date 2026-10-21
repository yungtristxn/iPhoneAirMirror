$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# Ask Cargo to prove freshness before assigning source provenance. An old binary
# must never be relabelled with the current clean commit. Read Cargo's artifact
# path instead of assuming that a caller's target configuration is the default.
$previousCargoHome = $env:CARGO_HOME
Push-Location -LiteralPath $root
try {
    $env:CARGO_HOME = Join-Path $root 'work/cargo'
    $buildOutput = @(cargo build --release --locked --offline --target-dir (Join-Path $root 'target') --message-format=json-render-diagnostics)
    $buildExitCode = $LASTEXITCODE
    $events = @($buildOutput | ForEach-Object {
        $event = $_ | ConvertFrom-Json
        if ($event.reason -eq 'compiler-message' -and $event.message.rendered) { Write-Host $event.message.rendered }
        $event
    })
    if ($buildExitCode) { throw 'Package release build failed' }
    $artifacts = @($events | Where-Object {
        $_.reason -eq 'compiler-artifact' -and $_.target.name -eq 'airmirror-lite' -and $_.executable
    })
    if ($artifacts.Count -ne 1) { throw 'Cargo did not report exactly one receiver executable' }
    $binary = $artifacts[0].executable
    $compiler = rustc --version
    if ($LASTEXITCODE) { throw 'Compiler version lookup failed' }
} finally {
    $env:CARGO_HOME = $previousCargoHome
    Pop-Location
}
$commit = git -C $root rev-parse HEAD
if ($LASTEXITCODE) { throw 'Source commit lookup failed' }
$sourceStatus = @(git -C $root status --porcelain)
if ($LASTEXITCODE) { throw 'Source status lookup failed' }
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
@{
    executable = 'AirMirrorLite.exe'; sha256 = $hash.Hash; sourceBaseCommit = $commit
    sourceDirty = ($sourceStatus.Count -ne 0)
    packagedUtc = [DateTime]::UtcNow.ToString('o'); purpose = 'Local development / device acceptance only'
    buildVerification = 'cargo build --release --locked --offline (freshness checked before copy)'
    target = 'x86_64-pc-windows-msvc'; rust = $compiler
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dist 'BUILD.json') -Encoding utf8
Write-Output $hash
