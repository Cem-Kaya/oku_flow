#Requires -Version 7.0
[CmdletBinding(DefaultParameterSetName = 'Build')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Build')][string]$BuildDirectory,
    [Parameter(Mandatory, ParameterSetName = 'Preset')]
    [ValidateSet('msvc-cpu-tests', 'msvc-cuda-tests')][string]$Preset,
    [string]$Configuration = 'Release',
    [switch]$Resume,
    [string[]]$Test,
    [string[]]$Label,
    [string[]]$Select,
    [ValidateRange(1, 1024)][int]$Parallel = [Environment]::ProcessorCount,
    [ValidateSet('msvc-cpu-tests', 'msvc-cuda-tests')][string]$UncoveredByPreset,
    [string[]]$RetryInterrupted,
    [switch]$ExcludeGui,
    [ValidateSet('development', 'windows-cpu', 'windows-cuda', 'windows-cuda-kernels')]
    [string]$CapabilityProfile = 'development',
    [switch]$List
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path $PSScriptRoot -Parent

function Get-Digest([string]$Text) {
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($Text)))
}

function Get-Labels($Case) {
    @($Case.properties | Where-Object name -eq LABELS | ForEach-Object value)
}

function Get-CapabilityRequirements([string]$Profile) {
    $cuda = @('yuv_color_conversion_cuda', 'text_clarity_sauvola_cuda',
        'stabilization_similarity_cuda', 'spatial_upscaler_reference_cuda',
        'stabilization_replay_regression', 'cuda_architecture_coverage')
    $base = @('shared_source_equivalence', 'settings_store_round_trip',
        'phone_camera_contract', 'windows_runtime_contract', 'recording_manager',
        'agent_control_contract', 'mcp_bridge_contract')
    switch ($Profile) {
        'windows-cpu' { @{ tests = $base; labels = @('gui'); cuda = $false } }
        'windows-cuda' { @{ tests = $base + $cuda + @('media_writer_gpu_codecs',
            'maxine_lifecycle_cuda', 'maxine_navigation_cuda', 'maxine_async_cuda'); labels = @('gui'); cuda = $true } }
        'windows-cuda-kernels' { @{ tests = $cuda; labels = @(); cuda = $true } }
        default { @{ tests = @(); labels = @(); cuda = $null } }
    }
}

function Get-CapabilityVerdict($Requirements, $Inventory, $Records, [string[]]$SelectedNames) {
    $names = @($Requirements.tests)
    $rows = @()
    foreach ($label in $Requirements.labels) {
        $matches = @($Inventory | Where-Object { $label -cin (Get-Labels $_) })
        if (!$matches.Count) { $rows += @{ name = "label:$label"; state = 'NotRun'; reason = 'Required label has no registered cases.' } }
        $names += @($matches.name)
    }
    foreach ($name in @($names | Sort-Object -Unique)) {
        $record = $Records[$name]
        $state = if ($name -cnotin $Inventory.name -or $name -cnotin $SelectedNames -or !$record) { 'NotRun' }
            elseif ($record.status -ceq 'Passed') { 'Passed' }
            elseif ($record.status -ceq 'Failed') { 'Failed' }
            else { 'Blocked' } # A CTest skip or skipped Qt row is not capability acceptance.
        $rows += @{ name = $name; state = $state; sourceStatus = $(if ($record) { $record.status } else { 'Missing' }) }
    }
    $state = if (@($rows | Where-Object state -eq 'Failed').Count) { 'Failed' }
        elseif (@($rows | Where-Object state -ne 'Passed').Count) { 'Blocked' }
        elseif (!$rows.Count) { 'NotApplicable' } else { 'Passed' }
    @{ state = $state; cases = $rows }
}

function Get-PresetDirectory([string]$Name) {
    $presets = Get-Content -LiteralPath (Join-Path $repo 'cmake/CMakePresets.json') -Raw | ConvertFrom-Json
    $testPreset = $presets.testPresets | Where-Object name -eq $Name
    $configure = $presets.configurePresets | Where-Object name -eq $testPreset.configurePreset
    $configure.binaryDir.Replace('${sourceDir}', $repo)
}

