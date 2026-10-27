param(
    [Parameter(Mandatory = $true)]
    [string]$BundlePath,

    [Parameter(Mandatory = $true)]
    [string]$RepositoryPath,

    # Actual Qt runtime version staged into the bundle (qmake -query
    # QT_VERSION). Never hardcode: QT_PREFIX/Qt6_DIR overrides change it.
    [string]$QtVersion = '',

    # Optional compatibility assertion only. Configured target facts are the
    # authority, including when a shell or later CMake argument disagrees.
    [ValidateSet('', 'ON', 'OFF', '1', '0', 'TRUE', 'FALSE', 'YES', 'NO')]
    [string]$CudaEnabled = '',
    [Parameter(Mandatory)][string]$BuildDirectory,
    [string]$Configuration = 'Release',
    # New tracked shipping bundles always stage OCR. Standalone legacy and
    # synthetic non-OCR inventories retain their original metadata contract.
    [switch]$RequireUctPayload
)

$qtRuntimeVersion =
    if ([string]::IsNullOrWhiteSpace($QtVersion)) { 'unknown' }
    else { $QtVersion.Trim() }
$ErrorActionPreference = 'Stop'
$bundle = (Resolve-Path -LiteralPath $BundlePath).Path
. (Join-Path $PSScriptRoot 'windows-installer/bundle_layout.ps1')
. (Join-Path $PSScriptRoot 'windows-installer/mcp_bundle.ps1')
. (Join-Path $PSScriptRoot 'windows-installer/release_build_facts.ps1')
. (Join-Path $PSScriptRoot 'windows-installer/uct_bundle.ps1')
. (Join-Path $PSScriptRoot 'windows-installer/pdf_bundle.ps1')
$layout = Get-OkuFlowBundleLayout $bundle
$payload = $layout.PayloadRoot
$buildFacts = Get-OkuFlowReleaseBuildFacts $BuildDirectory $Configuration (Join-Path $bundle $layout.ApplicationRelativePath)
$cudaIsEnabled = $buildFacts.cuda.enabled
$cudaToolkitVersion = $buildFacts.cuda.toolkitVersion
if ($CudaEnabled -and (($CudaEnabled.ToUpperInvariant() -in @('1','ON','TRUE','YES')) -ne $cudaIsEnabled)) {
    throw 'Requested CUDA metadata assertion disagrees with the effective configured build.'
}
if ($cudaIsEnabled -and (Get-FileHash -LiteralPath (Join-Path $payload 'licenses/NVIDIA_CUDA_EULA.txt')).Hash.ToLowerInvariant() -cne $buildFacts.cuda.licenseSha256) {
    throw 'Staged CUDA license differs from the effective build toolkit.'
}
$mcpExecutable = Join-Path $payload 'okuflow_mcp.exe'
$mcpInfo = $null
if (Test-Path -LiteralPath $mcpExecutable -PathType Leaf) {
    if (-not $BuildDirectory) { throw 'MCP metadata requires the matching validated build directory.' }
    $mcpInfo = Get-McpReleaseBuild $BuildDirectory $mcpExecutable
}
$repository = (Resolve-Path -LiteralPath $RepositoryPath).Path
if ([IO.Path]::GetFullPath($buildFacts.sourceDirectory) -ne $repository) { throw 'Build facts belong to a different source checkout.' }
$uctInfo = Get-OkuFlowUctBundleMetadata -BundleRoot $bundle -RepositoryRoot $repository -Required:$RequireUctPayload
$pdfInfo = Get-OkuFlowPdfBundleMetadata -PayloadRoot $payload -RepositoryRoot $repository -Prefix $layout.Prefix
$mathNotices = [ordered]@{
    'MICROTEX_MIT.txt' = 'third_party/microtex/LICENSE'
    'TINYXML2_ZLIB.txt' = 'third_party/tinyxml2/LICENSE.txt'
    'OKUFLOW_MODIFICATIONS.txt' = 'third_party/microtex/OKUFLOW_MODIFICATIONS.txt'
    'OFL.txt' = 'third_party/microtex/res/fonts/licences/OFL.txt'
    'Knuth_License.txt' = 'third_party/microtex/res/fonts/licences/Knuth_License.txt'
    'License_for_dsrom.txt' = 'third_party/microtex/res/fonts/licences/License_for_dsrom.txt'
}
$mathIncluded = $RequireUctPayload -or (Test-Path -LiteralPath (Join-Path $payload 'licenses/math'))
if ($mathIncluded) {
    foreach ($name in $mathNotices.Keys) {
        $staged = Get-Item -LiteralPath (Join-Path $payload "licenses/math/$name")
        $original = Get-Item -LiteralPath (Join-Path $repository $mathNotices[$name])
        if ($staged.Length -gt 1048576 -or $staged.Length -ne $original.Length -or
            (Get-FileHash -LiteralPath $staged.FullName).Hash -cne (Get-FileHash -LiteralPath $original.FullName).Hash) {
            throw "Math dependency notice differs from reviewed source: $name"
        }
    }
}
if ($uctInfo -and $cudaIsEnabled) {
    $nvrtcLicense = @($uctInfo.Inventory | Where-Object path -CEQ 'app/ocr/tensorrt/cuda/LICENSE')
    if ($nvrtcLicense.Count -ne 1 -or $nvrtcLicense[0].sha256 -cne $buildFacts.cuda.licenseSha256) {
        throw 'UCT NVRTC license differs from the effective build toolkit.'
    }
}
$runtimePin = Get-Content (Join-Path $repository 'scripts/vc-runtime/pins.json') -Raw | ConvertFrom-Json
$runtimeInstaller = Join-Path $payload $runtimePin.installer.name
$runtimeLicense = Join-Path $payload 'licenses/MICROSOFT_VC_RUNTIME_LICENSE.rtf'
if ((Get-FileHash -LiteralPath $runtimeInstaller).Hash.ToLowerInvariant() -ne $runtimePin.installer.sha256 -or
    (Get-FileHash -LiteralPath $runtimeLicense).Hash.ToLowerInvariant() -ne $runtimePin.licenseSha256) {
    throw 'Microsoft runtime installer/license differ from the reviewed pins.'
}
$runtimeSignature = Get-AuthenticodeSignature -LiteralPath $runtimeInstaller
if ($runtimeSignature.Status -ne 'Valid' -or
    $runtimeSignature.SignerCertificate.Subject -notmatch 'CN=Microsoft Corporation(?:,|$)') {
    throw 'Microsoft runtime installer signature could not be verified.'
}
$d3dCompilerPath = Join-Path $payload 'D3Dcompiler_47.dll'
$d3dCompilerVersion =
    if (Test-Path -LiteralPath $d3dCompilerPath -PathType Leaf) {
        $version = (Get-Item -LiteralPath $d3dCompilerPath).VersionInfo.FileVersion
        if ([string]::IsNullOrWhiteSpace($version)) { 'unknown' } else { $version }
    } else {
        $null
    }
