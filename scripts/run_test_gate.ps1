#Requires -Version 7.0
[CmdletBinding(DefaultParameterSetName = 'Build')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Build')][string]$BuildDirectory,
    [Parameter(Mandatory, ParameterSetName = 'Preset')]
    [ValidateSet('msvc-cpu-tests', 'msvc-cuda-tests')][string]$Preset,
    [string]$Configuration = 'Release',
    [switch]$Resume,
    [string[]]$Test,
    [string[]]$RetryInterrupted,
    [switch]$ExcludeGui,
    [switch]$List
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path $PSScriptRoot -Parent

function Get-Digest([string]$Text) {
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($Text)))
}

function Get-InventoryDigest($Tests, [string]$Build, [string]$Cache, [string]$CTest, [ref]$Snapshot) {
    # Conservative whole-build invalidation, not a dependency database. Store
    # only the digest: environment values may contain private credentials.
    $files = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $inputs = & git -C $repo -c core.quotepath=false ls-files --cached --others --exclude-standard -- `
        CMakeLists.txt CMakePresets.json cmake src include tests translations assets third_party scripts
    if ($LASTEXITCODE) { throw 'Cannot inventory repository inputs.' }
    foreach ($path in $inputs) { [void]$files.Add((Join-Path $repo $path)) }
    [void]$files.Add((Join-Path $Build 'CMakeCache.txt'))
    [void]$files.Add($CTest)
    # Includes app/launcher and fixture helpers, not just the selected tests.
    Get-ChildItem -LiteralPath $Build -Recurse -File | Where-Object {
        $_.Extension -in '.exe', '.dll', '.qm' -or $_.Name -in 'CTestTestfile.cmake', 'build.ninja', 'compile_commands.json'
    } | ForEach-Object { [void]$files.Add($_.FullName) }
    foreach ($case in $Tests) {
        if (!$case.command -or !(Test-Path -LiteralPath $case.command[0] -PathType Leaf)) {
            throw "Build the complete test configuration first: $($case.name) executable is absent."
        }
        [void]$files.Add($case.command[0])
        for ($argumentIndex = 1; $argumentIndex -lt $case.command.Count; $argumentIndex++) {
            $argument = $case.command[$argumentIndex]
            # Fixture CLI convention: these flags name generated result files.
            # The command/path remains fingerprinted, but output bytes cannot
            # be inputs: replay rewrites its image and timing trace each run.
            if ($case.command[$argumentIndex - 1] -in '--output', '--trace') { continue }
            if ([IO.Path]::IsPathRooted($argument) -and (Test-Path -LiteralPath $argument -PathType Leaf)) {
                [void]$files.Add($argument)
            }
        }
    }
    $qtMatch = [regex]::Match($Cache, '(?m)^Qt6_DIR:[^=]+=(.+)\r?$')
    if (!$qtMatch.Success) { throw 'Qt6_DIR missing from build cache.' }
    $qt = [IO.Path]::GetFullPath((Join-Path $qtMatch.Groups[1].Value.Trim() '../../..'))
    foreach ($subdir in 'bin', 'plugins') {
        Get-ChildItem -LiteralPath (Join-Path $qt $subdir) -Recurse -File -Filter '*.dll' |
            ForEach-Object { [void]$files.Add($_.FullName) }
    }
    foreach ($library in 'd3d12.dll', 'dxgi.dll', 'd3dcompiler_47.dll', 'nvcuda.dll',
                         'mfplat.dll', 'mfreadwrite.dll', 'ucrtbase.dll', 'vcruntime140.dll', 'msvcp140.dll') {
        [void]$files.Add((Join-Path ([Environment]::SystemDirectory) $library))
    }
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("$Build|$Configuration|$([Environment]::OSVersion.VersionString)")
    # Stable desktop/driver inputs, never current pointer/focus or user content.
    $display = @(Get-CimInstance Win32_VideoController | Select-Object DeviceID, DriverVersion,
        CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate | Sort-Object DeviceID)
    $lines.Add(($display | ConvertTo-Json -Compress))
    $lines.Add([string](Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().Ticks)
    $lines.Add("$([cultureinfo]::CurrentCulture.Name)|$([cultureinfo]::CurrentUICulture.Name)")
    foreach ($key in 'HKCU:\Control Panel\Desktop', 'HKCU:\Control Panel\Desktop\WindowMetrics',
                      'HKCU:\Control Panel\Desktop\PerMonitorSettings') {
        if (Test-Path -LiteralPath $key) {
            $dpiKeys = @((Get-Item -LiteralPath $key)) + @(Get-ChildItem -LiteralPath $key -Recurse)
            foreach ($dpiKey in $dpiKeys) {
                $values = Get-ItemProperty -LiteralPath $dpiKey.PSPath |
                    Select-Object PSChildName, AppliedDPI, LogPixels, Win8DpiScaling, DpiValue
                $lines.Add(($values | ConvertTo-Json -Compress))
            }
        }
    }
    $lines.Add(($Tests | ConvertTo-Json -Depth 30 -Compress))
    foreach ($path in ($files | Sort-Object)) {
        $hash = if (Test-Path -LiteralPath $path -PathType Leaf) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { 'MISSING' }
        $lines.Add("$path=$hash")
    }
    Get-ChildItem Env: | Where-Object {
        $_.Name -match '^(QT_|OKUFLOW_|CUDA_|NV_|PATH$|LANG$|LC_|PROCESSOR_ARCHITECTURE$)' -and
        $_.Name -notin 'OKUFLOW_TEST_RESUME', 'OKUFLOW_AGENT_EXCLUDE_GUI'
    } | Sort-Object Name | ForEach-Object { $lines.Add("env:$($_.Name)=$($_.Value)") }
    # Retain only hashes for diagnostics, including environment/desktop values.
    # A changed inventory must explain what changed without exposing secrets.
    $parts = [ordered]@{}
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $line = $lines[$index]
        $label = if ($line -match '^(env:[^=]+|[A-Za-z]:[\\/][^=]+)=') {
            $Matches[1]
        } else { "desktop-or-test-metadata:$index" }
        $parts[$label] = Get-Digest $line
    }
    $Snapshot.Value = $parts
    Get-Digest ($lines -join "`n")
}

