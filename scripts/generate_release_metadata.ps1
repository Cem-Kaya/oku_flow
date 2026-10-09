param(
    [Parameter(Mandatory = $true)]
    [string]$BundlePath,

    [Parameter(Mandatory = $true)]
    [string]$RepositoryPath,

    # Actual Qt runtime version staged into the bundle (qmake -query
    # QT_VERSION). Never hardcode: QT_PREFIX/Qt6_DIR overrides change it.
    [string]$QtVersion = '',

    # Mirrors OKUFLOW_ENABLE_CUDA from the validated build so the SBOM does
    # not claim a static CUDA runtime in a CPU-only emergency bundle.
    [string]$CudaEnabled = 'ON'
)

$qtRuntimeVersion =
    if ([string]::IsNullOrWhiteSpace($QtVersion)) { 'unknown' }
    else { $QtVersion.Trim() }
$cudaIsEnabled = $CudaEnabled.Trim().ToUpperInvariant() -in @('1', 'ON', 'TRUE', 'YES')
$cudaToolkitVersion =
    if ([string]::IsNullOrWhiteSpace($env:CUDA_PATH)) {
        'unknown'
    } else {
        (Split-Path -Leaf $env:CUDA_PATH).TrimStart('v')
    }
$ErrorActionPreference = 'Stop'
$bundle = (Resolve-Path -LiteralPath $BundlePath).Path
$repository = (Resolve-Path -LiteralPath $RepositoryPath).Path
$d3dCompilerPath = Join-Path $bundle 'D3Dcompiler_47.dll'
$d3dCompilerVersion =
    if (Test-Path -LiteralPath $d3dCompilerPath -PathType Leaf) {
        $version = (Get-Item -LiteralPath $d3dCompilerPath).VersionInfo.FileVersion
        if ([string]::IsNullOrWhiteSpace($version)) { 'unknown' } else { $version }
    } else {
        $null
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
$manifest = [ordered]@{
    schemaVersion = 1
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
    files = @($fileRecords)
}
$manifest | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $bundle 'release-manifest.json') -Encoding utf8NoBOM

$namespaceId = if ($commit -ne 'unknown') {
    ([string]$commit).Trim()
} else {
    [Guid]::NewGuid().ToString('N')
}
$packages = @(
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
        comment = 'cudart_static is linked into oku_flow.exe. The separately installed NVIDIA display driver is not bundled. The Toolkit EULA is staged as licenses/NVIDIA_CUDA_EULA.txt.'
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
        SPDXID = 'SPDXRef-Package-libdatachannel'
        name = 'libdatachannel'
        versionInfo = 'v0.24.5'
        downloadLocation = 'https://github.com/paullouisageneau/libdatachannel/tree/443f6934d9007eb7076ab7825ba330f355fcbead'
        filesAnalyzed = $false
        licenseConcluded = 'MPL-2.0'
        licenseDeclared = 'MPL-2.0'
        copyrightText = 'Copyright Paul-Louis Ageneau and contributors'
        comment = 'Statically linked native WebRTC stack for live transcription (plan 36 Carrier D): PeerConnection, ICE, DTLS-SRTP, SCTP data channel, and RTP packetization.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-libjuice'
        name = 'libjuice'
        versionInfo = 'v1.7.2 (3c40a3545b6b1b62c7adee7f8f2bd58aa290afd6)'
        downloadLocation = 'https://github.com/paullouisageneau/libjuice/tree/3c40a3545b6b1b62c7adee7f8f2bd58aa290afd6'
        filesAnalyzed = $false
        licenseConcluded = 'MPL-2.0'
        licenseDeclared = 'MPL-2.0'
        copyrightText = 'Copyright Paul-Louis Ageneau and contributors'
        comment = 'Pinned libdatachannel submodule providing ICE with host candidates only.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-libsrtp'
        name = 'libSRTP'
        versionInfo = '2.8.0 (24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5)'
        downloadLocation = 'https://github.com/cisco/libsrtp/tree/24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5'
        filesAnalyzed = $false
        licenseConcluded = 'BSD-3-Clause'
        licenseDeclared = 'BSD-3-Clause'
        copyrightText = 'Copyright Cisco Systems, Inc. and contributors'
        comment = 'Pinned libdatachannel submodule providing SRTP protection.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-usrsctp'
        name = 'usrsctp'
        versionInfo = 'fec583d54493f879d2ae44a743423bf8a04371ab'
        downloadLocation = 'https://github.com/sctplab/usrsctp/tree/fec583d54493f879d2ae44a743423bf8a04371ab'
        filesAnalyzed = $false
        licenseConcluded = 'BSD-3-Clause'
        licenseDeclared = 'BSD-3-Clause'
        copyrightText = 'Copyright Randall Stewart, Michael Tuexen, and contributors'
        comment = 'Pinned libdatachannel submodule providing SCTP for the oai-events data channel.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-plog'
        name = 'plog'
        versionInfo = '1.1.10 (94899e0b926ac1b0f4750bfbd495167b4a6ae9ef)'
        downloadLocation = 'https://github.com/SergiusTheBest/plog/tree/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef'
        filesAnalyzed = $false
        licenseConcluded = 'MIT'
        licenseDeclared = 'MIT'
        copyrightText = 'Copyright Sergey Podobry'
        comment = 'Pinned header-only logging dependency used by libdatachannel.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-nlohmann-json'
        name = 'JSON for Modern C++'
        versionInfo = '3.12.0 (55f93686c01528224f448c19128836e7df245f72)'
        downloadLocation = 'https://github.com/nlohmann/json/tree/55f93686c01528224f448c19128836e7df245f72'
        filesAnalyzed = $false
        licenseConcluded = 'MIT'
        licenseDeclared = 'MIT'
        copyrightText = 'Copyright (c) 2013-2025 Niels Lohmann'
        comment = 'Pinned header-only JSON dependency used by libdatachannel; the complete notice is staged as licenses/NLOHMANN_JSON_LICENSE.txt.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-opus'
        name = 'Opus audio codec'
        versionInfo = 'v1.5.2'
        downloadLocation = 'https://github.com/xiph/opus/tree/ddbe48383984d56acd9e1ab6a090c54ca6b735a6'
        filesAnalyzed = $false
        licenseConcluded = 'BSD-3-Clause'
        licenseDeclared = 'BSD-3-Clause'
        copyrightText = 'Copyright Xiph.Org Foundation and contributors'
        comment = 'Statically linked Opus encoder for the live-transcription audio track. No decoder path exists: remote audio is never decoded.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-mbedtls'
        name = 'Mbed TLS'
        versionInfo = '3.6.7'
        downloadLocation = 'https://github.com/Mbed-TLS/mbedtls/tree/068ff080b369adfac81509f9b57b2afabaf82dc5'
        filesAnalyzed = $false
        licenseConcluded = 'Apache-2.0 OR GPL-2.0-or-later'
        licenseDeclared = 'Apache-2.0 OR GPL-2.0-or-later'
        copyrightText = 'Copyright The Mbed TLS Contributors'
        comment = 'Statically linked DTLS/crypto backend (LTS branch) for the native WebRTC stack, built with MBEDTLS_SSL_DTLS_SRTP enabled.'
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
    }
)

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