function Get-TestInventory([string]$Build, [string]$CTest) {
    $json = & $CTest --test-dir $Build -C $Configuration --show-only=json-v1
    if ($LASTEXITCODE) { throw 'CTest inventory failed.' }
    $cases = @(($json -join "`n" | ConvertFrom-Json).tests)
    if (!$cases.Count) { throw 'No tests discovered.' }
    $cases
}

function Initialize-HashCache([string]$Path) {
    $script:hashCachePath = $Path
    $script:hashCache = @{}
    $script:hashCacheDirty = $false
    # Persisted hashes are diagnostic only. Length/mtime cannot certify exact
    # bytes across invocations: builds, copies or deliberate edits can restore
    # both metadata values. Memoize only within this fingerprint pass.
}

function Save-HashCache {
    if (!$script:hashCacheDirty) { return }
    $temporary = "$script:hashCachePath.tmp"
    @{ schema = 1; files = $script:hashCache } | ConvertTo-Json -Depth 10 -Compress |
        Set-Content -LiteralPath $temporary -Encoding utf8
    [IO.File]::Move($temporary, $script:hashCachePath, $true)
    $script:hashCacheDirty = $false
}

function Add-FileDigest($Parts, [string]$Path) {
    $Path = [IO.Path]::GetFullPath($Path)
    $file = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if (!$file -or $file.PSIsContainer) {
        $Parts["file:$Path"] = 'MISSING'
        if ($script:hashCache.ContainsKey($Path)) {
            $script:hashCache.Remove($Path)
            $script:hashCacheDirty = $true
        }
        return
    }
    $cached = $script:hashCache[$Path]
    if (!$cached -or $cached.length -ne $file.Length -or $cached.ticks -ne $file.LastWriteTimeUtc.Ticks -or
        $cached.sha256 -notmatch '^[0-9A-Fa-f]{64}$') {
        $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        $after = Get-Item -LiteralPath $Path
        if ($file.Length -ne $after.Length -or $file.LastWriteTimeUtc.Ticks -ne $after.LastWriteTimeUtc.Ticks) {
            throw "File changed while hashing: $Path"
        }
        $cached = @{ length = $file.Length; ticks = $file.LastWriteTimeUtc.Ticks; sha256 = $hash }
        $script:hashCache[$Path] = $cached
        $script:hashCacheDirty = $true
    }
    $Parts["file:$Path"] = $cached.sha256
}

