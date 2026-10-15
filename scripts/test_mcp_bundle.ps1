#requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-installer/mcp_bundle.ps1')
function Refused([scriptblock]$Action) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Invalid MCP release contract was accepted.' }
}
function Info([string]$Edition='v1',[object]$Policy=$true) {
    [pscustomobject]@{schema=1;product='OkuFlow MCP';protocol='2025-11-25';catalogEdition=$Edition;deployedImagePolicy=$Policy}
}
Assert-McpReleaseBuildInfo (Info)
foreach ($edition in @('dev-core','dev-capture','dev-camera','V1','unknown')) {
    Refused { Assert-McpReleaseBuildInfo (Info $edition) }
}
foreach ($policy in @($false,'true',1)) { Refused { Assert-McpReleaseBuildInfo (Info 'v1' $policy) } }
$extra = Info; $extra | Add-Member unexpected 'unsafe'
Refused { Assert-McpReleaseBuildInfo $extra }
$work = Join-Path $env:LOCALAPPDATA ('OkuFlow/mcp-bundle-fixtures/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$cache = Join-Path $work 'CMakeCache.txt'
foreach ($edition in @('dev-core','dev-capture','dev-camera','v1')) {
    foreach ($policy in @('OFF','ON')) {
        [IO.File]::WriteAllLines($cache,@("OKUFLOW_CONTROL_CATALOG_EDITION:STRING=$edition","OKUFLOW_MCP_DEPLOYED_IMAGE_POLICY:BOOL=$policy"))
        if ($edition -eq 'v1' -and $policy -eq 'ON') { Assert-McpReleaseCache $work }
        else { Refused { Assert-McpReleaseCache $work } }
    }
}
foreach ($prefix in @('','app/')) {
    $layout = [pscustomobject]@{Prefix=$prefix}
    $relative = $prefix + 'okuflow_mcp.exe'
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [void]$paths.Add($relative)
    $record = [pscustomobject]@{path=$relative;sha256=('1'*64)}
    $manifest = [pscustomobject]@{files=@($record);agentControl=[pscustomobject]@{
        path=$relative;sha256=('1'*64);buildInfo=(Info)}}
    Assert-McpBundleInventory $manifest $layout $paths
    $manifest.agentControl.sha256 = '2'*64
    Refused { Assert-McpBundleInventory $manifest $layout $paths }
    $manifest.agentControl.sha256 = '1'*64
    [void]$paths.Remove($relative)
    Refused { Assert-McpBundleInventory $manifest $layout $paths }
    [void]$paths.Add($relative)
    $manifest.agentControl.path = 'wrong/okuflow_mcp.exe'
    Refused { Assert-McpBundleInventory $manifest $layout $paths }
    $legacy = [pscustomobject]@{files=@()}
    Refused { Assert-McpBundleInventory $legacy $layout $paths }
    $paths.Clear()
    Assert-McpBundleInventory $legacy $layout $paths
}
Write-Output 'PASS: MCP V1/deployed-policy declarations, legacy/nested inventory, missing payload, undeclared payload and tampered hash rejection. Pure synthetic contracts; no V1 binary, app or desktop.'
