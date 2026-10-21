param([string]$Executable = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $root 'target/release/airmirror-lite.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$data = Join-Path $root ('local/worker-deployment/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $data -Force | Out-Null
$copy = Join-Path $data 'AirMirrorLite.exe'
Copy-Item -LiteralPath $Executable -Destination $copy
if (-not ('AirMirrorWorkerSid' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class AirMirrorWorkerSid {
  [DllImport("userenv.dll", CharSet=CharSet.Unicode)]
  public static extern int DeriveAppContainerSidFromAppContainerName(string name, out IntPtr sid);
  [DllImport("advapi32.dll")] public static extern IntPtr FreeSid(IntPtr sid);
}
'@
}
[IntPtr]$sidPointer = [IntPtr]::Zero
$hr = [AirMirrorWorkerSid]::DeriveAppContainerSidFromAppContainerName('AirMirrorLite.Codec', [ref]$sidPointer)
if ($hr -lt 0) { throw 'Could not derive the existing worker identity' }
try { $workerSid = [Security.Principal.SecurityIdentifier]::new($sidPointer) }
finally { [void][AirMirrorWorkerSid]::FreeSid($sidPointer) }
# Mutate only the disposable test executable, never the source EXE or a parent ACL.
$acl = Get-Acl -LiteralPath $copy
$acl.PurgeAccessRules($workerSid)
Set-Acl -LiteralPath $copy -AclObject $acl

function Invoke-Receiver([string]$Label, [string[]]$ReceiverArguments) {
    $stdout = Join-Path $data ($Label + '-out.txt')
    $stderr = Join-Path $data ($Label + '-err.txt')
    $process = Start-Process -FilePath $copy -ArgumentList $ReceiverArguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    try {
        if (-not $process.WaitForExit(15000)) { $process.Kill(); $process.WaitForExit(); throw "$Label did not stop within 15 seconds" }
        $process.Refresh()
        return @{ exitCode=$process.ExitCode; stderr=(Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue) }
    } finally { $process.Dispose() }
}

$launch = @('--hidden','--no-discovery','--bind','127.0.0.1','--port','0','--seconds','1','--data-dir',('"' + (Join-Path $data 'state') + '"'))
$missing = Invoke-Receiver 'missing-grant' $launch
if ($missing.exitCode -eq 0 -or $missing.stderr -notmatch 'provision-worker') { throw 'Missing worker grant was not rejected with deployment guidance' }
$prepared = Invoke-Receiver 'provision' @('--provision-worker')
if ($prepared.exitCode -ne 0) { throw ('Worker provisioning failed: ' + $prepared.stderr) }
$before = (Get-Acl -LiteralPath $copy).Sddl
foreach ($iteration in 1..2) {
    $normal = Invoke-Receiver "normal-$iteration" $launch
    if ($normal.exitCode -ne 0) { throw ('Normal restricted worker launch failed: ' + $normal.stderr) }
    if ((Get-Acl -LiteralPath $copy).Sddl -ne $before) { throw 'Ordinary launch changed the executable DACL' }
}
$repeat = Invoke-Receiver 'provision-repeat' @('--provision-worker')
if ($repeat.exitCode -ne 0 -or (Get-Acl -LiteralPath $copy).Sddl -ne $before) { throw 'Provisioning was not idempotent' }
@{
    executable=$Executable; sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
    missingGrantRejected=$true; normalLaunches=2; normalLaunchDaclUnchanged=$true; provisioningIdempotent=$true
    evidence='Silent owned-copy deployment checks. Does not simulate a separate administrator owner or a different Windows system directory.'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $data 'result.json') -Encoding utf8
Write-Output "PASS: missing worker grant rejected; explicit provisioning succeeds; two normal launches leave DACL unchanged. Evidence: $data"
