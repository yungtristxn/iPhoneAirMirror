param(
    [Alias('Profile')][ValidateSet('legacy','hap')][string]$ReceiverProfile = 'legacy',
    [string]$Destination = '',
    [string]$Zip = ''
)
# Build/package FFmpeg only. Every destination must be new; preserved receiver
# builds, identities and logs are never overwritten or copied into this package.
$ErrorActionPreference = 'Stop'
$arguments = @('-B', (Join-Path $PSScriptRoot 'Package-Share.py'), '--profile', $ReceiverProfile)
if ($Destination) { $arguments += @('--directory', $Destination) }
if ($Zip) { $arguments += @('--output', $Zip) }
python @arguments
if ($LASTEXITCODE) { throw 'Release candidate packaging failed' }
