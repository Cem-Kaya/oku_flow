# Execute only files extracted from the reviewed package, never a NuGet shim
# resolved from ambient feeds or a previously installed global/local tool.
function Assert-VerifiedWixTree($Tool) {
    if ($Tool.Root -and (Get-Item -LiteralPath $Tool.Root).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'WiX tool root must not be a reparse point.'
    }
    if ((Get-FileHash -LiteralPath $Tool.Archive -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Tool.Sha256) {
        throw 'WiX package differs from the reviewed pin.'
    }
    $zip = [IO.Compression.ZipFile]::OpenRead($Tool.Archive)
    try {
        $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName
            if ($name -match '(^/|\\|:|(^|/)\.\.?(/|$))') { throw "Unsafe WiX package entry: $name" }
            if ($name.EndsWith('/')) { continue }
            if (-not $expected.Add($name)) { throw "Duplicate WiX package entry: $name" }
            if (-not $Tool.Root) { continue } # Validate paths before extraction, too.
            $file = Get-Item -LiteralPath (Join-Path $Tool.Root $name)
            if ($file.PSIsContainer -or $file.Attributes -band [IO.FileAttributes]::ReparsePoint -or $file.Length -ne $entry.Length) {
                throw "Invalid WiX tool file: $name"
            }
            $stream = $entry.Open()
            try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
            finally { $stream.Dispose() }
            if ((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -cne $hash) {
                throw "WiX tool differs from the reviewed package: $name"
            }
        }
        if ($Tool.Root) {
            foreach ($file in Get-ChildItem -LiteralPath $Tool.Root -Recurse -Force) {
                if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'WiX tool tree contains a reparse point.' }
                if (-not $file.PSIsContainer -and -not $expected.Contains([IO.Path]::GetRelativePath($Tool.Root, $file.FullName).Replace('\','/'))) {
                    throw "Unexpected file in verified WiX tool tree: $($file.FullName)"
                }
            }
        }
    } finally { $zip.Dispose() }
}

function Get-VerifiedWixTool($Pin, [string]$ToolDirectory) {
    if ($Pin.version -notmatch '^\d+\.\d+\.\d+$' -or $Pin.sha256 -cnotmatch '^[a-f0-9]{64}$') {
        throw 'Invalid WiX package pin.'
    }
    New-Item -ItemType Directory -Path $ToolDirectory -Force | Out-Null
    $archive = Join-Path $ToolDirectory "wix.$($Pin.version).nupkg"
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        $download = Join-Path $ToolDirectory ('download-' + [Guid]::NewGuid().ToString('N') + '.nupkg')
        Invoke-WebRequest $Pin.url -OutFile $download
        if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Pin.sha256) {
            throw "Downloaded WiX package differs from the reviewed pin; retained: $download"
        }
        [IO.File]::Move($download, $archive)
    }
    $tool = [pscustomobject]@{Archive=$archive;Sha256=[string]$Pin.sha256;Version=[string]$Pin.version;Root='';Executable=''}
    Assert-VerifiedWixTree $tool
    $root = Join-Path $ToolDirectory ('verified-' + $Pin.sha256)
    if (-not (Test-Path -LiteralPath $root)) {
        # A unique staging directory prevents another resolver from observing
        # a half-extracted tree. Never repair a corrupt cached generation silently.
        $staging = Join-Path $ToolDirectory ('extract-' + [Guid]::NewGuid().ToString('N'))
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, $staging)
        $tool.Root = $staging
        Assert-VerifiedWixTree $tool
        try { [IO.Directory]::Move($staging, $root) }
        catch { if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw } }
    }
    $tool.Root = $root
    $tool.Executable = Join-Path $root 'tools/net6.0/any/wix.exe'
    Assert-VerifiedWixTree $tool
    if (-not (Test-Path -LiteralPath $tool.Executable -PathType Leaf)) { throw 'Reviewed WiX package lacks its expected apphost.' }
    return $tool
}

function Invoke-VerifiedWix($Tool, [string[]]$Arguments) {
    Assert-VerifiedWixTree $Tool
    & $Tool.Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Verified WiX failed with exit code $LASTEXITCODE." }
}
