#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('', 'Preflight', 'Cuda')][string]$Action = '',
    [string]$OcrRuntimePath,
    [string]$OcrSdkPath,
    [string]$OcrTensorRtPath,
    [string]$OcrModelsPath,
    [string]$OcrCudaPath,
    [string]$ConfiguredCudaLicense
)
$ErrorActionPreference = 'Stop'
# Only definitions are imported; dot-sourcing never invokes staging.
. (Join-Path $PSScriptRoot '../stage_uct_payload.ps1')

function Assert-OkuFlowUctSourcePaths([string]$Runtime, [string]$Sdk, [string]$TensorRt,
    [string]$Models, [string]$Cuda = '') {
    $roots = @{runtime=$Runtime;tensorrt=$TensorRt;models=$Models;
        repo=(Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)}
    $manifest = Get-Content -LiteralPath (Join-Path $roots.repo 'scripts/uct/distribution.json') -Raw | ConvertFrom-Json -AsHashtable
    $null = Test-UctPayloadCatalog $manifest $roots.repo
    $requiredRoots = @($manifest.files.root | Sort-Object -Unique | Where-Object { $_ -notin @('repo','cuda') })
    foreach ($key in $requiredRoots) {
        if (!$roots[$key]) { throw "Missing UCT $key source. Set the corresponding OKUFLOW_UCT_PAYLOAD_RUNTIME/SDK/MODELS pointer to a prepared physical directory; no download or preparation is automatic." }
        $roots[$key] = Get-UctAbsolutePath $roots[$key]
        Assert-UctRegularPath $roots[$key] -Directory
    }
    $sdkPath = Get-UctAbsolutePath $Sdk
    Assert-UctRegularPath $sdkPath -Directory
    foreach ($header in @('onnxruntime-win-x64-1.31.0/onnxruntime-win-x64-1.31.0/include/onnxruntime_cxx_api.h')) {
        Assert-UctRegularPath (Join-UctContainedPath $sdkPath $header) -File
    }
    Assert-UctRegularPath (Join-UctContainedPath $roots.tensorrt 'include/NvInfer.h') -File
    if ($Cuda) {
        $roots.cuda = Get-UctAbsolutePath $Cuda
        Assert-UctRegularPath $roots.cuda -Directory
    }
    foreach ($entry in $manifest.files) {
        # CUDA may be resolved from the effective sealed build facts later.
        if ($entry.root -ceq 'cuda' -and !$Cuda) { continue }
        if (!$roots.ContainsKey($entry.root)) { throw "Unknown UCT input root: $($entry.root)" }
        $path = Join-UctContainedPath $roots[$entry.root] $entry.source
        Assert-UctRegularPath $path -File
        if ((Get-Item -LiteralPath $path).Length -ne $entry.bytes) { throw "UCT source size mismatch before build: $path" }
    }
}

function Assert-OkuFlowUctCudaSource([string]$Cuda, [string]$BuildLicense = '') {
    if (!$Cuda) { throw 'Set OKUFLOW_UCT_PAYLOAD_CUDA to the physical prepared CUDA root; a CPU build has no effective CUDA toolkit default.' }
    $root = Get-UctAbsolutePath $Cuda
    Assert-UctRegularPath $root -Directory
    $license = Join-UctContainedPath $root 'LICENSE'
    Assert-UctRegularPath $license -File
    if ($BuildLicense -and (Get-FileHash -LiteralPath $license).Hash -cne (Get-FileHash -LiteralPath $BuildLicense).Hash) {
        throw 'UCT NVRTC source license differs from the effective configured CUDA toolkit license.'
    }
    foreach ($relative in @('bin/nvrtc64_120_0.dll','bin/nvrtc-builtins64_129.dll')) {
        Assert-UctRegularPath (Join-UctContainedPath $root $relative) -File
    }
}

