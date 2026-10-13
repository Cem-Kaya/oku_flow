#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$BundlePath)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $BundlePath).Path
$payload = [IO.Path]::GetFullPath((Join-Path $root 'app'))
$rootPrefix = $root.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if (-not $payload.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Runtime destination escaped the staging directory.'
}
foreach ($reserved in @('app','output','captures','release-manifest.json','SHA256SUMS.txt','SBOM.spdx.json')) {
    if (Test-Path -LiteralPath (Join-Path $root $reserved)) {
        throw 'Use a fresh staging directory without captures or existing release metadata.'
    }
}
$entries = @(Get-ChildItem -LiteralPath $root -Force)
if (@(Get-ChildItem -LiteralPath $root -Recurse -Force | Where-Object {
    $_.Attributes -band [IO.FileAttributes]::ReparsePoint
}).Count -or (Get-Item -LiteralPath $root).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Staging directory must not contain reparse points.'
}
foreach ($required in @('oku_flow.exe','OkuFlowApp.exe','Qt6Core.dll','START_HERE.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $required) -PathType Leaf)) {
        throw "Incomplete flat staging payload: $required"
    }
}
New-Item -ItemType Directory -Path $payload | Out-Null
$visible = @('oku_flow.exe','START_HERE.txt','README.txt','LICENSE','COMMERCIAL.md','THIRD_PARTY_LICENSES.md')
foreach ($entry in $entries) {
    if ($entry.Name -notin $visible) {
        Move-Item -LiteralPath $entry.FullName -Destination (Join-Path $payload $entry.Name)
    }
}
Write-Output 'PASS: one public launcher; supporting application/runtime files are inside app/.'
