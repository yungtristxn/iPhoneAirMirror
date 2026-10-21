param([Parameter(Mandatory=$true)][string]$Prefix)
$ErrorActionPreference='Stop'
$Prefix=(Resolve-Path -LiteralPath $Prefix).Path
$pins=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'ffmpeg-pins.json') -Raw | ConvertFrom-Json
$manifest=Get-Content -LiteralPath (Join-Path $Prefix 'airmirror-ffmpeg.json') -Raw | ConvertFrom-Json
if($manifest.schemaVersion -ne 1 -or $manifest.source.version -ne $pins.source.version -or
   $manifest.source.sha256 -ne $pins.source.sha256 -or $manifest.source.tag -ne $pins.source.tag -or
   $manifest.source.commit -ne $pins.source.commit -or $manifest.source.signingFingerprint -ne $pins.source.signingFingerprint -or
   $manifest.target -ne 'x86_64-pc-windows-msvc' -or $manifest.runtime -ne 'MD') {
    throw 'Unexpected FFmpeg source, target or runtime; rebuild the pinned experiment dependency'
}
foreach($library in @('avcodec','avutil')) {
    $entry=$manifest.libraries.$library
    if($entry.file -ne "lib/$library.lib"){throw "Unexpected library path: $library"}
    $path=Join-Path $Prefix $entry.file
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256 -or
       (Get-Item -LiteralPath $path).Length -ne $entry.size){throw "FFmpeg library verification failed: $library"}
}
if((Get-FileHash -LiteralPath (Join-Path $Prefix 'config/config.h')).Hash -ne $manifest.configuration.configSha256 -or
   (Get-FileHash -LiteralPath (Join-Path $Prefix 'config/config_components.h')).Hash -ne $manifest.configuration.componentsSha256){
    throw 'FFmpeg configuration verification failed'
}
Write-Output 'Verified pinned FFmpeg source identity, x64 /MD build, configuration and static library hashes.'
