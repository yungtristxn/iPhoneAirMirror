param(
    [Parameter(Mandatory=$true)][string]$Prefix,
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [Parameter(Mandatory=$true)][string]$CompilerDirectory,
    [Parameter(Mandatory=$true)][string]$MsvcToolset,
    [Parameter(Mandatory=$true)][string]$WindowsSdk,
    [Parameter(Mandatory=$true)][string[]]$ConfigureFlags
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$pins=Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'ffmpeg-pins.json') | ConvertFrom-Json
if((Get-Content -Raw -LiteralPath (Join-Path $SourceDirectory 'RELEASE')).Trim() -ne $pins.source.version){throw 'Unexpected FFmpeg release'}
$configuration=@{}
foreach($line in Get-Content -LiteralPath (Join-Path $BuildDirectory 'config.h')){
    if($line -match '^#define ((CONFIG|HAVE|ARCH)_[A-Z0-9_]+) ([01])$'){$configuration[$Matches[1]]=[int]$Matches[3]}
}
foreach($name in @('CONFIG_AVCODEC','CONFIG_AVUTIL','CONFIG_STATIC','CONFIG_SAFE_BITSTREAM_READER','CONFIG_RUNTIME_CPUDETECT','ARCH_X86_64','HAVE_X86ASM')){
    if($configuration[$name] -ne 1){throw ('Required FFmpeg capability missing: '+$name)}
}
foreach($name in @('CONFIG_NETWORK','CONFIG_LIBFDK_AAC','CONFIG_GPL','CONFIG_VERSION3','CONFIG_NONFREE','CONFIG_SHARED','CONFIG_AVFORMAT','CONFIG_AVDEVICE','CONFIG_AVFILTER','CONFIG_SWRESAMPLE','CONFIG_SWSCALE','CONFIG_FFMPEG','CONFIG_FFPLAY','CONFIG_FFPROBE')){
    if($configuration[$name] -ne 0){throw ('Unexpected FFmpeg capability: '+$name)}
}
$enabled=@(Get-Content -LiteralPath (Join-Path $BuildDirectory 'config_components.h') | Where-Object {$_ -match '^#define CONFIG_\w+ 1$'})
if($enabled.Count -ne 1 -or $enabled[0] -ne '#define CONFIG_AAC_DECODER 1'){throw 'The experimental FFmpeg build must enable only the native AAC decoder'}
$nativeFlags=@(Get-Content -LiteralPath (Join-Path $BuildDirectory 'ffbuild/config.mak') | Where-Object {$_ -match '^(CC_IDENT|CC|CFLAGS|CPPFLAGS|X86ASM|X86ASMFLAGS|EXTRALIBS-avcodec|EXTRALIBS-avutil)='})
if(-not($nativeFlags -match '^CFLAGS=.* -MD( |$)')){throw 'FFmpeg compiler flags do not request /MD'}
$libraries=@{}
foreach($name in @('avcodec','avutil')){
    $library=Join-Path $Prefix ('lib/'+$name+'.lib')
    $directives=@(& (Join-Path $CompilerDirectory 'dumpbin.exe') /directives $library)
    if($LASTEXITCODE){throw ('Cannot inspect runtime linkage: '+$library)}
    if(-not($directives -match 'DEFAULTLIB:"?msvcrt\.lib') -or ($directives -match 'DEFAULTLIB:"?(libcmt|msvcrtd)')){throw ('Library is not release /MD: '+$library)}
    $libraries[$name]=@{file=('lib/'+$name+'.lib');sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $library).Hash.ToLowerInvariant();size=(Get-Item -LiteralPath $library).Length}
}
New-Item -ItemType Directory -Force -Path (Join-Path $Prefix 'config'),(Join-Path $Prefix 'licenses') | Out-Null
foreach($name in @('config.h','config_components.h')){Copy-Item -LiteralPath (Join-Path $BuildDirectory $name) -Destination (Join-Path $Prefix ('config/'+$name))}
Copy-Item -LiteralPath (Join-Path $SourceDirectory 'COPYING.LGPLv2.1'),(Join-Path $SourceDirectory 'LICENSE.md') -Destination (Join-Path $Prefix 'licenses')
$clang=Join-Path $root ('work/llvm-'+$pins.llvm.version+'/bin/clang-cl.exe')
$record=@{
    schemaVersion=1;source=$pins.source;target='x86_64-pc-windows-msvc';runtime='MD';configureFlags=$ConfigureFlags
    compiler=@{family='clang-cl';version=(& $clang --version | Select-Object -First 1);msvcToolset=$MsvcToolset;windowsSdk=$WindowsSdk;flags=$nativeFlags}
    tools=@{llvm=$pins.llvm;nasm=$pins.nasm;make=$pins.make;gitBash=(& 'C:/Program Files/Git/bin/bash.exe' --version | Select-Object -First 1)}
    libraries=$libraries
    configuration=@{configFile='config/config.h';componentsFile='config/config_components.h';configSha256=(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Prefix 'config/config.h')).Hash.ToLowerInvariant();componentsSha256=(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Prefix 'config/config_components.h')).Hash.ToLowerInvariant()}
    enabled=@{decoders=@('aac');libraries=@('avcodec','avutil');assembly=$true;runtimeCpuDetection=$true;programs=@();encoders=@();parsers=@();demuxers=@();muxers=@();protocols=@();filters=@();bsfs=@();hwaccels=@();network=$false;externalCodecs=@()}
    builtUtc=[DateTime]::UtcNow.ToString('o');distributionApproved=$false
    purpose='Local native-AAC decoder experiment; generated archives include generic avutil support, but no other media components are enabled. This manifest is not a distribution authorization or phone/audio acceptance.'
}
$record | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $Prefix 'airmirror-ffmpeg.json') -Encoding utf8
Write-Output ('Verified native AAC-only /MD libraries: '+$Prefix)