function Get-CaseResult($Case) {
    if ($Case.SelectSingleNode('failure')) {
        $output = [string]$Case.'system-out'
        # A marker alone is insufficient: mixed application/assertion failures
        # stay failed. Only one marked Qt failure or pre-QApplication lock
        # refusal is eligible for an explicitly requested interruption retry.
        $failures = [regex]::Matches($output, '(?m)^FAIL!\s*:').Count
        $marked = $output.Contains('OKUFLOW_DESKTOP_INTERRUPTED:')
        $oneQtFailure = $failures -eq 1 -and $output -match 'Totals: \d+ passed, 1 failed, 0 skipped'
        $leaseRefusal = $failures -eq 0 -and $output.Trim() -eq 'OKUFLOW_DESKTOP_INTERRUPTED: desktop lease unavailable'
        if ($marked -and ($oneQtFailure -or $leaseRefusal)) { return 'Interrupted' }
        return 'Failed'
    }
    if ($Case.SelectSingleNode('skipped')) { return 'Skipped' }
    if ($Case.status -ne 'run') { return 'Failed' }
    if ([string]$Case.'system-out' -match '(?m)^SKIP\s*:') { return 'PassedWithSkips' }
    return 'Passed'
}

function Save-Ledger($Ledger, [string]$Path) {
    $temporary = "$Path.tmp"
    $Ledger | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding utf8
    [IO.File]::Move($temporary, $Path, $true)
}

