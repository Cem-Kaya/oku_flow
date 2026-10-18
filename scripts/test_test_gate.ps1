#Requires -Version 7.0
# Private command-line CTest fixtures only; never executes a gui-labeled test.
param([Parameter(Mandatory)][string]$CMake)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$originalLocal = $env:LOCALAPPDATA
$originalScale = $env:QT_SCALE_FACTOR
$originalSecret = $env:OKUFLOW_GATE_CONTRACT_SECRET
$root = Join-Path $originalLocal ('OkuFlow/test-gate-contract/' + [Guid]::NewGuid().ToString('N'))
$runner = Join-Path $PSScriptRoot 'run_test_gate.ps1'
$clock = [Diagnostics.Stopwatch]::StartNew()
$checks = 0

function Assert-Contract([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
    $script:checks++
}

function Invoke-Gate([string[]]$Options, [int]$Expected, [string]$Contains) {
    $output = & "$PSHOME/pwsh.exe" -NoProfile -File $runner -BuildDirectory (Join-Path $root 'build') @Options 2>&1
    $actual = $LASTEXITCODE
    $text = $output -join "`n"
    $script:lastOutput = $text
    if ($actual -ne $Expected) { throw "Gate exit $actual, expected $Expected`n$text" }
    if ($Contains -and $text -notlike "*$Contains*") { throw "Missing gate diagnostic: $Contains`n$text" }
    $script:checks++
    $text -split "`n" | Where-Object { $_ -match '^(SCOPED |Gate:|CTest partition:|Inputs changed)' } | ForEach-Object { Write-Host $_ }
}

function Read-Ledger {
    $records = @(Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'OkuFlow/test-evidence') -Recurse -Filter 'ledger-v2.json')
    Assert-Contract ($records.Count -eq 1) 'Expected one build ledger.'
    Get-Content -LiteralPath $records[0].FullName -Raw | ConvertFrom-Json -AsHashtable
}

function Configure-Fixture([string]$Text) {
    Set-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Value $Text
    & $CMake -S $root -B (Join-Path $root 'build') "-DQt6_DIR:PATH=$root/qt/lib/cmake/Qt6" *> (Join-Path $root 'configure.log')
    if ($LASTEXITCODE) { throw 'Fixture CMake configuration failed.' }
}

