#Requires -Version 7.0
# Private command-line CTest fixtures only; never creates a Qt/Windows window.
param([Parameter(Mandatory)][string]$CMake)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$originalLocal = $env:LOCALAPPDATA
$originalScale = $env:QT_SCALE_FACTOR
$root = Join-Path $originalLocal ('OkuFlow/test-gate-contract/' + [Guid]::NewGuid().ToString('N'))
$runner = Join-Path $PSScriptRoot 'run_test_gate.ps1'
$clock = [Diagnostics.Stopwatch]::StartNew()
$checks = 0

function Assert-Contract([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
    $script:checks++
}

function Invoke-Gate([string[]]$Options, [int]$Expected, [string]$Contains) {
    $output = & pwsh -NoProfile -File $runner -BuildDirectory (Join-Path $root 'build') @Options 2>&1
    $actual = $LASTEXITCODE
    if ($actual -ne $Expected) { throw "Gate exit $actual, expected $Expected`n$($output -join "`n")" }
    if ($Contains -and ($output -join "`n") -notlike "*$Contains*") { throw "Missing gate diagnostic: $Contains`n$($output -join "`n")" }
    $script:checks++
}

function Read-Ledger {
    $records = @(Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'OkuFlow/test-evidence') -Recurse -Filter '*.json')
    Assert-Contract ($records.Count -eq 1) 'Expected one inventory ledger.'
    Get-Content -LiteralPath $records[0].FullName -Raw | ConvertFrom-Json -AsHashtable
}

