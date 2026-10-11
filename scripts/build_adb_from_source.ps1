#requires -Version 7.0
<#
.SYNOPSIS
Builds the redistributable Windows adb runtime and both USB DLLs from pinned source.
.DESCRIPTION
Uses an isolated MSYS2 UCRT64 toolchain and Visual Studio 2022. Source archives,
Windows patches and library packages are verified against adb-source/pins.json.
The output includes complete adb/libusb source and relinking inputs. It never
starts, replaces or stops an adb server. See docs/adb_distribution.md.
#>
[CmdletBinding()]
param(
    [string]$BuildRoot = (Join-Path $env:LOCALAPPDATA 'OkuFlow\adb-source-build'),
    [string]$MsysRoot,
    [string]$InputDirectory,
    [switch]$SkipToolchainSetup
)
$ErrorActionPreference = 'Stop'
$recipeRoot = Join-Path $PSScriptRoot 'adb-source'
$pins = Get-Content (Join-Path $recipeRoot 'pins.json') -Raw | ConvertFrom-Json
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if (-not $MsysRoot) { $MsysRoot = Join-Path $BuildRoot 'msys64' }
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
$inputs = Join-Path $BuildRoot 'inputs'
New-Item -ItemType Directory -Path $inputs -Force | Out-Null

function Get-VerifiedInput([string]$Name, [string]$Url, [string]$Sha256) {
    $path = Join-Path $inputs $Name
    if (-not (Test-Path -LiteralPath $path)) {
        $supplied = if ($InputDirectory) { Join-Path $InputDirectory $Name } else { '' }
        if ($supplied -and (Test-Path -LiteralPath $supplied)) {
            Copy-Item -LiteralPath $supplied -Destination $path
        } else { Invoke-WebRequest $Url -OutFile $path }
    }
    if ((Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -ne $Sha256) {
        throw "Checksum mismatch: $Name. Refusing unreviewed build input."
    }
    return $path
}
foreach ($input in $pins.inputs) {
    [void](Get-VerifiedInput $input.name $input.url $input.sha256)
}
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) {
    if ($MsysRoot -ne (Join-Path $BuildRoot 'msys64')) {
        throw 'An explicitly selected MSYS2 installation does not exist.'
    }
    $archive = Get-VerifiedInput 'msys2-base.tar.xz' $pins.msys2.url $pins.msys2.sha256
    & tar -xJf $archive -C $BuildRoot
    if ($LASTEXITCODE -ne 0) { throw 'MSYS2 extraction failed.' }
}
$previousMsystem = $env:MSYSTEM
$previousChere = $env:CHERE_INVOKING
$previousBuildRoot = $env:OKUFLOW_ADB_BUILD_ROOT
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$env:OKUFLOW_ADB_BUILD_ROOT = $BuildRoot
try {
    if (-not $SkipToolchainSetup) {
        # The first update may close MSYS2 to finish updating its runtime.
        & $bash -lc 'pacman -Syu --noconfirm'
        if ($LASTEXITCODE -ne 0) { throw 'MSYS2 runtime update failed.' }
        & $bash -lc 'pacman -Syu --noconfirm && pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-nasm mingw-w64-ucrt-x86_64-pkgconf patch git'
        if ($LASTEXITCODE -ne 0) { throw 'MSYS2 build-tool installation failed.' }
        & $bash -lc 'root="$(cygpath -u "$OKUFLOW_ADB_BUILD_ROOT")"; pacman -U --needed --noconfirm "$root"/inputs/*.pkg.tar.zst'
        if ($LASTEXITCODE -ne 0) { throw 'Pinned dependency installation failed.' }
    }
    $compiler = & (Join-Path $MsysRoot 'ucrt64\bin\g++.exe') -dumpfullversion
    if ($compiler.Trim() -ne $pins.compiler) {
        throw "Compiler $compiler differs from validated GCC $($pins.compiler). Review a toolchain change explicitly."
    }
    $prepared = Join-Path $BuildRoot 'prepared'
    $source = Join-Path $prepared 'android-tools-37.0.0'
    $marker = Join-Path $prepared 'recipe.sha256'
    $recipeHash = (Get-FileHash (Join-Path $recipeRoot 'pins.json')).Hash
    if (Test-Path -LiteralPath $marker) {
        if ((Get-Content $marker -Raw).Trim() -ne $recipeHash) {
            throw 'Prepared source belongs to a different recipe; choose a fresh -BuildRoot.'
        }
    } else {
        if (Test-Path -LiteralPath $prepared) {
            throw 'Unmarked prepared source already exists; choose a fresh -BuildRoot.'
        }
        New-Item -ItemType Directory -Path $prepared | Out-Null
        & tar -xJf (Join-Path $inputs 'android-tools-37.0.0.tar.xz') -C $prepared
        if ($LASTEXITCODE -ne 0) { throw 'adb source extraction failed.' }
        Expand-Archive (Join-Path $inputs 'adbwinapi_sources.zip') (Join-Path $prepared 'usb-api')
        $apiSource = Join-Path $prepared 'usb-api\development'
        Copy-Item (Join-Path $inputs 'usb-api-CMakeLists.txt') (Join-Path $apiSource 'CMakeLists.txt')
        Copy-Item (Join-Path $inputs 'USB_API_LICENSE.txt') (Join-Path $apiSource 'LICENSE')
        Copy-Item (Join-Path $inputs 'AdbWinApi.def') (Join-Path $source 'vendor')
        Copy-Item -LiteralPath $apiSource -Destination (Join-Path $source 'vendor\development') -Recurse
        & $bash -lc 'set -e; root="$(cygpath -u "$OKUFLOW_ADB_BUILD_ROOT")"; cd "$root/prepared/android-tools-37.0.0"; for file in "$root"/inputs/000[1-5]-*.patch; do patch -p1 -i "$file"; done; cd vendor/adb; patch -p1 -i "$root/inputs/protobuf36.patch"'
        if ($LASTEXITCODE -ne 0) { throw 'Source patching failed.' }
        Set-Content -LiteralPath $marker -Value $recipeHash -Encoding ascii
    }
    # Build only adb. Fastboot/filesystem tools and their GPL code do not link
    # into this executable. Wireless mDNS discovery is outside our USB feature.
    & $bash -lc 'set -e; root="$(cygpath -u "$OKUFLOW_ADB_BUILD_ROOT")"; cmake -S "$root/prepared/android-tools-37.0.0" -B "$root/repro-adb-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DANDROID_TOOLS_PATCH_VENDOR=OFF -DANDROID_TOOLS_USE_BUNDLED_LIBUSB=ON -DANDROID_TOOLS_USE_BUNDLED_FMT=OFF -DANDROID_TOOLS_ADB_ENABLE_MDNS=OFF -DANDROID_TOOLS_LIBUSB_ENABLE_UDEV=OFF -DCMAKE_CXX_FLAGS=-Wno-narrowing; cmake --build "$root/repro-adb-build" --target adb -j 6'
    if ($LASTEXITCODE -ne 0) { throw 'adb source compilation failed.' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = (& $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (-not $vs) { throw 'Visual Studio 2022 C++ tools are required for the USB DLLs.' }
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    & $cmake -S (Join-Path $prepared 'usb-api\development') -B (Join-Path $BuildRoot 'repro-api-build') -G 'Visual Studio 17 2022' -A x64
    if ($LASTEXITCODE -ne 0) { throw 'USB DLL configuration failed.' }
    & $cmake --build (Join-Path $BuildRoot 'repro-api-build') --config Release --parallel 6
    if ($LASTEXITCODE -ne 0) { throw 'USB DLL compilation failed.' }
    & (Join-Path $PSScriptRoot 'package_adb_source_build.ps1') -BuildRoot $BuildRoot -MsysRoot $MsysRoot
} finally {
    $env:MSYSTEM = $previousMsystem
    $env:CHERE_INVOKING = $previousChere
    $env:OKUFLOW_ADB_BUILD_ROOT = $previousBuildRoot
}
