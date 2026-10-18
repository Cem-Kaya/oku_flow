#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RepositoryPath,
    [Parameter(Mandatory)][string]$BuildDirectory,
    [switch]$ConfigurationOnly
)
$ErrorActionPreference = 'Stop'
if ($env:CMAKE_GENERATOR -and $env:CMAKE_GENERATOR -ne 'Ninja') {
    throw 'Release bundles require Ninja. Select a fresh OKUFLOW_BUNDLE_BUILD_DIR when changing generators; existing caches are never deleted automatically.'
}
if ($env:CMAKE_ARGS -match '(?i)(^|\s|\")(-G|--generator)(\S|\s|$)') {
    throw 'Select the required Ninja generator through CMAKE_GENERATOR, not CMAKE_ARGS. Set OKUFLOW_BUNDLE_BUILD_DIR to a fresh directory when changing generators; caches are never deleted automatically.'
}
$cachePath = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath) {
    $cache = @{}
    foreach ($line in Get-Content -LiteralPath $cachePath) {
        if ($line -match '^([^#/:][^:]*):[^=]*=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
    }
    $source = [IO.Path]::GetFullPath($RepositoryPath).TrimEnd('\','/')
    if ($cache.CMAKE_GENERATOR -ne 'Ninja' -or
        -not $cache.CMAKE_HOME_DIRECTORY -or
        [IO.Path]::GetFullPath($cache.CMAKE_HOME_DIRECTORY).TrimEnd('\','/') -ne $source -or
        $cache.CMAKE_BUILD_TYPE -ne 'Release') {
        throw 'Bundle cache is incompatible with Ninja/Release or this checkout. Set OKUFLOW_BUNDLE_BUILD_DIR to a fresh directory; existing caches are never deleted automatically.'
    }
}
if ($ConfigurationOnly) {
    Write-Output 'PASS: bundle configuration only; toolchain identity and Qt readiness remain unchecked.'
    return
}
if ($env:VSCMD_VER -notmatch '^17\.' -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -ne 'x64') {
    throw 'Initialize the Visual Studio 2022 x64 developer environment before bundle validation.'
}
# Environment claims alone are not compiler identity. Independently confirm
# that the selected root is an installed 17.x C++ instance before trusting PATH.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio Installer vswhere.exe is required to verify the selected VS2022 installation.'
}
$installations = @(& $vswhere -version '[17.0,18.0)' -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if ($LASTEXITCODE -ne 0) { throw 'Installed Visual Studio 2022 identity discovery failed.' }
$selectedRoot = if ($env:VSINSTALLDIR) { [IO.Path]::GetFullPath($env:VSINSTALLDIR).TrimEnd('\','/') } else { '' }
$registeredRoots = @($installations | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object {
    [IO.Path]::GetFullPath($_.Trim()).TrimEnd('\','/')
})
if (-not $selectedRoot -or $selectedRoot -notin $registeredRoots) {
    throw 'The claimed environment root is not a registered Visual Studio 2022 C++ installation.'
}
foreach ($tool in @('cmake.exe','ninja.exe','cl.exe')) {
    if (-not (Get-Command $tool -CommandType Application -ErrorAction SilentlyContinue)) {
        throw "Required bundle tool is unavailable after VS2022 initialization: $tool"
    }
}
$compiler = (Get-Command cl.exe -CommandType Application).Source
$compilerRelative = [IO.Path]::GetRelativePath($selectedRoot,$compiler)
if ($compilerRelative -notmatch '^VC[\\/]Tools[\\/]MSVC[\\/][^\\/]+[\\/]bin[\\/]Hostx64[\\/]x64[\\/]cl\.exe$') {
    throw 'The active compiler is outside the selected Visual Studio 2022 x64 host/target toolchain.'
}
Write-Output 'PASS: Ninja, VS2022 x64 tools and existing bundle cache preflight. No configuration, compilation or dist mutation was performed.'
