#requires -Version 7.0
<#
.SYNOPSIS
Stages the reviewed source-built adb runtime with its notices and relinking source.
.DESCRIPTION
Source selection: -SourcePath, OKUFLOW_PLATFORM_TOOLS_DIR, otherwise the output
of build_adb_from_source.ps1 under LOCALAPPDATA. SDK/Google prebuilt binaries
are never accepted. Every file is checked against the reviewed distribution
manifest. This script never starts or stops any adb server.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$BundlePath, [string]$SourcePath)
$ErrorActionPreference = 'Stop'
if (-not $SourcePath) { $SourcePath = $env:OKUFLOW_PLATFORM_TOOLS_DIR }
if (-not $SourcePath) { $SourcePath = Join-Path $env:LOCALAPPDATA 'OkuFlow\adb-source-build\build-output' }
$source = (Resolve-Path -LiteralPath $SourcePath).Path
$bundle = (Resolve-Path -LiteralPath $BundlePath).Path
$pin = Get-Content (Join-Path $PSScriptRoot 'adb-source\distribution.json') -Raw | ConvertFrom-Json
$manifestPath = Join-Path $source 'platform-tools\adb-build.json'
if ((Get-FileHash -LiteralPath $manifestPath).Hash.ToLowerInvariant() -ne $pin.manifestSha256) {
    throw 'adb build provenance differs from the reviewed source-build pin.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if (-not $manifest.sourceBuilt -or $manifest.version -ne $pin.version -or $manifest.protocol -ne 41) {
    throw 'The selected adb output is not the approved source build.'
}
foreach ($required in @('platform-tools/adb.exe','platform-tools/AdbWinApi.dll',
        'platform-tools/AdbWinUsbApi.dll','platform-tools/NOTICE.txt','platform-tools/source.properties',
        'sources/adb-37.0.0-okuflow-source.zip','licenses/adb/LIBUSB_LGPL_2_1.txt')) {
    if ($required -notin $manifest.files.path) { throw "Missing required redistribution material: $required" }
}
$records = @($manifest.files) + [pscustomobject]@{path='platform-tools/adb-build.json';sha256=$pin.manifestSha256}
$seen = @{}
foreach ($record in $records) {
    $relative = [string]$record.path
    if ($relative -notmatch '^(platform-tools|licenses/adb|sources)/' -or
        $relative.Contains('..') -or $relative.Contains(':') -or $relative.Contains('\') -or
        $seen.ContainsKey($relative)) { throw "Invalid or duplicate manifest path: $relative" }
    $seen[$relative] = $true
    $input = Join-Path $source $relative
    $target = Join-Path $bundle $relative
    if ((Get-FileHash -LiteralPath $input).Hash.ToLowerInvariant() -ne $record.sha256) {
        throw "adb source-build file failed verification: $relative"
    }
    if (Test-Path -LiteralPath $target) { throw "Refusing to overwrite existing bundle file: $relative" }
}
$actual = @(Get-ChildItem $source -File -Recurse)
if ($actual.Count -ne $records.Count) { throw 'Unexpected or missing files in the adb build output.' }
foreach ($file in $actual) {
    $relative = $file.FullName.Substring($source.Length).TrimStart('\').Replace('\','/')
    if (-not $seen.ContainsKey($relative)) { throw "Unexpected adb output file: $relative" }
}
foreach ($record in $records) {
    $target = Join-Path $bundle $record.path
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $source $record.path) -Destination $target
    if ((Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant() -ne $record.sha256) {
        throw "Staged adb file failed verification: $($record.path)"
    }
}
Write-Host "Staged source-built adb $($manifest.version), dependency DLLs, notices and complete relinking source."