try {
    [void][IO.Directory]::CreateDirectory($root)
    $env:LOCALAPPDATA = Join-Path $root 'private-local-app-data'
    [void][IO.Directory]::CreateDirectory($env:LOCALAPPDATA)
    foreach ($dir in 'qt/bin', 'qt/plugins', 'qt/lib/cmake/Qt6') {
        [void][IO.Directory]::CreateDirectory((Join-Path $root $dir))
    }
    $env:OKUFLOW_GATE_CONTRACT_SECRET = 'fixture-secret-must-never-be-saved'
    # Load only function definitions for isolated cache/ledger contracts. No
    # gate entry point, native desktop probing or CTest execution occurs here.
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($runner, [ref]$null, [ref]$parseErrors)
    Assert-Contract (!$parseErrors.Count) 'Runner has PowerShell parse errors.'
    foreach ($definition in $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
        . ([scriptblock]::Create($definition.Extent.Text))
    }
    $requirements = @{ tests = @('required'); labels = @('gui') }
    $inventory = @(@{ name = 'required'; properties = @() },
        @{ name = 'native'; properties = @(@{ name = 'LABELS'; value = @('gui') }) })
    foreach ($status in 'Skipped', 'PassedWithSkips', 'Interrupted', 'Unfinished', 'Failed') {
        $records = @{ required = @{ status = $status }; native = @{ status = 'Passed' } }
        $verdict = Get-CapabilityVerdict $requirements $inventory $records @('required', 'native')
        Assert-Contract ($verdict.state -in 'Failed', 'Blocked') "Required capability accepted $status."
        Assert-Contract ($records.required.status -eq $status) 'Capability verdict rewrote original CTest evidence.'
    }
    $records = @{ required = @{ status = 'Passed' }; native = @{ status = 'Passed' } }
    Assert-Contract ((Get-CapabilityVerdict $requirements $inventory $records @('required', 'native')).state -eq 'Passed') 'Complete required-capability evidence did not pass.'
    Assert-Contract ((Get-CapabilityVerdict $requirements $inventory $records @('required')).state -eq 'Blocked') 'An unselected GUI pass was reused without fingerprinting.'
    Assert-Contract ((Get-CapabilityVerdict $requirements @($inventory[0]) $records @('required')).state -eq 'Blocked') 'Absent GUI inventory accepted.'
    Assert-Contract ((Get-CapabilityVerdict $requirements $inventory @{} @('required', 'native')).state -eq 'Blocked') 'Missing capability evidence accepted.'
    Assert-Contract ((Get-CapabilityVerdict (Get-CapabilityRequirements 'development') $inventory @{} @()).state -eq 'NotApplicable') 'Development was represented as release acceptance.'
    [xml]$truncated = '<testsuite><testcase status="run"><system-out>PASS : Fixture::early()
[This part of the test output was removed since it exceeds the threshold of 1048576 bytes.]
</system-out></testcase></testsuite>'
    Assert-Contract ((Get-CaseResult $truncated.testsuite.testcase) -eq 'Unfinished') 'Truncated passing output was accepted as complete coverage.'
    $cachePath = Join-Path $root 'unit-hash-cache.json'
    $hashFile = Join-Path $root 'hash-input.bin'
    [IO.File]::WriteAllText($hashFile, 'abcd')
    $script:hashCalls = 0
    function Get-FileHash {
        param([string]$LiteralPath, [string]$Algorithm)
        $script:hashCalls++
        Microsoft.PowerShell.Utility\Get-FileHash -LiteralPath $LiteralPath -Algorithm $Algorithm
    }
    Initialize-HashCache $cachePath
    $parts = [ordered]@{}
    Add-FileDigest $parts $hashFile
    $initialHash = $parts["file:$hashFile"]
    Add-FileDigest $parts $hashFile
    Assert-Contract ($script:hashCalls -eq 1) 'Repeated input in one fingerprint pass was not deduplicated.'
    Save-HashCache
    Initialize-HashCache $cachePath
    Add-FileDigest $parts $hashFile
    Assert-Contract ($script:hashCalls -eq 2 -and $parts["file:$hashFile"] -eq $initialHash) 'New fingerprint pass trusted a persisted metadata-only digest.'
    $time = (Get-Item -LiteralPath $hashFile).LastWriteTimeUtc
    [IO.File]::WriteAllText($hashFile, 'wxyz')
    [IO.File]::SetLastWriteTimeUtc($hashFile, $time)
    Save-HashCache
    Initialize-HashCache $cachePath
    Add-FileDigest $parts $hashFile
    Assert-Contract ($script:hashCalls -eq 3 -and $parts["file:$hashFile"] -ne $initialHash) 'Same-length bytes with restored timestamp reused passing evidence.'
    $initialHash = $parts["file:$hashFile"]
    $time = (Get-Item -LiteralPath $hashFile).LastWriteTimeUtc
    [IO.File]::WriteAllText($hashFile, 'efgh')
    [IO.File]::SetLastWriteTimeUtc($hashFile, $time.AddSeconds(2))
    Add-FileDigest $parts $hashFile
    Assert-Contract ($script:hashCalls -eq 4 -and $parts["file:$hashFile"] -ne $initialHash) 'Same-length mtime change did not invalidate hash cache.'
    $time = (Get-Item -LiteralPath $hashFile).LastWriteTimeUtc
    $previousHash = $parts["file:$hashFile"]
    [IO.File]::WriteAllText($hashFile, 'longer content')
    [IO.File]::SetLastWriteTimeUtc($hashFile, $time)
    Add-FileDigest $parts $hashFile
    Assert-Contract ($script:hashCalls -eq 5 -and $parts["file:$hashFile"] -ne $previousHash) 'Length change with unchanged mtime did not invalidate hash cache.'
    Remove-Item -LiteralPath $hashFile
    Add-FileDigest $parts $hashFile
    Assert-Contract ($parts["file:$hashFile"] -eq 'MISSING' -and !$script:hashCache.ContainsKey($hashFile)) 'Missing file reused cached hash.'
    Remove-Item Function:\Get-FileHash
    Write-Host 'PASS: fingerprint passes hash exact bytes afresh, including restored metadata; duplicate inputs are deduplicated within one pass.'

    # Resolve a cached launch alias to the readable image, as both production
    # CMake consumers must do before evidence tries to hash the interpreter.
    $powershellModule = (Join-Path (Split-Path $PSScriptRoot -Parent) 'cmake/PowerShell.cmake').Replace('\', '/')
    $requestedPowerShell = (Get-Command pwsh.exe).Source.Replace('\', '/')
    $resolveScript = Join-Path $root 'resolve-powershell.cmake'
    @"
include("$powershellModule")
set(TEST_PWSH "$requestedPowerShell" CACHE FILEPATH "" FORCE)
okuflow_find_powershell(TEST_PWSH)
get_property(launcher CACHE TEST_PWSH PROPERTY VALUE)
if(NOT launcher STREQUAL "$requestedPowerShell")
    message(FATAL_ERROR "Discovery replaced the stable launcher cache")
endif()
file(SHA256 "`${TEST_PWSH}" image_hash)
file(WRITE "$($root.Replace('\', '/'))/resolved-powershell.txt" "`${TEST_PWSH}")
unset(TEST_PWSH)
set(TEST_PWSH "$($root.Replace('\', '/'))/removed-store-version/pwsh.exe" CACHE FILEPATH "" FORCE)
okuflow_find_powershell(TEST_PWSH)
file(SHA256 "`${TEST_PWSH}" rediscovered_hash)
if(NOT image_hash STREQUAL rediscovered_hash)
    message(FATAL_ERROR "Stale launcher rediscovery changed the interpreter")
endif()
"@ | Set-Content -LiteralPath $resolveScript
    & $CMake -P $resolveScript *> (Join-Path $root 'resolve-powershell.log')
    Assert-Contract ($LASTEXITCODE -eq 0) 'Cached PowerShell alias did not resolve to a hashable interpreter.'
    $resolvedPowerShell = Get-Content -LiteralPath (Join-Path $root 'resolved-powershell.txt') -Raw
    Assert-Contract ((Get-FileHash -LiteralPath $resolvedPowerShell -Algorithm SHA256).Hash -eq
        (Get-FileHash -LiteralPath "$PSHOME/pwsh.exe" -Algorithm SHA256).Hash) 'PowerShell discovery changed the interpreter.'

    # Representative pre-change layout: 92 tests, 12 distinct fingerprints
    # each, 500 common DLL/runtime keys. Preserve all compact attempt records.
    $shared = [ordered]@{}
    for ($i = 0; $i -lt 500; $i++) {
        $shared["file:C:\Qt\6.12.0\msvc2022_64\plugins\fixture-$i.dll"] = Get-Digest "runtime-$i"
    }
    $runtime = @{ shared = $shared; sharedDigest = (Get-Digest ($shared | ConvertTo-Json -Compress)) }
    $synthetic = @{ schema = 2; results = @{}; evidence = @{}; attempts = @() }
    for ($i = 0; $i -lt 92; $i++) {
        $name = "test-$i"
        $synthetic.evidence[$name] = @{}
        for ($j = 0; $j -lt 12; $j++) {
            $inputs = [ordered]@{}
            foreach ($key in $shared.Keys) { $inputs[$key] = $shared[$key] }
            $inputs.test = Get-Digest "$i-$j"
            $fingerprint = Get-Digest ($inputs | ConvertTo-Json -Compress)
            $record = @{ fingerprint = $fingerprint; inputs = $inputs; status = 'Passed'; attempt = ('attempt-{0:d2}' -f $j) }
            $synthetic.evidence[$name][$fingerprint] = $record
            if ($j -eq 0) {
                $record.status = @('Failed', 'Interrupted', 'Unfinished')[$i % 3]
                $synthetic.results[$name] = $record
            }
        }
    }
    $oldBytes = [Text.Encoding]::UTF8.GetByteCount(($synthetic | ConvertTo-Json -Depth 30))
    # Mimic a disk roundtrip: results and evidence no longer share references.
    $synthetic = $synthetic | ConvertTo-Json -Depth 30 -Compress | ConvertFrom-Json -AsHashtable
    Convert-LedgerInputs $synthetic $runtime
    $syntheticPath = Join-Path $root 'synthetic-ledger.json'
    Save-Ledger $synthetic $syntheticPath
    $newBytes = (Get-Item -LiteralPath $syntheticPath).Length
    $bounded = Get-Content -LiteralPath $syntheticPath -Raw | ConvertFrom-Json -AsHashtable
    Assert-Contract ($bounded.sharedInputs.Count -eq 1) 'Shared runtime inputs were duplicated.'
    foreach ($name in $bounded.evidence.Keys) {
        $history = $bounded.evidence[$name]
        $current = $bounded.results[$name]
        Assert-Contract ($history.Count -eq 4) "Unbounded fingerprint history: $name"
        Assert-Contract ($history[$current.fingerprint].status -eq $current.status) "Current blocking evidence pruned: $name"
        Assert-Contract (@($history.Values | Where-Object { $_.inputs.Count -ne 1 -or !$_.shared }).Count -eq 0) "Per-test inputs contain common runtime keys: $name"
        Assert-Contract (@($history.Values | Where-Object { $_.attempt -in 'attempt-09', 'attempt-10', 'attempt-11' }).Count -eq 3) "Most recent history not retained: $name"
    }
    Assert-Contract ($newBytes -lt ($oldBytes / 20)) 'Representative ledger did not shrink sufficiently.'
    # Unreferenced shared maps are collected when their fingerprints expire.
    $bounded.sharedInputs.unused = @{ obsolete = 'digest' }
    Limit-LedgerEvidence $bounded
    Assert-Contract (!$bounded.sharedInputs.ContainsKey('unused')) 'Unreferenced shared input history was retained.'
    # The blocking fingerprint can be old rather than current. A later fixed
    # build must not evict it and permit retry when those exact inputs return.
    foreach ($status in 'Failed', 'Interrupted', 'Unfinished') {
        $history = @{}
        $history.old = @{ fingerprint = 'old'; status = $status; attempt = '0000';
            inputs = @{ file = 'old bytes' }; shared = $runtime.sharedDigest }
        for ($i = 1; $i -le 8; $i++) {
            $history["pass-$i"] = @{ fingerprint = "pass-$i"; status = 'Passed'; attempt = "9999-$i";
                inputs = @{ file = "$i" }; shared = $runtime.sharedDigest }
        }
        $aged = @{ results = @{ test = $history['pass-8'] }; evidence = @{ test = $history };
            sharedInputs = @{ $runtime.sharedDigest = $shared } }
        Limit-LedgerEvidence $aged
        Assert-Contract ($aged.evidence.test.old.status -eq $status) "Historical $status fingerprint was evicted."
        Assert-Contract ($aged.evidence.test.old.blockerOnly -and !$aged.evidence.test.old.inputs.Count) 'Old blocker was not compacted.'
        $roundtrip = $aged | ConvertTo-Json -Depth 30 | ConvertFrom-Json -AsHashtable
        Convert-LedgerInputs $roundtrip $runtime
        $roundtrip.results.test = $roundtrip.evidence.test.old
        Limit-LedgerEvidence $roundtrip
        Assert-Contract ($roundtrip.results.test.status -eq $status) "Reverting to historical $status lost its blocker."
    }
    Write-Host "PASS: representative ledger $oldBytes -> $newBytes bytes (92 tests x 12 histories, 500 shared inputs); current blockers retained."
    $fixture = @'
param([string]$Case, [Parameter(ValueFromRemainingArguments)][string[]]$Remaining)
$countFile = Join-Path $PSScriptRoot "$Case.count"
$count = if (Test-Path $countFile) { [int](Get-Content $countFile) + 1 } else { 1 }
Set-Content $countFile $count
if ($Case -eq 'prerequisite' -and !(Test-Path (Join-Path $PSScriptRoot 'prerequisite.restored'))) {
    Write-Output 'FAIL! : Fixture::prerequisite() missing external prerequisite'
    exit 1
}
if ($Case -eq 'mutate') {
    $path = Join-Path $PSScriptRoot 'data.json'
    $time = (Get-Item -LiteralPath $path).LastWriteTimeUtc
    $bytes = [IO.File]::ReadAllBytes($path)
    $bytes[0] = $bytes[0] -bxor 1
    [IO.File]::WriteAllBytes($path, $bytes)
    [IO.File]::SetLastWriteTimeUtc($path, $time)
}
if ($Case -eq 'runtime_mutate') {
    $path = Join-Path $PSScriptRoot 'qt/bin/runtime.dll'
    $time = (Get-Item -LiteralPath $path).LastWriteTimeUtc
    $bytes = [IO.File]::ReadAllBytes($path)
    $bytes[0] = $bytes[0] -bxor 1
    [IO.File]::WriteAllBytes($path, $bytes)
    [IO.File]::SetLastWriteTimeUtc($path, $time)
}
if ($Case -eq 'inventory_mutate') {
    Add-Content (Join-Path $PSScriptRoot 'build/CTestTestfile.cmake') 'add_test(fixture_late "cmd.exe" "/c" "exit 0")'
}
if ($Case -eq 'generated') {
    for ($i = 1; $i -lt $Remaining.Count; $i += 2) {
        Set-Content -LiteralPath $Remaining[$i] -Value ([Guid]::NewGuid().ToString())
    }
}
if ($Case -in 'parallel_a', 'parallel_b', 'gpu', 'codec') {
    $start = [DateTime]::UtcNow.Ticks
    if ($Case -like 'parallel_*') {
        Set-Content (Join-Path $PSScriptRoot "$Case.ready") 'ready'
        $other = if ($Case -eq 'parallel_a') { 'parallel_b' } else { 'parallel_a' }
        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        while (!(Test-Path (Join-Path $PSScriptRoot "$other.ready"))) {
            if ([DateTime]::UtcNow -gt $deadline) { throw 'Background tests did not overlap.' }
            Start-Sleep -Milliseconds 50
        }
    }
    Start-Sleep -Milliseconds 300
    @{ start = $start; end = [DateTime]::UtcNow.Ticks } | ConvertTo-Json | Set-Content (Join-Path $PSScriptRoot "$Case.timing.json")
}
if ($Case -eq 'assertion') {
    Write-Output 'FAIL!  : Fixture::assertion() application assertion failed'
    Write-Output 'Totals: 0 passed, 1 failed, 0 skipped, 0 blacklisted'
    exit 1
}
if ($Case -eq 'interrupted' -and $count -eq 1) {
    Write-Output 'FAIL!  : Fixture::interrupted() OKUFLOW_DESKTOP_INTERRUPTED: native pointer moved during fixture'
    Write-Output 'Totals: 0 passed, 1 failed, 0 skipped, 0 blacklisted'
    exit 1
}
if ($Case -eq 'mixed') {
    Write-Output 'FAIL!  : Fixture::mixed() OKUFLOW_DESKTOP_INTERRUPTED: native activation failed'
    Write-Output 'FAIL!  : Fixture::mixed() application assertion failed'
    Write-Output 'Totals: 0 passed, 2 failed, 0 skipped, 0 blacklisted'
    exit 2
}
if ($Case -eq 'optional') {
    1..40 | ForEach-Object { Write-Output ('PASS   : Fixture::diagnostic() ' + ('x' * 100)) }
    Write-Output 'SKIP   : Fixture::optional() unavailable optional fixture'
    exit 0
}
if ($Case -eq 'skipped') { exit 77 }
Write-Output 'PASS   : Fixture::pass()'
Write-Output 'Totals: 1 passed, 0 failed, 0 skipped, 0 blacklisted'
'@
    Set-Content -LiteralPath (Join-Path $root 'fixture.ps1') -Value $fixture
    Set-Content -LiteralPath (Join-Path $root 'data.json') -Value 'original input'
    $header = @'
cmake_minimum_required(VERSION 3.23)
project(GateContract NONE)
enable_testing()
include("@POWERSHELL_MODULE@")
okuflow_find_powershell(PWSH)
# Absent GUI executable is inventory only, and can never execute in this suite.
add_test(NAME fixture_gui COMMAND "${CMAKE_SOURCE_DIR}/never-run-gui.exe")
set_tests_properties(fixture_gui PROPERTIES LABELS "gui")
'@
    $header = $header.Replace('@POWERSHELL_MODULE@', $powershellModule)
    $cmakeText = $header + @'

foreach(case pass interrupted assertion mixed optional skipped)
    add_test(NAME fixture_${case} COMMAND "${PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/fixture.ps1" -Case ${case})
    set_tests_properties(fixture_${case} PROPERTIES LABELS "contract" SKIP_RETURN_CODE 77)
endforeach()
'@
    Configure-Fixture ($cmakeText + @'

add_test(NAME fixture_absent COMMAND "${CMAKE_SOURCE_DIR}/absent.exe")
'@)
    Invoke-Gate -Options @('-Test', 'fixture_pass') -Expected 0 -Contains 'SCOPED PASS: 1 of 8 tests (fixture_pass)'
    # The batch's bookkeeping changes between the CPU and CUDA gates. Derive
    # its actual variable names, then exercise the real child gate so its
    # persisted fingerprint proves whether a pass would be repeated.
    $batch = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'agent_build.bat') -Raw
    $bookkeeping = @([regex]::Matches($batch, 'set "([A-Z_]*CUDA_(?:RESULT|SCOPE|SELECTION))=') |
        ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
    Assert-Contract ($bookkeeping.Count -eq 3) 'CUDA gate bookkeeping inventory is incomplete.'
    $saved = @{}
    try {
        foreach ($name in $bookkeeping) {
            $saved[$name] = [Environment]::GetEnvironmentVariable($name)
            [Environment]::SetEnvironmentVariable($name, 'second gate state')
        }
        Invoke-Gate -Options @('-Test', 'fixture_pass', '-Resume') -Expected 0
        Assert-Contract ((Get-Content (Join-Path $root 'pass.count')) -eq '1') 'Batch bookkeeping repeated a passing test.'
    } finally {
        foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    }
    Write-Host 'PASS: batch bookkeeping changes do not repeat matching evidence.'
    Invoke-Gate -Options @('-Label', 'unknown') -Expected 1 -Contains 'Unknown CTest label'
    Invoke-Gate -Options @('-Test', 'Fixture_pass') -Expected 1 -Contains 'Unknown exact test name'
    Invoke-Gate -Options @('-Select', 'not-a-selection') -Expected 1 -Contains 'Unknown test or label'
    Configure-Fixture $cmakeText
    Invoke-Gate -Options @('-Resume', '-Label', 'contract', '-Parallel', '2') -Expected 1
    $ledger = Read-Ledger
    Assert-Contract (!$ledger.results.ContainsKey('fixture_gui')) 'GUI test received credit.'
    Assert-Contract ($ledger.results.fixture_interrupted.status -eq 'Interrupted') 'Interruption not recognized.'
    Assert-Contract ($ledger.results.fixture_assertion.status -eq 'Failed') 'Assertion was not a failure.'
    Assert-Contract ($ledger.results.fixture_mixed.status -eq 'Failed') 'Mixed failures were retryable.'
    Assert-Contract ($ledger.results.fixture_optional.status -eq 'PassedWithSkips') 'Optional Qt skip was hidden.'
    Assert-Contract ($ledger.results.fixture_skipped.status -eq 'Skipped') 'CTest skip was hidden.'
    Assert-Contract ($ledger.sharedInputs.Count -eq 1) 'Executed tests duplicated their shared runtime map.'
    Assert-Contract (@($ledger.results.Values | Where-Object { $_ -and ($_.inputs.Contains('platform') -or !$_.shared) }).Count -eq 0) 'Executed result contains common runtime inputs.'
    # An old matching failure must survive pruning despite newer unrelated
    # fingerprints, then still suppress execution in a real resumed gate.
    $failed = $ledger.results.fixture_assertion
    for ($i = 0; $i -lt 9; $i++) {
        $fake = Get-Digest "other-inputs-$i"
        $ledger.evidence.fixture_assertion[$fake] = @{ fingerprint = $fake; inputs = @{ test = $fake };
            shared = $failed.shared; status = 'Passed'; attempt = "9999-$i" }
    }
    # Also emulate the round-1 flattened record, proving migration keeps its
    # exact matching fingerprint and blocking semantics across a disk reload.
    $legacyInputs = [ordered]@{}
    foreach ($key in $ledger.sharedInputs[$failed.shared].Keys) { $legacyInputs[$key] = $ledger.sharedInputs[$failed.shared][$key] }
    foreach ($key in $failed.inputs.Keys) { $legacyInputs[$key] = $failed.inputs[$key] }
    $failed.inputs = $legacyInputs
    $failed.Remove('shared')
    $ledger.evidence.fixture_assertion[$failed.fingerprint] = $failed
    $ledgerPath = (Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'OkuFlow/test-evidence') -Recurse -Filter 'ledger-v2.json')[0].FullName
    Save-Ledger $ledger $ledgerPath
    Invoke-Gate -Options @('-Test', 'fixture_assertion', '-Resume') -Expected 1 -Contains 'Failed: fixture_assertion'
    $ledger = Read-Ledger
    Assert-Contract ($ledger.evidence.fixture_assertion.Count -eq 4) 'Real gate history exceeded retention limit.'
    Assert-Contract ((Get-Content (Join-Path $root 'assertion.count')) -eq '1') 'Pruning caused a matching failed test to rerun.'
    Assert-Contract ($ledger.results.fixture_assertion.shared -and !$ledger.results.fixture_assertion.inputs.Contains('platform')) 'Legacy failure inputs were not deduplicated.'
    Write-Host 'PASS: matching legacy Failed evidence still blocks after pruning and migration.'
    $env:QT_SCALE_FACTOR = '1.619'
    Invoke-Gate -Options @('-Test', 'fixture_assertion', '-Resume') -Expected 1 -Contains 'Resume inputs changed for unresolved test: fixture_assertion'
    Assert-Contract ((Get-Content (Join-Path $root 'assertion.count')) -eq '1') 'Changed resume inputs retried an unresolved assertion.'
    $ledger = Read-Ledger
    $savedResult = $ledger.results.fixture_assertion
    $ledger.results.fixture_assertion = @{ fingerprint = $savedResult.fingerprint; status = 'Failed'; inputs = @{}; blockerOnly = $true }
    Save-Ledger $ledger $ledgerPath
    Invoke-Gate -Options @('-Test', 'fixture_assertion', '-Resume') -Expected 1 -Contains 'prior shared inputs unavailable'
    Assert-Contract ((Get-Content (Join-Path $root 'assertion.count')) -eq '1') 'A compact blocker with unknown runtime was retried.'
    $ledger.results.fixture_assertion = $savedResult
    Save-Ledger $ledger $ledgerPath
    $env:QT_SCALE_FACTOR = $originalScale
    Invoke-Gate -Options @('-ExcludeGui', '-Resume', '-RetryInterrupted', 'fixture_assertion') -Expected 1 -Contains 'Only a recorded desktop interruption'
    Invoke-Gate -Options @('-ExcludeGui', '-Resume', '-RetryInterrupted', 'fixture_mixed') -Expected 1 -Contains 'Only a recorded desktop interruption'
    Invoke-Gate -Options @('-ExcludeGui', '-Resume', '-RetryInterrupted', 'fixture_interrupted') -Expected 1
    $ledger = Read-Ledger
    Assert-Contract ($ledger.results.fixture_interrupted.status -eq 'Passed') 'Explicit interruption retry failed.'
    Assert-Contract ((Get-Content (Join-Path $root 'pass.count')) -eq '1') 'A previous pass was rerun.'
    Assert-Contract ((Get-Content (Join-Path $root 'assertion.count')) -eq '1') 'An application failure was retried.'
    Assert-Contract ($ledger.attempts.Count -eq 3) 'Attempt evidence was overwritten.'
    foreach ($attempt in $ledger.attempts) {
        Assert-Contract ((Test-Path $attempt.log) -and (Test-Path $attempt.junit)) 'Original evidence missing.'
    }
    Invoke-Gate -Options @('-ExcludeGui', '-Resume') -Expected 1
    Assert-Contract ((Get-Content (Join-Path $root 'assertion.count')) -eq '1') 'Resume retried a blocked failure.'
    Assert-Contract ((Get-Content (Join-Path $root 'optional.count')) -eq '2') 'Optional Qt skip was reused as a pass.'
    Assert-Contract ((Get-Content (Join-Path $root 'skipped.count')) -eq '2') 'CTest skip was reused as a pass.'
    Invoke-Gate -Options @('-Test', 'fixture_pass', '-Resume', '-Configuration', 'Debug') -Expected 1 -Contains 'No evidence matches these inputs'
    $env:QT_SCALE_FACTOR = '1.731'
    Invoke-Gate -Options @('-Test', 'fixture_pass', '-Resume') -Expected 0
    Assert-Contract ((Get-Content (Join-Path $root 'pass.count')) -eq '2') 'Environment change reused stale evidence.'
    $env:QT_SCALE_FACTOR = $originalScale
    $repaired = $fixture.Replace("if (`$Case -eq 'assertion')", 'if ($false)').Replace("if (`$Case -eq 'mixed')", 'if ($false)')
    Set-Content -LiteralPath (Join-Path $root 'fixture.ps1') -Value $repaired
    Invoke-Gate -Options @('-Test', 'fixture_assertion', '-Resume') -Expected 0
    Assert-Contract ((Get-Content (Join-Path $root 'assertion.count')) -eq '2') 'Repaired per-test inputs did not resume under the same shared runtime.'
    Invoke-Gate -Options @('-ExcludeGui') -Expected 0 -Contains '0 unresolved'
    Invoke-Gate -Options @('-ExcludeGui', '-Resume') -Expected 0 -Contains '0 unresolved'
    Assert-Contract ((Get-Content (Join-Path $root 'pass.count')) -eq '3') 'Successful resume repeated a pass.'
    $raw = Get-Content -LiteralPath (Get-ChildItem (Join-Path $env:LOCALAPPDATA 'OkuFlow/test-evidence') -Recurse -Filter 'ledger-v2.json')[0].FullName -Raw
    Assert-Contract (!$raw.Contains($env:OKUFLOW_GATE_CONTRACT_SECRET)) 'Secret environment value was persisted.'
    # An external prerequisite can be repaired without changing a test's bytes.
    # Normal fresh/resume gates must still block; only an audited recheck runs it.
    Configure-Fixture ($header + @'

add_test(NAME fixture_prerequisite COMMAND "${PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/fixture.ps1" -Case prerequisite)
add_test(NAME fixture_pass COMMAND "${PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/fixture.ps1" -Case pass)
'@)
    Invoke-Gate -Options @('-ExcludeGui') -Expected 1
    $failedPrerequisite = (Read-Ledger).results.fixture_prerequisite
    $passCount = Get-Content (Join-Path $root 'pass.count')
    Set-Content (Join-Path $root 'prerequisite.restored') 'original prerequisite restored'
    Invoke-Gate -Options @('-Test', 'fixture_prerequisite') -Expected 1
    Assert-Contract ((Get-Content (Join-Path $root 'prerequisite.count')) -eq '1') 'Ordinary fresh gate retried unchanged failed inputs.'
    Invoke-Gate -Options @('-Resume', '-RecheckAfterRepair', 'Restored the missing external prerequisite.') -Expected 1 -Contains 'cannot combine'
    Invoke-Gate -Options @('-ExcludeGui', '-RecheckAfterRepair', 'Restored the missing external prerequisite.') -Expected 1 -Contains 'requires exact -Test'
    Invoke-Gate -Options @('-Test', 'fixture_pass', '-RecheckAfterRepair', 'Restored the missing external prerequisite.') -Expected 1 -Contains 'Only a recorded'
    Invoke-Gate -Options @('-Test', 'fixture_prerequisite', '-RecheckAfterRepair', 'Restored the missing external prerequisite.') -Expected 0 -Contains 'Passed after diagnosed-repair recheck'
    $rechecked = Read-Ledger
    $repairAttempt = $rechecked.attempts[-1]
    Assert-Contract ((Get-Content (Join-Path $root 'prerequisite.count')) -eq '2') 'Explicit repaired-prerequisite recheck did not execute once.'
    Assert-Contract ((Get-Content (Join-Path $root 'pass.count')) -eq $passCount) 'Recheck unnecessarily repeated a matching pass.'
    Assert-Contract ($repairAttempt.repairReason -eq 'Restored the missing external prerequisite.') 'Repair diagnosis was not retained.'
    Assert-Contract ($repairAttempt.priorBlockers.fixture_prerequisite.attempt -eq $failedPrerequisite.attempt) 'Original blocking attempt was not recorded.'
    Assert-Contract ($rechecked.results.fixture_prerequisite.repairRecheckOf -eq $failedPrerequisite.attempt) 'Per-case repair identity missing.'
    Assert-Contract ($rechecked.capabilityChecks[-1].repairRechecks.fixture_prerequisite.reason -eq $repairAttempt.repairReason) 'Capability evidence hid repair provenance.'
    $oldAttempt = $rechecked.attempts | Where-Object id -eq $failedPrerequisite.attempt
    Assert-Contract ((Test-Path $oldAttempt.log) -and (Test-Path $oldAttempt.junit)) 'Recheck removed original failed evidence.'
    Configure-Fixture ($header + @'

add_test(NAME fixture_repair_fails COMMAND "cmd.exe" /c "exit 1")
'@)
    Invoke-Gate -Options @('-Test', 'fixture_repair_fails') -Expected 1
    Invoke-Gate -Options @('-Test', 'fixture_repair_fails', '-RecheckAfterRepair', 'Fixture simulates a diagnosed repair that still fails.') -Expected 1
    $failedRepairAttempt = (Read-Ledger).results.fixture_repair_fails.attempt
    Invoke-Gate -Options @('-Test', 'fixture_repair_fails', '-RecheckAfterRepair', 'Fixture simulates a second attempt at the same repair.') -Expected 1 -Contains 'Already rechecked'
    Assert-Contract ((Read-Ledger).results.fixture_repair_fails.attempt -eq $failedRepairAttempt) 'Unchanged failing repair was automatically repeated.'
    # Independent executable fingerprints: changing B cannot rerun A.
    foreach ($name in 'a', 'b') { Copy-Item -LiteralPath $CMake -Destination (Join-Path $root "$name.exe") }
    Configure-Fixture ($header + @'

foreach(case a b)
    add_test(NAME fixture_${case} COMMAND "${CMAKE_SOURCE_DIR}/${case}.exe" -E echo ${case})
    set_tests_properties(fixture_${case} PROPERTIES LABELS "independent")
endforeach()
'@)
    Invoke-Gate -Options @('-Label', 'independent') -Expected 0 -Contains 'SCOPED PASS: 2 of 3 tests'
    $ledger = Read-Ledger
    $aAttempt = $ledger.results.fixture_a.attempt
    $bAttempt = $ledger.results.fixture_b.attempt
    $stream = [IO.File]::Open((Join-Path $root 'b.exe'), 'Append')
    try { $stream.WriteByte(0) } finally { $stream.Dispose() }
    Invoke-Gate -Options @('-Label', 'independent', '-Resume') -Expected 0
    $ledger = Read-Ledger
    Assert-Contract ($ledger.results.fixture_a.attempt -eq $aAttempt) 'Unrelated executable invalidated A.'
    Assert-Contract ($ledger.results.fixture_b.attempt -ne $bAttempt) 'Changed executable reused B.'
    $aAttempt = $ledger.results.fixture_a.attempt
    Set-Content -LiteralPath (Join-Path $root 'neighbor.qm') -Value 'new translation runtime'
    Invoke-Gate -Options @('-Label', 'independent', '-Resume') -Expected 0
    $ledger = Read-Ledger
    Assert-Contract ($ledger.results.fixture_a.attempt -ne $aAttempt) 'Adjacent QM addition did not invalidate evidence.'
    $aAttempt = $ledger.results.fixture_a.attempt
    Add-Content -LiteralPath (Join-Path $root 'build/CTestTestfile.cmake') -Value 'set_tests_properties(fixture_a PROPERTIES ENVIRONMENT "FIXTURE_INPUT=changed")'
    Invoke-Gate -Options @('-Label', 'independent', '-Resume') -Expected 0
    $ledger = Read-Ledger
    Assert-Contract ($ledger.results.fixture_a.attempt -ne $aAttempt) 'Changed CTest properties reused evidence.'
    # REQUIRED_FILES catches non-command runtime data; unaffected tests retain credit.
    Configure-Fixture ($header + @'

foreach(case mutate pass)
    add_test(NAME fixture_${case} COMMAND "${PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/fixture.ps1" -Case ${case})
endforeach()
set_tests_properties(fixture_mutate PROPERTIES REQUIRED_FILES "${CMAKE_SOURCE_DIR}/data.json")
'@)
    Invoke-Gate -Options @('-ExcludeGui', '-Parallel', '2') -Expected 1 -Contains 'Inputs changed during testing*Changed:*data.json'
    $ledger = Read-Ledger
    Assert-Contract ($ledger.results.fixture_mutate.status -eq 'Unfinished') 'Changed data was credited.'
    Assert-Contract ($ledger.results.fixture_pass.status -eq 'Passed') 'Unrelated input mutation invalidated a pass.'
    $mutateAttempt = $ledger.results.fixture_mutate.attempt
    Set-Content -LiteralPath (Join-Path $root 'data.json') -Value 'original input'
    Invoke-Gate -Options @('-ExcludeGui', '-Resume') -Expected 1 -Contains 'Unfinished: fixture_mutate'
    Assert-Contract ((Read-Ledger).results.fixture_mutate.attempt -eq $mutateAttempt) 'Unfinished run was retried.'
    Set-Content -LiteralPath (Join-Path $root 'qt/bin/runtime.dll') -Value 'original runtime'
    Configure-Fixture ($header + @'

add_test(NAME fixture_runtime_mutate COMMAND "${PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/fixture.ps1" -Case runtime_mutate)
'@)
    Invoke-Gate -Options @('-ExcludeGui') -Expected 1 -Contains 'Inputs changed during testing*Changed:*runtime.dll'
    $ledger = Read-Ledger
    $runtimeResult = $ledger.results.fixture_runtime_mutate
    Assert-Contract ($runtimeResult.status -eq 'Unfinished' -and !$runtimeResult.inputs.Contains("file:$(Join-Path $root 'qt/bin/runtime.dll')")) 'Shared mutation was credited or stored per test.'
    Assert-Contract ($ledger.sharedInputs[$runtimeResult.shared].Contains("file:$(Join-Path $root 'qt/bin/runtime.dll')")) 'Named changed input is absent from shared evidence.'
    Configure-Fixture ($header + @'

add_test(NAME fixture_generated COMMAND "${PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/fixture.ps1" -Case generated --output "${CMAKE_BINARY_DIR}/result.bgra" --trace "${CMAKE_BINARY_DIR}/trace.csv")
'@)
    Invoke-Gate -Options @('-ExcludeGui') -Expected 0 -Contains '0 unresolved'
    Invoke-Gate -Options @('-ExcludeGui') -Expected 0 -Contains '0 unresolved'
    Configure-Fixture ($header + @'

foreach(case parallel_a parallel_b gpu codec)
    add_test(NAME fixture_${case} COMMAND "${PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/fixture.ps1" -Case ${case})
    set_tests_properties(fixture_${case} PROPERTIES LABELS "partition" TIMEOUT 25)
endforeach()
set_property(TEST fixture_gpu APPEND PROPERTY LABELS gpu)
set_property(TEST fixture_codec APPEND PROPERTY LABELS codec)
'@)
    Invoke-Gate -Options @('-ExcludeGui', '-Parallel', '2') -Expected 0
    $ledger = Read-Ledger
    $latest = @($ledger.attempts | Select-Object -Last 2)
    Assert-Contract ($latest[0].partition -eq 'background' -and $latest[0].parallel -eq 2 -and $latest[0].tests.Count -eq 2) 'Background partition is not parallel.'
    Assert-Contract ($latest[1].partition -eq 'gpu-codec' -and $latest[1].parallel -eq 1 -and $latest[1].tests.Count -eq 2) 'GPU/codec partition is not serial.'
    $times = @{}
    foreach ($name in 'parallel_a', 'parallel_b', 'gpu', 'codec') {
        $times[$name] = Get-Content -LiteralPath (Join-Path $root "$name.timing.json") -Raw | ConvertFrom-Json
        Assert-Contract ($ledger.results["fixture_$name"].status -eq 'Passed') 'Partition JUnit result was not merged.'
    }
    Assert-Contract ($times.parallel_a.start -lt $times.parallel_b.end -and $times.parallel_b.start -lt $times.parallel_a.end) 'Background fixtures did not actually overlap.'
    Assert-Contract ($times.gpu.end -le $times.codec.start -or $times.codec.end -le $times.gpu.start) 'GPU and codec fixtures overlapped.'
    Assert-Contract ([math]::Max($times.parallel_a.end, $times.parallel_b.end) -le [math]::Min($times.gpu.start, $times.codec.start)) 'Background and serial partitions overlapped.'
    # GUI partition inspection only: this contract forbids GUI-labeled runs.
    $runnerText = Get-Content -LiteralPath $runner -Raw
    Assert-Contract ($runnerText.Contains("name = 'gui'; parallel = 1; cases = @(")) 'Dedicated serial GUI partition is missing.'
    Invoke-Gate -Options @('-List', '-Select', 'gpu,fixture_parallel_a') -Expected 0 -Contains 'fixture_parallel_a'
    Assert-Contract ($lastOutput -match 'fixture_gpu' -and $lastOutput -notmatch 'fixture_gui|fixture_codec|fixture_parallel_b') 'Mixed selection was not any-match.'
    # Native PowerShell array binding provides exact any-match label selection.
    $labelOutput = & $runner -BuildDirectory (Join-Path $root 'build') -Label @('gpu', 'codec') -List 2>&1
    Assert-Contract ($LASTEXITCODE -eq 0 -and ($labelOutput -join ',') -eq 'fixture_gpu,fixture_codec') 'Label array was not any-match.'
    # Test the CUDA delta selector against a private comparison inventory and
    # private preset file, without configuring or touching real project builds.
    foreach ($directory in 'scripts', 'cmake', 'covered') {
        [void][IO.Directory]::CreateDirectory((Join-Path $root $directory))
    }
    Copy-Item -LiteralPath $runner -Destination (Join-Path $root 'scripts/run_test_gate.ps1')
    Set-Content -LiteralPath (Join-Path $root 'cmake/CMakePresets.json') -Value @'
{"configurePresets":[{"name":"msvc-cpu","binaryDir":"${sourceDir}/covered"}],"testPresets":[{"name":"msvc-cpu-tests","configurePreset":"msvc-cpu"}]}
'@
    Set-Content -LiteralPath (Join-Path $root 'covered/CTestTestfile.cmake') -Value @'
add_test(fixture_parallel_a "cmd.exe" "/c" "exit 0")
add_test(fixture_gpu "cmd.exe" "/c" "exit 0")
'@
    $originalRunner = $runner
    $runner = Join-Path $root 'scripts/run_test_gate.ps1'
    Invoke-Gate -Options @('-List', '-ExcludeGui', '-UncoveredByPreset', 'msvc-cpu-tests') -Expected 0
    Assert-Contract ($lastOutput -match 'fixture_parallel_b' -and $lastOutput -match 'fixture_gpu' -and $lastOutput -match 'fixture_codec' -and $lastOutput -notmatch 'fixture_parallel_a|fixture_gui') 'CUDA delta omitted GPU/unique tests or included a CPU-covered test.'
    # No cuda-variant label existed above; it is an optional built-in delta
    # criterion, never an unknown-label validation failure. Now cover that label.
    Add-Content -LiteralPath (Join-Path $root 'build/CTestTestfile.cmake') -Value 'set_tests_properties(fixture_parallel_a PROPERTIES LABELS "partition;cuda-variant")'
    Invoke-Gate -Options @('-List', '-ExcludeGui', '-UncoveredByPreset', 'msvc-cpu-tests') -Expected 0
    Assert-Contract ($lastOutput -match 'fixture_parallel_a' -and $lastOutput -match 'fixture_gpu' -and $lastOutput -match 'fixture_parallel_b' -and $lastOutput -notmatch 'fixture_gui') 'CUDA delta omitted a CPU-covered cuda-variant test.'
    Write-Host 'PASS: CUDA delta selects gpu, cuda-variant and unique tests, with or without the optional label.'
    $runner = $originalRunner
    # A full gate must notice newly registered tests even though it no longer
    # fingerprints the entire generated CTest file as a common input.
    Configure-Fixture ($header + @'

add_test(NAME fixture_inventory_mutate COMMAND "${PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/fixture.ps1" -Case inventory_mutate)
'@)
    Invoke-Gate -Options @('-ExcludeGui') -Expected 1 -Contains 'Test inventory changed during testing; new required tests: fixture_late'
    # Also exercise the unchanged full-gate verdict with no GUI in inventory.
    Configure-Fixture @'
cmake_minimum_required(VERSION 3.23)
project(GateContract NONE)
enable_testing()
add_test(NAME fixture_full COMMAND "${CMAKE_COMMAND}" -E echo full)
'@
    Invoke-Gate -Options @() -Expected 0 -Contains 'Gate: 1 passed (0 with skipped Qt rows), 0 skipped, 0 unresolved.'
    Assert-Contract ($lastOutput -notmatch 'SCOPED |PARTIAL VALIDATION') 'Full gate was mislabeled scoped or partial.'
    # Required kernel capability fixtures are private command-line cases, never
    # native GPU/GUI programs. Exercise actual runner exit and persisted verdict.
    $kernelRequirements = Get-CapabilityRequirements 'windows-cuda-kernels'
    foreach ($mode in 'skipped', 'optional', 'pass') {
        $capabilityFixture = @'
cmake_minimum_required(VERSION 3.23)
project(GateContract NONE)
enable_testing()
set(OKUFLOW_ENABLE_CUDA ON CACHE BOOL "fixture" FORCE)
'@
        $pwshPath = "$PSHOME/pwsh.exe".Replace('\', '/')
        $fixturePath = "$root/fixture.ps1".Replace('\', '/')
        foreach ($name in $kernelRequirements.tests) {
            $case = if ($name -eq 'yuv_color_conversion_cuda') { $mode } else { 'pass' }
            $capabilityFixture += "`nadd_test(NAME $name COMMAND `"$pwshPath`" -NoProfile -File `"$fixturePath`" -Case $case)`nset_tests_properties($name PROPERTIES LABELS `"gpu`" SKIP_RETURN_CODE 77)`n"
        }
        Configure-Fixture $capabilityFixture
        $expected = if ($mode -eq 'pass') { 0 } else { 1 }
        $state = if ($mode -eq 'pass') { 'Passed' } else { 'Blocked' }
        Invoke-Gate -Options @('-CapabilityProfile', 'windows-cuda-kernels') -Expected $expected -Contains "Capabilities (windows-cuda-kernels): $state"
        $ledger = Read-Ledger
        Assert-Contract ($ledger.capabilityChecks[-1].state -eq $state) 'Capability verdict missing from ledger.'
        Assert-Contract ($ledger.capabilityChecks[-1].evidence.yuv_color_conversion_cuda -eq $ledger.results.yuv_color_conversion_cuda.fingerprint) 'Capability acceptance detached from exact case bytes.'
    }
    Invoke-Gate -Options @('-CapabilityProfile', 'windows-cpu') -Expected 1 -Contains 'disagrees with the configured CUDA build'
    Invoke-Gate -Options @('-CapabilityProfile', 'windows-cuda-kernels', '-Test', 'yuv_color_conversion_cuda') -Expected 1 -Contains 'Capability NotRun'
    Write-Host "PASS: $checks gate contract checks in $([math]::Round($clock.Elapsed.TotalSeconds, 2)) seconds; no GUI executed."
    Write-Host "Private fixture evidence: $root"
} finally {
    $env:LOCALAPPDATA = $originalLocal
    $env:QT_SCALE_FACTOR = $originalScale
    $env:OKUFLOW_GATE_CONTRACT_SECRET = $originalSecret
}