function Read-OkuFlowUctJson([string]$Path) {
    Assert-UctRegularPath $Path -File
    $length = (Get-Item -LiteralPath $Path).Length
    $hash = Get-UctFileHash $Path $length 1048576
    $bytes = Read-UctPinnedBytes $Path $length $hash
    return [pscustomobject]@{Value=([Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json -AsHashtable -Depth 20);
        Bytes=$length; Sha256=$hash}
}

function Get-OkuFlowUctBundleMetadata([string]$BundleRoot, [string]$RepositoryRoot, [switch]$Required) {
    $bundlePath = Get-UctAbsolutePath $BundleRoot
    Assert-UctRegularPath $bundlePath -Directory
    $receiptRelative = 'app/ocr/payload-receipt.json'
    $markerRelative = 'app/ocr/payload-receipt.sha256'
    $receiptPath = Join-UctContainedPath $bundlePath $receiptRelative
    if (!(Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
        foreach ($relative in @('app/ocr','app/models/uct','ocr','models/uct')) {
            if (Test-Path -LiteralPath (Join-UctContainedPath $bundlePath $relative)) {
                throw 'Partial or legacy unsealed OCR payload: a complete pinned UCT receipt is required.'
            }
        }
        if ($Required) { throw 'The shipping bundle requires a staged, sealed UCT payload.' }
        return $null
    }
    $repo = Get-UctAbsolutePath $RepositoryRoot
    $distribution = Read-OkuFlowUctJson (Join-UctContainedPath $repo 'scripts/uct/distribution.json')
    $catalog = $distribution.Value
    if ($catalog.schema -ne 1 -or $catalog.maxFileBytes -lt 1 -or $catalog.maxFileBytes -gt 536870912 -or
        $catalog.maxTotalBytes -lt 1 -or $catalog.maxTotalBytes -gt 3221225472 -or
        @($catalog.files).Count -lt 1 -or @($catalog.files).Count -gt 128) { throw 'Invalid UCT distribution bounds.' }
    $bf16 = Test-UctPayloadCatalog $catalog $repo
    $receiptDocument = Read-OkuFlowUctJson $receiptPath
    $receipt = $receiptDocument.Value
    $markerPath = Join-UctContainedPath $bundlePath $markerRelative
    $marker = Read-UctPinnedBytes $markerPath 65 (Get-UctFileHash $markerPath 65 65)
    if ([Text.Encoding]::ASCII.GetString($marker) -cne ($receiptDocument.Sha256 + "`n") -or
        $receipt.schema -ne 1 -or $receipt.distributionSha256 -cne $distribution.Sha256 -or
        @($receipt.files).Count -ne @($catalog.files).Count -or @($receipt.generatedFiles).Count -ne 2) {
        throw 'UCT receipt marker, distribution pin or inventory does not match.'
    }
    $packageMap = @{}
    foreach ($package in $catalog.packages) {
        if ($package.id -cnotmatch '^[a-z][a-z0-9-]*$' -or $packageMap.ContainsKey($package.id) -or
            !$package.version -or !$package.licenseDeclared) { throw 'Invalid UCT package identity.' }
        $packageMap[$package.id] = $package
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $records = @()
    [long]$total = 0
    for ($i = 0; $i -lt $catalog.files.Count; $i++) {
        $entry = $catalog.files[$i]
        $record = $receipt.files[$i]
        Assert-UctRelativePath $entry.path
        Assert-UctRelativePath $entry.source
        if ($entry.path -cnotmatch '^app/(ocr/|models/uct/)' -or !$seen.Add($entry.path) -or
            $entry.root -cnotin @('repo','runtime','models','tensorrt','cuda') -or !$packageMap.ContainsKey($entry.package) -or
            $entry.sha256 -cnotmatch '^[0-9a-f]{64}$' -or [long]$entry.bytes -lt 1 -or
            [long]$entry.bytes -gt $catalog.maxFileBytes -or [decimal]$entry.bytes -ne [long]$entry.bytes) {
            throw "Invalid pinned UCT file: $($entry.path)"
        }
        foreach ($field in @('root','source','path','bytes','sha256','package')) {
            if ([string]$record[$field] -cne [string]$entry[$field]) { throw "UCT receipt/catalog mismatch: $($entry.path) $field" }
        }
        if ($record.licenseDeclared -cne $packageMap[$entry.package].licenseDeclared) { throw 'UCT receipt package license mismatch.' }
        if ((Get-UctFileHash (Join-UctContainedPath $bundlePath $entry.path) $entry.bytes $catalog.maxFileBytes) -cne $entry.sha256) {
            throw "UCT staged bytes changed: $($entry.path)"
        }
        $records += $record
        $total += [long]$record.bytes
    }
    $generatedExpected = @{
        'app/ocr/distribution.json' = @{bytes=$distribution.Bytes;sha256=$distribution.Sha256}
        'app/models/uct/MODIFICATIONS.txt' = @{bytes=0;sha256=''}
    }
    $modifications = Get-UctModificationBytes $bf16
    $generatedExpected['app/models/uct/MODIFICATIONS.txt'] = @{bytes=$modifications.Length;sha256=(Get-UctBytesHash $modifications)}
    foreach ($record in $receipt.generatedFiles) {
        Assert-UctRelativePath $record.path
        if ($record.ContainsKey('package') -or $record.path -cnotin @($generatedExpected.Keys) -or !$seen.Add($record.path) -or
            $record.bytes -ne $generatedExpected[$record.path].bytes -or
            $record.sha256 -cne $generatedExpected[$record.path].sha256 -or
            (Get-UctFileHash (Join-UctContainedPath $bundlePath $record.path) $record.bytes 1048576) -cne $record.sha256) {
            throw 'UCT generated distribution/modification notice differs from current pins.'
        }
        $total += [long]$record.bytes
    }
    $sourceLines = @($records | ForEach-Object { "$($_.root)|$($_.source)|$($_.bytes)|$($_.sha256)`n" }) -join ''
    $stagedRecords = @($records | ForEach-Object { @{path=$_.path;bytes=$_.bytes;sha256=$_.sha256} }) + @($receipt.generatedFiles)
    $stagedLines = @($stagedRecords | ForEach-Object { "$($_.path)|$($_.bytes)|$($_.sha256)`n" }) -join ''
    if ($total -gt $catalog.maxTotalBytes -or $total -ne $receipt.totalBytes -or
        (Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($sourceLines))) -cne $receipt.sourceSealSha256 -or
        (Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($stagedLines))) -cne $receipt.stagedSealSha256) { throw 'UCT receipt content seals or byte total do not match.' }
    foreach ($relative in @($receiptRelative,$markerRelative)) { [void]$seen.Add($relative) }
    foreach ($subtree in @('app/ocr','app/models')) {
        foreach ($item in Get-ChildItem -LiteralPath (Join-UctContainedPath $bundlePath $subtree) -Recurse -Force) {
            Assert-UctRegularPath $item.FullName
            if (!$item.PSIsContainer) {
                $relative = [IO.Path]::GetRelativePath($bundlePath,$item.FullName).Replace('\','/')
                if (!$seen.Contains($relative)) { throw "Unlisted UCT payload file: $relative" }
            }
        }
    }
    $spdxPackages = @()
    $relationships = @()
    $spdxFiles = @()
    $licenses = @()
    $externalNotices = @{}
    foreach ($package in $catalog.packages) {
        $id = 'SPDXRef-Package-UCT-' + $package.id
        $notices = @($package.noticePaths)
        if (!$notices.Count) { throw "Missing retained notice paths for UCT package $($package.id)" }
        foreach ($notice in $notices) {
            Assert-UctRelativePath $notice
            if ($package.id -ceq 'paddleocr' -and $notice -ceq 'app/models/uct/MODIFICATIONS.txt') { continue }
            if ($package.id -ceq 'okuflow' -and $notice -cin @('LICENSE','COMMERCIAL.md')) {
                # These belong to the normal bundle, outside the OCR receipt.
                # Retain their actual file metadata and verify the source bytes.
                $noticePath = Join-UctContainedPath $bundlePath $notice
                Assert-UctRegularPath $noticePath -File
                $length = (Get-Item -LiteralPath $noticePath).Length
                $hash = Get-UctFileHash $noticePath $length 1048576
                if ((Get-UctFileHash (Join-UctContainedPath $repo $notice) $length 1048576) -cne $hash) {
                    throw "UCT first-party bundle notice differs from repository: $notice"
                }
                $externalNotices[$notice] = @{path=$notice;bytes=$length;sha256=$hash;package='okuflow'}
                continue
            }
            $noticeEntry = @($catalog.files | Where-Object { $_.path -ceq $notice -and $_.package -ceq $package.id })
            if ($noticeEntry.Count -ne 1) { throw "UCT package notice is not in its pinned inventory: $notice" }
        }
        $spdxPackages += [ordered]@{SPDXID=$id;name= $(if ($package.name) { $package.name } else { $package.id });
            versionInfo=$package.version;downloadLocation=$(if ($package.downloadLocation) { $package.downloadLocation } else { 'NOASSERTION' });
            filesAnalyzed=$false;licenseDeclared=$package.licenseDeclared;licenseConcluded=$package.licenseDeclared;
            copyrightText='See retained notices';comment=('Private UCT payload. Exact retained notices: ' + ($notices -join ', '))}
        $relationships += [ordered]@{spdxElementId='SPDXRef-Package-OkuFlow';relationshipType='CONTAINS';relatedSpdxElement=$id}
        if ($package.licenseDeclared -cmatch '^LicenseRef-') {
            $licensePaths = @(if ($package.licensePath) { $package.licensePath }
                elseif ($package.id -ceq 'tensorrt') { @($notices | Where-Object { $_ -cmatch '/TENSORRT_11_4_LICENSE\.html$' }) }
                elseif ($package.id -ceq 'cuda-nvrtc') { @($notices | Where-Object { $_ -cmatch '/LICENSE$' }) }
                else { @() })
            if ($licensePaths.Count -ne 1 -or $licensePaths[0] -cnotin $notices) { throw 'UCT custom license requires a unique exact retained license path.' }
            $licenses += [ordered]@{licenseId=$package.licenseDeclared;
                extractedText=(Get-Content -LiteralPath (Join-UctContainedPath $bundlePath $licensePaths[0]) -Raw);
                name=$package.id;seeAlsos=@($package.licenseUrl)}
        }
    }
    # Preserve the package declared by each pinned catalog record. The reduced
    # staged-seal records intentionally contain only path/size/hash.
    $generatedInventory = @($receipt.generatedFiles | ForEach-Object {
        @{path=$_.path;bytes=$_.bytes;sha256=$_.sha256;
            package=$(if ($_.path -ceq 'app/models/uct/MODIFICATIONS.txt') { 'paddleocr' } else { 'okuflow' })}
    })
    $allFiles = @($records) + $generatedInventory + @($externalNotices.Values) + @(
        @{path=$receiptRelative;bytes=$receiptDocument.Bytes;sha256=$receiptDocument.Sha256},
        @{path=$markerRelative;bytes=65;sha256=(Get-UctBytesHash $marker)})
    foreach ($record in $allFiles) {
        $fileId = 'SPDXRef-File-UCT-' + (Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($record.path)))
        $owner = if ($record.package) { 'SPDXRef-Package-UCT-' + $record.package }
            elseif ($record.path -ceq 'app/models/uct/MODIFICATIONS.txt') { 'SPDXRef-Package-UCT-paddleocr' }
            else { 'SPDXRef-Package-UCT-okuflow' }
        if (!$packageMap.ContainsKey('okuflow')) { throw 'UCT generated receipt/catalog files require the first-party package.' }
        $spdxFiles += [ordered]@{SPDXID=$fileId;fileName=('./' + $record.path);
            checksums=@([ordered]@{algorithm='SHA256';checksumValue=$record.sha256});
            licenseConcluded='NOASSERTION';copyrightText='NOASSERTION'}
        $relationships += [ordered]@{spdxElementId=$owner;relationshipType='CONTAINS';relatedSpdxElement=$fileId}
    }
    return [pscustomobject]@{
        Manifest=[ordered]@{schemaVersion=1;distribution='app/ocr/distribution.json';distributionSha256=$distribution.Sha256;
            receipt=$receiptRelative;receiptSha256=$receiptDocument.Sha256;marker=$markerRelative;
            sourceSealSha256=$receipt.sourceSealSha256;stagedSealSha256=$receipt.stagedSealSha256;
            inventoryFileCount=$stagedRecords.Count;totalBytes=$total;packages=@($catalog.packages);
            precision=[ordered]@{gpuPreference='BF16';gpuFallback='same-tier FP32';cpu='FP32';
                modelTiers=@($catalog.modelTiers);optionalModelTiers=@('tiny','small');roles=@('detector','recognizer');
                arithmetic='Mixed BF16/FP16/FP32 with FLOAT I/O';accuracyGate=$false};
            acceptance='Byte inventory and provenance only; no runtime, GPU, model-quality or installer acceptance.'};
        Packages=$spdxPackages;Files=$spdxFiles;Relationships=$relationships;Licenses=$licenses;Inventory=$allFiles
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    if ($Action -ceq 'Preflight') {
        if (!$OcrCudaPath -and $env:OKUFLOW_ENABLE_CUDA -in @('OFF','0','FALSE','NO')) {
            throw 'A CPU-only bundle requires OKUFLOW_UCT_PAYLOAD_CUDA for the private NVRTC subset before building.'
        }
        Assert-OkuFlowUctSourcePaths $OcrRuntimePath $OcrSdkPath $OcrTensorRtPath $OcrModelsPath $OcrCudaPath
        Write-Output 'PASS: UCT source paths, headers and selected file sizes; no copying or runtime acceptance.'
    } elseif ($Action -ceq 'Cuda') {
        Assert-OkuFlowUctCudaSource $OcrCudaPath $ConfiguredCudaLicense
    } else { throw 'Specify -Action Preflight or Cuda, or dot-source the metadata functions.' }
}
