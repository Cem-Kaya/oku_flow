#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$QtPrefix,
    [string]$CacheRoot = (Join-Path $env:LOCALAPPDATA 'OkuFlow/tool-cache/qt-pdf-6.12.0')
)
$ErrorActionPreference = 'Stop'
$QtPrefix = [IO.Path]::GetFullPath($QtPrefix)
if (-not (Test-Path -LiteralPath (Join-Path $QtPrefix 'lib/cmake/Qt6Core/Qt6CoreConfig.cmake'))) {
    throw 'Install the matching Qt 6.12.0 MSVC x64 base SDK first.'
}
$version = Get-Content -LiteralPath (Join-Path $QtPrefix 'include/QtCore/qtcoreversion.h') -Raw
if ($version -notmatch 'QTCORE_VERSION_STR\s+"6\.12\.0"') { throw 'Qt PDF pin requires Qt 6.12.0.' }
$core = [IO.File]::ReadAllBytes((Join-Path $QtPrefix 'bin/Qt6Core.dll'))
$pe = [BitConverter]::ToInt32($core, 0x3c)
if ($pe -lt 64 -or $pe + 6 -ge $core.Length -or
    [BitConverter]::ToUInt16($core, $pe + 4) -ne 0x8664) { throw 'Qt PDF pin requires the Windows x64 SDK.' }
$name = '6.12.0-0-202607081026qtpdf-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z'
$url = 'https://download.qt.io/online/qtsdkrepository/windows_x86/extensions/qtpdf/6120/msvc2022_64/extensions.qtpdf.6120.win64_msvc2022_64/' + $name
$sha256 = '2de49f8cee61bd6d77bbb0fbdc57157340becdb8adb0a4a6dc4bd9ae6639a5d6'
New-Item -ItemType Directory -Path $CacheRoot -Force | Out-Null
$archive = Join-Path $CacheRoot $name
if (-not (Test-Path -LiteralPath $archive)) {
    $temporary = $archive + '.' + [Guid]::NewGuid().ToString('N') + '.download'
    Invoke-WebRequest -Uri $url -OutFile $temporary -TimeoutSec 120
    if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ine $sha256) {
        throw "Qt PDF download hash mismatch; retained $temporary"
    }
    Move-Item -LiteralPath $temporary -Destination $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $sha256) {
    throw "Qt PDF cached archive hash mismatch: $archive"
}
$sevenZip = (Get-Command 7z.exe -ErrorAction SilentlyContinue).Source
if (-not $sevenZip) { $sevenZip = Join-Path $env:ProgramFiles '7-Zip/7z.exe' }
if (-not (Test-Path -LiteralPath $sevenZip)) { throw '7-Zip is required to extract the pinned Qt SDK archive.' }
$extract = Join-Path $CacheRoot ('extract-' + [Guid]::NewGuid().ToString('N'))
& $sevenZip x $archive "-o$extract" -y -bso0
if ($LASTEXITCODE -ne 0) { throw "Qt PDF extraction failed; retained $extract" }
$files = @(Get-ChildItem -LiteralPath $extract -File -Recurse)
if (-not (Test-Path -LiteralPath (Join-Path $extract 'sbom/qtpdf-6.12.0.spdx.json'))) {
    throw 'The pinned PDF SDK lacks its original SPDX document.'
}
# Validate every existing destination before writing any new SDK file.
foreach ($file in $files) {
    $relative = [IO.Path]::GetRelativePath($extract, $file.FullName)
    $destination = Join-Path $QtPrefix $relative
    if ((Test-Path -LiteralPath $destination) -and
        (Get-FileHash -LiteralPath $destination).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
        throw "Existing SDK file differs; leave it untouched and use a matching SDK: $relative"
    }
}
foreach ($file in $files) {
    $destination = Join-Path $QtPrefix ([IO.Path]::GetRelativePath($extract, $file.FullName))
    if (-not (Test-Path -LiteralPath $destination)) {
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination
    }
}
Write-Output "PASS: pinned Qt PDF 6.12.0 SDK and notices verified ($($files.Count) files)."
