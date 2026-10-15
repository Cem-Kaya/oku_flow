#Requires -Version 7.0
[CmdletBinding()]
param([string]$EnvironmentFile = $env:GITHUB_ENV)

$ErrorActionPreference = 'Stop'
if (!$IsWindows) { throw 'Visual Studio setup requires Windows.' }
if (!$EnvironmentFile) { throw 'An Actions environment file or explicit -EnvironmentFile is required.' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$installation) { throw 'Visual Studio 2022 with the x64 C++ tools is required.' }
$installation = $installation.Trim()
$before = @{}
Get-ChildItem Env: | ForEach-Object { $before[$_.Name] = $_.Value }

# Use Microsoft's installed shell directly, without a JavaScript setup action.
# VsDevCmd also invokes vswhere by name; do not depend on current-directory search.
$env:PATH = (Split-Path $vswhere -Parent) + ';' + $env:PATH
& (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -VsInstallationPath $installation -VsWherePath $vswhere -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
if ($env:VisualStudioVersion -notlike '17.*' -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -ne 'x64') {
    throw 'The developer shell did not select Visual Studio 2022 x64 host/target.'
}
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
if (!$compiler.StartsWith($installation + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The selected compiler is outside the discovered Visual Studio 2022 installation.'
}

# Each Actions step has a fresh shell. Preserve only developer-shell changes.
$lines = [Collections.Generic.List[string]]::new()
foreach ($entry in (Get-ChildItem Env: | Sort-Object Name)) {
    if (!$before.ContainsKey($entry.Name) -or $before[$entry.Name] -cne $entry.Value) {
        $delimiter = 'OKUFLOW_ENV_' + [Guid]::NewGuid().ToString('N')
        $lines.Add($entry.Name + '<<' + $delimiter)
        $lines.Add($entry.Value)
        $lines.Add($delimiter)
    }
}
[IO.File]::AppendAllLines($EnvironmentFile, $lines, [Text.UTF8Encoding]::new($false))
Write-Host "Visual Studio $env:VisualStudioVersion x64: $compiler"
