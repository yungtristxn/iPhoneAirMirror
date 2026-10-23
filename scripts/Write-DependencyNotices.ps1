param(
    [Parameter(Mandatory=$true,ParameterSetName='Object')]$Metadata,
    [Parameter(Mandatory=$true,ParameterSetName='File')][string]$MetadataFile,
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
if ($PSCmdlet.ParameterSetName -eq 'File') { $Metadata = Get-Content -Raw -LiteralPath $MetadataFile | ConvertFrom-Json }
if (-not $Metadata.resolve.root) { throw 'Cargo metadata must identify a root package' }
$nodes=@{}; foreach($node in $Metadata.resolve.nodes){$nodes[$node.id]=$node}
$packages=@{}; foreach($package in $Metadata.packages){$packages[$package.id]=$package}
$selected=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$pending=[Collections.Generic.Queue[string]]::new();$pending.Enqueue($Metadata.resolve.root)
while($pending.Count){
    $id=$pending.Dequeue();if(-not $selected.Add($id)){continue}
    foreach($dependency in $nodes[$id].deps){
        # The graph was filtered for the supported target. Include normal
        # transitive dependencies, including proc-macro generated runtime code;
        # test-only and native build-tool crates are not shipped as runtime code.
        if(@($dependency.dep_kinds | Where-Object {$null -eq $_.kind}).Count){$pending.Enqueue($dependency.pkg)}
    }
}
$entries=@()
foreach($package in @($selected | Where-Object {$_ -ne $Metadata.resolve.root} | ForEach-Object {$packages[$_]} | Sort-Object name,version)){
    $crateDirectory=Split-Path -Parent $package.manifest_path
    $notices=@(Get-ChildItem -LiteralPath $crateDirectory -Recurse -File | Where-Object {$_.Name -match '^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE|AUTHORS)([-._].*)?$'})
    if($package.license_file){$notices+=Get-Item -LiteralPath (Join-Path $crateDirectory $package.license_file)}
    $notices=@($notices | Sort-Object FullName -Unique)
    if(-not $notices.Count){throw ('No packaged copyright/license material found for '+$package.name+' '+$package.version)}
    $files=@()
    foreach($notice in $notices){
        $relative=$notice.FullName.Substring($crateDirectory.Length+1)
        $target=Join-Path $Destination ('rust/'+$package.name+'-'+$package.version+'/'+$relative)
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $notice.FullName -Destination $target
        $files+=@{path=$relative.Replace('\','/');sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash}
    }
    $indexDirectory=Split-Path -Parent $crateDirectory
    $registryDirectory=Split-Path -Parent (Split-Path -Parent $indexDirectory)
    $archive=Join-Path $registryDirectory ('cache/'+(Split-Path -Leaf $indexDirectory)+'/'+$package.name+'-'+$package.version+'.crate')
    $entries+=@{name=$package.name;version=$package.version;declaredLicense=$package.license;source=$package.source;crateSha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash;files=$files}
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
@{target='x86_64-pc-windows-msvc';scope='Resolved normal runtime dependency closure; declarations are inventory, not a compatibility approval.';packages=$entries} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Destination 'RUST-DEPENDENCIES.json') -Encoding utf8
$toolchainRoot=rustc --print sysroot
if($LASTEXITCODE){throw 'Rust toolchain copyright lookup failed'}
$rustNotices=Join-Path $Destination 'rust-standard-library'
New-Item -ItemType Directory -Force -Path $rustNotices | Out-Null
foreach($name in @('COPYRIGHT-library.html','COPYRIGHT.html')){
    Copy-Item -LiteralPath (Join-Path $toolchainRoot ('share/doc/rust/'+$name)) -Destination (Join-Path $rustNotices $name)
}
