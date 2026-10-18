#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [string]$Configuration = 'Release',
    [string]$BatchEnvironmentPath = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'windows-installer/release_build_facts.ps1')
$facts = Get-OkuFlowReleaseBuildFacts $BuildDirectory $Configuration
if ($BatchEnvironmentPath) {
    $enabled = if ($facts.cuda.enabled) { 'ON' } else { 'OFF' }
    $license = [string]$facts.cuda.license
    if ($license -match '["%!^&|<>\r\n]') { throw 'Configured CUDA license path cannot be represented safely in a batch environment file.' }
    if (Test-Path -LiteralPath $BatchEnvironmentPath) { throw 'Build-facts environment output must be a fresh private path.' }
    [IO.File]::WriteAllText([IO.Path]::GetFullPath($BatchEnvironmentPath),
        "@set `"BUNDLE_EFFECTIVE_CUDA=$enabled`"`r`n@set `"BUNDLE_CUDA_LICENSE_PATH=$license`"`r`n", [Text.UTF8Encoding]::new($false))
} else {
    $facts | ConvertTo-Json -Depth 8
}
