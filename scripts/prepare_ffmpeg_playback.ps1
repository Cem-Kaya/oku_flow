#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$CacheRoot = (Join-Path $env:LOCALAPPDATA 'OkuFlow/embedded-video-sdk'),
    [string]$MsysRoot = (Join-Path $env:LOCALAPPDATA 'OkuFlow/adb-source-build/msys64'),
    [string]$Python = 'python',
    [ValidateRange(1,16)][int]$Jobs = 6
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$pins = Get-Content -LiteralPath "$PSScriptRoot/ffmpeg/pins.json" -Raw | ConvertFrom-Json
$CacheRoot = [IO.Path]::GetFullPath($CacheRoot)
$local = [IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\') + '\'
if (!$CacheRoot.StartsWith($local,[StringComparison]::OrdinalIgnoreCase) -or $CacheRoot -match "['`n`r]") {
    throw 'Use a private LocalAppData cache; no SDK inputs or builds belong in the checkout.'
}
$MsysRoot = (Resolve-Path -LiteralPath $MsysRoot).Path
foreach ($tool in @('usr/bin/bash.exe','usr/bin/cygpath.exe','ucrt64/bin/gcc.exe','ucrt64/bin/nasm.exe','ucrt64/bin/ninja.exe','ucrt64/bin/pkg-config.exe','ucrt64/bin/objdump.exe')) {
    if (!(Test-Path -LiteralPath "$MsysRoot/$tool")) { throw "Missing existing private MSYS2 prerequisite: $tool. No system package installation is performed." }
}
$gcc = (& "$MsysRoot/ucrt64/bin/gcc.exe" -dumpfullversion).Trim()
if ($gcc -ne $pins.gccVersion) { throw "Reviewed GCC $($pins.gccVersion) required; got $gcc." }
[IO.Directory]::CreateDirectory($CacheRoot) | Out-Null
foreach ($input in $pins.inputs) {
    $path = Join-Path $CacheRoot $input.name
    if (!(Test-Path -LiteralPath $path)) { Invoke-WebRequest -Uri $input.url -OutFile $path -TimeoutSec 180 }
    if ((Get-FileHash -LiteralPath $path).Hash -cne $input.sha256.ToUpperInvariant()) { throw "SDK input hash mismatch: $($input.name)" }
}
foreach ($source in @('ffmpeg-7.1.1','dav1d-1.5.1')) {
    if (!(Test-Path -LiteralPath "$CacheRoot/$source")) {
        & tar -xf "$CacheRoot/$source.tar.xz" -C $CacheRoot
        if ($LASTEXITCODE) { throw "Source extraction failed: $source" }
    }
}
# Verify every source file against the pinned archives before compiling a
# reused tree. Out-of-tree generated configuration never alters these inputs.
$verify = @'
import pathlib,sys,tarfile,hashlib
root=pathlib.Path(sys.argv[1])
for name in ('ffmpeg-7.1.1','dav1d-1.5.1'):
 with tarfile.open(root/(name+'.tar.xz')) as archive:
  for member in archive:
   if member.isfile():
    p=root/member.name
    if not p.is_file() or hashlib.sha256(p.read_bytes()).digest()!=hashlib.sha256(archive.extractfile(member).read()).digest():
     raise SystemExit('Modified source refused: '+member.name)
'@
$verify | & $Python -B - $CacheRoot
if ($LASTEXITCODE) { throw 'Pinned source-tree verification failed.' }
foreach ($pair in @(@('make','make-4.4.1-3-x86_64.pkg.tar.zst'),@('diffutils','diffutils-3.12-1-x86_64.pkg.tar.zst'))) {
    [IO.Directory]::CreateDirectory("$CacheRoot/$($pair[0])") | Out-Null
    & tar -xf "$CacheRoot/$($pair[1])" -C "$CacheRoot/$($pair[0])"
    if ($LASTEXITCODE) { throw 'Private build-tool extraction failed.' }
}
if (!(Test-Path -LiteralPath "$CacheRoot/tools/Scripts/python.exe")) {
    & $Python -m venv "$CacheRoot/tools"
    if ($LASTEXITCODE) { throw 'Private build-tool environment failed.' }
}
& "$CacheRoot/tools/Scripts/python.exe" -m pip install --no-index --no-deps "$CacheRoot/meson-1.7.0-py3-none-any.whl"
if ($LASTEXITCODE) { throw 'Pinned private Meson setup failed.' }
$savedPath=$env:PATH;$savedCc=$env:CC
try {
    $env:PATH="$CacheRoot/tools/Scripts;$MsysRoot/ucrt64/bin;$MsysRoot/usr/bin;$savedPath";$env:CC='gcc'
    if (!(Test-Path -LiteralPath "$CacheRoot/dav1d-build/build.ninja")) {
        & "$CacheRoot/tools/Scripts/meson.exe" setup "$CacheRoot/dav1d-build" "$CacheRoot/dav1d-1.5.1" --prefix "$CacheRoot/sdk" --buildtype release --default-library shared -Denable_tools=false -Denable_tests=false
        if ($LASTEXITCODE) { throw 'dav1d configure failed.' }
    }
    & "$MsysRoot/ucrt64/bin/ninja.exe" -C "$CacheRoot/dav1d-build" -j $Jobs install *> "$CacheRoot/dav1d-build-tracked.log"
    if ($LASTEXITCODE) { throw 'dav1d compilation failed; retain its log.' }
    # Use the actual MSYS2 cygpath, never an unrelated Anaconda/Git bridge.
    $rootPosix=(& "$MsysRoot/usr/bin/cygpath.exe" -u $CacheRoot).Trim()
    $configure=($pins.configure -join ' ')
    $shell=@"
set -eu
root='$rootPosix'
export PATH="`$root/make/usr/bin:`$root/diffutils/usr/bin:/ucrt64/bin:/usr/bin:`$PATH"
export PKG_CONFIG_PATH="`$root/sdk/lib/pkgconfig"
mkdir -p "`$root/ffmpeg-build"
cd "`$root/ffmpeg-build"
"`$root/ffmpeg-7.1.1/configure" --prefix="`$root/sdk" $configure > "`$root/ffmpeg-configure-tracked.log" 2>&1
make -j$Jobs > "`$root/ffmpeg-build-tracked.log" 2>&1
make install >> "`$root/ffmpeg-build-tracked.log" 2>&1
"@
    [IO.File]::WriteAllText("$CacheRoot/build-ffmpeg-tracked.sh",$shell.Replace("`r`n","`n"),[Text.UTF8Encoding]::new($false))
    $env:PATH="$MsysRoot/usr/bin;$MsysRoot/ucrt64/bin;$savedPath"
    & "$MsysRoot/usr/bin/bash.exe" "$CacheRoot/build-ffmpeg-tracked.sh"
    if ($LASTEXITCODE) { throw 'FFmpeg compilation failed; retain configure/build logs.' }
} finally { $env:PATH=$savedPath;$env:CC=$savedCc }
$pthread="$MsysRoot/ucrt64/bin/libwinpthread-1.dll"
if ((Get-FileHash -LiteralPath $pthread).Hash -cne $pins.winpthreadSha256.ToUpperInvariant()) { throw 'Reviewed private winpthread dependency differs.' }
Copy-Item -LiteralPath $pthread -Destination "$CacheRoot/sdk/bin/libwinpthread-1.dll"
$licenses="$CacheRoot/sdk/licenses"
[IO.Directory]::CreateDirectory($licenses) | Out-Null
Copy-Item -LiteralPath "$CacheRoot/ffmpeg-7.1.1/COPYING.LGPLv2.1" -Destination "$licenses/FFMPEG_LGPL_2_1.txt"
Copy-Item -LiteralPath "$CacheRoot/ffmpeg-7.1.1/LICENSE.md" -Destination "$licenses/FFMPEG_COMPONENTS.md"
Copy-Item -LiteralPath "$CacheRoot/dav1d-1.5.1/COPYING" -Destination "$licenses/DAV1D_COPYING.txt"
foreach ($component in @('libwinpthread','crt','headers')) {
    $sourceNoticeRoot=(Resolve-Path -LiteralPath "$MsysRoot/ucrt64/share/licenses/$component").Path
    foreach ($notice in Get-ChildItem -LiteralPath $sourceNoticeRoot -File -Recurse) {
        $destination=Join-Path "$licenses/$component" ([IO.Path]::GetRelativePath($sourceNoticeRoot,$notice.FullName))
        [IO.Directory]::CreateDirectory((Split-Path $destination)) | Out-Null
        Copy-Item -LiteralPath $notice.FullName -Destination $destination -Force
        if ((Get-FileHash -LiteralPath $destination).Hash -cne (Get-FileHash -LiteralPath $notice.FullName).Hash) { throw 'Notice byte verification failed.' }
    }
}
$inventory=@($pins.runtime | ForEach-Object {
    $p="$CacheRoot/sdk/bin/$_"
    if (!(Test-Path -LiteralPath $p)) { throw "Missing compiled runtime: $_" }
    @{name=$_;sha256=(Get-FileHash -LiteralPath $p).Hash;bytes=(Get-Item -LiteralPath $p).Length
      imports=@(& "$MsysRoot/ucrt64/bin/objdump.exe" -p $p | Select-String 'DLL Name:' | ForEach-Object {$_.Line.Trim()})}
})
$config=Get-Content -LiteralPath "$CacheRoot/ffmpeg-build/config.h" -Raw
if ($config -notmatch '#define CONFIG_GPL 0' -or $config -notmatch '#define CONFIG_NONFREE 0' -or $config -notmatch '#define CONFIG_LIBDAV1D 1') { throw 'Unexpected FFmpeg licensing/configuration.' }
$receipt=@{schema=1;developmentOnly=$true;publicRedistributionAccepted=$false;gcc=$gcc
    pins=$pins;runtime=$inventory;configure=(Get-Content -LiteralPath "$CacheRoot/ffmpeg-build/ffbuild/config.mak" | Select-String '^FFMPEG_CONFIGURATION=').Line
    nasm=(& "$MsysRoot/ucrt64/bin/nasm.exe" -v);python=(& $Python --version)
    configurationSha256=(Get-FileHash -LiteralPath "$CacheRoot/ffmpeg-build/config.h").Hash}
$receipt | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath "$CacheRoot/sdk/okuflow-ffmpeg-sdk.json" -Encoding utf8
Write-Host "Private dynamic SDK ready: $CacheRoot/sdk. No application/Qt runtime was changed or deployed."
