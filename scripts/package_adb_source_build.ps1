#requires -Version 7.0
<# Builds a complete redistribution directory; called after source compilation.
   This script copies only the transitive adb runtime, its notices and source.
   It never invokes a server command. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildRoot,
    [Parameter(Mandatory)][string]$MsysRoot
)
$ErrorActionPreference = 'Stop'
$recipe = Join-Path $PSScriptRoot 'adb-source'
$pins = Get-Content (Join-Path $recipe 'pins.json') -Raw | ConvertFrom-Json
$output = Join-Path $BuildRoot 'build-output'
if (Test-Path -LiteralPath $output) {
    $resolvedRoot = [IO.Path]::GetFullPath($BuildRoot).TrimEnd('\') + '\'
    $resolvedOutput = (Resolve-Path -LiteralPath $output).Path
    if (-not $resolvedOutput.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Generated output escaped the selected build directory.'
    }
    Move-Item -LiteralPath $resolvedOutput -Destination (Join-Path $BuildRoot ('previous-output-' + [guid]::NewGuid().ToString('N')))
}
$runtime = Join-Path $output 'platform-tools'
$licenses = Join-Path $output 'licenses\adb'
$sources = Join-Path $output 'sources'
New-Item -ItemType Directory -Path $runtime,$licenses,$sources -Force | Out-Null
$vendor = Join-Path $BuildRoot 'prepared\android-tools-37.0.0\vendor'
Copy-Item (Join-Path $BuildRoot 'repro-adb-build\vendor\adb.exe') $runtime
foreach ($dll in @('AdbWinApi.dll','AdbWinUsbApi.dll')) {
    Copy-Item (Join-Path $BuildRoot "repro-api-build\Release\$dll") $runtime
}
$objdump = Join-Path $MsysRoot 'ucrt64\bin\objdump.exe'
$queue = [Collections.Generic.Queue[string]]::new()
Get-ChildItem $runtime -File | ForEach-Object { $queue.Enqueue($_.FullName) }
$seen = @{}
while ($queue.Count) {
    $file = $queue.Dequeue()
    foreach ($line in (& $objdump -p $file)) {
        if ($line -notmatch 'DLL Name:\s*(.+)$') { continue }
        $name = $Matches[1].Trim()
        if ($seen.ContainsKey($name)) { continue }
        $seen[$name] = $true
        if (Test-Path -LiteralPath (Join-Path $runtime $name)) { continue }
        $dependency = Join-Path $MsysRoot "ucrt64\bin\$name"
        if (Test-Path -LiteralPath $dependency) {
            Copy-Item -LiteralPath $dependency -Destination $runtime
            $queue.Enqueue((Join-Path $runtime $name))
        } elseif ($name -notlike 'api-ms-win-*' -and
                  -not (Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) $name))) {
            throw "Unresolved runtime dependency: $name"
        }
    }
}
$versionOutput = & (Join-Path $runtime 'adb.exe') version
if ($LASTEXITCODE -ne 0 -or ($versionOutput -join "`n") -notmatch 'version 1\.0\.41') {
    throw 'The source-built adb runtime did not start or report protocol 41.'
}

# Keep full original notices. BoringSSL's file contains its composite licenses.
$noticeFiles = [ordered]@{
    'AOSP_ADB_NOTICE.txt' = (Join-Path $vendor 'adb\NOTICE')
    'APACHE_2_0.txt' = (Join-Path $BuildRoot 'inputs\USB_API_LICENSE.txt')
    'LIBUSB_LGPL_2_1.txt' = (Join-Path $vendor 'libusb\COPYING')
    'LIBUSB_NOTICE.txt' = (Join-Path $vendor 'libusb\NOTICE')
    'BORINGSSL_LICENSE.txt' = (Join-Path $vendor 'boringssl\LICENSE')
    'MSYS2_RECIPE_LICENSE.txt' = (Join-Path $BuildRoot 'inputs\MSYS2_RECIPE_LICENSE.txt')
    'UTF8_RANGE_LICENSE.txt' = (Join-Path $BuildRoot 'inputs\UTF8_RANGE_LICENSE.txt')
}
foreach ($name in $noticeFiles.Keys) {
    Copy-Item -LiteralPath $noticeFiles[$name] -Destination (Join-Path $licenses $name)
}
$dependencyLicenses = @('abseil-cpp','brotli','fmt','protobuf','zstd','zlib',
                      'libgcc','libstdc++','libwinpthread','crt','headers')
foreach ($name in $dependencyLicenses) {
    $directory = Join-Path $MsysRoot "ucrt64\share\licenses\$name"
    if (-not (Test-Path -LiteralPath $directory)) { throw "Missing notice directory: $name" }
    Copy-Item -LiteralPath $directory -Destination (Join-Path $licenses $name) -Recurse
}
New-Item -ItemType Directory -Path (Join-Path $licenses 'lz4') | Out-Null
Copy-Item (Join-Path $BuildRoot 'inputs\LZ4_LICENSE.txt') (Join-Path $licenses 'lz4\LICENSE')
[IO.File]::WriteAllText((Join-Path $runtime 'source.properties'),
    "Pkg.Revision=$($pins.version)`nPkg.Desc=OkuFlow source-built Android Debug Bridge (USB)`n", [Text.UTF8Encoding]::new($false))
$notice = @'
OkuFlow source-built Android Debug Bridge 37.0.0, server protocol 41.
adb.exe, AdbWinApi.dll and AdbWinUsbApi.dll were compiled from pinned source.
This is an OkuFlow build, not a Google-signed SDK package.
adb is a separate executable and is not linked into OkuFlow.

