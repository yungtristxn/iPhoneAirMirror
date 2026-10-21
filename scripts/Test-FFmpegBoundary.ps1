param([string]$Prefix='')
# Standalone synthetic C-ABI test only: no network, audio endpoint or UI.
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(-not $Prefix){$Prefix=Join-Path $root 'work/ffmpeg-aac-msvc'}
$output=Join-Path $root 'local/decoder-boundary'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$resultPath=Join-Path $output 'result.json'
if(Test-Path -LiteralPath $resultPath){Remove-Item -LiteralPath $resultPath}
$result=[ordered]@{
    schemaVersion=1
    startedUtc=[DateTime]::UtcNow.ToString('o')
    pass=$false
    phase='verify'
    target='x86_64-pc-windows-msvc'
    audioPlayed=$false
    evidence='Standalone synthetic C-ABI bounds and canaries; not whole-library sanitizer, AppContainer, phone or physical-audio evidence.'
}
function File-Evidence([string]$Path){
    $item=Get-Item -LiteralPath $Path
    [ordered]@{path=$item.FullName;size=$item.Length;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$saved=@{Path=$env:PATH;Include=$env:INCLUDE;Lib=$env:LIB;VsLang=$env:VSLANG}
try{
    & (Join-Path $PSScriptRoot 'Verify-FFmpeg.ps1') -Prefix $Prefix
    $Prefix=(Resolve-Path -LiteralPath $Prefix).Path
    $manifest=Get-Content -LiteralPath (Join-Path $Prefix 'airmirror-ffmpeg.json') -Raw | ConvertFrom-Json
    $sources=@((Join-Path $root 'native/codec_ffmpeg.cpp'),(Join-Path $root 'tests/decoder-experiment/ffmpeg_boundary.cpp'))
    $libraries=@((Join-Path $Prefix 'lib/avcodec.lib'),(Join-Path $Prefix 'lib/avutil.lib'))
    $fixtures=@((Join-Path $root 'tests/fixtures/noise-44100-stereo-eld-experiment.frames'),(Join-Path $root 'tests/fixtures/noise-44100-stereo-lc-experiment.frames'))
    $result.prefix=$Prefix
    $result.ffmpegSource=$manifest.source
    $result.configuration=$manifest.configuration
    $result.sources=@($sources | ForEach-Object {File-Evidence $_})
    $result.recipes=@((File-Evidence $PSCommandPath),(File-Evidence (Join-Path $PSScriptRoot 'Verify-FFmpeg.ps1')))
    $result.libraries=@($libraries | ForEach-Object {File-Evidence $_})
    $result.fixtures=@($fixtures | ForEach-Object {File-Evidence $_})

    $result.phase='toolchain'
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if(-not(Test-Path -LiteralPath $vswhere)){throw 'An installed x64 MSVC toolset is required'}
    $visualStudio=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if($LASTEXITCODE -or -not $visualStudio){throw 'An installed x64 MSVC toolset is required'}
    $toolset=(Get-Content -LiteralPath (Join-Path $visualStudio 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
    $msvc=Join-Path $visualStudio ('VC/Tools/MSVC/'+$toolset)
    $sdk=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
    $sdkVersion=(Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory | Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'um/Windows.h')} | Sort-Object {[version]$_.Name} -Descending | Select-Object -First 1).Name
    if(-not $sdkVersion){throw 'An installed Windows SDK is required'}
    $compilerDirectory=Join-Path $msvc 'bin/HostX64/x64'
    $compiler=Join-Path $compilerDirectory 'cl.exe'
    $result.compiler=File-Evidence $compiler
    $result.toolset=$toolset
    $result.windowsSdk=$sdkVersion
    $env:PATH=$compilerDirectory+';'+$saved.Path
    $env:INCLUDE=(@((Join-Path $msvc 'include'))+@(@('ucrt','um','shared','winrt','cppwinrt') | ForEach-Object {Join-Path $sdk ('Include/'+$sdkVersion+'/'+$_)})) -join ';'
    $env:LIB=(@((Join-Path $msvc 'lib/x64'))+@(@('ucrt','um') | ForEach-Object {Join-Path $sdk ('Lib/'+$sdkVersion+'/'+$_+'/x64')})) -join ';'
    $env:VSLANG='1033'
    $executable=Join-Path $output 'ffmpeg_boundary.exe'
    # Keep our sources strict; upstream's public C headers contain intentional
    # narrowed clipping helpers that warn when consumed as C++ with /W4.
    $arguments=@('/nologo','/std:c++17','/EHsc','/MD','/O2','/W4','/WX','/external:W0',('/external:I'+(Join-Path $Prefix 'include')),('/Fo'+$output+'\'),('/Fe'+$executable))
    $arguments+=$sources
    $arguments+=@('/link','/MACHINE:X64')+$libraries+@('ole32.lib','user32.lib','bcrypt.lib')
    $result.phase='build'
    $result.buildArguments=$arguments
    $result.buildLog=Join-Path $output 'build.log'
    & $compiler @arguments *> $result.buildLog
    $result.buildExitCode=$LASTEXITCODE
    if($result.buildExitCode){Get-Content -LiteralPath $result.buildLog;throw ('Boundary compilation failed: '+$result.buildExitCode)}
    $result.executable=File-Evidence $executable
    $result.phase='run'
    $result.stdout=Join-Path $output 'stdout.txt'
    $result.stderr=Join-Path $output 'stderr.txt'
    $process=Start-Process -FilePath $executable -ArgumentList @($fixtures | ForEach-Object {'"'+$_+'"'}) -PassThru -WindowStyle Hidden -RedirectStandardOutput $result.stdout -RedirectStandardError $result.stderr
    try{
        if(-not $process.WaitForExit(30000)){$process.Kill();[void]$process.WaitForExit(5000);throw 'Boundary test exceeded 30 seconds'}
        $result.exitCode=$process.ExitCode
    }finally{$process.Dispose()}
    Get-Content -LiteralPath $result.stdout,$result.stderr
    if($result.exitCode -ne 0){throw ('Boundary test failed: '+$result.exitCode)}
    # Refuse success if a concurrent edit/rebuild changed an input during this run.
    foreach($entry in @($result.sources)+@($result.recipes)+@($result.libraries)+@($result.fixtures)){
        if((Get-FileHash -LiteralPath $entry.path -Algorithm SHA256).Hash -ne $entry.sha256){throw ('Input changed during boundary test: '+$entry.path)}
    }
    $result.pass=$true
    $result.phase='complete'
}catch{
    $result.error=$_.Exception.Message
    throw
}finally{
    try{
        $result.finishedUtc=[DateTime]::UtcNow.ToString('o')
        $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    }finally{
        $env:PATH=$saved.Path;$env:INCLUDE=$saved.Include;$env:LIB=$saved.Lib;$env:VSLANG=$saved.VsLang
    }
}
