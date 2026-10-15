# First-party release contract. Development catalogs never become packages.
function Assert-McpReleaseBuildInfo($Info) {
    $keys = @($Info.PSObject.Properties.Name | Sort-Object)
    if (($keys -join ',') -ne 'catalogEdition,deployedImagePolicy,product,protocol,schema' -or
        $Info.schema -ne 1 -or $Info.product -cne 'OkuFlow MCP' -or
        $Info.protocol -cne '2025-11-25' -or $Info.catalogEdition -cne 'v1' -or
        $Info.deployedImagePolicy -isnot [bool] -or -not $Info.deployedImagePolicy) {
        throw 'MCP packaging requires the accepted V1 catalog and deployed application identity policy.'
    }
}

function Get-McpBuildInfo([string]$Executable) {
    $file = Get-Item -LiteralPath $Executable
    if ($file.PSIsContainer -or $file.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'MCP build diagnostics require an owned regular executable.'
    }
    $start = [Diagnostics.ProcessStartInfo]::new($file.FullName)
    $start.ArgumentList.Add('--build-info')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'MCP diagnostic child did not start.' }
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(2000)) {
            $process.Kill($true)
            throw 'MCP build diagnostics timed out.'
        }
        $text = $output.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0 -or $text.Length -gt 1024 -or
            $errors.GetAwaiter().GetResult().Length -ne 0) { throw 'Invalid MCP build diagnostics.' }
        $text | ConvertFrom-Json -Depth 4
    } finally { $process.Dispose() }
}

function Assert-McpReleaseCache([string]$BuildDirectory) {
    $cache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt')
    foreach ($expected in @('OKUFLOW_CONTROL_CATALOG_EDITION:STRING=v1',
                           'OKUFLOW_MCP_DEPLOYED_IMAGE_POLICY:BOOL=ON')) {
        if ($expected -cnotin $cache) { throw "Release build prerequisite missing: $expected" }
    }
}
function Get-McpReleaseBuild([string]$BuildDirectory,[string]$Executable) {
    Assert-McpReleaseCache $BuildDirectory
    $info = Get-McpBuildInfo $Executable
    Assert-McpReleaseBuildInfo $info
    Assert-McpNativeImports $Executable
    return $info
}

function Assert-McpNativeImports([string]$Executable) {
    # Select the same VS2022 family as tracked builds; never install a tool.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    # -all permits this read-only utility during a VS update pending reboot;
    # it does not replace the tracked build's toolchain discovery/validation.
    $installation = & $vswhere -all -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'VS2022 native import validation tool is unavailable.' }
    $version = (Get-Content -LiteralPath (Join-Path $installation 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
    $dumpbin = Join-Path $installation "VC/Tools/MSVC/$version/bin/Hostx64/x64/dumpbin.exe"
    $imports = @(& $dumpbin /nologo /dependents $Executable)
    if ($LASTEXITCODE -ne 0) { throw 'MCP native import validation failed.' }
    $libraries = @($imports | ForEach-Object {
        if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $Matches[1].ToLowerInvariant() }
    } | Sort-Object -Unique)
    $allowed = @('advapi32.dll','bcrypt.dll','shell32.dll','ole32.dll','kernel32.dll')
    if ($libraries.Count -eq 0 -or 'kernel32.dll' -notin $libraries -or
        @($libraries | Where-Object {$_ -notin $allowed}).Count -ne 0) {
        throw 'MCP bridge must import only its reviewed Windows libraries, with no Qt or dynamic VC runtime.'
    }
}

function Assert-McpBundleInventory($Manifest,$Layout,$Paths) {
    $property = $Manifest.PSObject.Properties['agentControl']
    $relative = $Layout.Prefix + 'okuflow_mcp.exe'
    if (-not $property) {
        if ($Paths.Contains($relative)) { throw 'MCP executable has no declared release contract.' }
        return # Previously published non-MCP bundles remain readable.
    }
    $control = $property.Value
    Assert-McpReleaseBuildInfo $control.buildInfo
    if ($control.path -cne $relative -or -not $Paths.Contains($relative)) {
        throw 'MCP release inventory is missing or in the wrong payload directory.'
    }
    $record = @($Manifest.files | Where-Object {$_.path -ceq $relative})
    if ($record.Count -ne 1 -or $record[0].sha256 -cne $control.sha256 -or
        $control.sha256 -cnotmatch '^[a-f0-9]{64}$') { throw 'MCP release hash declaration does not match inventory.' }
}
