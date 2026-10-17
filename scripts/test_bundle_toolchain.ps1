#requires -Version 7.0
# Exercises the actual batch entrypoint without configure/build/publish.
$ErrorActionPreference = 'Stop'
$work = Join-Path $env:LOCALAPPDATA ('OkuFlow/bundle-toolchain-fixtures/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$batch = Join-Path $PSScriptRoot 'build_release_bundle.bat'
$checker = Join-Path $PSScriptRoot 'check_bundle_toolchain.ps1'
$repository = Split-Path $PSScriptRoot
$dist = Join-Path $repository 'dist'
$distExisted = Test-Path -LiteralPath $dist
$saved = @{}
foreach ($name in @('CMAKE_GENERATOR','CMAKE_ARGS','OKUFLOW_BUNDLE_BUILD_DIR',
    'VSCMD_VER','VisualStudioVersion','VSCMD_ARG_TGT_ARCH','VSCMD_ARG_HOST_ARCH','VSINSTALLDIR')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
}
$initializationMarker = 'Initializing Visual Studio 2022 x64 toolchain.'
$script:initializationsObserved = 0
function Assert-Toolchain([bool]$Value,[string]$Message) { if (-not $Value) { throw $Message } }
function Invoke-Preflight([string]$Case,[bool]$ExpectPass,[string]$ExpectedMessage,[bool]$ExpectInitialization = $false) {
    Write-Output "BEGIN bundle preflight: $Case"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $output = (& $env:ComSpec /d /c "call `"$batch`" --check-toolchain" 2>&1 | Out-String)
    $exitCode = $LASTEXITCODE
    $watch.Stop()
    $output | Set-Content -LiteralPath (Join-Path $work ($Case + '.log'))
    $initializations = [regex]::Matches($output,[regex]::Escape($initializationMarker)).Count
    $script:initializationsObserved += $initializations
    Assert-Toolchain ($initializations -eq [int]$ExpectInitialization) "Unexpected VS initialization count in $Case : $output"
    Assert-Toolchain (($exitCode -eq 0) -eq $ExpectPass) "Unexpected preflight exit $exitCode : $output"
    Assert-Toolchain ($output.Contains($ExpectedMessage)) "Missing actionable preflight message '$ExpectedMessage': $output"
    Write-Output ("PASS bundle preflight: {0} ({1:F2} s)" -f $Case,$watch.Elapsed.TotalSeconds)
}
try {
    Remove-Item Env:CMAKE_GENERATOR -ErrorAction SilentlyContinue
    Remove-Item Env:CMAKE_ARGS -ErrorAction SilentlyContinue
    $env:OKUFLOW_BUNDLE_BUILD_DIR = Join-Path $work 'never-created-build'
    # Real tools remain untouched. These forged declarations must not certify
    # a compiler just because an arbitrary drive root contains its PATH image.
    $env:VSCMD_VER = '17.0'
    $env:VisualStudioVersion = '17.0'
    $env:VSCMD_ARG_TGT_ARCH = 'x64'
    $env:VSCMD_ARG_HOST_ARCH = 'x64'
    $env:VSINSTALLDIR = [IO.Path]::GetPathRoot(${env:ProgramFiles(x86)})
    Write-Output 'BEGIN bundle preflight: reject-forged-environment'
    $claimWatch = [Diagnostics.Stopwatch]::StartNew()
    $configurationOutput = (& $checker -RepositoryPath $repository -BuildDirectory $env:OKUFLOW_BUNDLE_BUILD_DIR -ConfigurationOnly | Out-String)
    Assert-Toolchain ($configurationOutput.Contains('configuration only')) 'Configuration-only check did not identify its limited scope.'
    Assert-Toolchain (-not $configurationOutput.Contains('Ninja, VS2022 x64 tools')) 'Configuration-only mode certified a claimed toolchain.'
    $spoofFailure = ''
    try { & $checker -RepositoryPath $repository -BuildDirectory $env:OKUFLOW_BUNDLE_BUILD_DIR | Out-Null }
    catch { $spoofFailure = $_.Exception.Message }
    Assert-Toolchain ($spoofFailure.Contains('not a registered Visual Studio 2022')) "A forged VS environment claim was not independently refused: $spoofFailure"
    $claimWatch.Stop()
    Write-Output ("PASS bundle preflight: reject-forged-environment ({0:F2} s)" -f $claimWatch.Elapsed.TotalSeconds)
    # The actual batch must initialize the installed shell even with those
    # same forged declarations, then validate its real registered compiler.
    Invoke-Preflight 'default-real-toolchain' $true 'No configuration, compilation or dist mutation' $true
    Assert-Toolchain (-not (Test-Path -LiteralPath $env:OKUFLOW_BUNDLE_BUILD_DIR)) 'Check-only created a build directory.'
    $caseIndex = 0
    foreach ($generator in @('Visual Studio 17 2022','Ninja Multi-Config','Unix Makefiles')) {
        $env:CMAKE_GENERATOR = $generator
        Invoke-Preflight ('unsupported-generator-' + $caseIndex++) $false 'Release bundles require Ninja'
        Assert-Toolchain (-not (Test-Path -LiteralPath $env:OKUFLOW_BUNDLE_BUILD_DIR)) 'Generator refusal created a cache.'
    }
    $env:CMAKE_GENERATOR = 'Ninja'
    $env:CMAKE_ARGS = '-G "Visual Studio 17 2022"'
    Invoke-Preflight 'generator-argument' $false 'not CMAKE_ARGS'
    Remove-Item Env:CMAKE_ARGS
    $cacheRoot = Join-Path $work 'stale-cache'
    New-Item -ItemType Directory -Path $cacheRoot | Out-Null
    $cache = Join-Path $cacheRoot 'CMakeCache.txt'
    [IO.File]::WriteAllText($cache,"CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022`nCMAKE_HOME_DIRECTORY:INTERNAL=$repository`nCMAKE_BUILD_TYPE:STRING=Release`n")
    $before = (Get-FileHash -LiteralPath $cache).Hash
    $env:OKUFLOW_BUNDLE_BUILD_DIR = $cacheRoot
    Invoke-Preflight 'stale-cache' $false 'OKUFLOW_BUNDLE_BUILD_DIR'
    Assert-Toolchain ((Get-FileHash -LiteralPath $cache).Hash -eq $before) 'Stale-cache refusal changed cached data.'
    Assert-Toolchain ((Test-Path -LiteralPath $dist) -eq $distExisted) 'Check-only changed dist existence.'
    Assert-Toolchain ($script:initializationsObserved -eq 1) 'The fixture did not preserve exactly one real VS initialization.'
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
Write-Output "PASS: one real batch Ninja/VS2022 initialization, forged environment refusal, early generator/override/cache refusals and stale-cache preservation; no build/publish. Private logs: $work"
