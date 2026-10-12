#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$BundlePath, [string]$InstallerPath)
$ErrorActionPreference = 'Stop'
$bundle = (Resolve-Path -LiteralPath $BundlePath).Path
$pinRoot = Join-Path $PSScriptRoot 'vc-runtime'
$pin = Get-Content (Join-Path $pinRoot 'pins.json') -Raw | ConvertFrom-Json
$target = Join-Path $bundle $pin.installer.name
if (-not $InstallerPath) { $InstallerPath = $target }
if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    if ($env:VCToolsRedistDir) { $InstallerPath = Join-Path $env:VCToolsRedistDir $pin.installer.name }
}
if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    throw 'Microsoft runtime installer is missing from Qt deployment/Visual Studio redistributables.'
}
$installer = (Resolve-Path -LiteralPath $InstallerPath).Path
$signature = Get-AuthenticodeSignature -LiteralPath $installer
if ((Get-FileHash $installer).Hash.ToLowerInvariant() -ne $pin.installer.sha256 -or
    $signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=Microsoft Corporation(?:,|$)') {
    throw 'Microsoft runtime installer differs from the reviewed signed runtime pin.'
}
$license = Join-Path $pinRoot 'LICENSE.rtf'
if ((Get-FileHash $license).Hash.ToLowerInvariant() -ne $pin.licenseSha256) { throw 'Microsoft runtime license differs from the reviewed pin.' }
if ($installer -ne $target) { Copy-Item -LiteralPath $installer -Destination $target -Force }
New-Item -ItemType Directory -Path (Join-Path $bundle 'licenses') -Force | Out-Null
Copy-Item -LiteralPath $license -Destination (Join-Path $bundle 'licenses\MICROSOFT_VC_RUNTIME_LICENSE.rtf') -Force
if ((Get-FileHash $target).Hash.ToLowerInvariant() -ne $pin.installer.sha256) { throw 'Staged Microsoft installer hash mismatch.' }
Write-Host "Staged verified Microsoft runtime installer $($pin.version) and its exact license. No app-local runtime DLLs are copied."