function Get-RuntimeInputs([string]$Cache, [string]$CTest, [bool]$Gui) {
    # Shared hashes are computed once before and once after execution, never
    # once per test. Persist digests only; environment values may be secrets.
    $parts = [ordered]@{}
    $parts.platform = Get-Digest "$BuildDirectory|$Configuration|$([Environment]::OSVersion.VersionString)"
    $parts.culture = Get-Digest "$([cultureinfo]::CurrentCulture.Name)|$([cultureinfo]::CurrentUICulture.Name)"
    Add-FileDigest $parts $CTest
    Add-FileDigest $parts $PSCommandPath
    $qtMatch = [regex]::Match($Cache, '(?m)^Qt6_DIR:[^=]+=(.+)\r?$')
    if (!$qtMatch.Success) { throw 'Qt6_DIR missing from build cache.' }
    $qt = [IO.Path]::GetFullPath((Join-Path $qtMatch.Groups[1].Value.Trim() '../../..'))
    foreach ($subdir in 'bin', 'plugins') {
        Get-ChildItem -LiteralPath (Join-Path $qt $subdir) -Recurse -File -Filter '*.dll' |
            Sort-Object FullName | ForEach-Object { Add-FileDigest $parts $_.FullName }
    }
    foreach ($library in 'd3d12.dll', 'dxgi.dll', 'd3dcompiler_47.dll', 'nvcuda.dll',
                         'mfplat.dll', 'mfreadwrite.dll', 'ucrtbase.dll', 'vcruntime140.dll', 'msvcp140.dll') {
        Add-FileDigest $parts (Join-Path ([Environment]::SystemDirectory) $library)
    }
    Get-ChildItem Env: | Where-Object {
        $_.Name -match '^(QT_|OKUFLOW_|CUDA_|NV_|PATH$|LANG$|LC_|PROCESSOR_ARCHITECTURE$)' -and
        $_.Name -notin 'OKUFLOW_TEST_RESUME', 'OKUFLOW_AGENT_EXCLUDE_GUI', 'OKUFLOW_FULL_CUDA_SUITE'
    } | Sort-Object Name | ForEach-Object { $parts["env:$($_.Name)"] = Get-Digest $_.Value }
    $desktop = [ordered]@{}
    if ($Gui) {
        # Stable display inputs only, never pointer/focus or user content.
        $display = @(Get-CimInstance Win32_VideoController | Select-Object DeviceID, DriverVersion,
            CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate | Sort-Object DeviceID)
        $desktop.display = Get-Digest ($display | ConvertTo-Json -Compress)
        $desktop.boot = Get-Digest ([string](Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().Ticks)
        foreach ($key in 'HKCU:\Control Panel\Desktop', 'HKCU:\Control Panel\Desktop\WindowMetrics',
                          'HKCU:\Control Panel\Desktop\PerMonitorSettings') {
            if (Test-Path -LiteralPath $key) {
                $dpiKeys = @((Get-Item -LiteralPath $key)) + @(Get-ChildItem -LiteralPath $key -Recurse)
                foreach ($dpiKey in $dpiKeys) {
                    $values = Get-ItemProperty -LiteralPath $dpiKey.PSPath |
                        Select-Object PSChildName, AppliedDPI, LogPixels, Win8DpiScaling, DpiValue
                    $desktop[$dpiKey.Name] = Get-Digest ($values | ConvertTo-Json -Compress)
                }
            }
        }
    }
    @{ shared = $parts; sharedDigest = (Get-Digest ($parts | ConvertTo-Json -Depth 10 -Compress)); desktop = $desktop }
}

function Get-TestFingerprint($Case, $Runtime) {
    if (!$Case.command -or !(Test-Path -LiteralPath $Case.command[0] -PathType Leaf)) {
        throw "Build the selected test first: $($Case.name) executable is absent."
    }
    $parts = [ordered]@{}
    if ('gui' -in (Get-Labels $Case)) {
        foreach ($key in $Runtime.desktop.Keys) { $parts["desktop:$key"] = $Runtime.desktop[$key] }
    }
    $metadata = [ordered]@{ name = $Case.name; command = $Case.command; properties = @($Case.properties | Sort-Object name) }
    $parts.test = Get-Digest ($metadata | ConvertTo-Json -Depth 30 -Compress)
    Add-FileDigest $parts $Case.command[0]
    Get-ChildItem -LiteralPath (Split-Path $Case.command[0] -Parent) -File | Where-Object {
        $_.Extension -in '.dll', '.qm'
    } | Sort-Object FullName | ForEach-Object { Add-FileDigest $parts $_.FullName }
    for ($index = 1; $index -lt $Case.command.Count; $index++) {
        $argument = $Case.command[$index]
        if ($Case.command[$index - 1] -in '--output', '--trace') { continue }
        if ([IO.Path]::IsPathRooted($argument)) { Add-FileDigest $parts $argument }
    }
    # REQUIRED_FILES is CTest's runtime-input contract. Prefer absolute paths;
    # relative files resolve against the test's WORKING_DIRECTORY, as in CTest.
    $working = @($Case.properties | Where-Object name -eq WORKING_DIRECTORY | ForEach-Object value)
    $base = if ($working.Count) { $working[0] } else { $BuildDirectory }
    foreach ($inputFile in @($Case.properties | Where-Object name -eq REQUIRED_FILES | ForEach-Object value)) {
        $resolved = if ([IO.Path]::IsPathRooted($inputFile)) { $inputFile } else { Join-Path $base $inputFile }
        Add-FileDigest $parts ([IO.Path]::GetFullPath($resolved))
    }
    # Preserve the v2 fingerprint algorithm so matching historical blockers
    # survive migration. Only the persisted representation is deduplicated.
    $combined = [ordered]@{}
    foreach ($key in $Runtime.shared.Keys) { $combined[$key] = $Runtime.shared[$key] }
    foreach ($key in $parts.Keys) { $combined[$key] = $parts[$key] }
    @{ digest = (Get-Digest ($combined | ConvertTo-Json -Depth 10 -Compress)); inputs = $parts; shared = $Runtime.sharedDigest }
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
    if ([string]$Case.'system-out' -match '\[This part of the test output was removed since it exceeds the threshold of') { return 'Unfinished' }
    if ([string]$Case.'system-out' -match '(?m)^SKIP\s*:') { return 'PassedWithSkips' }
    return 'Passed'
}

function Convert-LedgerInputs($Ledger, $Runtime) {
    if (!$Ledger.ContainsKey('sharedInputs')) { $Ledger.sharedInputs = @{} }
    # Old v2 entries carry flattened inputs. Split them without changing their
    # fingerprint or status; even a pre-migration failed attempt must block.
    $records = @($Ledger.results.Values) + @($Ledger.evidence.Values | ForEach-Object { $_.Values })
    foreach ($record in $records) {
        if (!$record -or $record.shared -or $record.blockerOnly) { continue }
        $shared = [ordered]@{}
        foreach ($key in $Runtime.shared.Keys) {
            if ($record.inputs.Contains($key)) { $shared[$key] = $record.inputs[$key] }
        }
        $record.shared = Get-Digest ($shared | ConvertTo-Json -Depth 10 -Compress)
        $Ledger.sharedInputs[$record.shared] = $shared
        $specific = [ordered]@{}
        foreach ($key in $record.inputs.Keys) {
            if (!$shared.Contains($key)) { $specific[$key] = $record.inputs[$key] }
        }
        $record.inputs = $specific
    }
    $Ledger.sharedInputs[$Runtime.sharedDigest] = $Runtime.shared
}

function Limit-LedgerEvidence($Ledger) {
    $usedShared = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($name in @($Ledger.evidence.Keys)) {
        $history = $Ledger.evidence[$name]
        $current = $Ledger.results[$name].fingerprint
        # Bound reusable history, but never forget a failed exact fingerprint.
        # Older blockers become small tombstones; reverting inputs must not
        # turn an assertion failure into an automatic retry.
        $recent = @($history.Keys | Where-Object {
                $_ -ne $current -and $history[$_].status -notin 'Failed', 'Interrupted', 'Unfinished'
            } |
            Sort-Object @{ Expression = { $history[$_].attempt }; Descending = $true },
                        @{ Expression = { $_ }; Descending = $true } | Select-Object -First 3)
        foreach ($fingerprint in @($history.Keys)) {
            if ($fingerprint -eq $current -or $fingerprint -in $recent) { continue }
            $record = $history[$fingerprint]
            if ($record.status -in 'Failed', 'Interrupted', 'Unfinished') {
                $history[$fingerprint] = @{ fingerprint = $fingerprint; status = $record.status;
                    attempt = $record.attempt; inputs = @{}; blockerOnly = $true }
            } else { $history.Remove($fingerprint) }
        }
        foreach ($record in $history.Values) { if ($record.shared) { [void]$usedShared.Add($record.shared) } }
    }
    foreach ($record in $Ledger.results.Values) { if ($record.shared) { [void]$usedShared.Add($record.shared) } }
    foreach ($key in @($Ledger.sharedInputs.Keys)) {
        if (!$usedShared.Contains($key)) { $Ledger.sharedInputs.Remove($key) }
    }
    # Attempt/log/JUnit references remain an audit trail even after an old
    # fingerprint is evicted. They contain no duplicated runtime input maps.
}

function Save-Ledger($Ledger, [string]$Path) {
    Limit-LedgerEvidence $Ledger
    $temporary = "$Path.tmp"
    $Ledger | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $temporary -Encoding utf8
    [IO.File]::Move($temporary, $Path, $true)
}

$runnerLock = $null
try {
    if (!$env:LOCALAPPDATA) { throw 'LOCALAPPDATA is required for private test evidence.' }
    # A comma list also works through pwsh -File, which cannot bind multiple
    # command-line arguments to an array parameter. Native PowerShell accepts arrays.
    $Select = @($Select | ForEach-Object { if ($_) { $_.Split(',') } })
    if ($Preset) { $BuildDirectory = Get-PresetDirectory $Preset }
    $BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $cache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw
    $cmake = [regex]::Match($cache, '(?m)^CMAKE_COMMAND:[^=]+=(.+)\r?$').Groups[1].Value.Trim()
    if (!$cmake) { throw 'CMAKE_COMMAND missing from cache.' }
    $ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
    $arguments = @('--test-dir', $BuildDirectory, '-C', $Configuration)
    $tests = @(Get-TestInventory $BuildDirectory $ctest)
    $knownLabels = @($tests | ForEach-Object { Get-Labels $_ } | Sort-Object -Unique)
    foreach ($name in @($Test) + @($RetryInterrupted)) {
        if ($name -and $name -cnotin $tests.name) { throw "Unknown exact test name: $name" }
    }
    foreach ($name in $Label) {
        if ($name -cnotin $knownLabels) { throw "Unknown CTest label: $name" }
    }
    foreach ($name in $Select) {
        if ($name -cnotin $tests.name -and $name -cnotin $knownLabels) { throw "Unknown test or label: $name" }
    }
    if ($RetryInterrupted -and (!$Resume -or $Test -or $Label -or $Select -or $UncoveredByPreset)) {
        throw '-RetryInterrupted requires -Resume and cannot be combined with another selection.'
    }
    $covered = if ($UncoveredByPreset) { @(Get-TestInventory (Get-PresetDirectory $UncoveredByPreset) $ctest).name } else { @() }
    $scoped = [bool]($Test -or $Label -or $Select -or $UncoveredByPreset)
    $selected = @($tests | Where-Object {
        $labels = @(Get-Labels $_)
        $matchesSelection = !$scoped -or $_.name -cin $Test -or $_.name -cin $Select -or
            @($labels | Where-Object { $_ -cin $Label -or $_ -cin $Select }).Count -gt 0 -or
            ($UncoveredByPreset -and ('gpu' -in $labels -or 'cuda-variant' -in $labels -or $_.name -cnotin $covered))
        (!$ExcludeGui -or 'gui' -notin $labels) -and $matchesSelection -and
            (!$RetryInterrupted -or $_.name -cin $RetryInterrupted)
    })
    if (!$selected.Count) { throw 'Selection contains no tests.' }
    if ($List) { $selected | ForEach-Object { $_.name }; exit 0 }
    $capabilityRequirements = Get-CapabilityRequirements $CapabilityProfile
    if ($CapabilityProfile -ne 'development') {
        $cudaEnabled = [regex]::IsMatch($cache, '(?m)^OKUFLOW_ENABLE_CUDA:[^=]+=ON\r?$')
        if ($cudaEnabled -ne $capabilityRequirements.cuda) { throw "Capability profile $CapabilityProfile disagrees with the configured CUDA build." }
        if ($RetryInterrupted -or ($CapabilityProfile -ne 'windows-cuda-kernels' -and ($scoped -or $ExcludeGui))) {
            throw "Capability profile $CapabilityProfile requires the complete unfiltered build inventory."
        }
        $missing = Get-CapabilityVerdict $capabilityRequirements $tests @{} @($selected.name)
        $unregistered = @($missing.cases | Where-Object {
            $_.name -like 'label:*' -or $_.name -cnotin $tests.name -or $_.name -cnotin $selected.name
        })
        if ($unregistered.Count) { throw "Capability NotRun: required cases absent from selection/inventory: $($unregistered.name -join ', ')" }
    }
    $root = Join-Path $env:LOCALAPPDATA 'OkuFlow/test-evidence'
    [void][IO.Directory]::CreateDirectory($root)
    # Never delete another runner's lock or overlap CPU/CUDA/bundle gates.
    for ($wait = 0; !$runnerLock; $wait++) {
        try { $runnerLock = [IO.File]::Open((Join-Path $root 'runner.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
        catch [IO.IOException] {
            if ($wait -ge 120) { throw 'Another tracked gate still holds runner.lock; retry after it completes.' }
            if ($wait -eq 0) { Write-Host 'Waiting for the tracked runner lock.' }
            Start-Sleep -Seconds 1
        }
    }
    $identity = Get-Digest "$BuildDirectory|$Configuration"
    $directory = Join-Path $root $identity
    [void][IO.Directory]::CreateDirectory($directory)
    $path = Join-Path $directory 'ledger-v2.json'
    $ledger = if (Test-Path -LiteralPath $path) { Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable } else { $null }
    if ($ledger -and $ledger.schema -ne 2) { $ledger = $null }
    if ($Resume -and !$ledger) { throw 'No evidence matches these inputs. Build and run a fresh gate; old passes cannot be reused.' }
    if (!$ledger) { $ledger = @{ schema = 2; build = $BuildDirectory; configuration = $Configuration; results = @{}; evidence = @{}; attempts = @() } }
    $fingerprinted = if ($RetryInterrupted) {
        @($tests | Where-Object { !$ExcludeGui -or 'gui' -notin (Get-Labels $_) })
    } else { $selected }
    $hasGui = @($fingerprinted | Where-Object { 'gui' -in (Get-Labels $_) }).Count -gt 0
    Initialize-HashCache (Join-Path $root 'hash-cache.json')
    $runtime = Get-RuntimeInputs $cache $ctest $hasGui
    Convert-LedgerInputs $ledger $runtime
    $before = @{}
    foreach ($case in $fingerprinted) {
        $name = $case.name
        $before[$name] = Get-TestFingerprint $case $runtime
        $fingerprint = $before[$name].digest
        if (!$ledger.evidence.ContainsKey($name)) { $ledger.evidence[$name] = @{} }
        $prior = $ledger.evidence[$name][$fingerprint]
        $previous = $ledger.results[$name]
        if ($Resume -and !$prior -and $previous -and
            $previous.status -in 'Failed', 'Interrupted', 'Unfinished' -and
            (!$previous.shared -or $previous.shared -ne $runtime.sharedDigest)) {
            $oldShared = if ($previous.shared) { $ledger.sharedInputs[$previous.shared] } else { $null }
            $changed = if ($oldShared) {
                @(@($oldShared.Keys) + @($runtime.shared.Keys) | Sort-Object -Unique | Where-Object { $oldShared[$_] -ne $runtime.shared[$_] })
            } else { @('prior shared inputs unavailable') }
            throw "Resume inputs changed for unresolved test: $name. Shared inputs: $($changed -join ', '). Diagnose the failure and run a fresh gate explicitly; resume cannot retry it under a different shared runtime."
        }
        if ($name -cin $RetryInterrupted -and (!$prior -or $prior.status -ne 'Interrupted')) {
            throw "Only a recorded desktop interruption may be retried with matching inputs: $name"
        }
        $ledger.results[$name] = $prior
    }
    $pending = @($selected | Where-Object {
        $prior = $ledger.results[$_.name]
        if (!$prior) { return $true }
        if ($prior.status -in 'Failed', 'Interrupted', 'Unfinished') { return $_.name -cin $RetryInterrupted }
        if ($prior.status -in 'Skipped', 'PassedWithSkips') { return $true }
        return !$Resume
    })
    Write-Host "Selection: $($selected.Count) of $($tests.Count) tests; $($selected.Count - $pending.Count) matching results retained."
    $partitions = @(
        @{ name = 'background'; parallel = $Parallel; cases = @($pending | Where-Object {
            $labels = @(Get-Labels $_); 'gui' -notin $labels -and 'gpu' -notin $labels -and 'codec' -notin $labels
        }) },
        @{ name = 'gpu-codec'; parallel = 1; cases = @($pending | Where-Object {
            $labels = @(Get-Labels $_); 'gui' -notin $labels -and ('gpu' -in $labels -or 'codec' -in $labels)
        }) },
        @{ name = 'gui'; parallel = 1; cases = @($pending | Where-Object { 'gui' -in (Get-Labels $_) }) }
    )
    $errors = [Collections.Generic.List[string]]::new()
    foreach ($partition in $partitions) {
        $cases = $partition.cases
        if (!$cases.Count) { continue }
        $id = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
        $junit = Join-Path $directory "$id.xml"
        $log = Join-Path $directory "$id.log"
        $expression = '^(' + (($cases.name | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'
        foreach ($case in $cases) {
            $result = @{ status = 'Unfinished'; attempt = $id; fingerprint = $before[$case.name].digest;
                inputs = $before[$case.name].inputs; shared = $before[$case.name].shared }
            $ledger.results[$case.name] = $result
            $ledger.evidence[$case.name][$result.fingerprint] = $result
        }
        $ledger.attempts += @{ id = $id; tests = @($cases.name); partition = $partition.name; parallel = $partition.parallel; log = $log; junit = $junit }
        Save-Ledger $ledger $path
        Write-Host "CTest partition: $($partition.name), $($cases.Count) tests, --parallel $($partition.parallel)"
        & $ctest @arguments --parallel $partition.parallel --output-on-failure --no-tests=error -R $expression --output-junit $junit --test-output-size-passed 1048576 --test-output-size-failed 1048576 2>&1 |
            Tee-Object -FilePath $log | ForEach-Object { Write-Host $_ }
        $exitCode = $LASTEXITCODE
        # CTest overwrites these on the next invocation, including a narrow
        # settings check. Retain the original process output for each attempt.
        $details = Join-Path $BuildDirectory 'Testing/Temporary'
        foreach ($detail in 'LastTest.log', 'LastTestsFailed.log') {
            $source = Join-Path $details $detail
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Copy-Item -LiteralPath $source -Destination (Join-Path $directory "$id-$detail")
            }
        }
        if (!(Test-Path -LiteralPath $junit)) {
            $errors.Add("CTest did not produce results (exit $exitCode).")
            continue
        }
        [xml]$report = Get-Content -LiteralPath $junit -Raw
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($case in $report.testsuite.testcase) {
            if ($case.name -cnotin $cases.name -or !$seen.Add($case.name)) { throw "Unexpected CTest result: $($case.name)" }
            $result = $ledger.results[$case.name]
            $result.status = Get-CaseResult $case
            $result.seconds = [string]$case.time
            if ($result.status -eq 'Unfinished') {
                $errors.Add("Incomplete CTest output for $($case.name); inspect $id-LastTest.log before accepting coverage.")
            }
        }
        if ($exitCode -and !@($cases | Where-Object { $ledger.results[$_.name].status -in 'Failed', 'Interrupted' }).Count) {
            foreach ($case in $cases) { $ledger.results[$case.name].status = 'Unfinished' }
            $errors.Add("CTest failed outside an assertion (exit $exitCode); do not accept this gate.")
        }
        Save-Ledger $ledger $path
    }
    # Re-read test properties as well as files. Credit unaffected tests even
    # when a different selected input changes, retaining all attempted logs.
    $afterTests = @(Get-TestInventory $BuildDirectory $ctest)
    if (!$scoped) {
        $added = @($afterTests | Where-Object {
            $_.name -cnotin $tests.name -and (!$ExcludeGui -or 'gui' -notin (Get-Labels $_))
        })
        if ($added.Count) { $errors.Add("Test inventory changed during testing; new required tests: $($added.name -join ', ')") }
    }
    # Read bytes again even when lengths/timestamps were preserved by a writer.
    $script:hashCache.Clear()
    $afterRuntime = Get-RuntimeInputs (Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw) $ctest $hasGui
    foreach ($case in $fingerprinted) {
        $afterCase = $afterTests | Where-Object name -CEQ $case.name
        $after = try {
            if (!$afterCase) { throw 'Test removed during execution.' }
            Get-TestFingerprint $afterCase $afterRuntime
        } catch { @{ digest = ''; inputs = @{} } }
        if ($after.digest -ne $before[$case.name].digest) {
            $oldInputs = [ordered]@{}
            $newInputs = [ordered]@{}
            foreach ($key in $runtime.shared.Keys) { $oldInputs[$key] = $runtime.shared[$key] }
            foreach ($key in $before[$case.name].inputs.Keys) { $oldInputs[$key] = $before[$case.name].inputs[$key] }
            foreach ($key in $afterRuntime.shared.Keys) { $newInputs[$key] = $afterRuntime.shared[$key] }
            foreach ($key in $after.inputs.Keys) { $newInputs[$key] = $after.inputs[$key] }
            $changed = @(@($oldInputs.Keys) + @($newInputs.Keys) | Sort-Object -Unique | Where-Object {
                $oldInputs[$_] -ne $newInputs[$_]
            })
            if ($ledger.results[$case.name]) { $ledger.results[$case.name].status = 'Unfinished' }
            $errors.Add("Inputs changed during testing: $($case.name); this attempt cannot provide passing evidence. Changed: $($changed -join ', ')")
        }
    }
    Save-Ledger $ledger $path
    foreach ($message in $errors) { Write-Host $message }
    # A retry preserves the historical whole-gate verdict. Explicit names or
    # labels instead constitute a complete result for that declared scope.
    $required = if ($RetryInterrupted) { @($tests | Where-Object { !$ExcludeGui -or 'gui' -notin (Get-Labels $_) }) } else { $selected }
    $unresolved = @($required | Where-Object {
        $result = $ledger.results[$_.name]
        !$result -or $result.status -notin 'Passed', 'PassedWithSkips', 'Skipped'
    })
    $skipped = @($required | Where-Object { $ledger.results[$_.name].status -eq 'Skipped' })
    $rowSkips = @($required | Where-Object { $ledger.results[$_.name].status -eq 'PassedWithSkips' })
    $capability = Get-CapabilityVerdict $capabilityRequirements $tests $ledger.results @($required.name)
    $capability.profile = $CapabilityProfile
    $capability.checkedAt = [DateTime]::UtcNow.ToString('O')
    $capability.shared = $runtime.sharedDigest
    # The case records retain their exact fingerprints and original CTest statuses.
    # This separate verdict cannot turn a skipped capability into passing evidence.
    $capability.evidence = @{}
    foreach ($row in $capability.cases) {
        if ($ledger.results[$row.name]) { $capability.evidence[$row.name] = $ledger.results[$row.name].fingerprint }
    }
    $ledger.capabilityChecks = @($ledger.capabilityChecks | Select-Object -Last 31) + @($capability)
    Save-Ledger $ledger $path
    Write-Host "Capabilities ($CapabilityProfile): $($capability.state)"
    foreach ($row in @($capability.cases | Where-Object state -ne 'Passed')) {
        Write-Host "Capability $($row.state): $($row.name) [$($row.sourceStatus)]"
    }
    Write-Host "Evidence: $path"
    Write-Host "Gate: $($required.Count - $unresolved.Count - $skipped.Count) passed ($($rowSkips.Count) with skipped Qt rows), $($skipped.Count) skipped, $($unresolved.Count) unresolved."
    foreach ($case in $unresolved) {
        $state = $ledger.results[$case.name].status
        if (!$state) { $state = 'Pending' }
        Write-Host "$state`: $($case.name)"
    }
    if ($scoped) {
        $verdict = if ($unresolved.Count -or $errors.Count -or $capability.state -in 'Failed', 'Blocked') { 'FAIL' } else { 'PASS' }
        Write-Host "SCOPED $verdict`: $($required.Count) of $($tests.Count) tests ($($required.name -join ', '))."
    }
    if ($ExcludeGui) { Write-Host 'PARTIAL VALIDATION: GUI checks remain pending; not the full release gate.' }
    if ($unresolved.Count -or $errors.Count -or $capability.state -in 'Failed', 'Blocked') { exit 1 }
    exit 0
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    if ($runnerLock) {
        try { Save-HashCache } finally { $runnerLock.Dispose() }
    }
}
