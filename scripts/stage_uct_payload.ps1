#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BundleRoot,
    [string]$RuntimeRoot,
    [string]$TensorRtRoot,
    [string]$ModelsRoot,
    [string]$CudaRoot
)
$ErrorActionPreference = 'Stop'

# Dot-sourcing defines the model-free primitives without staging anything.
function Assert-UctRelativePath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path.Length -gt 240 -or
        $Path.Contains('\') -or $Path.StartsWith('/')) { throw "Unsafe relative UCT path: $Path" }
    foreach ($part in $Path.Split('/')) {
        $deviceStem = $part.Split('.')[0].TrimEnd(' ')
        if ($part -in @('', '.', '..') -or $part -match '[<>:"|?*\x00-\x1f\[\]]' -or
            $part -match '[. ]$' -or $part -match '^ ' -or
            $deviceStem -match '^(?i:CON|PRN|AUX|NUL|CLOCK\$|CONIN\$|CONOUT\$|COM[0-9¹²³]|LPT[0-9¹²³])$') {
            throw "Unsafe Windows UCT component: $part"
        }
    }
}

function Get-UctAbsolutePath([string]$Path) {
    # Drive-qualified local paths only. Reject aliases before GetFullPath normalizes them.
    if ($Path -notmatch '^[A-Za-z]:[\\/]' -or $Path.Length -gt 220) { throw "Local absolute UCT path required: $Path" }
    $tail = $Path.Substring(3).Replace('\', '/').TrimEnd('/')
    if (!$tail) { throw "UCT input/output cannot be a volume root: $Path" }
    Assert-UctRelativePath $tail
    return [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Assert-UctRegularPath([string]$Path, [switch]$File, [switch]$Directory) {
    # Inspect every existing ancestor, including a source-root junction. Callers
    # must supply its physical target rather than relying on a workstation alias.
    $cursor = $Path
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "UCT reparse point refused: $cursor" }
            if ($cursor -ne $Path -and !$item.PSIsContainer) { throw "UCT ancestor is not a directory: $cursor" }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    if ($File -or $Directory) {
        $item = Get-Item -LiteralPath $Path -Force
        if ($File -and ($item.PSIsContainer -or $item -isnot [IO.FileInfo] -or
            ($item.Attributes -band [IO.FileAttributes]::Device))) { throw "Regular UCT file required: $Path" }
        if ($Directory -and !$item.PSIsContainer) { throw "UCT directory required: $Path" }
        if ($File) {
            $streams = @(Get-Item -LiteralPath $Path -Stream * -ErrorAction Stop)
            if (@($streams | Where-Object Stream -ne ':$DATA').Count) { throw "UCT alternate data stream refused: $Path" }
        }
    }
}

function Join-UctContainedPath([string]$Root, [string]$Relative) {
    Assert-UctRelativePath $Relative
    $path = [IO.Path]::GetFullPath([IO.Path]::Combine($Root, $Relative))
    if (!$path.StartsWith($Root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "UCT path escapes its root: $Relative"
    }
    Assert-UctRegularPath $path
    return $path
}

function Test-UctPathOverlap([string]$Left, [string]$Right) {
    return $Left.Equals($Right, [StringComparison]::OrdinalIgnoreCase) -or
        $Left.StartsWith($Right + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $Right.StartsWith($Left + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Get-UctBytesHash([byte[]]$Bytes) {
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

function Get-UctFileHash([string]$Path, [long]$Bytes, [long]$Limit = 536870912) {
    Assert-UctRegularPath $Path -File
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($Bytes -lt 1 -or $Bytes -gt $Limit -or $stream.Length -ne $Bytes) { throw "UCT file size mismatch: $Path" }
        return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant()
    } finally { $stream.Dispose() }
}

function Read-UctPinnedBytes([string]$Path, [long]$Bytes, [string]$Sha256) {
    Assert-UctRegularPath $Path -File
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($Bytes -lt 1 -or $Bytes -gt 1048576 -or $stream.Length -ne $Bytes) { throw "UCT metadata size mismatch: $Path" }
        $data = [byte[]]::new([int]$Bytes)
        $offset = 0
        while ($offset -lt $data.Length) {
            $count = $stream.Read($data, $offset, $data.Length - $offset)
            if (!$count) { throw "UCT metadata truncated: $Path" }
            $offset += $count
        }
        if ($stream.ReadByte() -ne -1 -or (Get-UctBytesHash $data) -cne $Sha256) { throw "UCT metadata pin changed: $Path" }
        return ,$data
    } finally { $stream.Dispose() }
}

function New-UctDirectory([string]$Path) {
    Assert-UctRegularPath $Path
    if (Test-Path -LiteralPath $Path) { throw "UCT directory already exists; preserve it: $Path" }
    New-Item -ItemType Directory -Path $Path -ErrorAction Stop | Out-Null
    Assert-UctRegularPath $Path -Directory
}

function New-UctParentDirectories([string]$Path, [string]$Root) {
    $parent = [IO.Path]::GetDirectoryName($Path)
    if ($parent -eq $Root) { return }
    if (!(Test-Path -LiteralPath $parent)) {
        New-UctParentDirectories $parent $Root
        New-UctDirectory $parent
    }
    Assert-UctRegularPath $parent -Directory
}

function Copy-UctPinnedFile([string]$Source, [string]$Destination, [long]$Bytes, [string]$Sha256) {
    Assert-UctRegularPath $Source -File
    Assert-UctRegularPath $Destination
    $inputStream = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($Bytes -lt 1 -or $Bytes -gt 536870912 -or $inputStream.Length -ne $Bytes) { throw "UCT source size mismatch: $Source" }
        $before = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($inputStream)).ToLowerInvariant()
        if ($before -cne $Sha256) { throw "UCT source hash mismatch: $Source" }
        $inputStream.Position = 0
        # CreateNew is also the final race/overwrite guard. Never Copy-Item -Force.
        $outputStream = [IO.File]::Open($Destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        try {
            $buffer = [byte[]]::new(1048576)
            [long]$copied = 0
            while (($count = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $copied += $count
                if ($copied -gt $Bytes) { throw "UCT source grew while copying: $Source" }
                $outputStream.Write($buffer, 0, $count)
            }
            $outputStream.Flush($true)
            $outputStream.Position = 0
            $staged = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($outputStream)).ToLowerInvariant()
            $inputStream.Position = 0
            $after = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($inputStream)).ToLowerInvariant()
            if ($copied -ne $Bytes -or $outputStream.Length -ne $Bytes -or $staged -cne $Sha256 -or $after -cne $Sha256) {
                throw "UCT copy verification failed; partial destination preserved: $Destination"
            }
        } finally { $outputStream.Dispose() }
    } finally { $inputStream.Dispose() }
    Assert-UctRegularPath $Source -File
    Assert-UctRegularPath $Destination -File
}

function Write-UctNewBytes([string]$Path, [byte[]]$Bytes) {
    if ($Bytes.Length -lt 1 -or $Bytes.Length -gt 1048576) { throw 'UCT generated metadata exceeds 1 MiB.' }
    Assert-UctRegularPath $Path
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $stream.Write($Bytes, 0, $Bytes.Length)
        $stream.Flush($true)
        $stream.Position = 0
        if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant() -cne (Get-UctBytesHash $Bytes)) {
            throw "UCT generated file changed: $Path"
        }
    } finally { $stream.Dispose() }
    Assert-UctRegularPath $Path -File
}

function Test-UctPayloadCatalog([hashtable]$Manifest, [string]$RepoRoot) {
    # Model-free parity: read only small tracked catalogs/converter, never ONNX or SDK files.
    $expected = @{}
    $snapshots = @{}
    foreach ($relative in @('scripts/uct/pins.json', 'scripts/uct/bf16-models.json', 'scripts/uct/precision_models.py')) {
        $entries = @($Manifest.files | Where-Object { $_.root -ceq 'repo' -and $_.source -ceq $relative })
        if ($entries.Count -ne 1) { throw "UCT catalog inventory mismatch: $relative" }
        $entry = $entries[0]
        $path = Join-UctContainedPath $RepoRoot $relative
        $snapshots[$relative] = Read-UctPinnedBytes $path $entry.bytes $entry.sha256
    }
    $pins = [Text.Encoding]::UTF8.GetString($snapshots['scripts/uct/pins.json']) | ConvertFrom-Json -AsHashtable
    $bf16 = [Text.Encoding]::UTF8.GetString($snapshots['scripts/uct/bf16-models.json']) | ConvertFrom-Json -AsHashtable
    if ($pins.models.Count -ne 6 -or $bf16.models.Count -ne 6) { throw 'UCT requires all six source and BF16 model pins.' }
    foreach ($model in $pins.models) {
        foreach ($name in $model.files.Keys) {
            $relative = "$($model.directory)/$name"
            $expected["app/ocr/$relative"] = @('runtime', $relative, $model.files[$name], 'paddleocr')
        }
    }
    foreach ($model in $bf16.models) {
        $kind = if ($model.role -ceq 'detector') { 'det' } elseif ($model.role -ceq 'recognizer') { 'rec' } else { throw 'Unknown BF16 role.' }
        $source = @($pins.models | Where-Object { $_.kind -ceq $kind -and $_.tier -ceq $model.tier })
        if ($source.Count -ne 1 -or $source[0].files['inference.onnx'] -cne $model.sourceSha256 -or
            $source[0].files['inference.yml'] -cne $model.configSha256 -or $source[0].repository -cne $model.repository -or
            $source[0].revision -cne $model.revision -or $model.license -cne 'Apache-2.0' -or
            $model.bundlePath -cne "uct/$kind/$($model.tier)/bf16-linear/inference.onnx") { throw 'BF16 source/catalog parity failed.' }
        $expected["app/models/$($model.bundlePath)"] = @('models', $model.bundlePath, $model.convertedGraphSha256, 'paddleocr')
    }
    foreach ($path in $expected.Keys) {
        $entries = @($Manifest.files | Where-Object path -CEQ $path)
        $e = $expected[$path]
        if ($entries.Count -ne 1 -or $entries[0].root -cne $e[0] -or $entries[0].source -cne $e[1] -or
            $entries[0].sha256 -cne $e[2] -or $entries[0].package -cne $e[3]) { throw "UCT model distribution parity failed: $path" }
    }
    $converter = @($Manifest.files | Where-Object source -CEQ 'scripts/uct/precision_models.py')[0]
    $ort = @($Manifest.files | Where-Object path -CEQ 'app/ocr/cpu/onnxruntime.dll')[0]
    $trtPackage = @($Manifest.packages | Where-Object id -CEQ 'tensorrt')[0]
    $ortPackage = @($Manifest.packages | Where-Object id -CEQ 'onnxruntime')[0]
    if ($converter.sha256 -cne $bf16.converterSha256 -or $ort.sha256 -cne $pins.ort.dllSha256 -or
        $ortPackage.archiveSha256 -cne $pins.ort.sha256 -or $trtPackage.archiveSha256 -cne $pins.tensorrt.sha256) {
        throw 'UCT converter/runtime archive pin parity failed.'
    }
    return $bf16
}

function Get-UctModificationBytes([hashtable]$Catalog) {
    $lines = @('OkuFlow modifications to the Apache-2.0 PP-OCRv6 model graphs',
        'Original model cards/configurations and MODEL_LICENSES.txt are retained unchanged.',
        "Converter SHA-256: $($Catalog.converterSha256)",
        "Method: $($Catalog.conversion). FLOAT I/O is retained; arithmetic is mixed BF16/FP16/FP32.",
        'These graphs are modified by OkuFlow, not original upstream FP32 models.', '')
    foreach ($m in $Catalog.models) {
        $lines += "$($m.role)/$($m.tier): $($m.repository)@$($m.revision)"
        $lines += "  original SHA-256: $($m.sourceSha256)"
        $lines += "  configuration SHA-256: $($m.configSha256)"
        $lines += "  modified BF16 graph SHA-256: $($m.convertedGraphSha256)"
    }
    return ,([Text.Encoding]::UTF8.GetBytes(($lines -join "`n") + "`n"))
}

function Invoke-UctPayloadCopy([hashtable]$Manifest, [hashtable]$Roots, [string]$Bundle,
    [hashtable]$Generated = @{}, [string]$DistributionSha256 = '') {
    if (!$IsWindows) { throw 'UCT payload staging requires Windows file/stream semantics.' }
    if ($Manifest.schema -ne 1 -or $Manifest.maxFileBytes -lt 1 -or $Manifest.maxFileBytes -gt 536870912 -or
        $Manifest.maxTotalBytes -lt 1 -or $Manifest.maxTotalBytes -gt 2147483648 -or
        @($Manifest.files).Count -lt 1 -or @($Manifest.files).Count -gt 128) { throw 'Invalid bounded UCT inventory.' }
    $bundlePath = Get-UctAbsolutePath $Bundle
    Assert-UctRegularPath $bundlePath -Directory
    $resolvedRoots = @{}
    foreach ($key in $Roots.Keys) {
        $root = Get-UctAbsolutePath $Roots[$key]
        Assert-UctRegularPath $root -Directory
        if (Test-UctPathOverlap $root $bundlePath) {
            # The tracked bundle transaction stages beneath repo/dist. Only the
            # implicit repository root may contain the bundle; selected files
            # must still be outside both owned output subtrees.
            if ($key -cne 'repo' -or !$bundlePath.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw "UCT source/destination overlap: $key"
            }
        }
        $resolvedRoots[$key] = $root
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $packages = @{}
    foreach ($p in $Manifest.packages) {
        if (!$p.id -or !$p.licenseDeclared -or $packages.ContainsKey($p.id)) { throw 'Invalid UCT package/license identity.' }
        $packages[$p.id] = $p.licenseDeclared
    }
    $records = @()
    [long]$total = 0
    foreach ($entry in $Manifest.files) {
        Assert-UctRelativePath $entry.source
        Assert-UctRelativePath $entry.path
        if ($entry.path -cnotmatch '^app/(ocr/|models/uct/)' -or !$seen.Add($entry.path) -or
            !$resolvedRoots.ContainsKey($entry.root) -or !$packages.ContainsKey($entry.package) -or
            $entry.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $null -eq $entry.bytes -or
            [long]$entry.bytes -lt 1 -or [long]$entry.bytes -gt $Manifest.maxFileBytes -or
            [decimal]$entry.bytes -ne [long]$entry.bytes) { throw "Invalid or unpinned UCT inventory entry: $($entry.path)" }
        $total += [long]$entry.bytes
        if ($total -gt $Manifest.maxTotalBytes) { throw 'UCT inventory exceeds total size bound.' }
        $source = Join-UctContainedPath $resolvedRoots[$entry.root] $entry.source
        $destination = Join-UctContainedPath $bundlePath $entry.path
        if ($entry.root -ceq 'repo') {
            foreach ($owned in @('app/ocr', 'app/models')) {
                if (Test-UctPathOverlap $source (Join-UctContainedPath $bundlePath $owned)) {
                    throw "UCT repository source overlaps an owned output subtree: $($entry.source)"
                }
            }
        }
        if (Test-UctPathOverlap $source $destination) { throw "UCT input file/output overlap: $($entry.source)" }
        # Check all sources before creating any owned target.
        if ((Get-UctFileHash $source $entry.bytes $Manifest.maxFileBytes) -cne $entry.sha256) { throw "UCT source hash mismatch: $source" }
        $records += [ordered]@{root=$entry.root; source=$entry.source; path=$entry.path; bytes=[long]$entry.bytes;
            sha256=$entry.sha256; package=$entry.package; licenseDeclared=$packages[$entry.package]}
    }
    foreach ($path in $Generated.Keys) {
        Assert-UctRelativePath $path
        if ($path -cnotmatch '^app/(ocr/|models/uct/)' -or !$seen.Add($path) -or $Generated[$path] -isnot [byte[]] -or
            $Generated[$path].Length -lt 1 -or $Generated[$path].Length -gt 1048576) { throw "Invalid UCT generated entry: $path" }
        $total += $Generated[$path].Length
    }
    if ($total -gt $Manifest.maxTotalBytes) { throw 'UCT inventory exceeds total size bound.' }
    # Receipt paths are reserved, regardless of manifest/generated inputs.
    foreach ($name in @('app/ocr/payload-receipt.json', 'app/ocr/payload-receipt.sha256')) {
        if ($seen.Contains($name)) { throw "Reserved UCT receipt path: $name" }
    }
    $app = Join-UctContainedPath $bundlePath 'app'
    $ocr = Join-UctContainedPath $bundlePath 'app/ocr'
    $models = Join-UctContainedPath $bundlePath 'app/models'
    foreach ($target in @($ocr, $models)) {
        if (Test-Path -LiteralPath $target) { throw "Fresh UCT target required; preserve existing path: $target" }
    }
    if (!(Test-Path -LiteralPath $app)) { New-UctDirectory $app }
    Assert-UctRegularPath $app -Directory
    New-UctDirectory $ocr
    New-UctDirectory $models
    # Any failure leaves this generation, including partial files, for inspection.
    foreach ($record in $records) {
        $source = Join-UctContainedPath $resolvedRoots[$record.root] $record.source
        $destination = Join-UctContainedPath $bundlePath $record.path
        New-UctParentDirectories $destination $app
        Copy-UctPinnedFile $source $destination $record.bytes $record.sha256
    }
    $generatedRecords = @()
    foreach ($path in @($Generated.Keys | Sort-Object -CaseSensitive)) {
        $destination = Join-UctContainedPath $bundlePath $path
        New-UctParentDirectories $destination $app
        Write-UctNewBytes $destination $Generated[$path]
        $generatedRecords += [ordered]@{path=$path; bytes=$Generated[$path].Length; sha256=(Get-UctBytesHash $Generated[$path])}
    }
    # Rehash every selected source and staged byte immediately before sealing.
    foreach ($record in $records) {
        foreach ($path in @((Join-UctContainedPath $resolvedRoots[$record.root] $record.source),
            (Join-UctContainedPath $bundlePath $record.path))) {
            if ((Get-UctFileHash $path $record.bytes) -cne $record.sha256) { throw "UCT file changed before receipt: $path" }
        }
    }
    foreach ($record in $generatedRecords) {
        if ((Get-UctFileHash (Join-UctContainedPath $bundlePath $record.path) $record.bytes 1048576) -cne $record.sha256) {
            throw "UCT generated file changed before receipt: $($record.path)"
        }
    }
    $sourceLines = @($records | ForEach-Object { "$($_.root)|$($_.source)|$($_.bytes)|$($_.sha256)`n" }) -join ''
    $stagedRecords = @($records | ForEach-Object { [ordered]@{path=$_.path; bytes=$_.bytes; sha256=$_.sha256} }) + $generatedRecords
    $stagedLines = @($stagedRecords | ForEach-Object { "$($_.path)|$($_.bytes)|$($_.sha256)`n" }) -join ''
    $receipt = [ordered]@{schema=1; distributionSha256=$DistributionSha256;
        sourceSealSha256=(Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($sourceLines)));
        stagedSealSha256=(Get-UctBytesHash ([Text.Encoding]::UTF8.GetBytes($stagedLines)));
        files=$records; generatedFiles=$generatedRecords; totalBytes=$total}
    $receiptBytes = [Text.Encoding]::UTF8.GetBytes(($receipt | ConvertTo-Json -Depth 12) + "`n")
    $receiptHash = Get-UctBytesHash $receiptBytes
    Write-UctNewBytes (Join-UctContainedPath $bundlePath 'app/ocr/payload-receipt.json') $receiptBytes
    # This final new-only marker commits a successful staging attempt.
    Write-UctNewBytes (Join-UctContainedPath $bundlePath 'app/ocr/payload-receipt.sha256') ([Text.Encoding]::ASCII.GetBytes("$receiptHash`n"))
    return [pscustomobject]@{ReceiptPath=(Join-Path $ocr 'payload-receipt.json'); ReceiptSha256=$receiptHash;
        SourceSealSha256=$receipt.sourceSealSha256; StagedSealSha256=$receipt.stagedSealSha256;
        DistributionSha256=$DistributionSha256; FileCount=$stagedRecords.Count; TotalBytes=$total}
}

function Invoke-UctPayloadStage([string]$Bundle, [string]$Runtime, [string]$TensorRt, [string]$Models, [string]$Cuda) {
    $repo = Get-UctAbsolutePath (Split-Path $PSScriptRoot -Parent)
    $path = Join-UctContainedPath $repo 'scripts/uct/distribution.json'
    Assert-UctRegularPath $path -File
    $length = (Get-Item -LiteralPath $path).Length
    $hash = Get-UctFileHash $path $length 1048576
    $bytes = Read-UctPinnedBytes $path $length $hash
    $manifest = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json -AsHashtable
    $catalog = Test-UctPayloadCatalog $manifest $repo
    $generated = @{'app/ocr/distribution.json'=$bytes; 'app/models/uct/MODIFICATIONS.txt'=(Get-UctModificationBytes $catalog)}
    Invoke-UctPayloadCopy $manifest @{repo=$repo;runtime=$Runtime;tensorrt=$TensorRt;models=$Models;cuda=$Cuda} $Bundle $generated $hash
}

if ($MyInvocation.InvocationName -ne '.') {
    Invoke-UctPayloadStage $BundleRoot $RuntimeRoot $TensorRtRoot $ModelsRoot $CudaRoot
}
