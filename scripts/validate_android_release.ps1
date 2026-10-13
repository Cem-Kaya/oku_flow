#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Apk,
    [string]$Bundle,
    [string]$BuildRoot,
    [string]$Sdk = $env:ANDROID_HOME,
    [string]$Jdk = $env:JAVA_HOME,
    [switch]$AllowUnsignedRelease,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
# Expected signature failures in unsigned mode are inspected explicitly below.
$PSNativeCommandUseErrorActionPreference = $false

function Get-ApkUploadCertificateDigest {
    param([string]$Output)
    # Android build-tools 37 reports "V2 Signer:" instead of "Signer #1".
    # Repeated scheme reports may identify one key; distinct signers fail closed.
    $matches = [regex]::Matches($Output, '(?mi)^(?:Signer #\d+|V[234](?:\.\d+)? Signer):? certificate SHA-256 digest:\s*([a-f0-9]{64})\s*$')
    $digests = @($matches | ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } | Sort-Object -Unique)
    if ($digests.Count -ne 1) { throw 'APK must report exactly one valid upload certificate identity.' }
    return $digests[0]
}

function Assert-Elf16K {
    param([byte[]]$Bytes, [string]$Name)
    if ($Bytes.Length -lt 64 -or $Bytes[0] -ne 0x7f -or $Bytes[1] -ne 0x45 -or
        $Bytes[2] -ne 0x4c -or $Bytes[3] -ne 0x46 -or $Bytes[4] -ne 2 -or $Bytes[5] -ne 1) {
        throw "Invalid little-endian ELF64 library: $Name"
    }
    $table = [BitConverter]::ToUInt64($Bytes, 32)
    $entrySize = [BitConverter]::ToUInt16($Bytes, 54)
    $entryCount = [BitConverter]::ToUInt16($Bytes, 56)
    if ($entrySize -lt 56 -or $entryCount -lt 1 -or $entryCount -gt 4096 -or
        $table -gt $Bytes.Length -or ($entryCount * $entrySize) -gt ($Bytes.Length - $table)) {
        throw "Invalid ELF program table: $Name"
    }
    $loads = 0
    for ($index = 0; $index -lt $entryCount; $index++) {
        $offset = [int]($table + $index * $entrySize)
        if ([BitConverter]::ToUInt32($Bytes, $offset) -ne 1) { continue }
        $loads++
        $alignment = [BitConverter]::ToUInt64($Bytes, $offset + 48)
        $fileOffset = [BitConverter]::ToUInt64($Bytes, $offset + 8)
        $address = [BitConverter]::ToUInt64($Bytes, $offset + 16)
        if ($alignment -lt 16384 -or ($alignment -band ($alignment - 1)) -ne 0 -or
            ($fileOffset % $alignment) -ne ($address % $alignment)) {
            throw "Native library does not meet 16 KB ELF load alignment: $Name"
        }
    }
    if (-not $loads) { throw "ELF contains no load segments: $Name" }
}