$runnerLock = $null
try {
    if (!$env:LOCALAPPDATA) { throw 'LOCALAPPDATA is required for private test evidence.' }
    if ($Preset) {
        $presets = Get-Content -LiteralPath (Join-Path $repo 'cmake/CMakePresets.json') -Raw | ConvertFrom-Json
        $testPreset = $presets.testPresets | Where-Object name -eq $Preset
        $configure = $presets.configurePresets | Where-Object name -eq $testPreset.configurePreset
        $BuildDirectory = $configure.binaryDir.Replace('${sourceDir}', $repo)
    }
    $BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $cache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw
    $cmake = [regex]::Match($cache, '(?m)^CMAKE_COMMAND:[^=]+=(.+)\r?$').Groups[1].Value.Trim()
    if (!$cmake) { throw 'CMAKE_COMMAND missing from cache.' }
    $ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
    $arguments = @('--test-dir', $BuildDirectory, '-C', $Configuration)
    $json = & $ctest @arguments --show-only=json-v1
    if ($LASTEXITCODE) { throw 'CTest inventory failed.' }
    $tests = @(($json -join "`n" | ConvertFrom-Json).tests)
    if (!$tests.Count) { throw 'No tests discovered.' }
    if ($List) { $tests | ForEach-Object { $_.name }; exit 0 }
    foreach ($name in @($Test) + @($RetryInterrupted)) {
        if ($name -and $name -notin $tests.name) { throw "Unknown exact test name: $name" }
    }
    if ($RetryInterrupted -and !$Resume) { throw '-RetryInterrupted requires -Resume and matching evidence.' }
    $root = Join-Path $env:LOCALAPPDATA 'OkuFlow/test-evidence'
    [void][IO.Directory]::CreateDirectory($root)
    # Excludes concurrent tracked gate invocations, even CPU vs CUDA/bundle.
    $runnerLock = [IO.File]::Open((Join-Path $root 'runner.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    $identity = Get-Digest "$BuildDirectory|$Configuration"
    $directory = Join-Path $root $identity
    [void][IO.Directory]::CreateDirectory($directory)
    $beforeInputs = $null
    $fingerprint = Get-InventoryDigest $tests $BuildDirectory $cache $ctest ([ref]$beforeInputs)
    $path = Join-Path $directory "$fingerprint.json"
    $ledger = if (Test-Path -LiteralPath $path) { Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable } else { $null }
    if ($Resume -and !$ledger) { throw 'No evidence matches these inputs. Build and run a fresh gate; old passes cannot be reused.' }
    if (!$ledger) { $ledger = @{ schema = 1; inventory = $fingerprint; build = $BuildDirectory; configuration = $Configuration; results = @{}; attempts = @() } }
    foreach ($name in @($RetryInterrupted)) {
        if ($name -and (!$ledger.results.ContainsKey($name) -or $ledger.results[$name].status -ne 'Interrupted')) {
            throw "Only a recorded desktop interruption may be retried: $name"
        }
    }
    $selected = @($tests | Where-Object {
        $labels = @($_.properties | Where-Object name -eq LABELS | ForEach-Object value)
        (!$ExcludeGui -or 'gui' -notin $labels) -and (!$Test -or $_.name -in $Test) -and
        (!$RetryInterrupted -or $_.name -in $RetryInterrupted)
    })
    if (!$selected.Count) { throw 'Selection contains no tests.' }
    $pending = @($selected | Where-Object {
        $prior = $ledger.results[$_.name]
        if (!$prior) { return $true }
        if ($prior.status -in 'Failed', 'Interrupted', 'Unfinished') { return $_.name -in $RetryInterrupted }
        if ($prior.status -in 'Skipped', 'PassedWithSkips') { return $true }
        return !$Resume
    })
    if ($pending.Count) {
        $id = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
        $junit = Join-Path $directory "$id.xml"
        $log = Join-Path $directory "$id.log"
        $expression = '^(' + (($pending.name | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'
        # Save pending state before execution. An abandoned CTest run is never
        # mistaken for a completed pass, and earlier logs are never overwritten.
        foreach ($case in $pending) { $ledger.results[$case.name] = @{ status = 'Unfinished'; attempt = $id } }
        $ledger.attempts += @{ id = $id; tests = @($pending.name); log = $log; junit = $junit }
        Save-Ledger $ledger $path
        & $ctest @arguments --parallel 1 --output-on-failure --no-tests=error -R $expression --output-junit $junit 2>&1 |
            Tee-Object -FilePath $log | ForEach-Object { Write-Host $_ }
        $exitCode = $LASTEXITCODE
        $afterInputs = $null
        $after = Get-InventoryDigest $tests $BuildDirectory (Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw) $ctest ([ref]$afterInputs)
        if ($after -ne $fingerprint) {
            $changed = @(@($beforeInputs.Keys) + @($afterInputs.Keys) | Sort-Object -Unique | Where-Object {
                $beforeInputs[$_] -ne $afterInputs[$_]
            })
            throw "Inputs changed during testing; this attempt cannot provide passing evidence. Changed: $($changed -join ', ')"
        }
        if (!(Test-Path -LiteralPath $junit)) { throw "CTest did not produce results (exit $exitCode)." }
        [xml]$report = Get-Content -LiteralPath $junit -Raw
        foreach ($case in $report.testsuite.testcase) {
            if ($case.name -notin $pending.name) { throw "Unexpected CTest result: $($case.name)" }
            $ledger.results[$case.name] = @{ status = (Get-CaseResult $case); attempt = $id; seconds = [string]$case.time }
        }
        if ($exitCode -and !@($ledger.results.Values | Where-Object status -in 'Failed', 'Interrupted').Count) {
            foreach ($case in $pending) { $ledger.results[$case.name] = @{ status = 'Unfinished'; attempt = $id } }
            Save-Ledger $ledger $path
            throw "CTest failed outside an assertion (exit $exitCode); do not accept this gate."
        }
        Save-Ledger $ledger $path
    }
    $required = @($tests | Where-Object {
        !$ExcludeGui -or 'gui' -notin @($_.properties | Where-Object name -eq LABELS | ForEach-Object value)
    })
    $unresolved = @($required | Where-Object { $ledger.results[$_.name].status -notin 'Passed', 'PassedWithSkips', 'Skipped' })
    $skipped = @($required | Where-Object { $ledger.results[$_.name].status -eq 'Skipped' })
    $rowSkips = @($required | Where-Object { $ledger.results[$_.name].status -eq 'PassedWithSkips' })
    Write-Host "Evidence: $path"
    Write-Host "Gate: $($required.Count - $unresolved.Count - $skipped.Count) passed ($($rowSkips.Count) with skipped Qt rows), $($skipped.Count) skipped, $($unresolved.Count) unresolved."
    foreach ($case in $unresolved) {
        $state = $ledger.results[$case.name].status
        if (!$state) { $state = 'Pending' }
        Write-Host "$state`: $($case.name)"
    }
    if ($ExcludeGui) { Write-Host 'PARTIAL VALIDATION: GUI checks remain pending; not the full release gate.' }
    if ($unresolved.Count) { exit 1 }
    exit 0
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    if ($runnerLock) { $runnerLock.Dispose() }
}
