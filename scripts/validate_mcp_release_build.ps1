#requires -Version 7.0
param([string]$BuildDirectory,[string]$Executable,[switch]$CacheOnly,[switch]$NativeOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-installer/mcp_bundle.ps1')
if ($CacheOnly -and $NativeOnly) { throw 'Choose one validation mode.' }
if ($NativeOnly) {
    if (-not $Executable) { throw 'The exact built MCP executable is required.' }
    Assert-McpNativeImports $Executable
    return
}
if (-not $BuildDirectory) { throw 'The matching build directory is required.' }
if ($CacheOnly) { Assert-McpReleaseCache $BuildDirectory; return }
if (-not $Executable) { throw 'The exact built MCP executable is required.' }
Get-McpReleaseBuild $BuildDirectory $Executable | ConvertTo-Json -Compress