function Assert-LocalOutput {
    param([string]$Path, [string]$Root)
    $repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\', '/')
    if (-not [IO.Path]::IsPathFullyQualified($Path)) { throw 'Release artifact/output paths must be absolute.' }
    $absolute = [IO.Path]::GetFullPath($Path)
    if ($absolute -match '^[Yy]:[\\/]' -or $absolute -eq $repo -or
        $absolute.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Release artifacts and validation outputs must stay outside the synced checkout/Drive.'
    }
    if ($Root -and -not $absolute.StartsWith([IO.Path]::GetFullPath($Root).TrimEnd('\', '/') +
        [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Release artifacts must belong to the selected local build root.'
    }
    return $absolute
}

function Assert-ArchiveLibraries {
    param([string]$Path)
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $duplicates = @($archive.Entries | Group-Object FullName | Where-Object Count -gt 1)
        if ($duplicates.Count) { throw 'Ambiguous duplicate ZIP entries in release artifact.' }
        $native = @($archive.Entries | Where-Object { $_.FullName -match '(^|/)lib/(arm64-v8a|x86_64)/[^/]+\.so$' })
        foreach ($entry in $native) {
            if ($entry.Length -gt 64MB) { throw "Native entry exceeds the bounded inspection size: $($entry.FullName)" }
            $input = $entry.Open()
            $buffer = [IO.MemoryStream]::new()
            try {
                $input.CopyTo($buffer)
                Assert-Elf16K -Bytes $buffer.ToArray() -Name $entry.FullName
            } finally { $input.Dispose(); $buffer.Dispose() }
        }
        return $native.Count
    } finally { $archive.Dispose() }
}

function Assert-PackagedNotices {
    param([string]$Path, [string]$Prefix, $Inventory)
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $archive.GetEntry($Prefix + 'runtime-review.json')
        if (-not $entry -or $entry.Length -gt 4MB) { throw 'Packaged runtime license review manifest is missing or oversized.' }
        $input = $entry.Open()
        $buffer = [IO.MemoryStream]::new()
        try { $input.CopyTo($buffer); $bytes = $buffer.ToArray() }
        finally { $input.Dispose(); $buffer.Dispose() }
        $digest = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
        if ($digest -ne $Inventory.reviewManifestSha256) { throw 'Packaged review manifest does not match the resolved release inventory.' }
        $review = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
        $actual = @($Inventory.artifacts | ForEach-Object { "$($_.module)|$($_.artifact)|$($_.sha256)" } | Sort-Object -Unique)
        $expected = @($review.components | ForEach-Object { "$($_.module)|$($_.artifact)|$($_.sha256)" } | Sort-Object -Unique)
        if (@(Compare-Object $actual $expected).Count) { throw 'Packaged license review differs from the actual runtime graph.' }
        foreach ($asset in $review.assetFiles.PSObject.Properties) {
            if ($asset.Name -match '(^|/)\.\.|^[/\\]') { throw 'Unsafe retained license asset path.' }
            $retained = $archive.GetEntry($Prefix + $asset.Name)
            if (-not $retained -or $retained.Length -gt 4MB) { throw 'A required packaged license/NOTICE asset is missing or oversized.' }
            $input = $retained.Open()
            $hasher = [Security.Cryptography.SHA256]::Create()
            try { $actualHash = [Convert]::ToHexString($hasher.ComputeHash($input)).ToLowerInvariant() }
            finally { $input.Dispose(); $hasher.Dispose() }
            if ($actualHash -ne $asset.Value) { throw 'Packaged license/NOTICE hash mismatch.' }
        }
        return $review.uniqueArtifacts
    } finally { $archive.Dispose() }
}

function Assert-R8DiagnosticMapping {
    param([string]$Text)
    $compiler = [regex]::Match($Text, '(?m)^# compiler_version: ([0-9]+(?:\.[0-9]+)+)\r?$')
    if ($Text -notmatch '(?m)^# compiler: R8\r?$' -or -not $compiler.Success -or
        $Text -notmatch '"id":"com.android.tools.r8.mapping"' -or
        $Text -notmatch '(?m)^# pg_map_id: [a-f0-9]+\r?$') {
        throw 'Diagnostic mapping must contain the actual R8 compiler/version/mapping metadata.'
    }
    $classes = [regex]::Matches($Text, '(?m)^([^\s:]+) -> ([^\s:]+):\r?$')
    if (-not $classes.Count) { throw 'Diagnostic mapping contains no classes.' }
    $composeAliases = 0
    foreach ($class in $classes) {
        # AGP appends its generated Compose source retrace alias. It is not a runtime class rename.
        if ($class.Groups[1].Value -ceq 'ComposeStackTrace' -and $class.Groups[2].Value -ceq '$$compose') {
            $composeAliases++
        } elseif ($class.Groups[1].Value -cne $class.Groups[2].Value) {
            throw 'Diagnostic R8 policy forbids class-name obfuscation.'
        }
    }
    $required = @{
        'com.okuflow.camera.MainActivity' = 'onCreate'
        'com.okuflow.camera.camera.CameraStreamService' = 'onDestroy'
        'com.okuflow.camera.camera.StreamingPauseReceiver' = 'onReceive'
        'com.okuflow.camera.camera.CameraEngine' = 'stop'
        'com.okuflow.camera.link.UsbSocketServer' = 'disconnectHost'
        'com.okuflow.camera.setup.SetupViewModel' = 'onResumed'
    }
    foreach ($component in $required.GetEnumerator()) {
        $name = [regex]::Escape($component.Key)
        $body = [regex]::Match($Text, "(?m)^$name -> ${name}:\r?\n([\s\S]*?)(?=^[^\s:]+ -> [^\s:]+:\r?$|\z)")
        if (-not $body.Success -or $body.Groups[1].Value -cnotmatch
            ('(?m)^[ \t]+[^\r\n]+ -> ' + [regex]::Escape($component.Value) + '\r?$')) {
            throw 'Diagnostic mapping must retain readable application component and lifecycle method names.'
        }
    }
    if ($Text -notmatch '"id":"sourceFile"' -or $Text -notmatch '(?m)^[ \t]+[0-9]+:[0-9]+:') {
        throw 'Diagnostic mapping must include generated source-file and line mappings.'
    }
    return @{ compiler = 'R8'; compilerVersion = $compiler.Groups[1].Value; classes = $classes.Count - $composeAliases;
        composeRetraceAliases = $composeAliases;
        classNamesPreserved = $true; applicationLifecycleNamesPreserved = $true }
}

function Assert-BundleDiagnosticMapping {
    param([string]$BundlePath, [string]$MappingPath)
    if (-not (Test-Path -LiteralPath $MappingPath -PathType Leaf)) { throw 'Build-generated release mapping.txt is missing.' }
    $size = (Get-Item -LiteralPath $MappingPath).Length
    if ($size -eq 0 -or $size -gt 64MB) { throw 'Build-generated mapping is empty or exceeds the bounded inspection size.' }
    $local = [IO.File]::ReadAllBytes($MappingPath)
    if ($local.Length -ne $size) { throw 'Build-generated mapping changed during validation.' }
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($local)).ToLowerInvariant()
    $archive = [IO.Compression.ZipFile]::OpenRead($BundlePath)
    try {
        $entry = $archive.GetEntry('BUNDLE-METADATA/com.android.tools.build.obfuscation/proguard.map')
        if (-not $entry -or $entry.Length -ne $local.Length) { throw 'AAB must embed the exact build-generated R8 mapping.' }
        $input = $entry.Open()
        $hasher = [Security.Cryptography.SHA256]::Create()
        try { $embeddedHash = [Convert]::ToHexString($hasher.ComputeHash($input)).ToLowerInvariant() }
        finally { $input.Dispose(); $hasher.Dispose() }
        if ($embeddedHash -ne $hash) { throw 'AAB mapping does not match the local build-generated mapping.txt.' }
    } finally { $archive.Dispose() }
    $text = [Text.UTF8Encoding]::new($false, $true).GetString($local)
    $result = Assert-R8DiagnosticMapping $text
    $result.sha256 = $hash
    $result.bytes = $local.Length
    $result.embedded = $true
    return $result
}

