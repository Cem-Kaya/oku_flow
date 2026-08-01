param(
    [Parameter(Mandatory = $true)]
    [string]$BundlePath,

    [Parameter(Mandatory = $true)]
    [string]$RepositoryPath,

    # Actual Qt runtime version staged into the bundle (qmake -query
    # QT_VERSION). Never hardcode: QT_PREFIX/Qt6_DIR overrides change it.
    [string]$QtVersion = ''
)

$qtRuntimeVersion =
    if ([string]::IsNullOrWhiteSpace($QtVersion)) { 'unknown' }
    else { $QtVersion.Trim() }

$ErrorActionPreference = 'Stop'
$bundle = (Resolve-Path -LiteralPath $BundlePath).Path
$repository = (Resolve-Path -LiteralPath $RepositoryPath).Path
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

$executable = Join-Path $bundle 'open_zoom.exe'
$signature = Get-AuthenticodeSignature -LiteralPath $executable
$manifest = [ordered]@{
    schemaVersion = 1
    product = 'OpenZoom'
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
        SPDXID = 'SPDXRef-Package-OpenZoom'
        name = 'OpenZoom'
        versionInfo = $namespaceId
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'GPL-3.0-only OR LicenseRef-OpenZoom-Commercial'
        copyrightText = 'NOASSERTION'
        comment = 'OpenZoom is also available under a separate commercial license.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-Qt'
        name = 'Qt 6 runtime'
        versionInfo = $qtRuntimeVersion
        downloadLocation = 'https://www.qt.io/'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only'
        copyrightText = 'Copyright The Qt Company Ltd. and other contributors'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-CUDA'
        name = 'NVIDIA CUDA runtime and driver'
        versionInfo = 'runtime-provided'
        downloadLocation = 'https://developer.nvidia.com/cuda-downloads'
        filesAnalyzed = $false
        licenseConcluded = 'LicenseRef-NVIDIA-CUDA-EULA'
        licenseDeclared = 'LicenseRef-NVIDIA-CUDA-EULA'
        copyrightText = 'Copyright NVIDIA Corporation'
        comment = 'Required by GPU builds; driver/runtime components are not redistributed by this bundle.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-Maxine'
        name = 'NVIDIA Video Effects runtime'
        versionInfo = 'runtime-provided'
        downloadLocation = 'https://www.nvidia.com/en-us/geforce/broadcasting/broadcast-sdk/resources/'
        filesAnalyzed = $false
        licenseConcluded = 'LicenseRef-NVIDIA-Video-Effects-SDK'
        licenseDeclared = 'LicenseRef-NVIDIA-Video-Effects-SDK'
        copyrightText = 'Copyright NVIDIA Corporation'
        comment = 'Optional runtime installed separately by the user.'
    },
    [ordered]@{
        SPDXID = 'SPDXRef-Package-Tesseract'
        name = 'Tesseract OCR'
        versionInfo = '5.4.0.20240606'
        downloadLocation = 'https://github.com/UB-Mannheim/tesseract/wiki'
        filesAnalyzed = $false
        licenseConcluded = 'Apache-2.0'
        licenseDeclared = 'Apache-2.0'
        copyrightText = 'Copyright Tesseract contributors'
        comment = 'Optional executable installed separately by the user.'
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
    }
)

$relationships = foreach ($package in $packages | Select-Object -Skip 1) {
    [ordered]@{
        spdxElementId = 'SPDXRef-Package-OpenZoom'
        relationshipType = 'DEPENDS_ON'
        relatedSpdxElement = $package.SPDXID
    }
}
$sbom = [ordered]@{
    spdxVersion = 'SPDX-2.3'
    dataLicense = 'CC0-1.0'
    SPDXID = 'SPDXRef-DOCUMENT'
    name = 'OpenZoom release SBOM'
    documentNamespace = "https://openzoom.local/spdx/$namespaceId"
    creationInfo = [ordered]@{
        created = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
        creators = @('Tool: OpenZoom generate_release_metadata.ps1')
    }
    hasExtractedLicensingInfos = @(
        [ordered]@{
            licenseId = 'LicenseRef-OpenZoom-Commercial'
            extractedText =
                'OpenZoom is available under a separate commercial license from its copyright holder.'
            name = 'OpenZoom commercial license'
        }
    )
    packages = $packages
    relationships = @($relationships)
}
$sbom | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $bundle 'SBOM.spdx.json') -Encoding utf8NoBOM
