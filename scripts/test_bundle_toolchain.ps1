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
    'QT_PREFIX','Qt6_DIR','QT_ROOT_DIR','QT_SOURCE_ROOT',
    'VSCMD_VER','VisualStudioVersion','VSCMD_ARG_TGT_ARCH','VSCMD_ARG_HOST_ARCH','VSINSTALLDIR',
    'PATH','__VSCMD_PREINIT_PATH')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
}
$initializationMarker = 'Initializing Visual Studio 2022 x64 toolchain.'
$script:initializationsObserved = 0
function Assert-Toolchain([bool]$Value,[string]$Message) { if (-not $Value) { throw $Message } }
function Invoke-Preflight([string]$Case,[bool]$ExpectPass,[string]$ExpectedMessage,[bool]$ExpectInitialization = $false) {
    Write-Output "BEGIN bundle preflight: $Case"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    # Stream the real child while retaining exact case output. CTest must see
    # the last completed setup stage even if it terminates this fixture.
    $caseLog = Join-Path $work ($Case + '.log')
    & $env:ComSpec /d /c "call `"$batch`" --check-toolchain" 2>&1 |
        Tee-Object -FilePath $caseLog | ForEach-Object { Write-Host $_ }
    $exitCode = $LASTEXITCODE
    $watch.Stop()
    $output = Get-Content -LiteralPath $caseLog -Raw
    $initializations = [regex]::Matches($output,[regex]::Escape($initializationMarker)).Count
    $script:initializationsObserved += $initializations
    Assert-Toolchain ($initializations -eq [int]$ExpectInitialization) "Unexpected VS initialization count in $Case : $output"
    Assert-Toolchain (($exitCode -eq 0) -eq $ExpectPass) "Unexpected preflight exit $exitCode : $output"
    Assert-Toolchain ($output.Contains($ExpectedMessage)) "Missing actionable preflight message '$ExpectedMessage': $output"
    Write-Output ("PASS bundle preflight: {0} ({1:F2} s)" -f $Case,$watch.Elapsed.TotalSeconds)
}
try {
    # CTest supplies the SDK it actually configured. Expose its unchanged
    # runtime through owned junctions, omitting only the optional installer
    # license directory to exercise an archive-installed SDK. The matching
    # source tree must supply the real LGPL notice; never fabricate one.
    Assert-Toolchain (-not [string]::IsNullOrWhiteSpace($env:QT_ROOT_DIR)) 'CTest must supply the configured QT_ROOT_DIR.'
    $configuredQt = [IO.Path]::GetFullPath($env:QT_ROOT_DIR)
    $relocatedQt = Join-Path $work 'relocated Qt SDK'
    New-Item -ItemType Directory -Path $relocatedQt | Out-Null
    foreach ($entry in Get-ChildItem -LiteralPath $configuredQt -Force) {
        if ($entry.Name -ieq 'LICENSES') { continue }
        $destination = Join-Path $relocatedQt $entry.Name
        if ($entry.PSIsContainer) {
            New-Item -ItemType Junction -Path $destination -Target $entry.FullName | Out-Null
        } else {
            Copy-Item -LiteralPath $entry.FullName -Destination $destination
            Assert-Toolchain ((Get-FileHash -LiteralPath $destination).Hash -eq (Get-FileHash -LiteralPath $entry.FullName).Hash) 'Relocated SDK file bytes changed.'
        }
    }
    if (-not $env:QT_SOURCE_ROOT) { $env:QT_SOURCE_ROOT = [IO.Path]::GetFullPath((Join-Path $configuredQt '../Src')) }
    $env:QT_ROOT_DIR = $relocatedQt
    Remove-Item Env:QT_PREFIX -ErrorAction SilentlyContinue
    Remove-Item Env:Qt6_DIR -ErrorAction SilentlyContinue
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
    # Exercise nested developer-shell PATH growth without crossing cmd's entry
    # limit. The real initializer must restore its pre-initialization baseline.
    $baseline = $saved['__VSCMD_PREINIT_PATH']
    if ([string]::IsNullOrWhiteSpace($baseline)) { $baseline = $saved['PATH'] }
    $env:__VSCMD_PREINIT_PATH = $baseline
    $stressSegment = (Join-Path $configuredQt 'bin') + ';'
    $stress = $baseline
    while ($stress.Length + $stressSegment.Length -le 7000) { $stress = $stressSegment + $stress }
    $env:PATH = $stress
    Write-Output "Nested developer-shell PATH fixture length: $($stress.Length)."
    Invoke-Preflight 'default-real-toolchain' $true "Using Qt deployment prefix: `"$relocatedQt`"" $true
    $expectedLicense = Join-Path $env:QT_SOURCE_ROOT 'qtmultimedia/LICENSES/LGPL-3.0-only.txt'
    $positiveOutput = Get-Content -LiteralPath (Join-Path $work 'default-real-toolchain.log') -Raw
    Assert-Toolchain ($positiveOutput.Contains("Using Qt license text: `"$([IO.Path]::GetFullPath($expectedLicense))`"")) 'Archive SDK did not select its matching source LGPL notice.'
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