function Assert-ApplicationIdentity {
    param([xml]$Manifest)
    $ns = 'http://schemas.android.com/apk/res/android'
    if ($Manifest.manifest.package -ne 'com.okuflow') { throw 'APK package must match the immutable Play package com.okuflow.' }
    $activity = @($Manifest.manifest.application.activity | Where-Object { $_.GetAttribute('name', $ns) -eq 'com.okuflow.camera.MainActivity' })
    if ($activity.Count -ne 1 -or $activity[0].GetAttribute('exported', $ns) -ne 'true' -or
        $activity[0].GetAttribute('launchMode', $ns) -notin @('singleTask', '2')) {
        throw 'APK launch activity must be exported com.okuflow.camera.MainActivity with singleTask mode.'
    }
    $filters = @($activity[0].'intent-filter' | Where-Object {
        @($_.action | Where-Object { $_.GetAttribute('name', $ns) -eq 'android.intent.action.MAIN' }).Count -gt 0 -and
        @($_.category | Where-Object { $_.GetAttribute('name', $ns) -eq 'android.intent.category.LAUNCHER' }).Count -gt 0
    })
    if ($filters.Count -ne 1) { throw 'The fully qualified activity must have one MAIN/LAUNCHER filter.' }
}

function Assert-CameraAppLink {
    param([xml]$Manifest)
    $ns = 'http://schemas.android.com/apk/res/android'
    $activity = @($Manifest.manifest.application.activity | Where-Object { $_.GetAttribute('name', $ns) -eq 'com.okuflow.camera.MainActivity' })
    $links = @($activity[0].'intent-filter' | Where-Object {
        @($_.action | Where-Object { $_.GetAttribute('name', $ns) -eq 'android.intent.action.VIEW' }).Count -gt 0
    })
    if ($links.Count -ne 1 -or $links[0].GetAttribute('autoVerify', $ns) -ne 'true') { throw 'One verified camera app link is required.' }
    foreach ($category in @('android.intent.category.DEFAULT', 'android.intent.category.BROWSABLE')) {
        if (@($links[0].category | Where-Object { $_.GetAttribute('name', $ns) -eq $category }).Count -ne 1) { throw 'Camera app-link category missing.' }
    }
    $data = @($links[0].data)
    if ($data.Count -ne 1 -or $data[0].GetAttribute('scheme', $ns) -ne 'https' -or
        $data[0].GetAttribute('host', $ns) -ne 'okuflow.com' -or $data[0].GetAttribute('path', $ns) -ne '/camera/open' -or
        $data[0].HasAttribute('pathPrefix', $ns) -or $data[0].HasAttribute('pathPattern', $ns)) {
        throw 'App links must cover only https://okuflow.com/camera/open.'
    }
}

