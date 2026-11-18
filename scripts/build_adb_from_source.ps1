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
$BuildRoot = [IO.Path]::GetFullPath([IO.Path]::Combine((Get-Location).ProviderPath, $BuildRoot))
# An MSYS2 package script split, and CMake's compiler check failed on, a build
# root containing spaces and '&' (2026-11-18); refuse such a path before
# downloading anything.
if ($BuildRoot -match '[ &]') {
    throw "Build root '$BuildRoot' contains a space or '&'; pass -BuildRoot with a plain path such as C:\adb-build."
}
if (-not $MsysRoot) { $MsysRoot = Join-Path $BuildRoot 'msys64' }
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
$inputs = Join-Path $BuildRoot 'inputs'
New-Item -ItemType Directory -Path $inputs -Force | Out-Null

function Get-VerifiedInput([string]$Name, [string]$Url, [string]$Sha256, [string]$Directory = $inputs) {
    $path = Join-Path $Directory $Name
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
# The build tools are pinned like the sources: a rolling MSYS2 update would
# otherwise replace them and clash with the pinned dependency packages.
# Relinking with -SkipToolchainSetup reuses an existing toolchain and needs
# none of these downloads.
$toolchain = Join-Path $BuildRoot 'toolchain'
$packageFiles = [Collections.Generic.List[string]]::new()
if (-not $SkipToolchainSetup) {
    New-Item -ItemType Directory -Path $toolchain -Force | Out-Null
    foreach ($package in $pins.toolchain) {
        $packageFiles.Add((Get-VerifiedInput $package.name $package.url $package.sha256 $toolchain))
    }
}
foreach ($input in @($pins.inputs | Where-Object dependency)) {
    $packageFiles.Add((Join-Path $inputs $input.name))
}
function Get-PackageIdentity([string]$FileName) {
    # name-version-release-arch.pkg.tar.zst -> "name version-release", as pacman -Q prints it.
    if ($FileName -notmatch '^(.+)-([^-]+)-([^-]+)-(any|x86_64)\.pkg\.tar\.zst$') {
        throw "Unrecognized package file name: $FileName"
    }
    return "$($Matches[1]) $($Matches[2])-$($Matches[3])"
}
$prepared = Join-Path $BuildRoot 'prepared'
$source = Join-Path $prepared 'android-tools-37.0.0'
$marker = Join-Path $prepared 'recipe.sha256'
$recipeHash = (Get-FileHash (Join-Path $recipeRoot 'pins.json')).Hash
# Refuse a build root prepared by another recipe before changing its MSYS2.
if (Test-Path -LiteralPath $marker) {
    if ((Get-Content $marker -Raw).Trim() -ne $recipeHash) {
        throw 'Prepared source belongs to a different recipe; choose a fresh -BuildRoot.'
    }
} elseif (Test-Path -LiteralPath $prepared) {
    throw 'Unmarked prepared source already exists; choose a fresh -BuildRoot.'
}
$MsysRoot = [IO.Path]::GetFullPath($MsysRoot).TrimEnd('\')
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
        # One transaction installs exactly the hash-checked package files. The
        # configuration lists no repositories, so a dependency missing from the
        # pins fails here instead of being fetched from the rolling repository.
        $conf = @(Get-Content -LiteralPath (Join-Path $MsysRoot 'etc\pacman.conf'))
        $firstRepository = 0
        while ($firstRepository -lt $conf.Count -and
               ($conf[$firstRepository] -notmatch '^\s*\[' -or $conf[$firstRepository] -match '^\s*\[options\]')) {
            $firstRepository++
        }
        if ($firstRepository -eq 0) { throw 'Unrecognized MSYS2 pacman.conf.' }
        Set-Content -LiteralPath (Join-Path $BuildRoot 'pacman-offline.conf') -Value $conf[0..($firstRepository - 1)] -Encoding ascii
        # Paths relative to the build root: bash adds the root it received as
        # UTF-16 environment, so a non-ASCII user profile path stays intact.
        $relativePaths = @($packageFiles | ForEach-Object {
            $_.Substring($BuildRoot.Length).TrimStart('\').Replace('\', '/') })
        [IO.File]::WriteAllText((Join-Path $BuildRoot 'pinned-packages.txt'), (($relativePaths -join "`n") + "`n"),
            [Text.UTF8Encoding]::new($false))
        & $bash -lc 'root="$(cygpath -u "$OKUFLOW_ADB_BUILD_ROOT")"; mapfile -t packages < "$root/pinned-packages.txt"; pacman --config "$root/pacman-offline.conf" -U --needed --noconfirm "${packages[@]/#/"$root"/}"'
        if ($LASTEXITCODE -ne 0) { throw 'Pinned toolchain and dependency installation failed.' }
    }
    # The packaged DLLs, notices and relinking archives must be the pinned
    # versions, also when an existing toolchain is reused.
    $checked = @($pins.inputs | Where-Object dependency)
    if (-not $SkipToolchainSetup) { $checked += @($pins.toolchain) }
    $expected = @($checked | ForEach-Object { Get-PackageIdentity $_.name } | Sort-Object)
    $installed = @(& $bash -lc ('pacman -Q ' + (($expected | ForEach-Object { ($_ -split ' ')[0] }) -join ' ')) | Sort-Object)
    $queryExit = $LASTEXITCODE
    $difference = @(Compare-Object $expected $installed | ForEach-Object { $_.InputObject })
    if ($queryExit -ne 0 -or $difference.Count) {
        throw "Installed MSYS2 packages differ from pins.json: $($difference -join '; ')"
    }
    $compiler = & (Join-Path $MsysRoot 'ucrt64\bin\g++.exe') -dumpfullversion
    if ($compiler.Trim() -ne $pins.compiler) {
        throw "Compiler $compiler differs from validated GCC $($pins.compiler). Review a toolchain change explicitly."
    }
    if (-not (Test-Path -LiteralPath $marker)) {
        New-Item -ItemType Directory -Path $prepared | Out-Null
        & tar -xJf (Join-Path $inputs 'android-tools-37.0.0.tar.xz') -C $prepared
        if ($LASTEXITCODE -ne 0) { throw 'adb source extraction failed.' }
        Expand-Archive (Join-Path $inputs 'adbwinapi_sources.zip') (Join-Path $prepared 'usb-api')
        $apiSource = Join-Path $prepared 'usb-api\development'
        Copy-Item (Join-Path $inputs 'usb-api-CMakeLists.txt') (Join-Path $apiSource 'CMakeLists.txt')
        Copy-Item (Join-Path $inputs 'USB_API_LICENSE.txt') (Join-Path $apiSource 'LICENSE')
        Copy-Item (Join-Path $inputs 'AdbWinApi.def') (Join-Path $source 'vendor')
        Copy-Item -LiteralPath $apiSource -Destination (Join-Path $source 'vendor\development') -Recurse
        & $bash -lc 'set -e; root="$(cygpath -u "$OKUFLOW_ADB_BUILD_ROOT")"; cd "$root/prepared/android-tools-37.0.0"; for file in 0001-android-tools-cmake 0002-android-tools-vendor 0003-fix-building-with-gcc-mingw 0004-leave-wstat-as-is 0005-missing-includes; do patch -p1 -i "$root/inputs/$file.patch"; done; cd vendor/adb; patch -p1 -i "$root/inputs/protobuf36.patch"'
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
