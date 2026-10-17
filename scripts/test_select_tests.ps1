#Requires -Version 7.0
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$selector = Join-Path $PSScriptRoot 'select_tests.ps1'
$fixture = Join-Path ([IO.Path]::GetTempPath()) "okuflow impact-$([Guid]::NewGuid().ToString('N'))"
$build = Join-Path $fixture 'build'
[void][IO.Directory]::CreateDirectory($fixture)

function Assert-Selection([string]$Name, [string[]]$Files, [string]$Mode, [string[]]$Tests = @()) {
    $actual = & $selector -BuildDirectory $build -RepositoryRoot $fixture -Files $Files | ConvertFrom-Json
    if ($actual.mode -ne $Mode -or (@($actual.tests | Sort-Object) -join '|') -ne (@($Tests | Sort-Object) -join '|')) {
        throw "$Name failed: $($actual | ConvertTo-Json -Compress -Depth 8)"
    }
    Write-Host "PASS: $Name -> $Mode [$($actual.tests -join ', ')]"
    return $actual
}

try {
    @'
cmake_minimum_required(VERSION 3.23)
project(ImpactFixture LANGUAGES CXX)
enable_testing()
add_library(core STATIC core.cpp)
add_executable(dependent main.cpp)
target_link_libraries(dependent PRIVATE core)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated.cpp"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/asset.qrc" "${CMAKE_CURRENT_BINARY_DIR}/generated.cpp"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/asset.qrc")
target_sources(dependent PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated.cpp")
add_executable(unrelated unrelated.cpp)
add_executable(helper helper.cpp)
add_test(NAME dependent_test COMMAND dependent)
add_test(NAME dependent_gui COMMAND dependent)
set_tests_properties(dependent_gui PROPERTIES LABELS gui)
add_test(NAME unrelated_test COMMAND unrelated)
add_test(NAME data_test COMMAND unrelated "${CMAKE_CURRENT_SOURCE_DIR}/input.txt")
set_tests_properties(data_test PROPERTIES REQUIRED_FILES "${CMAKE_CURRENT_SOURCE_DIR}/required.txt")
add_test(NAME helper_test COMMAND unrelated "$<TARGET_FILE:helper>")
add_test(NAME shared_source_equivalence COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/guard.cmake")
set_tests_properties(shared_source_equivalence PROPERTIES
    REQUIRED_FILES "${CMAKE_CURRENT_SOURCE_DIR}/core.cpp;${CMAKE_CURRENT_SOURCE_DIR}/core.hpp")
'@ | Set-Content -LiteralPath "$fixture/CMakeLists.txt" -Encoding utf8
    '#pragma once', 'inline int value() { return 0; }' | Set-Content -LiteralPath "$fixture/core.hpp"
    '#include "core.hpp"', 'int core() { return value(); }' | Set-Content -LiteralPath "$fixture/core.cpp"
    'int core(); int main() { return core(); }' | Set-Content -LiteralPath "$fixture/main.cpp"
    'int main() { return 0; }' | Set-Content -LiteralPath "$fixture/unrelated.cpp"
    'int main() { return 0; }' | Set-Content -LiteralPath "$fixture/helper.cpp"
    'int resourceValue() { return 0; }' | Set-Content -LiteralPath "$fixture/asset.qrc"
    'fixture' | Set-Content -LiteralPath "$fixture/input.txt"
    'fixture' | Set-Content -LiteralPath "$fixture/required.txt"
    '# Synthetic script-based guard; no executable target owns its runtime inputs.' | Set-Content -LiteralPath "$fixture/guard.cmake"
    & cmake -S $fixture -B $build -G Ninja -DCMAKE_POLICY_DEFAULT_CMP0141=NEW -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded
    if ($LASTEXITCODE) { throw 'Synthetic configure failed. Run in a developer shell.' }
    if ($IsWindows) {
        $manifest = Get-Content -LiteralPath "$build/build.ninja" -Raw
        if ($manifest -notmatch '[-/]Z7\b' -or $manifest -match '[-/]Zi\b') { throw 'CI Embedded debug-info policy is ineffective.' }
        Write-Host 'PASS: CMake 3.23 policy baseline accepts CMP0141=NEW and /Z7 from the command line'
    }
    & cmake --build $build
    if ($LASTEXITCODE) { throw 'Synthetic build failed.' }
    $header = Assert-Selection 'header dependency' @('core.hpp') scoped @('dependent_test', 'dependent_gui', 'shared_source_equivalence')
    if (@($header.guiTests) -join ',' -ne 'dependent_gui') { throw 'GUI classification failed.' }
    if (!$header.targets.Count) { throw 'Selected executable build target is missing.' }
    if (@($header.targets | Where-Object { $_ -match 'unrelated|helper' }).Count) { throw 'Unrelated build targets selected.' }
    & cmake --build $build --target @($header.targets)
    if ($LASTEXITCODE) { throw 'Selected target names are not buildable.' }
    $null = Assert-Selection 'static library transitive source and script guard' @('core.cpp') scoped @('dependent_test', 'dependent_gui', 'shared_source_equivalence')
    $null = Assert-Selection 'generated resource input' @('asset.qrc') scoped @('dependent_test', 'dependent_gui')
    $null = Assert-Selection 'unrelated executable' @('unrelated.cpp') scoped @('unrelated_test', 'data_test', 'helper_test')
    $null = Assert-Selection 'rooted helper executable dependency' @('helper.cpp') scoped @('helper_test')
    $null = Assert-Selection 'rooted command input' @('input.txt') scoped @('data_test')
    $null = Assert-Selection 'required input' @('required.txt') scoped @('data_test')
    $null = Assert-Selection 'docs only' @('docs/example.md') none
    $null = Assert-Selection 'CMake input' @('CMakeLists.txt') full
    $null = Assert-Selection 'unknown input' @('unknown.bin') full
    $null = Assert-Selection 'mixed docs and unknown' @('README.md', 'unknown.bin') full
    $android = Assert-Selection 'Android host protocol' @('android/protocol.py') scoped
    if ('android-host-protocol' -notin $android.extraChecks) { throw 'Missing Android extra check.' }
    $probe = Assert-Selection 'host probe only' @('scripts/android_camera_probe.py') scoped
    if ('android-host-protocol' -notin $probe.extraChecks -or $probe.tests.Count -or $probe.targets.Count) {
        throw 'Probe-only changes must select host protocol checks without a Windows rebuild.'
    }
    # A configured but unbuilt object must never hide header consumers.
    'add_library(unbuilt STATIC unbuilt.cpp)' | Add-Content -LiteralPath "$fixture/CMakeLists.txt"
    '#include "core.hpp"' | Set-Content -LiteralPath "$fixture/unbuilt.cpp"
    & cmake -S $fixture -B $build
    if ($LASTEXITCODE) { throw 'Incomplete-inventory configure failed.' }
    $null = Assert-Selection 'incomplete dependency inventory' @('core.hpp') full
    & cmake --build $build
    if ($LASTEXITCODE) { throw 'Completing inventory failed.' }
    $null = Assert-Selection 'completed dependency inventory' @('core.hpp') scoped @('dependent_test', 'dependent_gui', 'shared_source_equivalence')
    $unused = Assert-Selection 'library without a test consumer' @('unbuilt.cpp') scoped
    if (!$unused.targets.Count) { throw 'An affected library still needs compilation.' }
    $objectFile = Join-Path $build $(if ($IsWindows) { 'CMakeFiles/core.dir/core.cpp.obj' } else { 'CMakeFiles/core.dir/core.cpp.o' })
    Move-Item -LiteralPath $objectFile -Destination "$objectFile.saved"
    $null = Assert-Selection 'stale dependency inventory' @('core.hpp') full
    Move-Item -LiteralPath "$objectFile.saved" -Destination $objectFile
    # Check git base together with staged/unstaged/untracked paths.
    & git -C $fixture init -q
    & git -C $fixture -c user.name=Fixture -c user.email=fixture@example.invalid add CMakeLists.txt core.cpp core.hpp main.cpp unrelated.cpp helper.cpp asset.qrc unbuilt.cpp input.txt required.txt guard.cmake
    & git -C $fixture -c user.name=Fixture -c user.email=fixture@example.invalid commit -qm baseline
    if ($LASTEXITCODE) { throw 'Fixture git initialization failed.' }
    'new data' | Set-Content -LiteralPath "$fixture/required.txt"
    & git -C $fixture add required.txt
    'changed data' | Set-Content -LiteralPath "$fixture/input.txt"
    'untracked' | Set-Content -LiteralPath "$fixture/unknown.bin"
    # Ignore generated build files, just as the production checkout does.
    'build/' | Set-Content -LiteralPath "$fixture/.git/info/exclude"
    $dirty = & $selector -BuildDirectory $build -RepositoryRoot $fixture -Base HEAD | ConvertFrom-Json
    if ($dirty.mode -ne 'full' -or 'data_test' -notin $dirty.tests) { throw 'Working-tree/untracked selection failed.' }
    Write-Host 'PASS: git base includes staged, unstaged and untracked inputs'
    Move-Item -LiteralPath "$build/.ninja_deps" -Destination "$build/saved-deps"
    $null = Assert-Selection 'missing dependency inventory' @('core.hpp') full
    $null = Assert-Selection 'docs without dependency inventory' @('README.md') none
    Write-Host 'PASS: all change-selection fixtures (desktop-free; no CTest execution)'
} finally {
    # Delete only this exact fresh fixture under the OS temp directory.
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (![IO.Path]::GetFullPath($fixture).StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cleanup escaped temp.' }
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
