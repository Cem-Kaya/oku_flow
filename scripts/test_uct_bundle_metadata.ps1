#Requires -Version 7.0
[CmdletBinding()]
param([string]$EvidenceRoot = (Join-Path $env:LOCALAPPDATA 'OkuFlow/uct-bundle-metadata-tests'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-installer/uct_bundle.ps1')
if (!$IsWindows) { throw 'UCT metadata fixtures require Windows path/stream semantics.' }
$watch = [Diagnostics.Stopwatch]::StartNew()
$evidence = Get-UctAbsolutePath $EvidenceRoot
Assert-UctRegularPath $evidence
[void][IO.Directory]::CreateDirectory($evidence)
$run = Join-Path $evidence ([Guid]::NewGuid().ToString('N'))
New-UctDirectory $run
$script:passed = 0

function Assert-Metadata([bool]$Value, [string]$Message) { if (!$Value) { throw $Message } }
function Assert-MetadataRefused([scriptblock]$Body, [string]$Reason) {
    $caught = $false
    try { & $Body | Out-Null } catch {
        $caught = $true
        if ($_.Exception.Message -notmatch $Reason) { throw "Wrong refusal: $($_.Exception.Message); expected $Reason" }
    }
    if (!$caught) { throw "Expected UCT metadata refusal: $Reason" }
}
function Write-FixtureText([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path,$Text,[Text.UTF8Encoding]::new($false))
}
function Add-FixtureFile([hashtable]$Fixture, [string]$Relative, [string]$Root,
    [string]$Source, [string]$Package, [string]$Text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    Write-FixtureText (Join-Path $Fixture.bundle $Relative) $Text
    $entry = [ordered]@{root=$Root;source=$Source;path=$Relative;bytes=$bytes.Length;
        sha256=(Get-UctBytesHash $bytes);package=$Package}
    $Fixture.catalog.files += $entry
    if ($Root -ceq 'repo') { Write-FixtureText (Join-Path $Fixture.repo $Source) $Text }
    return $entry
}
function Seal-Fixture([hashtable]$Fixture) {
    $catalogText = ($Fixture.catalog | ConvertTo-Json -Depth 20) + "`n"
    Write-FixtureText (Join-Path $Fixture.repo 'scripts/uct/distribution.json') $catalogText
    Write-FixtureText (Join-Path $Fixture.bundle 'app/ocr/distribution.json') $catalogText
    $mods = Get-UctModificationBytes $Fixture.bf16
    Write-FixtureText (Join-Path $Fixture.bundle 'app/models/uct/MODIFICATIONS.txt') ([Text.Encoding]::UTF8.GetString($mods))
    $records = @($Fixture.catalog.files | ForEach-Object {
        $p = $_
        $license = @($Fixture.catalog.packages | Where-Object id -CEQ $p.package)[0].licenseDeclared
        [ordered]@{root=$p.root;source=$p.source;path=$p.path;bytes=$p.bytes;sha256=$p.sha256;
            package=$p.package;licenseDeclared=$license}
    })
    $generated = @('app/ocr/distribution.json','app/models/uct/MODIFICATIONS.txt') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $Fixture.bundle $_)
        [ordered]@{path=$_;bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()}
    }
    $sourceLines = @($records | ForEach-Object { "$($_.root)|$($_.source)|$($_.bytes)|$($_.sha256)`n" }) -join ''
    $all = @($records) + @($generated)
    $stagedLines = @($all | ForEach-Object { "$($_.path)|$($_.bytes)|$($_.sha256)`n" }) -join ''
    [long]$totalBytes = 0
    foreach ($record in $all) { $totalBytes += [long]$record.bytes }
    $receipt = [ordered]@{schema=1;distributionSha256=(Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($catalogText)));
        sourceSealSha256=(Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($sourceLines)));
        stagedSealSha256=(Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($stagedLines)));
        files=$records;generatedFiles=@($generated);totalBytes=$totalBytes}
    Write-FixtureReceipt $Fixture $receipt
    $Fixture.receipt = $receipt
}
function Write-FixtureReceipt([hashtable]$Fixture, [System.Collections.IDictionary]$Receipt) {
    $text = ($Receipt | ConvertTo-Json -Depth 20) + "`n"
    Write-FixtureText (Join-Path $Fixture.bundle 'app/ocr/payload-receipt.json') $text
    Write-FixtureText (Join-Path $Fixture.bundle 'app/ocr/payload-receipt.sha256') ((Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($text))) + "`n")
}
function New-MetadataFixture([string]$Name) {
    $case = Join-Path $run $Name
    $f = @{bundle=(Join-Path $case 'bundle');repo=(Join-Path $case 'repo');catalog=@{
        schema=1;maxFileBytes=1048576;maxTotalBytes=2097152;files=@();packages=@(
            @{id='paddleocr';version='synthetic';licenseDeclared='Apache-2.0';noticePaths=@('app/ocr/MODEL_LICENSES.txt','app/models/uct/MODIFICATIONS.txt')},
            @{id='onnxruntime';version='synthetic';licenseDeclared='MIT';archiveSha256=('1'*64);noticePaths=@('app/ocr/cpu/LICENSE')},
            @{id='tensorrt';version='synthetic';licenseDeclared='LicenseRef-NVIDIA-TensorRT-11.4';archiveSha256=('2'*64);
                noticePaths=@('app/ocr/tensorrt/doc/LICENSE.html');licensePath='app/ocr/tensorrt/doc/LICENSE.html';licenseUrl='https://example.invalid/trt'},
            @{id='cuda-nvrtc';version='synthetic';licenseDeclared='LicenseRef-NVIDIA-CUDA-12.9';noticePaths=@('app/ocr/tensorrt/cuda/LICENSE');
                licensePath='app/ocr/tensorrt/cuda/LICENSE';licenseUrl='https://example.invalid/cuda'},
            @{id='okuflow';version='synthetic';licenseDeclared='GPL-3.0-only OR LicenseRef-OkuFlow-Commercial';noticePaths=@('LICENSE','COMMERCIAL.md')})}}
    [void][IO.Directory]::CreateDirectory($f.bundle)
    [void][IO.Directory]::CreateDirectory($f.repo)
    foreach ($notice in @('LICENSE','COMMERCIAL.md')) {
        Write-FixtureText (Join-Path $f.bundle $notice) "Synthetic first-party $notice"
        Write-FixtureText (Join-Path $f.repo $notice) "Synthetic first-party $notice"
    }
    $converter = Add-FixtureFile $f 'app/ocr/sources/precision_models.py' repo 'scripts/uct/precision_models.py' okuflow '# Synthetic converter text; never executed.'
    $ort = Add-FixtureFile $f 'app/ocr/cpu/onnxruntime.dll' runtime 'cpu/onnxruntime.dll' onnxruntime 'Synthetic CPU bytes; never loaded.'
    $pins = @{models=@();ort=@{dllSha256=$ort.sha256;sha256=('1'*64)};tensorrt=@{sha256=('2'*64)}}
    $bf16 = @{models=@();converterSha256=$converter.sha256;conversion='synthetic fixture'}
    foreach ($kind in @('det','rec')) {
        foreach ($tier in @('tiny','small','medium')) {
            $dir = "$kind/$tier"
            $graph = Add-FixtureFile $f "app/ocr/$dir/inference.onnx" runtime "$dir/inference.onnx" paddleocr "Synthetic FP32 $dir"
            $config = Add-FixtureFile $f "app/ocr/$dir/inference.yml" runtime "$dir/inference.yml" paddleocr "Synthetic configuration $dir"
            $modified = Add-FixtureFile $f "app/models/uct/$dir/bf16-linear/inference.onnx" models "uct/$dir/bf16-linear/inference.onnx" paddleocr "Synthetic BF16 $dir"
            $pins.models += @{directory=$dir;kind=$kind;tier=$tier;repository="synthetic/$dir";revision=('3'*40);
                files=@{'inference.onnx'=$graph.sha256;'inference.yml'=$config.sha256}}
            $bf16.models += @{role=$(if($kind -ceq 'det'){'detector'}else{'recognizer'});tier=$tier;
                sourceSha256=$graph.sha256;configSha256=$config.sha256;repository="synthetic/$dir";revision=('3'*40);
                license='Apache-2.0';bundlePath="uct/$dir/bf16-linear/inference.onnx";convertedGraphSha256=$modified.sha256}
        }
    }
    $f.bf16 = $bf16
    $null = Add-FixtureFile $f 'app/ocr/pins.json' repo 'scripts/uct/pins.json' okuflow ($pins | ConvertTo-Json -Depth 15)
    $null = Add-FixtureFile $f 'app/models/uct/bf16-models.json' repo 'scripts/uct/bf16-models.json' okuflow ($bf16 | ConvertTo-Json -Depth 15)
    foreach ($notice in @(
        @('app/ocr/MODEL_LICENSES.txt','paddleocr','Synthetic Apache notice'),
        @('app/ocr/cpu/LICENSE','onnxruntime','Synthetic MIT notice'),
        @('app/ocr/tensorrt/doc/LICENSE.html','tensorrt','<html>Exact synthetic TensorRT agreement</html>'),
        @('app/ocr/tensorrt/cuda/LICENSE','cuda-nvrtc','Exact synthetic CUDA NVRTC agreement'),
        @('app/ocr/sources/LICENSE','okuflow','Synthetic first-party notice'))) {
        $null = Add-FixtureFile $f $notice[0] repo ('notices/' + $notice[1] + '.txt') $notice[1] $notice[2]
    }
    Seal-Fixture $f
    return $f
}
function Read-FixtureMetadata([hashtable]$Fixture) {
    Get-OkuFlowUctBundleMetadata -BundleRoot $Fixture.bundle -RepositoryRoot $Fixture.repo -Required
}
function Invoke-MetadataCase([string]$Name, [scriptblock]$Body) {
    if ($watch.Elapsed.TotalSeconds -ge 110) { throw 'UCT metadata fixtures exceeded their 120-second budget.' }
    & $Body
    $script:passed++
    Write-Host "PASS $Name"
}

