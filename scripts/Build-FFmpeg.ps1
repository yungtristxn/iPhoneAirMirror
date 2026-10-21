param([switch]$Fetch,[ValidateRange(1,16)][int]$Jobs=8)
# Experimental dependency build only. Never writes vendor/, target/ or dist/.
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$pins=Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'ffmpeg-pins.json') | ConvertFrom-Json
$work=[IO.Path]::GetFullPath((Join-Path $root 'work'))
$deps=Join-Path $work 'ffmpeg-deps'
$source=Join-Path $deps ('ffmpeg-'+$pins.source.version)
$build=Join-Path $work 'ffmpeg-build-msvc'
$prefix=Join-Path $work 'ffmpeg-aac-msvc'
$llvm=Join-Path $work ('llvm-'+$pins.llvm.version)
$bash='C:/Program Files/Git/bin/bash.exe'
$sevenZip='C:/Program Files/7-Zip/7z.exe'
foreach($tool in @($bash,$sevenZip)){if(-not(Test-Path -LiteralPath $tool)){throw ('Required local development tool missing: '+$tool)}}
New-Item -ItemType Directory -Force -Path $deps,$llvm | Out-Null

function Get-PinnedFile([string]$Url,[string]$Hash,[string]$Path){
    if(-not(Test-Path -LiteralPath $Path)){
        if(-not $Fetch){throw ('Missing pinned archive; run Build-FFmpeg.ps1 -Fetch: '+$Path)}
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Path
    }
    if((Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash -ne $Hash){throw ('Pinned SHA-256 mismatch: '+$Path)}
}
function Reset-ScratchDirectory([string]$Path){
    $absolute=[IO.Path]::GetFullPath($Path)
    if(-not $absolute.StartsWith($work+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or $absolute -notin @($source,$build,$prefix)){
        throw ('Refusing cleanup outside the three dedicated FFmpeg scratch directories: '+$absolute)
    }
    foreach($ancestor in @($work,$deps)){
        if((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Refusing cleanup through a linked scratch ancestor'}
    }
    if(Test-Path -LiteralPath $absolute){
        if((Get-Item -LiteralPath $absolute).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Refusing a linked scratch directory'}
        Remove-Item -LiteralPath $absolute -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $absolute | Out-Null
}
function Bash-Path([string]$Path){
    $full=[IO.Path]::GetFullPath($Path)
    if($full -notmatch '^([A-Za-z]):\\(.+)$'){throw 'The isolated build requires a local drive path'}
    '/'+$Matches[1].ToLowerInvariant()+'/'+$Matches[2].Replace('\','/')
}

$sourceArchive=Join-Path $deps ('ffmpeg-'+$pins.source.version+'.tar.xz')
$signature=$sourceArchive+'.asc';$key=Join-Path $deps 'ffmpeg-devel.asc'
$nasmArchive=Join-Path $deps ('nasm-'+$pins.nasm.version+'-win64.zip')
$makeArchive=Join-Path $deps ('make-'+$pins.make.version+'-x86_64.pkg.tar.zst')
$llvmArchive=Join-Path $llvm ('LLVM-'+$pins.llvm.version+'-win64.exe')
Get-PinnedFile $pins.source.url $pins.source.sha256 $sourceArchive
Get-PinnedFile $pins.source.signatureUrl $pins.source.signatureSha256 $signature
Get-PinnedFile $pins.source.keyUrl $pins.source.keySha256 $key
Get-PinnedFile $pins.nasm.url $pins.nasm.sha256 $nasmArchive
Get-PinnedFile $pins.make.url $pins.make.sha256 $makeArchive
Get-PinnedFile $pins.llvm.url $pins.llvm.sha256 $llvmArchive

$keyring=Join-Path $deps 'gnupg'
New-Item -ItemType Directory -Force -Path $keyring | Out-Null
$gpg='C:/Program Files/Git/usr/bin/gpg.exe'
try{
    & $gpg --batch --homedir (Bash-Path $keyring) --import (Bash-Path $key) 2> (Join-Path $deps 'key-import.log')
    if($LASTEXITCODE){throw 'Isolated FFmpeg signing-key import failed'}
    $signatureStatus=@(& $gpg --batch --homedir (Bash-Path $keyring) --status-fd 1 --verify (Bash-Path $signature) (Bash-Path $sourceArchive) 2> (Join-Path $deps 'signature.log'))
    if($LASTEXITCODE -or -not($signatureStatus -match ('VALIDSIG '+$pins.source.signingFingerprint+' '))){throw 'FFmpeg source signature/fingerprint mismatch'}
}finally{
    # GnuPG may launch an agent even for public-key import. Close only this
    # task's isolated agent; never touch the user's normal keyring/agent.
    & 'C:/Program Files/Git/usr/bin/gpgconf.exe' --homedir (Bash-Path $keyring) --kill gpg-agent
}

# Extract development tools only; never execute their installers or edit PATH globally.
& $sevenZip x $llvmArchive ('-o'+$llvm) -y 'bin/clang.exe' 'bin/clang-cl.exe' 'lib/clang/*/include/*' | Out-Null
if($LASTEXITCODE){throw 'Portable LLVM extraction failed'}
Expand-Archive -LiteralPath $nasmArchive -DestinationPath $deps -Force
& $sevenZip x $makeArchive ('-o'+$deps) -y | Out-Null
if($LASTEXITCODE){throw 'Portable GNU Make decompression failed'}
& $sevenZip x ($makeArchive -replace '\.zst$','') ('-o'+(Join-Path $deps 'make')) -y | Out-Null
if($LASTEXITCODE){throw 'Portable GNU Make extraction failed'}

$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if($LASTEXITCODE -or -not $visualStudio){throw 'An installed x64 MSVC toolset is required'}
$toolset=(Get-Content -Raw -LiteralPath (Join-Path $visualStudio 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt')).Trim()
$msvc=Join-Path $visualStudio ('VC/Tools/MSVC/'+$toolset)
$sdk=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$sdkVersion=(Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory | Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'um/Windows.h')} | Sort-Object {[version]$_.Name} -Descending | Select-Object -First 1).Name
if(-not $sdkVersion){throw 'An installed Windows SDK is required'}
$compilerDirectory=Join-Path $msvc 'bin/HostX64/x64'
$nasm=Join-Path $deps ('nasm-'+$pins.nasm.version)
$make=Join-Path $deps 'make/usr/bin'
$saved=@{Path=$env:PATH;Include=$env:INCLUDE;Lib=$env:LIB;VsLang=$env:VSLANG}
Push-Location -LiteralPath $root
try{
    $env:PATH=(Join-Path $llvm 'bin')+';'+$compilerDirectory+';'+$nasm+';'+$make+';C:/Program Files/Git/usr/bin;'+$saved.Path
    $env:INCLUDE=(@((Join-Path $msvc 'include'))+@(@('ucrt','um','shared','winrt','cppwinrt') | ForEach-Object {Join-Path $sdk ('Include/'+$sdkVersion+'/'+$_)})) -join ';'
    $env:LIB=(@((Join-Path $msvc 'lib/x64'))+@(@('ucrt','um') | ForEach-Object {Join-Path $sdk ('Lib/'+$sdkVersion+'/'+$_+'/x64')})) -join ';'
    $env:VSLANG='1033'
    foreach($directory in @($source,$build,$prefix)){Reset-ScratchDirectory $directory}
    # PATH includes Git's GNU tools; call Windows tar explicitly so a C:\ path
    # cannot be mistaken for GNU tar's host:archive remote syntax.
    & (Join-Path $env:SystemRoot 'System32/tar.exe') -xf $sourceArchive -C $deps
    if($LASTEXITCODE){throw 'Verified source extraction failed'}
    $flags=@('--prefix=../ffmpeg-aac-msvc','--toolchain=msvc','--cc=clang-cl.exe','--host-cc=clang-cl.exe','--arch=x86_64','--target-os=win64','--disable-all','--disable-autodetect','--disable-network','--disable-programs','--disable-doc','--disable-shared','--enable-static','--enable-avcodec','--enable-avutil','--enable-decoder=aac','--disable-debug','--extra-cflags=-MD','--extra-cxxflags=-MD','--host-cflags=-MD')
    $shell=@'
set -eu
cd work/ffmpeg-build-msvc
../ffmpeg-deps/FFMPEG_SOURCE/configure FFMPEG_FLAGS > configure.stdout.txt 2> configure.stderr.txt
make -r -jFFMPEG_JOBS > build.stdout.txt 2> build.stderr.txt
make -r install > install.stdout.txt 2> install.stderr.txt
'@
    # These substitutions contain only fixed flags, a pinned version and a bounded integer.
    $shell=$shell.Replace('FFMPEG_SOURCE',('ffmpeg-'+$pins.source.version)).Replace('FFMPEG_FLAGS',($flags -join ' ')).Replace('FFMPEG_JOBS',[string]$Jobs)
    $shellPath=Join-Path $deps 'build-aac.sh'
    [IO.File]::WriteAllText($shellPath,$shell.Replace("`r`n","`n"),[Text.UTF8Encoding]::new($false))
    & $bash --noprofile --norc (Bash-Path $shellPath)
    if($LASTEXITCODE){throw ('FFmpeg build failed; inspect '+$build+'/configure.stderr.txt and build.stderr.txt')}
    & (Join-Path $PSScriptRoot 'Write-FFmpegManifest.ps1') -Prefix $prefix -BuildDirectory $build -SourceDirectory $source -CompilerDirectory $compilerDirectory -MsvcToolset $toolset -WindowsSdk $sdkVersion -ConfigureFlags $flags
}finally{
    $env:PATH=$saved.Path;$env:INCLUDE=$saved.Include;$env:LIB=$saved.Lib;$env:VSLANG=$saved.VsLang
    Pop-Location
}
Write-Output ('Experimental native AAC prefix: '+$prefix)
