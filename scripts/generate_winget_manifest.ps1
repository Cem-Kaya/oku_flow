#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallerPath,
    [Parameter(Mandatory)][ValidatePattern('^https://github\.com/Cem-Kaya/oku_flow/releases/download/[^/]+/[^/]+\.msi$')][string]$InstallerUrl,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$receipt=Get-Content -LiteralPath "$InstallerPath.json" -Raw | ConvertFrom-Json
$hash=(Get-FileHash -LiteralPath $InstallerPath).Hash
if($hash.ToLowerInvariant() -ne $receipt.sha256){throw 'Installer no longer matches its build receipt.'}
if([Uri]::UnescapeDataString(([Uri]$InstallerUrl).Segments[-1]) -ne (Split-Path $InstallerPath -Leaf)){throw 'Installer URL filename differs from the actual package.'}
$version=$receipt.version
$target=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) $version
New-Item -ItemType Directory -Path $target -Force | Out-Null
$common="PackageIdentifier: OkuFlow.OkuFlow`nPackageVersion: $version`nManifestVersion: 1.9.0"
$installer=@"
# Draft: validate and submit to microsoft/winget-pkgs after installer acceptance.
$common
InstallerType: wix
Scope: machine
InstallModes:
- interactive
- silent
- silentWithProgress
UpgradeBehavior: install
AppsAndFeaturesEntries:
- DisplayName: OkuFlow
  Publisher: OkuFlow
  DisplayVersion: $version
  ProductCode: '$($receipt.productCode)'
Installers:
- Architecture: x64
  InstallerUrl: $InstallerUrl
  InstallerSha256: $hash
  ProductCode: '$($receipt.productCode)'
ManifestType: installer
"@
$locale=@"
$common
PackageLocale: en-US
Publisher: OkuFlow
PublisherUrl: https://okuflow.com/
PublisherSupportUrl: https://github.com/Cem-Kaya/oku_flow/issues
PackageName: OkuFlow
PackageUrl: https://okuflow.com/
License: GPL-3.0-only OR commercial
LicenseUrl: https://github.com/Cem-Kaya/oku_flow/blob/main/LICENSE
ShortDescription: AI-assisted camera magnifier for people with low vision.
Description: 'Windows camera magnifier with NVIDIA CUDA image processing, local recording and optional AI reading/explanations. A supported NVIDIA GPU enables processing; AI providers require separate configuration. The USB Android companion is optional.'
Tags:
- accessibility
- magnifier
- low-vision
ManifestType: defaultLocale
"@
$versionManifest="$common`nDefaultLocale: en-US`nManifestType: version`n"
$installer | Set-Content -LiteralPath (Join-Path $target 'OkuFlow.OkuFlow.installer.yaml') -Encoding utf8NoBOM
$locale | Set-Content -LiteralPath (Join-Path $target 'OkuFlow.OkuFlow.locale.en-US.yaml') -Encoding utf8NoBOM
$versionManifest | Set-Content -LiteralPath (Join-Path $target 'OkuFlow.OkuFlow.yaml') -Encoding utf8NoBOM
Write-Output "Draft winget manifests: $target (not submitted to the catalog)"
