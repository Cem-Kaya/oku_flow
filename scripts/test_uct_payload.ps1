#Requires -Version 7.0
[CmdletBinding()]
param([string]$EvidenceRoot = (Join-Path $env:LOCALAPPDATA 'OkuFlow/uct-payload-tests'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'stage_uct_payload.ps1')
if (!$IsWindows) { throw 'UCT payload fixtures require Windows path/ADS semantics.' }
$watch = [Diagnostics.Stopwatch]::StartNew()
$root = Get-UctAbsolutePath $EvidenceRoot
Assert-UctRegularPath $root
if (!(Test-Path -LiteralPath $root)) { [IO.Directory]::CreateDirectory($root) | Out-Null }
Assert-UctRegularPath $root -Directory
$run = Join-Path $root ([Guid]::NewGuid().ToString('N'))
New-UctDirectory $run
$script:originalCopy = ${function:Copy-UctPinnedFile}
$script:passed = 0

function Assert-Fixture([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}
function Assert-Refused([scriptblock]$Action, [string]$Reason) {
    $caught = $false
    try { & $Action | Out-Null } catch {
        $caught = $true
        if ($_.Exception.Message -notmatch $Reason) { throw "Wrong refusal: $($_.Exception.Message); expected $Reason" }
    }
    if (!$caught) { throw "Expected refusal: $Reason" }
}
function Invoke-Fixture([string]$Name, [scriptblock]$Body) {
    if ($watch.Elapsed.TotalSeconds -ge 110) { throw 'UCT fixtures exceeded their 120-second budget.' }
    & $Body
    $script:passed++
    Write-Host "PASS $Name"
}
function New-Fixture([string]$Name) {
    $case = Join-Path $run $Name
    New-UctDirectory $case
    $source = Join-Path $case 'source'
    $bundle = Join-Path $case 'bundle'
    New-UctDirectory $source
    New-UctDirectory $bundle
    $bytes = [Text.Encoding]::ASCII.GetBytes('0123456789abcdef')
    [IO.File]::WriteAllBytes((Join-Path $source 'one.bin'), $bytes)
    $manifest = @{schema=1;maxFileBytes=1024;maxTotalBytes=4096;
        packages=@(@{id='synthetic';licenseDeclared='MIT'});
        files=@(@{root='fixture';source='one.bin';path='app/ocr/one.bin';bytes=$bytes.Length;
            sha256=(Get-UctBytesHash $bytes);package='synthetic'})}
    return @{source=$source;bundle=$bundle;manifest=$manifest;roots=@{fixture=$source}}
}
function Assert-NoSeal([hashtable]$Fixture) {
    Assert-Fixture (!(Test-Path -LiteralPath (Join-Path $Fixture.bundle 'app/ocr/payload-receipt.sha256'))) 'Failure must not seal a receipt.'
}

try {
    Invoke-Fixture 'real catalog / distribution / compiled pin parity (model-free)' {
        $repo = Get-UctAbsolutePath (Split-Path $PSScriptRoot -Parent)
        $manifest = Get-Content (Join-Path $PSScriptRoot 'uct/distribution.json') -Raw | ConvertFrom-Json -AsHashtable
        $catalog = Test-UctPayloadCatalog $manifest $repo
        $pins = Get-Content (Join-Path $PSScriptRoot 'uct/pins.json') -Raw | ConvertFrom-Json -AsHashtable
        $compiled = Get-Content (Join-Path $repo 'include/okuflow/common/uct_model_pins.hpp') -Raw
        $pattern = '\{(true|false),ModelTier::(Tiny|Small|Medium),"([^"]+)","([0-9a-f]{64})","([0-9a-f]{64})","([^"]+)","([0-9a-f]{40})"\}'
        $matches = [regex]::Matches($compiled, $pattern)
        Assert-Fixture ($matches.Count -eq 6) 'Expected six independently compiled original pins.'
        foreach ($match in $matches) {
            $kind = if ($match.Groups[1].Value -ceq 'true') { 'det' } else { 'rec' }
            $tier = $match.Groups[2].Value.ToLowerInvariant()
            $p = @($pins.models | Where-Object { $_.kind -ceq $kind -and $_.tier -ceq $tier })
            Assert-Fixture ($p.Count -eq 1 -and $p[0].directory -ceq $match.Groups[3].Value -and
                $p[0].files['inference.onnx'] -ceq $match.Groups[4].Value -and
                $p[0].files['inference.yml'] -ceq $match.Groups[5].Value -and
                $p[0].repository -ceq $match.Groups[6].Value -and $p[0].revision -ceq $match.Groups[7].Value) 'Compiled original pin differs from catalog.'
        }
        $detector = Get-Content (Join-Path $repo 'src/common/uct_bf16_detector.hpp') -Raw
        $recognizer = Get-Content (Join-Path $repo 'src/common/uct_public_bf16.hpp') -Raw
        $precision = Get-Content (Join-Path $repo 'src/common/uct_runtime_precision.hpp') -Raw
        foreach ($m in $catalog.models) {
            $tier = "$([char]::ToUpperInvariant($m.tier[0]))$($m.tier.Substring(1))"
            if ($m.role -ceq 'detector') {
                $pattern = 'case ModelTier::' + $tier + ':return "([0-9a-f]{64})"'
                $pin = [regex]::Match($detector, $pattern)
            } elseif ($m.tier -ceq 'medium') {
                Assert-Fixture ($recognizer.Contains('case ModelTier::Medium:return {tier,runtime_precision::kRecognizerGraph,')) 'Medium recognizer must use the compiled graph constant.'
                $pin = [regex]::Match($precision, 'kRecognizerGraph\[\]\s*=\s*"([0-9a-f]{64})"')
            } else {
                $pattern = 'case ModelTier::' + $tier + ':return \{tier,"([0-9a-f]{64})"'
                $pin = [regex]::Match($recognizer, $pattern)
            }
            Assert-Fixture ($pin.Success -and $pin.Groups[1].Value -ceq $m.convertedGraphSha256) 'Compiled BF16 tier/role pin differs from catalog.'
        }
        # Exact desktop resource allowlist: no tools, corpora or engine caches.
        $extra = @('app/ocr/cpu/onnxruntime.dll','app/ocr/cpu/LICENSE','app/ocr/cpu/ThirdPartyNotices.txt','app/ocr/cpu/Privacy.md',
            'app/ocr/tensorrt/bin/nvinfer_11.dll','app/ocr/tensorrt/bin/nvonnxparser_11.dll',
            'app/ocr/tensorrt/bin/nvinfer_builder_resource_sm75_11.dll','app/ocr/tensorrt/bin/nvinfer_builder_resource_ptx_11.dll',
            'app/ocr/tensorrt/bin/nvinfer_builder_resource_sm86_11.dll','app/ocr/tensorrt/bin/nvinfer_builder_resource_sm89_11.dll',
            'app/ocr/tensorrt/bin/nvinfer_builder_resource_sm120_11.dll',
            'app/ocr/tensorrt/doc/Acknowledgements.txt','app/ocr/tensorrt/doc/README.txt','app/ocr/tensorrt/doc/TENSORRT_11_4_LICENSE.html',
            'app/ocr/tensorrt/doc/PROVENANCE.json',
            'app/ocr/tensorrt/cuda/bin/nvrtc64_120_0.dll','app/ocr/tensorrt/cuda/bin/nvrtc-builtins64_129.dll','app/ocr/tensorrt/cuda/LICENSE',
            'app/ocr/pins.json','app/models/uct/bf16-models.json','app/ocr/sources/precision_models.py',
            'app/ocr/MODEL_LICENSES.txt','app/models/uct/MODEL_LICENSES.txt')
        $expected = @($extra)
        foreach ($m in $pins.models) { foreach ($name in $m.files.Keys) { $expected += "app/ocr/$($m.directory)/$name" } }
        foreach ($m in $catalog.models) { $expected += "app/models/$($m.bundlePath)" }
        Assert-Fixture ($manifest.files.Count -eq 47 -and !(Compare-Object ($expected | Sort-Object) ($manifest.files.path | Sort-Object))) 'Unexpected distribution file set.'
        foreach ($f in $manifest.files) {
            Assert-UctRelativePath $f.source
            Assert-UctRelativePath $f.path
            Assert-Fixture ($f.sha256 -cmatch '^[0-9a-f]{64}$' -and $f.bytes -gt 0 -and $f.bytes -le $manifest.maxFileBytes) 'Every manifest file needs a bounded exact pin.'
        }
        $notice = [Text.Encoding]::UTF8.GetString((Get-UctModificationBytes $catalog))
        foreach ($m in $catalog.models) { Assert-Fixture ($notice.Contains($m.sourceSha256) -and $notice.Contains($m.convertedGraphSha256)) 'Modification notice misses a graph.' }
    }
    Invoke-Fixture 'successful streaming copy and portable receipt seals' {
        $f = New-Fixture 'success'
        $result = Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle @{'app/models/uct/NOTICE.txt'=[Text.Encoding]::ASCII.GetBytes('synthetic')}
        $receiptBytes = [IO.File]::ReadAllBytes($result.ReceiptPath)
        Assert-Fixture ((Get-UctBytesHash $receiptBytes) -ceq $result.ReceiptSha256) 'Receipt byte seal mismatch.'
        Assert-Fixture (([IO.File]::ReadAllText((Join-Path $f.bundle 'app/ocr/payload-receipt.sha256'))).Trim() -ceq $result.ReceiptSha256) 'Commit marker differs.'
        $receipt = [Text.Encoding]::UTF8.GetString($receiptBytes) | ConvertFrom-Json -AsHashtable
        Assert-Fixture ($receipt.files.Count -eq 1 -and $receipt.generatedFiles.Count -eq 1 -and $result.FileCount -eq 2) 'Receipt lost files.'
        $sourceLine = "fixture|one.bin|16|$($f.manifest.files[0].sha256)`n"
        Assert-Fixture ($result.SourceSealSha256 -ceq (Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($sourceLine)))) 'Source seal mismatch.'
        $stageLines = "app/ocr/one.bin|16|$($f.manifest.files[0].sha256)`napp/models/uct/NOTICE.txt|9|$(Get-UctBytesHash ([Text.Encoding]::ASCII.GetBytes('synthetic')))`n"
        Assert-Fixture ($result.StagedSealSha256 -ceq (Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($stageLines)))) 'Staged seal mismatch.'
        Assert-Fixture (![Text.Encoding]::UTF8.GetString($receiptBytes).Contains($run)) 'Distributed receipt leaked physical source paths.'
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle } 'Fresh UCT target'
    }
    Invoke-Fixture 'same-length timestamp-preserving source mutation' {
        $f = New-Fixture 'mutation'
        $path = Join-Path $f.source 'one.bin'
        $timestamp = [IO.File]::GetLastWriteTimeUtc($path)
        [IO.File]::WriteAllText($path, 'fedcba9876543210')
        [IO.File]::SetLastWriteTimeUtc($path, $timestamp)
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle } 'hash mismatch'
        Assert-NoSeal $f
    }
    Invoke-Fixture 'CreateNew refuses overwrite, including generated metadata' {
        $f = New-Fixture 'create-new'
        $destination = Join-Path $f.bundle 'existing.bin'
        [IO.File]::WriteAllText($destination, 'preserve me')
        Assert-Refused { Copy-UctPinnedFile (Join-Path $f.source 'one.bin') $destination 16 $f.manifest.files[0].sha256 } 'exists'
        Assert-Refused { Write-UctNewBytes $destination ([Text.Encoding]::ASCII.GetBytes('replace')) } 'exists'
        Assert-Fixture ([IO.File]::ReadAllText($destination) -ceq 'preserve me') 'Existing bytes overwritten.'
    }
    Invoke-Fixture 'containment, unsafe aliases and duplicate case aliases' {
        foreach ($bad in @('../escape','x/../escape','x//file','/absolute','C:/absolute','x\file','x:stream','x/file.',
            'x/file ','CON','nul.txt','x/COM1.dll','x/COM1 .dll','x/CON .txt','x/LPT9.log','x/COM¹.txt','x/CONOUT$','x/[alias]','x/ leading')) {
            Assert-Refused { Assert-UctRelativePath $bad } 'Unsafe'
        }
        foreach ($bad in @('relative','C:relative','\\server\share','\\?\C:\path','C:\safe\..\alias','C:\safe\alias.','C:\safe\x:stream')) {
            Assert-Refused { Get-UctAbsolutePath $bad } 'Unsafe|absolute'
        }
        $f = New-Fixture 'containment'
        Assert-Refused { Join-UctContainedPath $f.source '../escape' } 'Unsafe'
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.source } 'overlap'
        $inside = Join-Path $f.source 'inside'
        New-UctDirectory $inside
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $inside } 'overlap'
        $f.manifest.files += @{root='fixture';source='one.bin';path='app/ocr/ONE.bin';bytes=16;sha256=$f.manifest.files[0].sha256;package='synthetic'}
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle } 'Invalid or unpinned'
    }
    Invoke-Fixture 'ADS, reparse ancestors and non-regular files' {
        $f = New-Fixture 'ads'
        [IO.File]::WriteAllText((Join-Path $f.source 'one.bin:extra'), 'ADS')
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle } 'alternate data stream'
        $f = New-Fixture 'junction'
        $alias = Join-Path (Split-Path $f.source -Parent) 'alias'
        New-Item -ItemType Junction -Path $alias -Target $f.source | Out-Null
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest @{fixture=$alias} $f.bundle } 'reparse'
        $f = New-Fixture 'destination-junction'
        New-Item -ItemType Junction -Path (Join-Path $f.bundle 'app') -Target $f.source | Out-Null
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle } 'reparse'
        $f = New-Fixture 'directory-input'
        New-UctDirectory (Join-Path $f.source 'directory.bin')
        $f.manifest.files[0].source = 'directory.bin'
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle } 'Regular UCT file'
    }
    Invoke-Fixture 'tracked repo/dist fresh staging and per-file repository containment' {
        $f = New-Fixture 'repo-dist'
        $dist = Join-Path $f.source 'dist'
        New-UctDirectory $dist
        $stage = Join-Path $dist ('OkuFlow.staging-' + [Guid]::NewGuid().ToString('N'))
        New-UctDirectory $stage
        $f.manifest.files[0].root = 'repo'
        $result = Invoke-UctPayloadCopy $f.manifest @{repo=$f.source} $stage
        Assert-Fixture ($result.FileCount -eq 1 -and (Test-Path -LiteralPath $result.ReceiptPath)) 'Tracked repo/dist stage must be supported.'
        $f.manifest.files[0].source = 'dist/' + [IO.Path]::GetFileName($stage) + '/app/ocr/one.bin'
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest @{repo=$f.source} $stage } 'repository source overlaps'
        $f.manifest.files[0].source = 'dist/' + [IO.Path]::GetFileName($stage) + '/app/models'
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest @{repo=$f.source} $stage } 'repository source overlaps'
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest @{repo=$f.source} (Split-Path $f.source -Parent) } 'overlap'
        $f = New-Fixture 'repo-source-ancestor'
        $dist = Join-Path $f.source 'dist'
        New-UctDirectory $dist
        $stage = Join-Path $dist ('OkuFlow.staging-' + [Guid]::NewGuid().ToString('N'))
        New-UctDirectory $stage
        [IO.File]::WriteAllText((Join-Path $stage 'app'), '0123456789abcdef')
        $f.manifest.files[0].root = 'repo'
        $f.manifest.files[0].source = 'dist/' + [IO.Path]::GetFileName($stage) + '/app'
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest @{repo=$f.source} $stage } 'ancestor is not a directory|source overlaps'
        Assert-Fixture ([IO.File]::ReadAllText((Join-Path $stage 'app')) -ceq '0123456789abcdef') 'Source ancestor file was changed.'
    }
    Invoke-Fixture 'bounds, incomplete pins and reserved receipt paths' {
        foreach ($variant in @('hash','length','oversize','total','reserved','generated-collision')) {
            $f = New-Fixture "bounds-$variant"
            switch ($variant) {
                'hash' { $f.manifest.files[0].sha256 = $null }
                'length' { $f.manifest.files[0].bytes = 15 }
                'oversize' { $f.manifest.files[0].bytes = 1025 }
                'total' { $f.manifest.maxTotalBytes = 15 }
                'reserved' { $f.manifest.files[0].path = 'app/ocr/payload-receipt.json' }
            }
            if ($variant -eq 'generated-collision') {
                Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle @{'app/ocr/ONE.bin'=[byte[]]@(1)} } 'Invalid UCT generated'
            } else {
                Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle } 'Invalid or unpinned|size mismatch|total size|Reserved'
            }
            Assert-NoSeal $f
        }
    }
    Invoke-Fixture 'mid-stage source mutation preserves earlier copy, refuses restart' {
        $f = New-Fixture 'partial-source'
        [IO.File]::WriteAllText((Join-Path $f.source 'two.bin'), '0123456789abcdef')
        $f.manifest.files += @{root='fixture';source='two.bin';path='app/ocr/two.bin';bytes=16;sha256=$f.manifest.files[0].sha256;package='synthetic'}
        function script:Copy-UctPinnedFile($Source, $Destination, $Bytes, $Sha256) {
            if ([IO.Path]::GetFileName($Source) -ceq 'two.bin') { [IO.File]::WriteAllText($Source, 'fedcba9876543210') }
            & $script:originalCopy $Source $Destination $Bytes $Sha256
        }
        try { Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle } 'source hash mismatch' }
        finally { Set-Item Function:script:Copy-UctPinnedFile -Value $script:originalCopy }
        Assert-Fixture ([IO.File]::ReadAllText((Join-Path $f.bundle 'app/ocr/one.bin')) -ceq '0123456789abcdef') 'Earlier successful copy was lost.'
        Assert-NoSeal $f
        [IO.File]::WriteAllText((Join-Path $f.source 'two.bin'), '0123456789abcdef')
        Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle } 'Fresh UCT target'
    }
    Invoke-Fixture 'staged mutation after copy prevents final seal' {
        $f = New-Fixture 'partial-staged'
        function script:Copy-UctPinnedFile($Source, $Destination, $Bytes, $Sha256) {
            & $script:originalCopy $Source $Destination $Bytes $Sha256
            $timestamp = [IO.File]::GetLastWriteTimeUtc($Destination)
            [IO.File]::WriteAllText($Destination, 'fedcba9876543210')
            [IO.File]::SetLastWriteTimeUtc($Destination, $timestamp)
        }
        try { Assert-Refused { Invoke-UctPayloadCopy $f.manifest $f.roots $f.bundle } 'changed before receipt' }
        finally { Set-Item Function:script:Copy-UctPinnedFile -Value $script:originalCopy }
        Assert-Fixture (Test-Path -LiteralPath (Join-Path $f.bundle 'app/ocr/one.bin')) 'Failed staged file must remain.'
        Assert-NoSeal $f
    }
    if ($watch.Elapsed.TotalSeconds -gt 120) { throw 'UCT fixtures exceeded 120 seconds.' }
    Write-Host "PASS UCT payload: $script:passed model-free cases in $([Math]::Round($watch.Elapsed.TotalSeconds, 2)) seconds. Evidence: $run"
} catch {
    Write-Host "FAIL UCT payload; preserved fixture evidence: $run"
    throw
} finally {
    Set-Item Function:script:Copy-UctPinnedFile -Value $script:originalCopy
}
