param([switch]$ForDistribution, [switch]$FfmpegCandidate)
$ErrorActionPreference = 'Stop'
if ($ForDistribution) {
    if ($FfmpegCandidate) { throw 'Distribution is blocked: the FFmpeg receiver candidate is for local development and device acceptance only, not a release.' }
    throw 'Distribution is blocked: GPL PlayFair and FDK AAC lack a documented compatible licensing path. This script creates local development packages only.'
}
if ($env:CARGO_BUILD_TARGET -and $env:CARGO_BUILD_TARGET -ne 'x86_64-pc-windows-msvc') { throw 'Only x86_64-pc-windows-msvc is supported' }
$root = Split-Path -Parent $PSScriptRoot
$targetDirectory = Join-Path $root $(if ($FfmpegCandidate) { 'work/ffmpeg-receiver-target' } else { 'target' })
$dist = Join-Path $root $(if ($FfmpegCandidate) { 'dist/AirMirrorLite-ffmpeg-candidate' } else { 'dist/AirMirrorLite' })
$ffmpegPrefix = $null
if ($FfmpegCandidate) {
    # The opt-in package has a fixed, separate destination. Refuse directory
    # redirects so a candidate can never write through a junction to the fallback.
    foreach ($path in @((Join-Path $root 'dist'), $dist, (Join-Path $root 'work'), $targetDirectory, (Join-Path $targetDirectory 'release'), (Join-Path $targetDirectory 'x86_64-pc-windows-msvc'), (Join-Path $targetDirectory 'x86_64-pc-windows-msvc/release'))) {
        if ((Test-Path -LiteralPath $path) -and ((Get-Item -Force -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw ('Candidate destination must not contain a directory link: ' + $path)
        }
    }
    $candidateDirectories = [Collections.Generic.Queue[string]]::new()
    if (Test-Path -LiteralPath $dist -PathType Container) { $candidateDirectories.Enqueue($dist) }
    while ($candidateDirectories.Count) {
        foreach ($entry in Get-ChildItem -Force -LiteralPath $candidateDirectories.Dequeue()) {
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or ($entry.PSObject.Properties['LinkType'] -and $entry.LinkType -eq 'HardLink')) {
                throw ('Candidate package entries must not link to another package: ' + $entry.FullName)
            }
            if ($entry.PSIsContainer) { $candidateDirectories.Enqueue($entry.FullName) }
        }
    }
    $prefixPath = if ($env:AIRMIRROR_FFMPEG_PREFIX) { $env:AIRMIRROR_FFMPEG_PREFIX } else { 'work/ffmpeg-aac-msvc' }
    if (-not [IO.Path]::IsPathRooted($prefixPath)) { $prefixPath = Join-Path $root $prefixPath }
    $ffmpegPrefix = (Resolve-Path -LiteralPath $prefixPath).Path
    foreach ($relative in @('licenses/COPYING.LGPLv2.1', 'licenses/LICENSE.md', 'airmirror-ffmpeg.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $ffmpegPrefix $relative) -PathType Leaf)) { throw ('Missing FFmpeg package provenance: ' + $relative) }
    }
}
# Ask Cargo to prove freshness before assigning source provenance. An old binary
# must never be relabelled with the current clean commit. Read Cargo's artifact
# path instead of assuming that a caller's target configuration is the default.
$previousCargoHome = $env:CARGO_HOME
Push-Location -LiteralPath $root
try {
    $env:CARGO_HOME = Join-Path $root 'work/cargo'
    $buildArguments = @('build', '--release', '--locked', '--offline', '--target-dir', $targetDirectory, '--message-format=json-render-diagnostics')
    if ($FfmpegCandidate) { $buildArguments += @('--features', 'ffmpeg-aac') }
    $buildOutput = @(cargo @buildArguments)
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
    if ($FfmpegCandidate) {
        if ($artifacts[0].features -notcontains 'ffmpeg-aac' -or $artifacts[0].features -contains 'decoder-lab') { throw 'Candidate must enable ffmpeg-aac without the decoder-lab startup restriction' }
        $targetPrefix = [IO.Path]::GetFullPath($targetDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        if (-not [IO.Path]::GetFullPath($binary).StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Candidate executable is outside its dedicated target directory' }
    }
    $nativeOutput=@($events | Where-Object {$_.reason -eq 'build-script-executed' -and $_.package_id -eq $artifacts[0].package_id})
    if($nativeOutput.Count -ne 1){throw 'Native build provenance is missing'}
    $nativeBuild=Get-Content -Raw -LiteralPath (Join-Path $nativeOutput[0].out_dir 'native-build.json') | ConvertFrom-Json
    if($nativeBuild.target -ne 'x86_64-pc-windows-msvc'){throw 'Unsupported native target'}
    if ($FfmpegCandidate) {
        if ($nativeBuild.aac.backend -notin @('ffmpeg-native-aac-experiment', 'ffmpeg-native-aac') -or -not $nativeBuild.aac.bridge.path -or -not $nativeBuild.aac.upstream.source) { throw 'Candidate native provenance does not identify the FFmpeg AAC backend' }
        & (Join-Path $PSScriptRoot 'Verify-FFmpeg.ps1') -Prefix $ffmpegPrefix
        $ffmpegManifest = Get-Content -Raw -LiteralPath (Join-Path $ffmpegPrefix 'airmirror-ffmpeg.json') | ConvertFrom-Json
        if (($nativeBuild.aac.upstream | ConvertTo-Json -Depth 20 -Compress) -cne ($ffmpegManifest | ConvertTo-Json -Depth 20 -Compress)) { throw 'FFmpeg prefix provenance differs from the dependency embedded in this receiver build' }
    }
    $compiler = (rustc --version --verbose | Out-String).Trim()
    if ($LASTEXITCODE) { throw 'Compiler version lookup failed' }
    $metadataArguments = @('metadata', '--locked', '--offline', '--format-version', '1', '--filter-platform', 'x86_64-pc-windows-msvc')
    if ($FfmpegCandidate) { $metadataArguments += @('--features', 'ffmpeg-aac') }
    $metadataOutput = cargo @metadataArguments
    if($LASTEXITCODE){throw 'Offline dependency metadata lookup failed'}
    $metadata=$metadataOutput | ConvertFrom-Json
} finally {
    $env:CARGO_HOME = $previousCargoHome
    Pop-Location
}
# Verify the actual output machine too; metadata alone is insufficient.
$binaryReader=[IO.BinaryReader]::new([IO.File]::OpenRead($binary))
try {
    if($binaryReader.ReadUInt16() -ne 0x5a4d){throw 'Missing PE DOS signature'}
    $binaryReader.BaseStream.Position=0x3c;$peOffset=$binaryReader.ReadUInt32()
    if($peOffset -gt $binaryReader.BaseStream.Length-6){throw 'Invalid PE header offset'}
    $binaryReader.BaseStream.Position=$peOffset
    if($binaryReader.ReadUInt32() -ne 0x4550 -or $binaryReader.ReadUInt16() -ne 0x8664){throw 'Package executable is not x64 PE'}
} finally {$binaryReader.Dispose()}
$commit = git -C $root rev-parse HEAD
if ($LASTEXITCODE) { throw 'Source commit lookup failed' }
$sourceStatus = @(git -C $root status --porcelain)
if ($LASTEXITCODE) { throw 'Source status lookup failed' }
New-Item -ItemType Directory -Force -Path $dist,(Join-Path $dist 'licenses') | Out-Null
$destination = Join-Path $dist 'AirMirrorLite.exe'
# Permit metadata/document refresh while the identical verified binary is running.
if (-not (Test-Path -LiteralPath $destination) -or (Get-FileHash -LiteralPath $binary).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) {
    Copy-Item -LiteralPath $binary -Destination $destination
}
& (Join-Path $PSScriptRoot 'Initialize-Worker.ps1') -Executable $destination
Copy-Item -LiteralPath (Join-Path $root 'vendor/playfair/LICENSE.md') -Destination (Join-Path $dist 'licenses/PlayFair-GPLv3.md')
Copy-Item -LiteralPath (Join-Path $root 'vendor/playfair/REPLY_TABLE_NOTICE.txt') -Destination (Join-Path $dist 'licenses/REPLY_TABLE_NOTICE.txt')
Copy-Item -LiteralPath (Join-Path $root 'vendor/playfair/LGPL-2.1.txt') -Destination (Join-Path $dist 'licenses/LGPL-2.1.txt')
Copy-Item -LiteralPath (Join-Path $root 'tests/fixtures/FAIRPLAY_NOTICE.txt') -Destination (Join-Path $dist 'licenses/FAIRPLAY_FIXTURE_NOTICE.txt')
if ($FfmpegCandidate) {
    $ffmpegNotices = Join-Path $dist 'licenses/FFmpeg'
    if ((Test-Path -LiteralPath $ffmpegNotices) -and ((Get-Item -Force -LiteralPath $ffmpegNotices).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'FFmpeg notice directory must not be a link' }
    New-Item -ItemType Directory -Force -Path $ffmpegNotices,(Join-Path $ffmpegNotices 'config') | Out-Null
    Copy-Item -LiteralPath (Join-Path $ffmpegPrefix 'licenses/COPYING.LGPLv2.1'),(Join-Path $ffmpegPrefix 'licenses/LICENSE.md'),(Join-Path $ffmpegPrefix 'airmirror-ffmpeg.json') -Destination $ffmpegNotices
    Copy-Item -LiteralPath (Join-Path $ffmpegPrefix 'config/config.h'),(Join-Path $ffmpegPrefix 'config/config_components.h') -Destination (Join-Path $ffmpegNotices 'config')
    $staleFdkNotice = Join-Path $dist 'licenses/FDK-AAC.txt'
    if (Test-Path -LiteralPath $staleFdkNotice -PathType Leaf) { Remove-Item -LiteralPath $staleFdkNotice }
} else {
    Copy-Item -LiteralPath (Join-Path $root 'vendor/fdk-aac/NOTICE') -Destination (Join-Path $dist 'licenses/FDK-AAC.txt')
}
Copy-Item -LiteralPath (Join-Path $root 'docs/THIRD_PARTY.md') -Destination (Join-Path $dist 'licenses/THIRD_PARTY.md')
Copy-Item -LiteralPath (Join-Path $root 'vendor/PATCHES.md') -Destination (Join-Path $dist 'licenses/VENDOR_PATCHES.md')
& (Join-Path $PSScriptRoot 'Write-DependencyNotices.ps1') -Metadata $metadata -Destination (Join-Path $dist 'licenses')
New-Item -ItemType Directory -Force -Path (Join-Path $dist 'docs') | Out-Null
Get-ChildItem -LiteralPath (Join-Path $root 'docs') -Filter '*.md' -File | Copy-Item -Destination (Join-Path $dist 'docs')
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination (Join-Path $dist 'README.md')
$guard='LOCAL DEVELOPMENT ONLY. Redistribution is blocked pending a documented compatible licensing path for GPL PlayFair plus FDK AAC and complete source arrangements. Included notices do not grant additional permission. See docs/THIRD_PARTY.md.'
if ($FfmpegCandidate) {
    $guard='LOCAL DEVELOPMENT CANDIDATE ONLY. This receiver links pinned FFmpeg native AAC instead of FDK AAC. Distribution remains disabled while candidate validation and package/source obligations are reviewed. Included notices do not grant additional permission. See docs/THIRD_PARTY.md.'
    Set-Content -LiteralPath (Join-Path $dist 'CANDIDATE.txt') -Encoding utf8 -Value 'Explicit FFmpeg AAC receiver candidate. The working FDK package remains separate in dist/AirMirrorLite. This candidate is not a release or physical A/V acceptance. No settings or identities are copied from the fallback package.'
}
Set-Content -LiteralPath (Join-Path $dist 'DISTRIBUTION-BLOCKED.txt') -Value $guard -Encoding utf8
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $dist 'AirMirrorLite.exe')
$nativeBuild | Add-Member -NotePropertyName compilerFileVersions -NotePropertyValue @{
    playfair=(Get-Item -LiteralPath $nativeBuild.playfair.path).VersionInfo.FileVersion
    aac=(Get-Item -LiteralPath $(if ($FfmpegCandidate) { $nativeBuild.aac.bridge.path } else { $nativeBuild.aac.path })).VersionInfo.FileVersion
    bridge=(Get-Item -LiteralPath $nativeBuild.bridge.path).VersionInfo.FileVersion
    resourceCompiler=(Get-Item -LiteralPath $nativeBuild.resourceCompiler).VersionInfo.FileVersion
}
$sourceFiles=@(git -C $root ls-files --cached --others --exclude-standard | Sort-Object -Unique)
if($LASTEXITCODE){throw 'Source inventory lookup failed'}
$sourceEntries=@(foreach($relative in $sourceFiles){
    $sourcePath=Join-Path $root $relative
    if(Test-Path -LiteralPath $sourcePath -PathType Leaf){$relative+':'+(Get-FileHash -Algorithm SHA256 -LiteralPath $sourcePath).Hash}
})
$sourceHasher=[Security.Cryptography.SHA256]::Create()
try{$sourceTreeHash=([BitConverter]::ToString($sourceHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes(($sourceEntries -join "`n"))))).Replace('-','')}finally{$sourceHasher.Dispose()}
$sourceProvenance=@{sourceTreeSha256=$sourceTreeHash;sourceFiles=$sourceEntries.Count;sourceHashScope='Git tracked and nonignored working-tree files: sorted relative name:SHA256 lines, UTF-8, LF without final newline';cargoLockSha256=(Get-FileHash (Join-Path $root 'Cargo.lock')).Hash;vendorManifestSha256=(Get-FileHash (Join-Path $root 'vendor/SHA256SUMS')).Hash;playfair='UxPlay 15a6b87172eca38b0df4e615fbf1bba108734dde plus vendor/PATCHES.md';fixture='rairplay 1.0.0 / 02f62af2269e1651f46e43220cee21af1cb35637'}
if ($FfmpegCandidate) {
    $sourceProvenance.ffmpeg=$nativeBuild.aac.upstream.source
    $sourceProvenance.ffmpegManifestSha256=(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $ffmpegPrefix 'airmirror-ffmpeg.json')).Hash
} else { $sourceProvenance.fdkAac='c41c3c7516ea8d0d78a53b96b30bed3a29745cc6' }
$buildRecord=@{
    executable = 'AirMirrorLite.exe'; sha256 = $hash.Hash; sourceBaseCommit = $commit
    sourceDirty = ($sourceStatus.Count -ne 0)
    packagedUtc = [DateTime]::UtcNow.ToString('o'); purpose = $(if ($FfmpegCandidate) { 'Local FFmpeg receiver candidate / device acceptance only; not a release' } else { 'Local development / device acceptance only' })
    buildVerification = $(if ($FfmpegCandidate) { 'cargo build --release --locked --offline --features ffmpeg-aac --target-dir work/ffmpeg-receiver-target (freshness checked before copy)' } else { 'cargo build --release --locked --offline (freshness checked before copy)' })
    target = $nativeBuild.target; peMachine='AMD64 (0x8664)'; rust = $compiler; native=$nativeBuild
    windows=[Environment]::OSVersion.VersionString; distributionAllowed=$false
    sourceProvenance=$sourceProvenance
} | ConvertTo-Json -Depth 10
# Keep toolset/SDK versions and effective flags while removing personal paths.
$buildRecord=$buildRecord.Replace($root.Replace('\','\\'),'PROJECT_ROOT').Replace($root.Replace('\','/'),'PROJECT_ROOT')
$buildRecord=$buildRecord.Replace($env:USERPROFILE.Replace('\','\\'),'HOME').Replace($env:USERPROFILE.Replace('\','/'),'HOME')
Set-Content -LiteralPath (Join-Path $dist 'BUILD.json') -Value $buildRecord -Encoding utf8
Write-Output $hash
