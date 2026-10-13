#requires -Version 7.0
# Desktop-free synthetic payload fixtures. These are not release artifacts.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-installer/common.ps1')
$work = Join-Path $env:LOCALAPPDATA ('OkuFlow/bundle-layout-fixtures/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$names = @('oku_flow.exe','OkuFlowApp.exe','Qt6Core.dll','Qt6Gui.dll','platforms/qwindows.dll',
    'platform-tools/adb.exe','sources/adb-37.0.0-okuflow-source.zip',
    'licenses/MICROSOFT_VC_RUNTIME_LICENSE.rtf','vc_redist.x64.exe','mf_dxva_minimal.exe',
    'START_HERE.txt','README.txt','LICENSE','COMMERCIAL.md','THIRD_PARTY_LICENSES.md')
function New-Fixture([string]$Name) {
    $root = Join-Path $work $Name
    foreach ($relative in $names) {
        $path = Join-Path $root $relative
        New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, "synthetic fixture: $relative", [Text.UTF8Encoding]::new($false))
    }
    return $root
}
function Assert-Layout([bool]$Condition,[string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Assert-Refused([scriptblock]$Action,[string]$Message) {
    $refused = $false
    try { & $Action | Out-Null } catch { $refused = $true }
    Assert-Layout $refused $Message
}
function Write-FixtureManifest([string]$Root) {
    # Explicitly fake provenance for the snapshot contract, never publication.
    $records = @(Get-ChildItem -LiteralPath $Root -File -Recurse | ForEach-Object {
        [ordered]@{path=[IO.Path]::GetRelativePath($Root,$_.FullName).Replace('\','/');
            bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
    })
    [ordered]@{schemaVersion=2;sourceTreeDirty=$false;sourceCommit=('0'*40);files=$records} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Root 'release-manifest.json')
    foreach ($name in @('SHA256SUMS.txt','SBOM.spdx.json')) {
        [IO.File]::WriteAllText((Join-Path $Root $name), 'synthetic metadata')
    }
}
$legacy = New-Fixture 'legacy'
Write-FixtureManifest $legacy
$legacySnapshot = New-VerifiedBundleSnapshot $legacy (Join-Path $work 'legacy-snapshot')
Assert-Layout ($legacySnapshot.layout.Prefix -eq '') 'Legacy snapshot picked a nested payload.'

$nested = New-Fixture 'nested'
$before = @{}
foreach ($name in $names) { $before[$name] = (Get-FileHash -LiteralPath (Join-Path $nested $name)).Hash }
& (Join-Path $PSScriptRoot 'organize_release_bundle.ps1') -BundlePath $nested
$layout = Get-OkuFlowBundleLayout $nested
Assert-Layout ($layout.ApplicationRelativePath -eq 'app/OkuFlowApp.exe') 'Wrong nested application path.'
Assert-Layout (@(Get-ChildItem -LiteralPath $nested -File -Filter '*.exe').Count -eq 1) 'More than one top-level EXE.'
foreach ($name in $names) {
    $path = if ($name -in @('oku_flow.exe','START_HERE.txt','README.txt','LICENSE','COMMERCIAL.md','THIRD_PARTY_LICENSES.md')) {
        Join-Path $nested $name
    } else { Join-Path $layout.PayloadRoot $name }
    Assert-Layout ((Get-FileHash -LiteralPath $path).Hash -eq $before[$name]) "Moved bytes changed: $name"
}
Assert-Refused { & (Join-Path $PSScriptRoot 'organize_release_bundle.ps1') -BundlePath $nested } 'Repeated organization must refuse.'
Write-FixtureManifest $nested
$snapshot = New-VerifiedBundleSnapshot $nested (Join-Path $work 'nested-snapshot')
Assert-Layout ($snapshot.layout.Prefix -eq 'app/') 'Nested snapshot lost its payload prefix.'
[IO.File]::AppendAllText((Join-Path $nested 'app/Qt6Core.dll'), 'tampered')
Assert-Refused { New-VerifiedBundleSnapshot $nested (Join-Path $work 'tampered-snapshot') } 'Tampered runtime accepted.'

foreach ($reserved in @('output','captures','app','release-manifest.json')) {
    $root = New-Fixture ('reserved-' + $reserved.Replace('.','-'))
    [IO.File]::WriteAllText((Join-Path $root $reserved), 'preserve fixture data')
    Assert-Refused { & (Join-Path $PSScriptRoot 'organize_release_bundle.ps1') -BundlePath $root } "Reserved path accepted: $reserved"
    Assert-Layout ((Get-Content -LiteralPath (Join-Path $root $reserved) -Raw) -eq 'preserve fixture data') 'Reserved bytes changed.'
    Assert-Layout (Test-Path -LiteralPath (Join-Path $root 'OkuFlowApp.exe')) 'Refusal moved the app.'
}
Write-Output "PASS: legacy/nested snapshot integrity, one public EXE, byte preservation, tamper rejection and owner-data refusals. Synthetic fixtures: $work"
