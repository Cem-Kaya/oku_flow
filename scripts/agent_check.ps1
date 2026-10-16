#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Base,
    [string[]]$Files,
    [switch]$IncludeGui,
    [switch]$AllowFull
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path $PSScriptRoot -Parent
if (!$IsWindows) { throw 'The fast CPU development check requires Windows and Visual Studio 2022.' }

# Import the environment of the same VS 2022 batch entry point as agent_build.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsDevCmd = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsDevCmd = @(& $vswhere -latest -version '[17.0,18.0)' -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find '**/VsDevCmd.bat') | Select-Object -First 1
}
$fallback = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
if (!$vsDevCmd -and (Test-Path -LiteralPath $fallback)) { $vsDevCmd = $fallback }
if (!$vsDevCmd) { throw 'Visual Studio 2022 x64 C++ tools were not found.' }
$env:PATH = (Split-Path $vswhere -Parent) + ';' + $env:PATH
$environment = & $env:ComSpec /d /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE) { throw 'VsDevCmd failed.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
if ($env:VisualStudioVersion -notlike '17.*' -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -ne 'x64') {
    throw 'Expected the Visual Studio 2022 x64 host/target environment.'
}
$vsRoot = Split-Path (Split-Path (Split-Path $vsDevCmd -Parent) -Parent) -Parent
if (!(Get-Command cl.exe).Source.StartsWith($vsRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The active compiler is outside the selected Visual Studio 2022 installation.'
}

Push-Location -LiteralPath $repo
try {
    $selectionArgs = @{ Preset = 'msvc-cpu-tests' }
    if ($Base) { $selectionArgs.Base = $Base }
    if ($PSBoundParameters.ContainsKey('Files')) { $selectionArgs.Files = $Files }
    # Documentation/Android-only changes do not require configuring Qt.
    $selection = & "$PSScriptRoot/select_tests.ps1" @selectionArgs | ConvertFrom-Json
    if ($selection.mode -eq 'none') {
        Write-Host "SCOPED: no Windows build/tests required. $($selection.reasons -join '; ')"
        Write-Host 'This is not the release gate.'
        exit 0
    }
    if (($selection.mode -eq 'full' -or $selection.tests.Count -or $selection.targets.Count) -and
        !(Test-Path -LiteralPath "$repo/build/msvc-cpu/build.ninja")) {
        & cmake --preset msvc-cpu
        if ($LASTEXITCODE) { throw 'CPU configure failed.' }
        $selection = & "$PSScriptRoot/select_tests.ps1" @selectionArgs | ConvertFrom-Json
    }
    if ($selection.mode -eq 'full' -and !$AllowFull) {
        Write-Host "FULL required: $($selection.reasons -join '; ')"
        Write-Host 'Run scripts/agent_build.bat for the release gate, or use -AllowFull for CPU background validation.'
        exit 2
    }
    if ($selection.mode -eq 'full') {
        & cmake --build --preset msvc-cpu-build
        if ($LASTEXITCODE) { throw 'CPU build failed.' }
        $inventory = (& ctest --preset msvc-cpu-tests --show-only=json-v1) -join "`n" | ConvertFrom-Json
        if ($LASTEXITCODE) { throw 'CTest discovery failed.' }
        $selection.tests = @($inventory.tests.name)
        $selection.guiTests = @($inventory.tests | Where-Object {
            'gui' -in @($_.properties | Where-Object name -eq LABELS | ForEach-Object value)
        } | ForEach-Object name)
        $selection.extraChecks = @('translations', 'android-host-protocol')
    } elseif ($selection.targets.Count) {
        & cmake --build --preset msvc-cpu-build --target @($selection.targets)
        if ($LASTEXITCODE) { throw 'Selected CPU build failed.' }
    }
    # -AllowFull deliberately remains background-only even with -IncludeGui.
    $allowGui = $IncludeGui -and $selection.mode -ne 'full'
    $pending = if ($allowGui) { @() } else { @($selection.guiTests) }
    $run = @($selection.tests | Where-Object { $_ -notin $pending })
    if ($run.Count) {
        $gate = @{ Preset = 'msvc-cpu-tests'; Test = $run }
        if (!$allowGui) { $gate.ExcludeGui = $true }
        # Never delete a live runner lease. Only wait for sharing violations;
        # application/test failures are returned unchanged and never retried.
        $lockPath = Join-Path $env:LOCALAPPDATA 'OkuFlow/test-evidence/runner.lock'
        $deadline = [DateTime]::UtcNow.AddMinutes(10)
        while (Test-Path -LiteralPath $lockPath) {
            try {
                $lease = [IO.File]::Open($lockPath, 'Open', 'ReadWrite', 'None')
                $lease.Dispose()
                break
            } catch [IO.IOException] {
                if ([DateTime]::UtcNow -ge $deadline) { throw 'Timed out waiting for the test runner lease; leave it intact.' }
                Write-Host 'Waiting for the existing test runner lease.'
                Start-Sleep -Seconds 5
            }
        }
        & "$PSScriptRoot/run_test_gate.ps1" @gate
        if ($LASTEXITCODE) { throw 'Selected test gate failed.' }
    }
    foreach ($check in $selection.extraChecks) {
        switch ($check) {
            'translations' {
                & "$PSScriptRoot/check_translations.ps1"
                if ($LASTEXITCODE) { throw 'Translation check failed.' }
            }
            'android-host-protocol' {
                & python -B -m unittest discover -s android/tests -v
                if ($LASTEXITCODE) { throw 'Android host protocol check failed.' }
            }
            default { throw "Unknown extra check: $check" }
        }
    }
    Write-Host "SCOPED PASS ($($selection.mode) selection). Tests ran ($($run.Count)): $($run -join ', ')"
    Write-Host "Extra checks: $($selection.extraChecks -join ', ')"
    Write-Host "GUI PENDING ($($pending.Count)): $($pending -join ', ')"
    Write-Host 'CPU development validation only. This is not the release gate.'
} finally {
    Pop-Location
}
