param([string]$Executable = '', [string]$UiRenderDirectory = '')
# Compatibility entry point: intrinsic hidden UI/GPU tests only. No external
# window manipulation, mouse/keyboard injection, desktop capture, or audio.
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Test-Media.ps1') -Executable $Executable -UiRenderDirectory $UiRenderDirectory
