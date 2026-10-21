param([switch]$Inspect)
# Restrict only rules for this project's exact executable paths. No global changes.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
trap { $_ | Out-String | Set-Content -LiteralPath (Join-Path $root 'local/firewall-error.txt'); exit 1 }
if (-not $Inspect -and -not ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Windows administrator token required' }
$programs = @((Join-Path $root 'dist/AirMirrorLite/AirMirrorLite.exe'), (Join-Path $root 'target/debug/airmirror-lite.exe'), (Join-Path $root 'target/release/airmirror-lite.exe')) | ForEach-Object { [IO.Path]::GetFullPath($_) }
$changes = @()
foreach ($program in $programs) {
    $filters = Get-NetFirewallApplicationFilter | Where-Object { $_.Program -eq $program }
    foreach ($filter in $filters) {
        foreach ($rule in @(Get-NetFirewallRule -AssociatedNetFirewallApplicationFilter $filter)) {
            if ($rule.Direction -ne 'Inbound' -or $rule.Action -ne 'Allow') { continue }
            $before = $rule.Profile.ToString()
            if ($Inspect) { [pscustomobject]@{Program=$program;Name=$rule.Name;Profile=$before}; continue }
            $rule | Set-NetFirewallRule -EdgeTraversalPolicy Block -Profile Private -RemoteAddress LocalSubnet
            $after = Get-NetFirewallRule -Name $rule.Name
            $address = $after | Get-NetFirewallAddressFilter
            if ($after.Profile.ToString() -ne 'Private' -or $address.RemoteAddress -notcontains 'LocalSubnet') { throw 'Firewall scope verification failed' }
            $changes += [pscustomobject]@{Program=$program;Name=$rule.Name;BeforeProfile=$before;AfterProfile=$after.Profile.ToString();RemoteAddress=$address.RemoteAddress}
        }
    }
}
if ($Inspect) { exit 0 }
if ($changes.Count -eq 0) { throw 'No AirMirrorLite allow rules matched the exact program paths' }
New-Item -ItemType Directory -Force -Path (Join-Path $root 'local') | Out-Null
$changes | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $root 'local/firewall-scope.json') -Encoding UTF8