adb/AOSP Windows USB sources: Apache-2.0.
Statically linked libusb: LGPL-2.1-or-later; BoringSSL: composite upstream license.
Runtime libraries: Abseil (Apache-2.0), Protobuf (BSD-3-Clause), utf8_range
(MIT), Brotli (MIT), LZ4 (BSD-2-Clause), Zstandard (BSD-3-Clause), zlib (Zlib),
GCC libraries (GPL-3.0-or-later WITH GCC-exception-3.1), winpthreads (MIT AND BSD-3-Clause).
fmt headers: MIT. MinGW runtime/header notices are also retained.

Exact notices: ../licenses/adb/ (including full LGPL text).
Complete adb/libusb source, patches, exact dependency/relinking inputs and
instructions: ../sources/adb-37.0.0-okuflow-source.zip.
Build provenance and runtime checksums: adb-build.json.

You may modify libusb and reverse engineer adb to debug those modifications.
OkuFlow's commercial license does not limit the rights granted by libusb's LGPL.
Redistribution must retain the notices and corresponding source/relinking materials.
'@
[IO.File]::WriteAllText((Join-Path $runtime 'NOTICE.txt'), $notice, [Text.UTF8Encoding]::new($false))

$sourcePackage = Join-Path $BuildRoot ('source-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $sourcePackage 'scripts') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $sourcePackage 'scripts\adb-source') | Out-Null
# The distribution pin refers to this package's manifest; it is a release
# approval record, not a rebuild input. Avoid a circular source archive pin.
foreach ($name in @('pins.json','README.md')) {
    Copy-Item (Join-Path $recipe $name) (Join-Path $sourcePackage 'scripts\adb-source')
}
foreach ($name in @('build_adb_from_source.ps1','package_adb_source_build.ps1','test_adb_source_build.ps1')) {
    Copy-Item (Join-Path $PSScriptRoot $name) (Join-Path $sourcePackage 'scripts')
}
Copy-Item (Join-Path $recipe 'README.md') (Join-Path $sourcePackage 'README.md')
New-Item -ItemType Directory -Path (Join-Path $sourcePackage 'inputs') | Out-Null
foreach ($input in $pins.inputs) {
    $file = Join-Path $BuildRoot "inputs\$($input.name)"
    if ((Get-FileHash $file).Hash.ToLowerInvariant() -ne $input.sha256) {
        throw "Source/relinking input changed: $($input.name)"
    }
    Copy-Item $file (Join-Path $sourcePackage 'inputs')
}
Copy-Item -LiteralPath $licenses -Destination (Join-Path $sourcePackage 'licenses') -Recurse
Compress-Archive -Path (Join-Path $sourcePackage '*') -DestinationPath (Join-Path $sources 'adb-37.0.0-okuflow-source.zip')

$componentLicenses = [ordered]@{
    'abseil-cpp'='Apache-2.0'; 'brotli'='MIT'; 'fmt'='MIT'; 'lz4'='BSD-2-Clause'
    'protobuf'='BSD-3-Clause'; 'zstd'='BSD-3-Clause'; 'zlib'='Zlib'
    'libgcc'='GPL-3.0-or-later WITH GCC-exception-3.1'
    'libstdc++'='GPL-3.0-or-later WITH GCC-exception-3.1'
    'libwinpthread'='MIT AND BSD-3-Clause'
}
$components = @(
    [ordered]@{name='libusb';version='1.0.29 AOSP e392f3b7f7c3550c665793db2eea37a95c3bb105';license='LGPL-2.1-or-later';linkage='static';source='sources/adb-37.0.0-okuflow-source.zip';notice='licenses/adb/LIBUSB_LGPL_2_1.txt'}
    [ordered]@{name='BoringSSL';version='snapshot in android-tools 37.0.0 source archive';license='LicenseRef-BoringSSL';linkage='static';source='sources/adb-37.0.0-okuflow-source.zip';notice='licenses/adb/BORINGSSL_LICENSE.txt'}
    [ordered]@{name='utf8-range';version='Protobuf 36.2 bundled snapshot';license='MIT';linkage='dynamic';source='https://github.com/protocolbuffers/protobuf/tree/v36.2/third_party/utf8_range';notice='licenses/adb/UTF8_RANGE_LICENSE.txt'}
)
foreach ($name in $componentLicenses.Keys) {
    $input = $pins.inputs | Where-Object name -match ('^mingw-w64-ucrt-x86_64-'+[regex]::Escape($name)+'-[0-9]') | Select-Object -First 1
    $version = $input.name.Replace("mingw-w64-ucrt-x86_64-$name-",'').Replace('-any.pkg.tar.zst','')
    $components += [ordered]@{name=$name;version=$version;license=$componentLicenses[$name];linkage=$(if($name -eq 'fmt'){'headers'}else{'dynamic'});source=$input.url;notice="licenses/adb/$name"}
}
$records = @(Get-ChildItem $output -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($output.Length).TrimStart('\').Replace('\','/');sha256=(Get-FileHash $_.FullName).Hash.ToLowerInvariant();bytes=$_.Length}
})
$manifest = [ordered]@{
    version=$pins.version;protocol=$pins.protocol;sourceBuilt=$true
    compiler=$pins.compiler;visualStudio='2022';mdns=$false
    recipeSha256=(Get-FileHash (Join-Path $recipe 'pins.json')).Hash.ToLowerInvariant()
    upstreamInputs=$pins.inputs;components=$components;files=$records
    rebuild='sources/adb-37.0.0-okuflow-source.zip'
}
$manifest | ConvertTo-Json -Depth 9 | Set-Content (Join-Path $runtime 'adb-build.json') -Encoding utf8NoBOM
Write-Host "Packaged source-built adb runtime, notices and relinking source in $output"