try {
    [void][IO.Directory]::CreateDirectory($root)
    $env:LOCALAPPDATA = Join-Path $root 'private-local-app-data'
    [void][IO.Directory]::CreateDirectory($env:LOCALAPPDATA)
    foreach ($dir in 'qt/bin', 'qt/plugins', 'qt/lib/cmake/Qt6') {
        [void][IO.Directory]::CreateDirectory((Join-Path $root $dir))
    }
    $fixture = @'
param([string]$Case, [Parameter(ValueFromRemainingArguments)][string[]]$Remaining)
$countFile = Join-Path $PSScriptRoot "$Case.count"
$count = if (Test-Path $countFile) { [int](Get-Content $countFile) + 1 } else { 1 }
Set-Content $countFile $count
if ($Case -eq 'mutate') {
    Set-Content (Join-Path $PSScriptRoot 'qt/bin/runtime.dll') 'changed during execution'
}
if ($Case -eq 'generated') {
    for ($i = 1; $i -lt $Remaining.Count; $i += 2) {
        Set-Content -LiteralPath $Remaining[$i] -Value ([Guid]::NewGuid().ToString())
    }
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
    Write-Output 'SKIP   : Fixture::optional() unavailable optional fixture'
    exit 0
}
Write-Output 'PASS   : Fixture::pass()'
Write-Output 'Totals: 1 passed, 0 failed, 0 skipped, 0 blacklisted'
'@
    Set-Content -LiteralPath (Join-Path $root 'fixture.ps1') -Value $fixture
    $cmakeText = @'
cmake_minimum_required(VERSION 3.23)
project(GateContract NONE)
enable_testing()
find_program(PWSH pwsh REQUIRED)
foreach(case pass interrupted assertion mixed optional)
    add_test(NAME fixture_${case} COMMAND "${PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/fixture.ps1" -Case ${case})
    if(NOT case STREQUAL "pass")
        set_tests_properties(fixture_${case} PROPERTIES LABELS "gui")
    endif()
endforeach()
'@
    Set-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Value $cmakeText
    & $CMake -S $root -B (Join-Path $root 'build') "-DQt6_DIR:PATH=$root/qt/lib/cmake/Qt6" *> (Join-Path $root 'configure.log')
    if ($LASTEXITCODE) { throw 'Fixture CMake configuration failed.' }
    Invoke-Gate -Options @('-ExcludeGui') -Expected 0
    $ledger = Read-Ledger
    Assert-Contract ($ledger.results.Count -eq 1 -and $ledger.results.fixture_pass.status -eq 'Passed') 'Partial gate credited GUI tests.'
    Invoke-Gate -Options @('-Resume') -Expected 1
    $ledger = Read-Ledger
    Assert-Contract ($ledger.results.fixture_interrupted.status -eq 'Interrupted') 'Interruption not recognized.'
    Assert-Contract ($ledger.results.fixture_assertion.status -eq 'Failed') 'Assertion was not a failure.'
    Assert-Contract ($ledger.results.fixture_mixed.status -eq 'Failed') 'Mixed failures were retryable.'
    Assert-Contract ($ledger.results.fixture_optional.status -eq 'PassedWithSkips') 'Optional Qt skip was hidden.'
    Invoke-Gate -Options @('-Resume', '-RetryInterrupted', 'fixture_assertion') -Expected 1 -Contains 'Only a recorded desktop interruption'
    Invoke-Gate -Options @('-Resume', '-RetryInterrupted', 'fixture_mixed') -Expected 1 -Contains 'Only a recorded desktop interruption'
    Invoke-Gate -Options @('-Resume', '-RetryInterrupted', 'fixture_interrupted') -Expected 1
    $ledger = Read-Ledger
    Assert-Contract ($ledger.results.fixture_interrupted.status -eq 'Passed') 'Explicit interruption retry failed.'
    Assert-Contract ((Get-Content (Join-Path $root 'pass.count')) -eq '1') 'A previous pass was rerun.'
    Assert-Contract ((Get-Content (Join-Path $root 'assertion.count')) -eq '1') 'An application failure was retried.'
    Assert-Contract ($ledger.attempts.Count -eq 3) 'Attempt evidence was overwritten.'
    foreach ($attempt in $ledger.attempts) {
        Assert-Contract ((Test-Path $attempt.log) -and (Test-Path $attempt.junit)) 'Original evidence missing.'
    }
    Invoke-Gate -Options @('-Resume') -Expected 1
    Assert-Contract ((Get-Content (Join-Path $root 'assertion.count')) -eq '1') 'Resume retried a blocked failure.'
    Assert-Contract ((Get-Content (Join-Path $root 'optional.count')) -eq '2') 'Optional hardware skip was reused as a pass.'
    Invoke-Gate -Options @('-Resume', '-Configuration', 'Debug') -Expected 1 -Contains 'No evidence matches these inputs'
    $env:QT_SCALE_FACTOR = '1.731'
    Invoke-Gate -Options @('-Resume') -Expected 1 -Contains 'No evidence matches these inputs'
    $env:QT_SCALE_FACTOR = $originalScale
    Add-Content -LiteralPath (Join-Path $root 'fixture.ps1') -Value '# changed input'
    Invoke-Gate -Options @('-Resume') -Expected 1 -Contains 'No evidence matches these inputs'
    Set-Content -LiteralPath (Join-Path $root 'fixture.ps1') -Value $fixture
    Set-Content -LiteralPath (Join-Path $root 'qt/bin/runtime.dll') -Value 'changed runtime inventory'
    Invoke-Gate -Options @('-Resume') -Expected 1 -Contains 'No evidence matches these inputs'
    # A real fixture repair changes the input inventory. Fresh validation can
    # now pass, and a subsequent resume must reuse those exact successful runs.
    $repaired = $fixture.Replace("if (`$Case -eq 'assertion')", 'if ($false)').Replace("if (`$Case -eq 'mixed')", 'if ($false)')
    Set-Content -LiteralPath (Join-Path $root 'fixture.ps1') -Value $repaired
    Invoke-Gate -Options @() -Expected 0 -Contains '0 unresolved'
    Invoke-Gate -Options @('-Resume') -Expected 0 -Contains '0 unresolved'
    Assert-Contract ((Get-Content (Join-Path $root 'pass.count')) -eq '2') 'Successful full resume repeated a pass.'
    Set-Content -LiteralPath (Join-Path $root 'build/new-helper.exe') -Value 'changed executable inventory'
    Invoke-Gate -Options @('-Resume') -Expected 1 -Contains 'No evidence matches these inputs'
    # Mutations during a run must be rejected with the exact input name.
    $mutating = $cmakeText.Replace('pass interrupted assertion mixed optional', 'mutate')
    Set-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Value $mutating
    & $CMake -S $root -B (Join-Path $root 'build') "-DQt6_DIR:PATH=$root/qt/lib/cmake/Qt6" *> (Join-Path $root 'configure.log')
    if ($LASTEXITCODE) { throw 'Mutation fixture configuration failed.' }
    Invoke-Gate -Options @() -Expected 1 -Contains 'Inputs changed during testing*Changed:*runtime.dll'
    $generating = $cmakeText.Replace('pass interrupted assertion mixed optional', 'generated').Replace(
        '-Case ${case})', '-Case ${case} --output "${CMAKE_BINARY_DIR}/result.bgra" --trace "${CMAKE_BINARY_DIR}/trace.csv")')
    Set-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Value $generating
    & $CMake -S $root -B (Join-Path $root 'build') "-DQt6_DIR:PATH=$root/qt/lib/cmake/Qt6" *> (Join-Path $root 'configure.log')
    if ($LASTEXITCODE) { throw 'Generated-output fixture configuration failed.' }
    Invoke-Gate -Options @() -Expected 0 -Contains '0 unresolved'
    Invoke-Gate -Options @() -Expected 0 -Contains '0 unresolved'
    Write-Host "PASS: $checks gate contract checks in $([math]::Round($clock.Elapsed.TotalSeconds, 2)) seconds; no GUI executed."
    Write-Host "Private fixture evidence: $root"
} finally {
    $env:LOCALAPPDATA = $originalLocal
    $env:QT_SCALE_FACTOR = $originalScale
}