try {
    Invoke-MetadataCase 'sealed synthetic inventory and SPDX relationships/licenses' {
        $f = New-MetadataFixture 'valid'
        $m = Read-FixtureMetadata $f
        Assert-Metadata ($m.Manifest.precision.cpu -ceq 'FP32' -and !$m.Manifest.precision.accuracyGate) 'Precision declarations changed.'
        Assert-Metadata ($m.Packages.Count -eq 5 -and $m.Files.Count -eq ($f.catalog.files.Count + 6)) 'SPDX inventory coverage mismatch.'
        Assert-Metadata (@($m.Packages | Where-Object filesAnalyzed -NE $false).Count -eq 0) 'Do not invent package verification codes.'
        Assert-Metadata (@($m.Relationships | Where-Object relationshipType -CEQ 'CONTAINS').Count -eq (5 + $m.Files.Count)) 'Missing CONTAINS relationships.'
        foreach ($entry in $f.catalog.files) {
            $file = @($m.Files | Where-Object fileName -CEQ ('./' + $entry.path))
            Assert-Metadata ($file.Count -eq 1) 'Pinned file missing from SPDX.'
            $relation = @($m.Relationships | Where-Object {
                $_.relatedSpdxElement -ceq $file[0].SPDXID -and $_.relationshipType -ceq 'CONTAINS'
            })
            Assert-Metadata ($relation.Count -eq 1 -and $relation[0].spdxElementId -ceq ('SPDXRef-Package-UCT-' + $entry.package)) 'Pinned file has the wrong SPDX package owner.'
        }
        Assert-Metadata ($m.Licenses.Count -eq 2 -and $m.Licenses[0].extractedText -ceq '<html>Exact synthetic TensorRT agreement</html>') 'Exact custom license bytes were not retained.'
        Assert-Metadata (($m | ConvertTo-Json -Depth 20) -notmatch [regex]::Escape($run)) 'Private absolute source path leaked.'
    }
    Invoke-MetadataCase 'legacy non-OCR compatibility and explicit shipping requirement' {
        $empty = Join-Path $run 'legacy'
        New-UctDirectory $empty
        Assert-Metadata ($null -eq (Get-OkuFlowUctBundleMetadata $empty $empty)) 'Legacy non-OCR bundle rejected.'
        Assert-MetadataRefused { Get-OkuFlowUctBundleMetadata $empty $empty -Required } 'requires a staged'
        [void][IO.Directory]::CreateDirectory((Join-Path $empty 'ocr'))
        Write-FixtureText (Join-Path $empty 'ocr/partial.bin') 'partial'
        Assert-MetadataRefused { Get-OkuFlowUctBundleMetadata $empty $empty } 'unsealed OCR'
    }
    Invoke-MetadataCase 'receipt mutation and missing final marker' {
        $f = New-MetadataFixture 'receipt'
        Write-FixtureText (Join-Path $f.bundle 'app/ocr/payload-receipt.sha256') (('0'*64) + "`n")
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'receipt marker'
        # Rename only a synthetic owned fixture file; preserve refused bytes.
        Move-Item -LiteralPath (Join-Path $f.bundle 'app/ocr/payload-receipt.sha256') -Destination (Join-Path $f.bundle 'app/ocr/refused-marker')
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'cannot find|does not exist|could not find'
    }
    Invoke-MetadataCase 'same-size staged mutation and unexpected payload file' {
        $f = New-MetadataFixture 'bytes'
        $path = Join-Path $f.bundle 'app/ocr/cpu/onnxruntime.dll'
        $item = Get-Item -LiteralPath $path
        $stamp = $item.LastWriteTimeUtc
        $bytes = [IO.File]::ReadAllBytes($path); $bytes[0] = $bytes[0] -bxor 1
        [IO.File]::WriteAllBytes($path,$bytes); [IO.File]::SetLastWriteTimeUtc($path,$stamp)
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'staged bytes changed'
        $f = New-MetadataFixture 'extra'
        Write-FixtureText (Join-Path $f.bundle 'app/ocr/unlisted.dll') 'unlisted'
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'Unlisted UCT'
        $f = New-MetadataFixture 'extra-model'
        Write-FixtureText (Join-Path $f.bundle 'app/models/unlisted.plan') 'unlisted'
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'Unlisted UCT'
    }
    Invoke-MetadataCase 'resealed source/staged digest and package tampering' {
        foreach ($field in @('sourceSealSha256','stagedSealSha256')) {
            $f = New-MetadataFixture $field
            $f.receipt[$field] = '0'*64
            Write-FixtureReceipt $f $f.receipt
            Assert-MetadataRefused { Read-FixtureMetadata $f } 'content seals'
        }
        $f = New-MetadataFixture 'package'
        $f.receipt.files[0].package = 'onnxruntime'
        Write-FixtureReceipt $f $f.receipt
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'receipt/catalog mismatch'
        $f = New-MetadataFixture 'generated-package'
        $f.receipt.generatedFiles[0].package = 'missing'
        Write-FixtureReceipt $f $f.receipt
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'generated distribution'
    }
    Invoke-MetadataCase 'current catalog, generated notice, and retained license pins' {
        $f = New-MetadataFixture 'catalog'
        Write-FixtureText (Join-Path $f.repo 'scripts/uct/bf16-models.json') '{}'
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'metadata size mismatch|metadata pin changed'
        $f = New-MetadataFixture 'modifications'
        Write-FixtureText (Join-Path $f.bundle 'app/models/uct/MODIFICATIONS.txt') 'changed'
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'size mismatch|generated distribution'
        $f = New-MetadataFixture 'license'
        Write-FixtureText (Join-Path $f.bundle 'app/ocr/tensorrt/doc/LICENSE.html') 'changed'
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'size mismatch|staged bytes changed'
        $f = New-MetadataFixture 'distribution'
        Write-FixtureText (Join-Path $f.bundle 'app/ocr/distribution.json') '{}'
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'size mismatch|generated distribution'
    }
    Invoke-MetadataCase 'missing notices and source/catalog graph disagreement' {
        $f = New-MetadataFixture 'notices'
        $f.catalog.packages[2].noticePaths = @('app/ocr/missing.html')
        Seal-Fixture $f
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'notice is not in'
        $f = New-MetadataFixture 'root-notice'
        Write-FixtureText (Join-Path $f.bundle 'COMMERCIAL.md') 'altered'
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'size mismatch|notice differs'
        $f = New-MetadataFixture 'model-pin'
        $f.catalog.files[2].sha256 = '0'*64
        Seal-Fixture $f
        Assert-MetadataRefused { Read-FixtureMetadata $f } 'model distribution parity'
    }
    Assert-Metadata ($script:passed -eq 7) 'Incomplete UCT metadata fixture coverage.'
    Write-Output "PASS: 7 model-free UCT metadata groups; synthetic catalogs/files only, no SDK/model/runtime/GUI/GPU execution. Evidence: $run"
} finally {
    Write-Host "UCT metadata evidence retained: $run"
}
