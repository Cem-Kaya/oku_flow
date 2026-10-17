#requires -Version 7.0
# Exercises the actual batch entrypoint without configure/build/publish.
$ErrorActionPreference = 'Stop'
$work = Join-Path $env:LOCALAPPDATA ('OkuFlow/bundle-toolchain-fixtures/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$batch = Join-Path $PSScriptRoot 'build_release_bundle.bat'
$repository = Split-Path $PSScriptRoot
$dist = Join-Path $repository 'dist'
$distExisted = Test-Path -LiteralPath $dist
$saved = @{}
foreach ($name in @('CMAKE_GENERATOR','CMAKE_ARGS','OKUFLOW_BUNDLE_BUILD_DIR')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
function Assert-Toolchain([bool]$Value,[string]$Message) { if (-not $Value) { throw $Message } }
function Invoke-Preflight([bool]$ExpectPass,[string]$ExpectedMessage) {
    $output = (& $env:ComSpec /d /c "call `"$batch`" --check-toolchain" 2>&1 | Out-String)
    $exitCode = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $work ([Guid]::NewGuid().ToString('N') + '.log'))
    Assert-Toolchain (($exitCode -eq 0) -eq $ExpectPass) "Unexpected preflight exit $exitCode : $output"
    Assert-Toolchain ($output.Contains($ExpectedMessage)) "Missing actionable preflight message '$ExpectedMessage': $output"
}
try {
    Remove-Item Env:CMAKE_GENERATOR -ErrorAction SilentlyContinue
    Remove-Item Env:CMAKE_ARGS -ErrorAction SilentlyContinue
    $env:OKUFLOW_BUNDLE_BUILD_DIR = Join-Path $work 'never-created-build'
    Invoke-Preflight $true 'No configuration, compilation or dist mutation'
    Assert-Toolchain (-not (Test-Path -LiteralPath $env:OKUFLOW_BUNDLE_BUILD_DIR)) 'Check-only created a build directory.'
    foreach ($generator in @('Visual Studio 17 2022','Ninja Multi-Config','Unix Makefiles')) {
        $env:CMAKE_GENERATOR = $generator
        Invoke-Preflight $false 'Release bundles require Ninja'
        Assert-Toolchain (-not (Test-Path -LiteralPath $env:OKUFLOW_BUNDLE_BUILD_DIR)) 'Generator refusal created a cache.'
    }
    $env:CMAKE_GENERATOR = 'Ninja'
    $env:CMAKE_ARGS = '-G "Visual Studio 17 2022"'
    Invoke-Preflight $false 'not CMAKE_ARGS'
    Remove-Item Env:CMAKE_ARGS
    $cacheRoot = Join-Path $work 'stale-cache'
    New-Item -ItemType Directory -Path $cacheRoot | Out-Null
    $cache = Join-Path $cacheRoot 'CMakeCache.txt'
    [IO.File]::WriteAllText($cache,"CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022`nCMAKE_HOME_DIRECTORY:INTERNAL=$repository`nCMAKE_BUILD_TYPE:STRING=Release`n")
    $before = (Get-FileHash -LiteralPath $cache).Hash
    $env:OKUFLOW_BUNDLE_BUILD_DIR = $cacheRoot
    Invoke-Preflight $false 'OKUFLOW_BUNDLE_BUILD_DIR'
    Assert-Toolchain ((Get-FileHash -LiteralPath $cache).Hash -eq $before) 'Stale-cache refusal changed cached data.'
    Assert-Toolchain ((Test-Path -LiteralPath $dist) -eq $distExisted) 'Check-only changed dist existence.'
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
Write-Output "PASS: actual batch Ninja/VS2022 preflight, unsupported generator and override refusals, stale-cache preservation; no build/publish. Private logs: $work"