# stage_platform_tools.ps1 pins and verifies these files; the SBOM records
# what was actually staged rather than repeating the pin.
$platformToolsPath = Join-Path $payload 'platform-tools'
foreach ($name in @('adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt', 'source.properties', 'adb-build.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $platformToolsPath $name) -PathType Leaf)) {
        throw "Required Android platform-tools file is missing: platform-tools/$name"
    }
}
$platformToolsRevisionLine = Select-String -LiteralPath (Join-Path $platformToolsPath 'source.properties') `
    -Pattern '^Pkg\.Revision=(.+)$' | Select-Object -First 1
if ($null -eq $platformToolsRevisionLine) {
    throw 'platform-tools/source.properties does not declare Pkg.Revision.'
}
$platformToolsRevision = $platformToolsRevisionLine.Matches[0].Groups[1].Value.Trim()
$adbBuild = Get-Content (Join-Path $platformToolsPath 'adb-build.json') -Raw | ConvertFrom-Json
if (-not $adbBuild.sourceBuilt -or $adbBuild.protocol -ne 41) {
    throw 'adb must be built from the reviewed source recipe.'
}
foreach ($record in $adbBuild.files) {
    if ($record.path.Contains('..') -or $record.path.Contains(':') -or
        (Get-FileHash -LiteralPath (Join-Path $payload $record.path)).Hash.ToLowerInvariant() -ne $record.sha256) {
        throw "adb runtime/source verification failed: $($record.path)"
    }
}
function Get-PlatformToolsFileVersion([string]$Name) {
    $version = (Get-Item -LiteralPath (Join-Path $platformToolsPath $Name)).VersionInfo.FileVersion
    if ([string]::IsNullOrWhiteSpace($version)) { 'unknown' } else { $version.Trim() }
}
$metadataNames = @(
    'SHA256SUMS.txt',
    'release-manifest.json',
    'SBOM.spdx.json'
)

$files = Get-ChildItem -LiteralPath $bundle -File -Recurse |
    Where-Object { $metadataNames -notcontains $_.Name } |
    Sort-Object { $_.FullName.Substring($bundle.Length).Replace('\', '/') }

$fileRecords = foreach ($file in $files) {
    $relative = $file.FullName.Substring($bundle.Length).TrimStart('\').Replace('\', '/')
    [ordered]@{
        path = $relative
        bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
if ($uctInfo) {
    foreach ($ocrRecord in $uctInfo.Inventory) {
        $actual = @($fileRecords | Where-Object path -CEQ $ocrRecord.path)
        if ($actual.Count -ne 1 -or $actual[0].bytes -ne $ocrRecord.bytes -or $actual[0].sha256 -cne $ocrRecord.sha256) {
            throw "UCT payload changed during release inventory generation: $($ocrRecord.path)"
        }
    }
}
if ($pdfInfo) {
    foreach ($record in $pdfInfo.Files) {
        $actual = @($fileRecords | Where-Object path -CEQ $record.fileName.Substring(2))
        if ($actual.Count -ne 1 -or $actual[0].sha256 -cne $record.checksums[0].checksumValue) {
            throw 'PDF export runtime changed during release inventory generation.'
        }
    }
}

$checksumLines = $fileRecords | ForEach-Object {
    '{0} *{1}' -f $_.sha256, $_.path
}
[IO.File]::WriteAllLines(
    (Join-Path $bundle 'SHA256SUMS.txt'),
    $checksumLines,
    [Text.UTF8Encoding]::new($false))

$commit = (& git -C $repository rev-parse HEAD 2>$null)
if ($LASTEXITCODE -ne 0) {
    $commit = 'unknown'
}
$statusLines = @(& git -C $repository status --porcelain --untracked-files=normal 2>$null)
$dirty = $LASTEXITCODE -ne 0 -or $statusLines.Count -gt 0

$executable = Join-Path $bundle 'oku_flow.exe'
$signature = Get-AuthenticodeSignature -LiteralPath $executable
$ownedExecutables = @('oku_flow.exe', $layout.ApplicationRelativePath)
if ($mcpInfo) { $ownedExecutables += $layout.Prefix + 'okuflow_mcp.exe' }
$executableSignatures = foreach ($name in $ownedExecutables) {
    $path = Join-Path $bundle $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required release executable missing: $name" }
    $signed = Get-AuthenticodeSignature -LiteralPath $path
    [ordered]@{
        path = $name
        status = [string]$signed.Status
        signer = if ($signed.SignerCertificate) { $signed.SignerCertificate.Subject } else { $null }
        thumbprint = if ($signed.SignerCertificate) { $signed.SignerCertificate.Thumbprint } else { $null }
    }
}
$manifest = [ordered]@{
    schemaVersion = 2
    product = 'OkuFlow'
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    sourceCommit = [string]$commit
    sourceTreeDirty = $dirty
    authenticode = [ordered]@{
        status = [string]$signature.Status
        signer = if ($signature.SignerCertificate) {
            $signature.SignerCertificate.Subject
        } else {
            $null
        }
        thumbprint = if ($signature.SignerCertificate) {
            $signature.SignerCertificate.Thumbprint
        } else {
            $null
        }
    }
    executables = @($executableSignatures)
    payloadDirectory = if ($layout.Prefix) { 'app' } else { '.' }
    buildFacts = $buildFacts
    windowsRuntime = [ordered]@{
        version = $runtimePin.version
        installation = 'system-wide, with user/admin approval only when required'
        installer = $layout.Prefix + $runtimePin.installer.name
        installerSha256 = $runtimePin.installer.sha256
        installerAuthenticode = [string]$runtimeSignature.Status
        license = $layout.Prefix + 'licenses/MICROSOFT_VC_RUNTIME_LICENSE.rtf'
    }
    files = @($fileRecords)
}
if ($mcpInfo) {
    $manifest.agentControl = [ordered]@{path=($layout.Prefix + 'okuflow_mcp.exe');
        sha256=(Get-FileHash -LiteralPath $mcpExecutable).Hash.ToLowerInvariant();buildInfo=$mcpInfo}
}
if ($uctInfo) { $manifest.ocr = $uctInfo.Manifest }
$manifest | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $bundle 'release-manifest.json') -Encoding utf8NoBOM

$namespaceId = if ($commit -ne 'unknown') {
    ([string]$commit).Trim()
} else {
    [Guid]::NewGuid().ToString('N')
}
$packages = @(
    [ordered]@{
        SPDXID = 'SPDXRef-Package-Microsoft-VC-Runtime'
        name = 'Microsoft Visual C++ x64 runtime redistributable'
        versionInfo = $runtimePin.version
        downloadLocation = $runtimePin.vendorPage
        filesAnalyzed = $false
        licenseConcluded = 'LicenseRef-Microsoft-VC-Runtime'
        licenseDeclared = 'LicenseRef-Microsoft-VC-Runtime'
        copyrightText = 'Copyright Microsoft Corporation'
        comment = 'Unmodified signed installer and exact license shipped; /MT native launcher uses the static CRT, while OkuFlowApp.exe uses the centrally installed runtime. No app-local runtime DLLs are copied.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-OkuFlow'
        name = 'OkuFlow'
        versionInfo = $namespaceId
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        licenseConcluded = 'GPL-3.0-only OR LicenseRef-OkuFlow-Commercial'
        licenseDeclared = 'GPL-3.0-only OR LicenseRef-OkuFlow-Commercial'
        copyrightText = 'NOASSERTION'
        comment = 'OkuFlow is also available under a separate commercial license.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-Qt'
        name = 'Qt 6 runtime'
        versionInfo = $qtRuntimeVersion
        downloadLocation = 'https://www.qt.io/'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'NOASSERTION'
        copyrightText = 'Copyright The Qt Company Ltd. and other contributors'
        comment = 'Dynamically linked Qt runtime deployment. The applicable Qt license text, FFmpeg LGPL text, and exact Qt module SPDX documents are staged under licenses/.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-Lucide'
        name = 'Lucide icon set'
        versionInfo = 'lucide-static 1.25.0'
        downloadLocation = 'https://lucide.dev/'
        filesAnalyzed = $false
        licenseConcluded = 'ISC AND MIT'
        licenseDeclared = 'ISC AND MIT'
        copyrightText = 'Copyright Lucide Contributors and Feather Icons contributors'
        comment = 'Embedded application icons; complete notices are staged as licenses/LUCIDE_LICENSE.txt.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-AMD-FSR1'
        name = 'AMD FidelityFX Super Resolution 1.0'
        versionInfo = '1.0.2 (a21ffb8f6c13233ba336352bdff293894c706575)'
        downloadLocation = 'https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/a21ffb8f6c13233ba336352bdff293894c706575'
        filesAnalyzed = $false
        licenseConcluded = 'MIT'
        licenseDeclared = 'MIT'
        copyrightText = 'Copyright (c) 2021 Advanced Micro Devices, Inc.'
        comment = 'EASU and RCAS reference math adapted to CUDA; complete notice is staged as licenses/AMD_FSR1_LICENSE.txt.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-NVIDIA-NIS'
        name = 'NVIDIA Image Scaling SDK'
        versionInfo = '1.0.3 (35e13ba316c98eeecf16f37eae70ce88019911f6)'
        downloadLocation = 'https://github.com/NVIDIAGameWorks/NVIDIAImageScaling/tree/35e13ba316c98eeecf16f37eae70ce88019911f6'
        filesAnalyzed = $false
        licenseConcluded = 'MIT'
        licenseDeclared = 'MIT'
        copyrightText = 'Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES'
        comment = 'NVScaler reference coefficients and filter equations adapted to CUDA; complete notice is staged as licenses/NVIDIA_NIS_LICENSE.txt.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-CUDA'
        name = 'NVIDIA CUDA Toolkit static runtime'
        versionInfo = $cudaToolkitVersion
        downloadLocation = 'https://developer.nvidia.com/cuda-downloads'
        filesAnalyzed = $false
        licenseConcluded = 'LicenseRef-NVIDIA-CUDA-EULA'
        licenseDeclared = 'LicenseRef-NVIDIA-CUDA-EULA'
        copyrightText = 'Copyright NVIDIA Corporation'
        comment = "cudart_static is linked into OkuFlowApp.exe. Effective compiler $($buildFacts.cuda.compilerVersion); target architectures $($buildFacts.cuda.architectures -join ','). The separately installed NVIDIA display driver is not bundled. The exact build Toolkit EULA is staged as licenses/NVIDIA_CUDA_EULA.txt."
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-Maxine-Headers'
        name = 'NVIDIA Maxine Video Effects SDK headers'
        versionInfo = '0.7.6'
        downloadLocation = 'https://github.com/NVIDIA/MAXINE-VFX-SDK'
        filesAnalyzed = $false
        licenseConcluded = 'MIT'
        licenseDeclared = 'MIT'
        copyrightText = 'Copyright (c) 2021 NVIDIA Corporation'
        comment = 'Runtime-loading adapter compiled from the vendored SDK header snapshot; the license and integration notice are staged under licenses/.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-Maxine-Runtime'
        name = 'NVIDIA Video Effects runtime'
        versionInfo = 'runtime-provided'
        downloadLocation = 'https://www.nvidia.com/en-us/geforce/broadcasting/broadcast-sdk/resources/'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'NOASSERTION'
        copyrightText = 'Copyright NVIDIA Corporation'
        comment = 'Optional runtime installed separately by the user.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-CodexCLI'
        name = 'OpenAI Codex CLI'
        versionInfo = 'user-installed'
        downloadLocation = 'https://github.com/openai/codex'
        filesAnalyzed = $false
        licenseConcluded = 'Apache-2.0'
        licenseDeclared = 'Apache-2.0'
        copyrightText = 'Copyright OpenAI'
        comment = 'Optional local app-server executable installed separately by the user.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-D3DCompiler47'
        name = 'Microsoft Direct3D Shader Compiler 47 redistributable'
        versionInfo = if ($null -eq $d3dCompilerVersion) { 'not-bundled' } else { $d3dCompilerVersion }
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'NOASSERTION'
        copyrightText = 'Copyright Microsoft Corporation'
        comment = 'D3Dcompiler_47.dll is deployed by windeployqt when required and is governed by the applicable Microsoft redistribution terms.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-Android-adb'
        name = 'Android Debug Bridge (OkuFlow source build)'
        versionInfo = $platformToolsRevision
        downloadLocation = 'https://github.com/nmeum/android-tools/releases/tag/37.0.0'
        filesAnalyzed = $false
        licenseConcluded = 'Apache-2.0'
        licenseDeclared = 'Apache-2.0'
        copyrightText = 'Copyright The Android Open Source Project and the copyright holders listed in platform-tools/NOTICE.txt'
        comment = 'Separate source-built executable, not linked into OkuFlow. Apache-2.0 describes the AOSP adb sources; statically linked LGPL libusb and BoringSSL, plus dependency DLLs, have separate packages below. The complete corresponding adb/libusb source, patches, relinking library packages and rebuild instructions are included in sources/adb-37.0.0-okuflow-source.zip. Exact notices are in licenses/adb/. No Google SDK binary or SDK license agreement is redistributed.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-AdbWinApi'
        name = 'AdbWinApi.dll'
        versionInfo = (Get-PlatformToolsFileVersion 'AdbWinApi.dll')
        downloadLocation = 'https://android.googlesource.com/platform/development/+/refs/heads/main/host/windows/usb/api'
        filesAnalyzed = $false
        licenseConcluded = 'Apache-2.0'
        licenseDeclared = 'Apache-2.0'
        copyrightText = 'Copyright (C) 2006 The Android Open Source Project'
        comment = 'Built locally from the pinned AOSP Windows USB API source archive with MSVC 2022. Corresponding source and the build recipe are included in the adb source ZIP.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-AdbWinUsbApi'
        name = 'AdbWinUsbApi.dll'
        versionInfo = (Get-PlatformToolsFileVersion 'AdbWinUsbApi.dll')
        downloadLocation = 'https://android.googlesource.com/platform/development/+/refs/heads/main/host/windows/usb/winusb'
        filesAnalyzed = $false
        licenseConcluded = 'Apache-2.0'
        licenseDeclared = 'Apache-2.0'
        copyrightText = 'Copyright (C) 2006-2015 The Android Open Source Project'
        comment = 'Built locally from the pinned AOSP WinUSB source archive with MSVC 2022. Loaded by AdbWinApi.dll; source and rebuild instructions are included.'
    }
)

foreach ($component in $adbBuild.components) {
    $packages += [ordered]@{
        SPDXID = 'SPDXRef-Package-adb-' + ($component.name -replace '[^A-Za-z0-9.-]', '-')
        name = $component.name
        versionInfo = $component.version
        downloadLocation = if ($component.source -match '^https://') { $component.source } else { 'NOASSERTION' }
        filesAnalyzed = $false
        licenseConcluded = $component.license
        licenseDeclared = $component.license
        copyrightText = 'See the full upstream notices in ' + $component.notice
        comment = "adb dependency ($($component.linkage)). Exact notices: $($component.notice). Source or relinking package: $($component.source). Build input hashes: platform-tools/adb-build.json."
    }
}

if (-not $cudaIsEnabled) {
    $packages = @(
        $packages | Where-Object {
            $_.SPDXID -notin @(
                'SPDXRef-Package-CUDA',
                'SPDXRef-Package-AMD-FSR1',
                'SPDXRef-Package-NVIDIA-NIS',
                'SPDXRef-Package-Maxine-Headers',
                'SPDXRef-Package-Maxine-Runtime')
        })
}

if ($null -eq $d3dCompilerVersion) {
    $packages = @(
        $packages | Where-Object {
            $_.SPDXID -ne 'SPDXRef-Package-D3DCompiler47'
        })
}

if ($mcpInfo) {
    $packages += [ordered]@{
        SPDXID = 'SPDXRef-Package-OkuFlow-MCP'
        name = 'OkuFlow first-party MCP bridge'
        versionInfo = $namespaceId
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        licenseConcluded = 'GPL-3.0-only OR LicenseRef-OkuFlow-Commercial'
        licenseDeclared = 'GPL-3.0-only OR LicenseRef-OkuFlow-Commercial'
        copyrightText = 'NOASSERTION'
        checksums = @([ordered]@{algorithm='SHA256';checksumValue=$manifest.agentControl.sha256})
        comment = 'Qt-free static-CRT first-party executable; no MCP SDK runtime is bundled. Exact path and V1/deployed-policy facts are in release-manifest.json.'
    }
}
if ($uctInfo) { $packages += @($uctInfo.Packages) }
if ($pdfInfo) { $packages += @($pdfInfo.Packages) }
if ($mathIncluded) {
    foreach ($dependency in @(
        @{id='MicroTeX';name='MicroTeX native formula layout';version='0e3707f6dafebb121d98b53c64364d16fefe481d';license='MIT';url='https://github.com/NanoMichael/MicroTeX';notice='MICROTEX_MIT.txt; OKUFLOW_MODIFICATIONS.txt'},
        @{id='tinyxml2';name='tinyxml2';version='11.0.0 (9148bdf719e997d1f474be6bcc7943881046dba1)';license='Zlib';url='https://github.com/leethomason/tinyxml2';notice='TINYXML2_ZLIB.txt'},
        @{id='Math-Fonts';name='MicroTeX unmodified base, Latin, Euler and maths fonts';version='0e3707f6dafebb121d98b53c64364d16fefe481d';license='OFL-1.1 AND LicenseRef-MicroTeX-Knuth AND LicenseRef-MicroTeX-dsrom';url='https://github.com/NanoMichael/MicroTeX';notice='OFL.txt; Knuth_License.txt; License_for_dsrom.txt'})) {
        $packages += [ordered]@{
            SPDXID = 'SPDXRef-Package-' + $dependency.id
            name = $dependency.name
            versionInfo = $dependency.version
            downloadLocation = $dependency.url
            filesAnalyzed = $false
            licenseConcluded = $dependency.license
            licenseDeclared = $dependency.license
            copyrightText = 'See complete retained upstream notices.'
            comment = 'Embedded in the native application; retained notices under licenses/math/: ' + $dependency.notice
        }
    }
}
$packageIds = @($packages | ForEach-Object { $_.SPDXID })
$relationships = @()
if ($uctInfo) { $relationships += @($uctInfo.Relationships) }
if ($pdfInfo) { $relationships += @($pdfInfo.Relationships) }
if ($mathIncluded) {
    $relationships += @(
        [ordered]@{spdxElementId='SPDXRef-Package-OkuFlow';relationshipType='STATIC_LINK';relatedSpdxElement='SPDXRef-Package-MicroTeX'},
        [ordered]@{spdxElementId='SPDXRef-Package-MicroTeX';relationshipType='STATIC_LINK';relatedSpdxElement='SPDXRef-Package-tinyxml2'},
        [ordered]@{spdxElementId='SPDXRef-Package-OkuFlow';relationshipType='CONTAINS';relatedSpdxElement='SPDXRef-Package-Math-Fonts'})
}
if ($mcpInfo) {
    $relationships += [ordered]@{spdxElementId='SPDXRef-Package-OkuFlow';
        relationshipType='CONTAINS';relatedSpdxElement='SPDXRef-Package-OkuFlow-MCP'}
}
foreach ($dependency in @(
    'SPDXRef-Package-Microsoft-VC-Runtime',
    'SPDXRef-Package-Qt',
    'SPDXRef-Package-Lucide',
    'SPDXRef-Package-AMD-FSR1',
    'SPDXRef-Package-NVIDIA-NIS',
    'SPDXRef-Package-CUDA',
    'SPDXRef-Package-Maxine-Headers',
    'SPDXRef-Package-D3DCompiler47',
    'SPDXRef-Package-Android-adb')) {
    if ($dependency -notin $packageIds) {
        continue
    }
    $relationships += [ordered]@{
        spdxElementId = 'SPDXRef-Package-OkuFlow'
        relationshipType = 'DEPENDS_ON'
        relatedSpdxElement = $dependency
    }
}

$relationships += [ordered]@{
    spdxElementId = 'SPDXRef-Package-Android-adb'
    relationshipType = 'DEPENDS_ON'
    relatedSpdxElement = 'SPDXRef-Package-AdbWinApi'
}
$relationships += [ordered]@{
    spdxElementId = 'SPDXRef-Package-AdbWinApi'
    relationshipType = 'DEPENDS_ON'
    relatedSpdxElement = 'SPDXRef-Package-AdbWinUsbApi'
}
foreach ($component in $adbBuild.components) {
    $relationships += [ordered]@{
        spdxElementId = 'SPDXRef-Package-Android-adb'
        relationshipType = if ($component.linkage -eq 'static') { 'STATIC_LINK' } elseif ($component.linkage -eq 'dynamic') { 'DYNAMIC_LINK' } else { 'DEPENDS_ON' }
        relatedSpdxElement = 'SPDXRef-Package-adb-' + ($component.name -replace '[^A-Za-z0-9.-]', '-')
    }
}

foreach ($dependency in @(
    'SPDXRef-Package-Maxine-Runtime',
    'SPDXRef-Package-CodexCLI')) {
    if ($dependency -notin $packageIds) {
        continue
    }
    $relationships += [ordered]@{
        spdxElementId = $dependency
        relationshipType = 'OPTIONAL_DEPENDENCY_OF'
        relatedSpdxElement = 'SPDXRef-Package-OkuFlow'
    }
}

$qtExternalDocumentRefs = @()
$qtModules = @(
    'qtbase',
    'qtimageformats',
    'qtmultimedia',
    'qtspeech',
    'qtsvg')
$pdfSbomPath = Join-Path $payload (
    "licenses/qt-sbom/qtpdf-{0}.spdx.json" -f $qtRuntimeVersion)
if ((Test-Path -LiteralPath (Join-Path $payload 'Qt6Pdf.dll')) -or
    (Test-Path -LiteralPath $pdfSbomPath)) {
    $qtModules += 'qtpdf'
}
foreach ($module in $qtModules) {
    $qtSbomPath = Join-Path $payload (
        "licenses/qt-sbom/{0}-{1}.spdx.json" -f $module, $qtRuntimeVersion)
    if (-not (Test-Path -LiteralPath $qtSbomPath -PathType Leaf)) {
        throw "Required Qt module SPDX document is missing: $qtSbomPath"
    }
    $qtSbom = Get-Content -LiteralPath $qtSbomPath -Raw | ConvertFrom-Json
    # Qt 6.12 adds a hash suffix to package IDs. Identify the unique module
    # package by its name, then link its actual SPDXID instead of inventing one.
    $qtRootPackages = @($qtSbom.packages | Where-Object {
        $_.name -eq $module -and
        $_.SPDXID -match "^SPDXRef-Package-$module(?:-[0-9a-f]+)?$"
    })
    if ([string]::IsNullOrWhiteSpace($qtSbom.documentNamespace) -or
        $qtRootPackages.Count -ne 1) {
        throw "Qt module SPDX document is invalid or lacks a unique $module package: $qtSbomPath"
    }
    $qtRootPackage = $qtRootPackages[0].SPDXID
    $externalDocumentId = "DocumentRef-Qt-$module"
    $qtExternalDocumentRefs += [ordered]@{
        externalDocumentId = $externalDocumentId
        spdxDocument = $qtSbom.documentNamespace
        checksum = [ordered]@{
            algorithm = 'SHA1'
            checksumValue = (
                Get-FileHash -LiteralPath $qtSbomPath -Algorithm SHA1
            ).Hash.ToLowerInvariant()
        }
    }
    $relationships += [ordered]@{
        spdxElementId = 'SPDXRef-Package-Qt'
        relationshipType = 'CONTAINS'
        relatedSpdxElement = "${externalDocumentId}:$qtRootPackage"
    }
}

$sbom = [ordered]@{
    spdxVersion = 'SPDX-2.3'
    dataLicense = 'CC0-1.0'
    SPDXID = 'SPDXRef-DOCUMENT'
    name = 'OkuFlow release SBOM'
    documentNamespace = "https://okuflow.com/spdx/$namespaceId"
    documentDescribes = @('SPDXRef-Package-OkuFlow')
    externalDocumentRefs = @($qtExternalDocumentRefs)
    creationInfo = [ordered]@{
        created = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
        creators = @('Tool: OkuFlow generate_release_metadata.ps1')
    }
    hasExtractedLicensingInfos = @(
        [ordered]@{
            licenseId = 'LicenseRef-Microsoft-VC-Runtime'
            extractedText = Get-Content -LiteralPath $runtimeLicense -Raw
            name = 'Microsoft Visual C++ runtime license (exact installer RTF)'
            seeAlsos = @($runtimePin.licensePage)
        },
        [ordered]@{
            licenseId = 'LicenseRef-BoringSSL'
            extractedText = Get-Content -LiteralPath (Join-Path $payload 'licenses/adb/BORINGSSL_LICENSE.txt') -Raw
            name = 'BoringSSL composite upstream license'
        },
        [ordered]@{
            licenseId = 'LicenseRef-OkuFlow-Commercial'
            extractedText =
                'OkuFlow is available under a separate commercial license from its copyright holder.'
            name = 'OkuFlow commercial license'
        }
    ) + @(
        if ($cudaIsEnabled) {
            $cudaEulaPath = Join-Path $payload 'licenses/NVIDIA_CUDA_EULA.txt'
            [ordered]@{
                licenseId = 'LicenseRef-NVIDIA-CUDA-EULA'
                extractedText = Get-Content -LiteralPath $cudaEulaPath -Raw
                name = 'NVIDIA CUDA Toolkit End User License Agreement'
            }
        })
    packages = $packages
    relationships = @($relationships)
}
if ($uctInfo) {
    $sbom.files = @($uctInfo.Files)
    $sbom.hasExtractedLicensingInfos += @($uctInfo.Licenses)
}
if ($pdfInfo) {
    if (!$sbom.Contains('files')) { $sbom.files = @() }
    $sbom.files = @($sbom.files) + @($pdfInfo.Files)
}
if ($mathIncluded) {
    foreach ($license in @(
        @{id='LicenseRef-MicroTeX-Knuth';file='Knuth_License.txt';name='Knuth font permission and rename notice'},
        @{id='LicenseRef-MicroTeX-dsrom';file='License_for_dsrom.txt';name='dsrom font permission and rename notice'})) {
        $sbom.hasExtractedLicensingInfos += [ordered]@{
            licenseId=$license.id;name=$license.name
            extractedText=(Get-Content -LiteralPath (Join-Path $payload ('licenses/math/' + $license.file)) -Raw)
        }
    }
}
$sbom | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $bundle 'SBOM.spdx.json') -Encoding utf8NoBOM