if ($SelfTest) {
    # Exercise the rejection boundary: good alignment, 4 KB, bad congruence and malformed tables.
    $fixture = [byte[]]::new(120)
    $fixture[0] = 0x7f; $fixture[1] = 0x45; $fixture[2] = 0x4c; $fixture[3] = 0x46
    $fixture[4] = 2; $fixture[5] = 1
    [BitConverter]::GetBytes([uint64]64).CopyTo($fixture, 32)
    [BitConverter]::GetBytes([uint16]56).CopyTo($fixture, 54)
    [BitConverter]::GetBytes([uint16]1).CopyTo($fixture, 56)
    [BitConverter]::GetBytes([uint32]1).CopyTo($fixture, 64)
    [BitConverter]::GetBytes([uint64]16384).CopyTo($fixture, 112)
    Assert-Elf16K $fixture 'valid-16k'
    foreach ($case in @('4k', 'incongruent', 'truncated-table')) {
        $broken = [byte[]]$fixture.Clone()
        switch ($case) {
            '4k' { [BitConverter]::GetBytes([uint64]4096).CopyTo($broken, 112) }
            'incongruent' { [BitConverter]::GetBytes([uint64]1).CopyTo($broken, 72) }
            'truncated-table' { [BitConverter]::GetBytes([uint64]119).CopyTo($broken, 32) }
        }
        $rejected = $false
        try { Assert-Elf16K $broken $case } catch { $rejected = $true }
        if (-not $rejected) { throw "Release validator accepted invalid fixture: $case" }
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $testDirectory = Join-Path ([IO.Path]::GetTempPath()) ('OkuFlow-notices-test-' + [Guid]::NewGuid().ToString('N'))
    $testDirectory = Assert-LocalOutput $testDirectory
    [IO.Directory]::CreateDirectory($testDirectory) | Out-Null
    $archivePath = Join-Path $testDirectory 'fixture.apk'
    $noticeBytes = [Text.Encoding]::UTF8.GetBytes('original upstream notice')
    $noticeHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($noticeBytes)).ToLowerInvariant()
    $component = @{ module = 'fixture:library:1'; artifact = 'library.jar'; sha256 = ('0' * 64) }
    $review = @{ uniqueArtifacts = 1; components = @($component); assetFiles = @{ 'notice.txt' = $noticeHash } }
    $reviewBytes = [Text.Encoding]::UTF8.GetBytes(($review | ConvertTo-Json -Depth 5))
    $graph = @{ artifacts = @($component); reviewManifestSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($reviewBytes)).ToLowerInvariant() }
    try {
        foreach ($case in @('valid', 'changed-notice', 'missing-notice', 'changed-graph')) {
            $stream = [IO.File]::Create($archivePath)
            $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
            try {
                $entry = $archive.CreateEntry('assets/licenses/runtime-review.json').Open()
                try { $entry.Write($reviewBytes, 0, $reviewBytes.Length) } finally { $entry.Dispose() }
                if ($case -ne 'missing-notice') {
                    $entry = $archive.CreateEntry('assets/licenses/notice.txt').Open()
                    try {
                        $caseBytes = if ($case -eq 'changed-notice') { [Text.Encoding]::UTF8.GetBytes('tampered') } else { $noticeBytes }
                        $entry.Write($caseBytes, 0, $caseBytes.Length)
                    }
                    finally { $entry.Dispose() }
                }
            } finally { $archive.Dispose(); $stream.Dispose() }
            $caseGraph = if ($case -eq 'changed-graph') {
                @{ artifacts = @(@{ module = 'fixture:different:1'; artifact = 'library.jar'; sha256 = ('0' * 64) }); reviewManifestSha256 = $graph.reviewManifestSha256 }
            } else { $graph }
            $rejected = $false
            try { $count = Assert-PackagedNotices $archivePath 'assets/licenses/' $caseGraph }
            catch { $rejected = $true }
            if (($case -eq 'valid' -and ($rejected -or $count -ne 1)) -or ($case -ne 'valid' -and -not $rejected)) {
                throw "Packaged-notice validator fixture failed: $case"
            }
        }
    } finally {
        # Only the exact fixture file and its empty, uniquely created directory are removed.
        [IO.File]::Delete($archivePath)
        [IO.Directory]::Delete($testDirectory)
    }
    $identityXml = '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.okuflow"><application><activity android:name="com.okuflow.camera.MainActivity" android:exported="true" android:launchMode="singleTask"><intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter></activity></application></manifest>'
    Assert-ApplicationIdentity ([xml]$identityXml)
    foreach ($brokenXml in @($identityXml.Replace('package="com.okuflow"', 'package="com.okuflow.camera"'),
        $identityXml.Replace('com.okuflow.camera.MainActivity', 'com.okuflow.MainActivity'),
        $identityXml.Replace('com.okuflow.camera.MainActivity', '.MainActivity'))) {
        $rejected = $false
        try { Assert-ApplicationIdentity ([xml]$brokenXml) } catch { $rejected = $true }
        if (-not $rejected) { throw 'Identity validator accepted a legacy package or incorrect/relative activity.' }
    }
    $linkXml = $identityXml.Replace('</activity>', '<intent-filter android:autoVerify="true"><action android:name="android.intent.action.VIEW"/><category android:name="android.intent.category.DEFAULT"/><category android:name="android.intent.category.BROWSABLE"/><data android:scheme="https" android:host="okuflow.com" android:path="/camera/open"/></intent-filter></activity>')
    Assert-CameraAppLink ([xml]$linkXml)
    foreach ($brokenXml in @($linkXml.Replace('autoVerify="true"', 'autoVerify="false"'),
        $linkXml.Replace('host="okuflow.com"', 'host="untrusted.example"'), $linkXml.Replace('path="/camera/open"', 'pathPrefix="/"'))) {
        $rejected = $false
        try { Assert-CameraAppLink ([xml]$brokenXml) } catch { $rejected = $true }
        if (-not $rejected) { throw 'App-link scope rejection fixture failed.' }
    }
    $digest = '69' * 32
    foreach ($text in @("Signer #1 certificate SHA-256 digest: $digest", "V2 Signer: certificate SHA-256 digest: $digest",
        "V2 Signer: certificate SHA-256 digest: $digest`nV3 Signer: certificate SHA-256 digest: $digest")) {
        if ((Get-ApkUploadCertificateDigest $text) -ne $digest) { throw 'APK signer format fixture failed.' }
    }
    foreach ($text in @("Signer #1 certificate SHA-256 digest: $digest`nSigner #2 certificate SHA-256 digest: $('ab' * 32)",
        "V2 Signer: certificate SHA-256 digest: ${digest}0", "unknown signer certificate SHA-256 digest: $digest")) {
        $rejected = $false
        try { Get-ApkUploadCertificateDigest $text | Out-Null } catch { $rejected = $true }
        if (-not $rejected) { throw 'APK certificate identity rejection fixture failed.' }
    }
    # Synthetic inputs test rejection only; release mappings are always generated by R8.
    $mappingFixture = @('# compiler: R8', '# compiler_version: 9.4.24',
        '# {"id":"com.android.tools.r8.mapping","version":"2.2"}', '# pg_map_id: abcd') -join "`n"
    foreach ($component in @(@('com.okuflow.camera.MainActivity', 'onCreate'),
        @('com.okuflow.camera.camera.CameraStreamService', 'onDestroy'),
        @('com.okuflow.camera.camera.StreamingPauseReceiver', 'onReceive'),
        @('com.okuflow.camera.camera.CameraEngine', 'stop'),
        @('com.okuflow.camera.link.UsbSocketServer', 'disconnectHost'),
        @('com.okuflow.camera.setup.SetupViewModel', 'onResumed'))) {
        $mappingFixture += "`n$($component[0]) -> $($component[0]):`n# {`"id`":`"sourceFile`",`"fileName`":`"Fixture.kt`"}`n    1:1:void $($component[1])() -> $($component[1])`n"
    }
    Assert-R8DiagnosticMapping $mappingFixture | Out-Null
    Assert-R8DiagnosticMapping ($mappingFixture + "`n" + 'ComposeStackTrace -> $$compose:' + "`n") | Out-Null
    foreach ($brokenMapping in @('', '# compiler: R8', $mappingFixture.Replace('# compiler: R8', '# compiler: D8'),
        $mappingFixture.Replace('MainActivity -> com.okuflow.camera.MainActivity:', 'MainActivity -> com.okuflow.camera.mainActivity:'),
        $mappingFixture.Replace('com.okuflow.camera.MainActivity', 'com.okuflow.camera.OtherActivity'),
        $mappingFixture.Replace('-> onDestroy', '-> OnDestroy'),
        $mappingFixture.Replace('"id":"sourceFile"', '"id":"missingSource"'))) {
        $rejected = $false
        try { Assert-R8DiagnosticMapping $brokenMapping | Out-Null } catch { $rejected = $true }
        if (-not $rejected) { throw 'Diagnostic mapping rejection fixture failed.' }
    }
    $mappingDirectory = Join-Path ([IO.Path]::GetTempPath()) ('OkuFlow-mapping-test-' + [Guid]::NewGuid().ToString('N'))
    $mappingDirectory = Assert-LocalOutput $mappingDirectory
    [IO.Directory]::CreateDirectory($mappingDirectory) | Out-Null
    $mappingPath = Join-Path $mappingDirectory 'mapping.txt'
    $mappingArchive = Join-Path $mappingDirectory 'fixture.aab'
    $mappingBytes = [Text.Encoding]::UTF8.GetBytes($mappingFixture)
    [IO.File]::WriteAllBytes($mappingPath, $mappingBytes)
    try {
        foreach ($case in @('valid', 'missing', 'changed')) {
            $stream = [IO.File]::Create($mappingArchive)
            $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
            try {
                if ($case -ne 'missing') {
                    $entry = $archive.CreateEntry('BUNDLE-METADATA/com.android.tools.build.obfuscation/proguard.map').Open()
                    try {
                        $caseBytes = if ($case -eq 'changed') { [Text.Encoding]::UTF8.GetBytes($mappingFixture.Replace('Fixture.kt', 'Changed.kt')) } else { $mappingBytes }
                        $entry.Write($caseBytes, 0, $caseBytes.Length)
                    } finally { $entry.Dispose() }
                }
            } finally { $archive.Dispose(); $stream.Dispose() }
            $rejected = $false
            try { Assert-BundleDiagnosticMapping $mappingArchive $mappingPath | Out-Null } catch { $rejected = $true }
            if (($case -eq 'valid' -and $rejected) -or ($case -ne 'valid' -and -not $rejected)) {
                throw "AAB mapping binding fixture failed: $case"
            }
        }
    } finally {
        [IO.File]::Delete($mappingArchive)
        [IO.File]::Delete($mappingPath)
        [IO.Directory]::Delete($mappingDirectory)
    }
    Write-Host 'PASS: ELF alignment (4), packaged notices (4), package/activity identity (4), app links (4), signer formats/identity (6), R8 mapping policy (9), AAB mapping binding (3) fixtures.'
    exit 0
}

$originalJava = $env:JAVA_HOME
$validationExit = 0
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (-not $BuildRoot -or -not $Sdk -or -not $Jdk) { throw 'BuildRoot, Sdk and Jdk are required.' }
    $BuildRoot = Assert-LocalOutput $BuildRoot
    $Apk = Assert-LocalOutput $Apk $BuildRoot
    $Bundle = Assert-LocalOutput $Bundle $BuildRoot
    $reportDirectory = Join-Path $BuildRoot 'app\reports\release'
    $reportPath = Join-Path $reportDirectory 'artifact-validation.json'
    # A failed validation must not leave a previous success receipt attached
    # to newly rebuilt artifacts. This is an owned generated file only.
    if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
    foreach ($path in @($Apk, $Bundle)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Release APK/AAB is missing.' } }
    $env:JAVA_HOME = $Jdk
    $buildTools = Join-Path $Sdk 'build-tools\37.0.0'
    $apkanalyzer = Join-Path $Sdk 'cmdline-tools\latest\bin\apkanalyzer.bat'
    $apksigner = Join-Path $buildTools 'apksigner.bat'
    $zipalign = Join-Path $buildTools 'zipalign.exe'
    $jarsigner = Join-Path $Jdk 'bin\jarsigner.exe'
    $keytool = Join-Path $Jdk 'bin\keytool.exe'
    foreach ($tool in @($apkanalyzer, $apksigner, $zipalign, $jarsigner, $keytool)) {
        if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Release validation tool missing: $tool" }
    }
    $manifestOutput = & $apkanalyzer manifest print $Apk 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'Unable to inspect the release APK merged manifest.' }
    [xml]$manifest = $manifestOutput -join "`n"
    $ns = 'http://schemas.android.com/apk/res/android'
    Assert-ApplicationIdentity $manifest
    Assert-CameraAppLink $manifest
    if ($manifest.manifest.application.GetAttribute('debuggable', $ns) -eq 'true' -or
        $manifest.manifest.application.GetAttribute('testOnly', $ns) -eq 'true') {
        throw 'Release APK identity/debuggable/testOnly contract failed.'
    }
    $sdkNode = $manifest.manifest.'uses-sdk'
    if ($sdkNode.GetAttribute('minSdkVersion', $ns) -ne '30' -or $sdkNode.GetAttribute('targetSdkVersion', $ns) -ne '37') {
        throw 'Release APK min/target SDK differs from the reviewed configuration.'
    }
    # XML's android:name attribute shadows PowerShell's adapted .Name property.
    $permissions = @($manifest.manifest.ChildNodes | Where-Object { $_.LocalName -match '^uses-permission' } |
        ForEach-Object { $_.GetAttribute('name', $ns) })
    $expected = @('android.permission.CAMERA', 'android.permission.FOREGROUND_SERVICE',
        'android.permission.FOREGROUND_SERVICE_CAMERA', 'android.permission.POST_NOTIFICATIONS')
    foreach ($required in $expected) { if ($required -notin $permissions) { throw "Required release permission missing: $required" } }
    foreach ($permission in $permissions) {
        if ($permission -notin $expected -and $permission -ne 'com.okuflow.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION') {
            throw "Unreviewed release permission: $permission"
        }
    }
    foreach ($feature in @('android.hardware.camera.any', 'android.hardware.camera', 'android.hardware.camera.autofocus')) {
        $node = @($manifest.manifest.'uses-feature' | Where-Object { $_.GetAttribute('name', $ns) -eq $feature })
        $required = if ($feature -eq 'android.hardware.camera.any') { 'true' } else { 'false' }
        if ($node.Count -ne 1 -or $node[0].GetAttribute('required', $ns) -ne $required) { throw "Camera feature contract failed: $feature" }
    }
    $service = @($manifest.manifest.application.service | Where-Object { $_.GetAttribute('name', $ns) -match '(^|\.)CameraStreamService$' })
    if ($service.Count -ne 1 -or $service[0].GetAttribute('exported', $ns) -ne 'false' -or
        $service[0].GetAttribute('foregroundServiceType', $ns) -notin @('camera', '0x40', '0x00000040', '64')) {
        throw 'Camera foreground service declaration failed.'
    }

    $pauseReceiver = @($manifest.manifest.application.receiver | Where-Object { $_.GetAttribute('name', $ns) -match '(^|\.)StreamingPauseReceiver$' })
    if ($pauseReceiver.Count -ne 1 -or $pauseReceiver[0].GetAttribute('exported', $ns) -ne 'false') {
        throw 'Notification Pause receiver must be present and non-exported.'
    }
    $signatureOutput = & $apksigner verify --print-certs $Apk 2>&1
    $apkVerified = $LASTEXITCODE -eq 0
    $bundleArchive = [IO.Compression.ZipFile]::OpenRead($Bundle)
    try { $bundleHasSignature = @($bundleArchive.Entries | Where-Object { $_.FullName -match '^META-INF/[^/]+\.(RSA|DSA|EC)$' }).Count -gt 0 }
    finally { $bundleArchive.Dispose() }
    if ($AllowUnsignedRelease) {
        if ($apkVerified -or $bundleHasSignature) { throw 'Unsigned review mode refuses a signed APK or AAB.' }
    } else {
        if (-not $apkVerified -or ($signatureOutput -join "`n") -match 'CN=Android Debug') { throw 'Release APK signature is missing, invalid or uses the debug certificate.' }
        if (-not $bundleHasSignature) { throw 'Release AAB upload signature is missing.' }
        $jarOutput = & $jarsigner '-J-Duser.language=en' -verify -verbose -certs $Bundle 2>&1
        if ($LASTEXITCODE -ne 0 -or ($jarOutput -join "`n") -notmatch 'jar verified\.' -or
            ($jarOutput -join "`n") -match 'jar is unsigned|unsigned entries|CN=Android Debug') {
            throw 'Release AAB upload signature is invalid, incomplete or a debug signature.'
        }
        $certificate = & $keytool '-J-Duser.language=en' -printcert -jarfile $Bundle 2>&1
        if ($LASTEXITCODE -ne 0) { throw 'Unable to inspect the AAB upload certificate.' }
        $apkDigest = Get-ApkUploadCertificateDigest ($signatureOutput -join "`n")
        $bundleDigest = [regex]::Match(($certificate -join "`n"), 'SHA256:\s*([a-fA-F0-9:]+)').Groups[1].Value.Replace(':', '')
        if ($bundleDigest.Length -ne 64 -or $apkDigest -ne $bundleDigest) { throw 'APK and AAB must share one upload certificate.' }
    }
    $alignmentOutput = & $zipalign -c -P 16 -v 4 $Apk 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'Release APK ZIP/native 16 KB alignment check failed.' }
    $apkLibraries = Assert-ArchiveLibraries $Apk
    $bundleLibraries = Assert-ArchiveLibraries $Bundle
    $reportDirectory = Join-Path $BuildRoot 'app\reports\release'
    $inventory = Join-Path $reportDirectory 'runtime-dependencies.json'
    if (-not (Test-Path -LiteralPath $inventory)) { throw 'Resolved release runtime dependency inventory is missing.' }
    $graph = Get-Content -LiteralPath $inventory -Raw | ConvertFrom-Json
    if ($graph.configuration -ne 'releaseRuntimeClasspath' -or -not $graph.artifacts.Count) { throw 'Resolved release inventory is empty or has the wrong configuration.' }
    foreach ($artifact in $graph.artifacts) { if (-not $artifact.module -or $artifact.sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Resolved release inventory contains an incomplete artifact.' } }
    if ($graph.licenseReview -ne 'verified-reviewed-evidence' -or $graph.reviewManifestSha256 -notmatch '^[a-f0-9]{64}$') {
        throw 'Release graph has no matching reviewed license/NOTICE evidence.'
    }
    $apkNoticeCount = Assert-PackagedNotices $Apk 'assets/licenses/' $graph
    $bundleNoticeCount = Assert-PackagedNotices $Bundle 'base/assets/licenses/' $graph
    $mapping = Assert-BundleDiagnosticMapping $Bundle (Join-Path $BuildRoot 'app\outputs\mapping\release\mapping.txt')
    $configurationPath = Join-Path $BuildRoot 'app\outputs\mapping\release\configuration.txt'
    if (-not (Test-Path -LiteralPath $configurationPath -PathType Leaf)) { throw 'Build-generated R8 configuration is missing.' }
    $configuration = Get-Content -LiteralPath $configurationPath -Raw
    foreach ($option in @('-dontobfuscate', '-dontshrink', '-dontoptimize')) {
        if ($configuration -notmatch ('(?m)^' + [regex]::Escape($option) + '\s*$')) {
            throw 'R8 diagnostic release must disable name obfuscation, shrinking and optimization.'
        }
    }
    $mapping.configurationSha256 = (Get-FileHash -LiteralPath $configurationPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $diagnosticArchive = [IO.Compression.ZipFile]::OpenRead($Bundle)
    try {
        $nativeDebugEntries = @($diagnosticArchive.Entries | Where-Object { $_.FullName -like 'BUNDLE-METADATA/com.android.tools.build.debugsymbols/*' } | ForEach-Object FullName)
    } finally { $diagnosticArchive.Dispose() }
    [IO.Directory]::CreateDirectory($reportDirectory) | Out-Null
    $manifestOutput -join "`n" | Set-Content -LiteralPath (Join-Path $reportDirectory 'merged-manifest.xml') -Encoding utf8
    $report = [ordered]@{
        reviewOnly = [bool]$AllowUnsignedRelease
        package = 'com.okuflow'
        launchComponent = 'com.okuflow/com.okuflow.camera.MainActivity'
        signed = $apkVerified
        uploadCertificateSha256 = if ($AllowUnsignedRelease) { $null } else { $apkDigest }
        apkSha256 = (Get-FileHash -LiteralPath $Apk -Algorithm SHA256).Hash.ToLowerInvariant()
        bundleSha256 = (Get-FileHash -LiteralPath $Bundle -Algorithm SHA256).Hash.ToLowerInvariant()
        inventorySha256 = (Get-FileHash -LiteralPath $inventory -Algorithm SHA256).Hash.ToLowerInvariant()
        native64BitEntries = @{ apk = $apkLibraries; bundle = $bundleLibraries }
        reviewedRuntimeArtifacts = @{ apk = $apkNoticeCount; bundle = $bundleNoticeCount }
        diagnosticMapping = $mapping
        nativeDebugMetadata = @{ entries = $nativeDebugEntries; present = [bool]$nativeDebugEntries.Count }
        checks = @('releaseManifest', 'cameraAppLink', 'permissions', 'cameraFeatures', 'pauseReceiver', 'signatureMode', 'apkZip16K', 'elf16K', 'resolvedInventory', 'packagedNotices', 'embeddedR8Mapping')
        remainingGates = @($(if ($AllowUnsignedRelease) { 'Signed Google Play closed-test artifact and owner upload key' }
            else { 'Play acceptance and upload-certificate registration' }),
            'Play encrypted-transmission policy applicability unresolved; unchanged USB is not approval',
            'Play-generated APK and 16 KB runtime acceptance', 'Published policy URL', 'Play Data Safety/FGS/reviewer declarations',
            'Developer account/test-track eligibility; production eligibility separate', 'Real tester-device pause/notification/accessibility acceptance')
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $reportDirectory 'artifact-validation.json') -Encoding utf8
    if ($AllowUnsignedRelease) { Write-Host 'PASS: UNSIGNED REVIEW ARTIFACT ONLY; NOT UPLOADABLE.' -ForegroundColor Yellow }
    else { Write-Host 'PASS: signed release technical artifact checks; Play publication gates remain.' }
} catch {
    Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red
    $validationExit = 1
} finally { $env:JAVA_HOME = $originalJava }
exit $validationExit
