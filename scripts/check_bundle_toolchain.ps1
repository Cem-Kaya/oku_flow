#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RepositoryPath,
    [Parameter(Mandatory)][string]$BuildDirectory
)
$ErrorActionPreference = 'Stop'
if ($env:CMAKE_ARGS -match '(?i)(^|\s|\")(-G|--generator)(\S|\s|$)') {
    throw 'Select the required Ninja generator through CMAKE_GENERATOR, not CMAKE_ARGS. Set OKUFLOW_BUNDLE_BUILD_DIR to a fresh directory when changing generators; caches are never deleted automatically.'
}
if ($env:VSCMD_VER -notmatch '^17\.' -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -ne 'x64') {
    throw 'Initialize the Visual Studio 2022 x64 developer environment before bundle validation.'
}
foreach ($tool in @('cmake.exe','ninja.exe','cl.exe')) {
    if (-not (Get-Command $tool -CommandType Application -ErrorAction SilentlyContinue)) {
        throw "Required bundle tool is unavailable after VS2022 initialization: $tool"
    }
}
$compiler = (Get-Command cl.exe -CommandType Application).Source
if (-not $env:VSINSTALLDIR -or -not $compiler.StartsWith(
    [IO.Path]::GetFullPath($env:VSINSTALLDIR).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar,
    [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The active compiler is outside the selected Visual Studio 2022 installation.'
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
Write-Output 'PASS: Ninja, VS2022 x64 tools and existing bundle cache preflight. No configuration, compilation or dist mutation was performed.'
