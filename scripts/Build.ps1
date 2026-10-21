param([switch]$Debug, [switch]$Test, [switch]$Fetch)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $root
try {
    $env:CARGO_HOME = Join-Path $root 'work/cargo'
    if ($Fetch) { cargo fetch --locked; if ($LASTEXITCODE) { throw 'Dependency fetch failed' } }
    cargo fmt --check
    if ($LASTEXITCODE) { throw 'Formatting check failed' }
    if ($Test) { cargo test --locked --offline; if ($LASTEXITCODE) { throw 'Tests failed' } }
    if ($Debug) { cargo build --locked --offline } else { cargo build --release --locked --offline }
    if ($LASTEXITCODE) { throw 'Build failed' }
} finally { Pop-Location }
