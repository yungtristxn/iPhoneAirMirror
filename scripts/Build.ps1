param([switch]$Debug, [switch]$Test, [switch]$Fetch)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($env:CARGO_BUILD_TARGET -and $env:CARGO_BUILD_TARGET -ne 'x86_64-pc-windows-msvc') { throw 'Only x86_64-pc-windows-msvc is supported' }
$previousCargoHome = $env:CARGO_HOME
Push-Location -LiteralPath $root
try {
    $env:CARGO_HOME = Join-Path $root 'work/cargo'
    if ($Fetch) { cargo fetch --locked; if ($LASTEXITCODE) { throw 'Dependency fetch failed' } }
    cargo fmt --check
    if ($LASTEXITCODE) { throw 'Formatting check failed' }
    if ($Test) { cargo test --locked --offline; if ($LASTEXITCODE) { throw 'Tests failed' } }
    $buildArguments=@('build','--locked','--offline','--message-format=json-render-diagnostics')
    if(-not $Debug){$buildArguments+='--release'}
    $buildOutput=@(cargo @buildArguments);$buildExitCode=$LASTEXITCODE
    $events=@($buildOutput | ForEach-Object {
        $event=$_ | ConvertFrom-Json
        if($event.reason -eq 'compiler-message' -and $event.message.rendered){Write-Host $event.message.rendered}
        $event
    })
    if ($buildExitCode) { throw 'Build failed' }
    $artifacts=@($events | Where-Object {$_.reason -eq 'compiler-artifact' -and $_.target.name -eq 'airmirror-lite' -and $_.executable})
    if($artifacts.Count -ne 1){throw 'Cargo did not report exactly one receiver executable'}
    & (Join-Path $PSScriptRoot 'Initialize-Worker.ps1') -Executable $artifacts[0].executable
} finally { $env:CARGO_HOME = $previousCargoHome; Pop-Location }
