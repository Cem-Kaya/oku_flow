#requires -Version 7.0
# Structural review package only; see docs/windows_distribution.md before upload.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+\.0$')][string]$Version,
    [Parameter(Mandatory)][switch]$ReviewOnly,
    [string]$BundlePath=(Join-Path $PSScriptRoot '..\dist\OkuFlow'),
    [string]$OutputDirectory=(Join-Path $PSScriptRoot '..\dist\Installers')
)
if(-not $ReviewOnly){throw 'Store runtime/certification acceptance is pending. Use -ReviewOnly; do not submit this package.'}
. (Join-Path $PSScriptRoot 'windows-installer\common.ps1')
$repository=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$identity=Get-Content (Join-Path $PSScriptRoot 'windows-installer\store-identity.json') -Raw | ConvertFrom-Json
$work=Join-Path $env:LOCALAPPDATA "OkuFlow\store-build\$Version-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work -Force | Out-Null
$snapshot=New-VerifiedBundleSnapshot $BundlePath (Join-Path $work 'payload')
$assets=Join-Path $snapshot.path 'StoreAssets'
New-Item -ItemType Directory -Path $assets | Out-Null
foreach($name in @('StoreLogo.png','Square44x44Logo.png','Square150x150Logo.png')) {
    Copy-Item -LiteralPath (Join-Path $repository "assets\store-icons\$name") -Destination (Join-Path $assets $name)
}
$manifest=@"
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
 xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
 xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
 IgnorableNamespaces="uap rescap">
 <Identity Name="$($identity.name)" Publisher="$($identity.publisher)" Version="$Version" ProcessorArchitecture="x64"/>
 <Properties><DisplayName>OkuFlow</DisplayName><PublisherDisplayName>$($identity.publisherDisplayName)</PublisherDisplayName><Logo>StoreAssets\StoreLogo.png</Logo><Description>AI-assisted camera magnifier for low vision.</Description></Properties>
 <Resources><Resource Language="en-US"/><Resource Language="tr-TR"/><Resource Language="de-DE"/></Resources>
 <Dependencies><TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.19041.0" MaxVersionTested="10.0.26100.0"/></Dependencies>
 <Applications><Application Id="OkuFlow" Executable="oku_flow.exe" EntryPoint="Windows.FullTrustApplication">
 <uap:VisualElements DisplayName="OkuFlow" Description="AI-assisted camera magnifier for low vision." BackgroundColor="transparent" Square44x44Logo="StoreAssets\Square44x44Logo.png" Square150x150Logo="StoreAssets\Square150x150Logo.png"/>
 </Application></Applications>
 <Capabilities><Capability Name="internetClient"/><rescap:Capability Name="runFullTrust"/><DeviceCapability Name="webcam"/><DeviceCapability Name="microphone"/></Capabilities>
</Package>
"@
$manifest | Set-Content -LiteralPath (Join-Path $snapshot.path 'AppxManifest.xml') -Encoding utf8NoBOM
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) "OkuFlow-$Version-x64-review.msix"
if(Test-Path -LiteralPath $output){throw 'Versioned review package already exists.'}
$makeappx=Get-WindowsSdkTool 'makeappx.exe'
& $makeappx pack /d $snapshot.path /p $output /o
if($LASTEXITCODE -ne 0){throw 'MSIX schema/package validation failed.'}
$unpack=Join-Path $work 'unpacked'
& $makeappx unpack /p $output /d $unpack
if($LASTEXITCODE -ne 0){throw 'MSIX round-trip extraction failed.'}
foreach($record in $snapshot.records){
    if((Get-FileHash -LiteralPath (Join-Path $unpack $record.path)).Hash.ToLowerInvariant() -ne $record.sha256){throw "MSIX payload changed: $($record.path)"}
}
[ordered]@{schema=1;version=$Version;sourceCommit=$snapshot.manifest.sourceCommit;identity=$identity;sha256=(Get-FileHash -LiteralPath $output).Hash.ToLowerInvariant();reviewOnly=$true;storeSubmitted=$false;remaining=@('Packaged runtime bootstrap without system-wide installer/UAC','Clean packaged camera/USB/AI/notes/update acceptance','Store listing, certification and rollout')} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$output.json" -Encoding utf8NoBOM
Write-Output "PASS: Store identity, MSIX schema and byte-identical payload. Review package only: $output"