$packageIds = @($packages | ForEach-Object { $_.SPDXID })
$relationships = @()
foreach ($dependency in @(
    'SPDXRef-Package-Qt',
    'SPDXRef-Package-Lucide',
    'SPDXRef-Package-AMD-FSR1',
    'SPDXRef-Package-NVIDIA-NIS',
    'SPDXRef-Package-CUDA',
    'SPDXRef-Package-Maxine-Headers',
    'SPDXRef-Package-libdatachannel',
    'SPDXRef-Package-opus',
    'SPDXRef-Package-D3DCompiler47')) {
    if ($dependency -notin $packageIds) {
        continue
    }
    $relationships += [ordered]@{
        spdxElementId = 'SPDXRef-Package-OkuFlow'
        relationshipType = 'DEPENDS_ON'
        relatedSpdxElement = $dependency
    }
}

foreach ($dependency in @(
    'SPDXRef-Package-libjuice',
    'SPDXRef-Package-libsrtp',
    'SPDXRef-Package-usrsctp',
    'SPDXRef-Package-plog',
    'SPDXRef-Package-nlohmann-json',
    'SPDXRef-Package-mbedtls')) {
    $relationships += [ordered]@{
        spdxElementId = 'SPDXRef-Package-libdatachannel'
        relationshipType = 'DEPENDS_ON'
        relatedSpdxElement = $dependency
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
$pdfSbomPath = Join-Path $bundle (
    "licenses/qt-sbom/qtpdf-{0}.spdx.json" -f $qtRuntimeVersion)
if ((Test-Path -LiteralPath (Join-Path $bundle 'Qt6Pdf.dll')) -or
    (Test-Path -LiteralPath $pdfSbomPath)) {
    $qtModules += 'qtpdf'
}
foreach ($module in $qtModules) {
    $qtSbomPath = Join-Path $bundle (
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
            licenseId = 'LicenseRef-OkuFlow-Commercial'
            extractedText =
                'OkuFlow is available under a separate commercial license from its copyright holder.'
            name = 'OkuFlow commercial license'
        }
    ) + @(
        if ($cudaIsEnabled) {
            $cudaEulaPath = Join-Path $bundle 'licenses/NVIDIA_CUDA_EULA.txt'
            [ordered]@{
                licenseId = 'LicenseRef-NVIDIA-CUDA-EULA'
                extractedText = Get-Content -LiteralPath $cudaEulaPath -Raw
                name = 'NVIDIA CUDA Toolkit End User License Agreement'
            }
        })
    packages = $packages
    relationships = @($relationships)
}
$sbom | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $bundle 'SBOM.spdx.json') -Encoding utf8NoBOM
