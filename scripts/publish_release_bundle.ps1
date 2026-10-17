#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DistDirectory,
    [Parameter(Mandatory)][string]$StagingDirectory,
    [ValidateSet('Publish','CreateStage','DiscardStage')][string]$Action = 'Publish',
    [string]$ResultPath = ''
)
$ErrorActionPreference = 'Stop'

function Assert-PlainPath([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse path refused: $cursor" }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}
function Get-PlainTree([string]$Root) {
    Assert-PlainPath $Root
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { throw "Bundle directory missing: $Root" }
    $entries = @(Get-ChildItem -LiteralPath $Root -Force -Recurse)
    foreach ($entry in $entries) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Bundle reparse point refused: $($entry.FullName)" }
    }
    foreach ($reserved in @('output','captures')) {
        if (Test-Path -LiteralPath (Join-Path $Root $reserved)) { throw "User data refused; bundle left untouched: $Root/$reserved" }
    }
    return $entries
}
function Assert-NoRunningImage([string]$Root) {
    $prefix = $Root.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    # No process is stopped. File leases below also catch unrecognized images,
    # loaded DLLs and foreign readers that could prevent an all-or-nothing switch.
    foreach ($process in Get-Process) {
        try { $image = $process.Path } catch { continue }
        if ($image -and $image.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Running bundle image refused (PID $($process.Id)); left untouched: $Root"
        }
    }
}
function Open-VerifiedBundle([string]$Root) {
    $leases = [Collections.Generic.List[IO.FileStream]]::new()
    try {
        $entries = @(Get-PlainTree $Root)
        Assert-NoRunningImage $Root
        $files = @{}
        foreach ($entry in $entries | Where-Object { -not $_.PSIsContainer }) {
            $relative = [IO.Path]::GetRelativePath($Root,$entry.FullName).Replace('\','/')
            # Deny other readers/writers while verifying the complete inventory.
            # Windows requires these leases to close before directory rename.
            $stream = [IO.File]::Open($entry.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Delete)
            $leases.Add($stream)
            $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant()
            $stream.Position = 0
            $files[$relative] = @{stream=$stream;bytes=$stream.Length;sha256=$hash}
        }
        foreach ($name in @('release-manifest.json','SHA256SUMS.txt','SBOM.spdx.json')) {
            if (-not $files.ContainsKey($name)) { throw "Bundle ownership metadata missing: $Root/$name" }
        }
        $reader = [IO.StreamReader]::new($files['release-manifest.json'].stream,[Text.Encoding]::UTF8,$true,4096,$true)
        try { $manifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if ($manifest.schemaVersion -ne 2 -or $manifest.product -ne 'OkuFlow' -or -not $manifest.files) {
            throw "Unrecognized bundle ownership manifest: $Root"
        }
        $declared = @{}
        foreach ($record in $manifest.files) {
            $relative = [string]$record.path
            if ($relative -notmatch '^[^\\:]+$' -or $relative.StartsWith('/') -or
                @($relative.Split('/') | Where-Object { $_ -in @('','.','..') -or $_ -match '[. ]$' }).Count -or
                $relative -in @('release-manifest.json','SHA256SUMS.txt','SBOM.spdx.json') -or
                $declared.ContainsKey($relative)) { throw "Unsafe or duplicate bundle inventory path: $relative" }
            if (-not $files.ContainsKey($relative) -or $files[$relative].bytes -ne $record.bytes -or
                $files[$relative].sha256 -cne $record.sha256) { throw "Bundle inventory bytes differ: $Root/$relative" }
            $declared[$relative] = $true
        }
        if (-not $declared.ContainsKey('oku_flow.exe') -or
            (-not $declared.ContainsKey('app/OkuFlowApp.exe') -and -not $declared.ContainsKey('OkuFlowApp.exe')) -or
            $files.Count -ne $declared.Count + 3) { throw "Bundle has missing runtime or unowned files: $Root" }
        # Unlisted empty directories can contain user intent too; never move them.
        foreach ($directory in $entries | Where-Object PSIsContainer) {
            $relative = [IO.Path]::GetRelativePath($Root,$directory.FullName).Replace('\','/') + '/'
            if (-not @($declared.Keys | Where-Object { $_.StartsWith($relative,[StringComparison]::OrdinalIgnoreCase) }).Count) {
                throw "Unowned bundle directory refused: $($directory.FullName)"
            }
        }
        return @{Root=$Root;Leases=$leases;Files=$files}
    } catch {
        foreach ($stream in $leases) { $stream.Dispose() }
        throw
    }
}
function Close-Bundle($Bundle) {
    if ($Bundle) { foreach ($stream in $Bundle.Leases) { $stream.Dispose() } }
}
function Move-BundleDirectory([string]$Source,[string]$Destination) {
    # Directory.Move is a same-volume rename; never merge/copy as a fallback.
    if ([IO.Path]::GetDirectoryName($Source) -ne [IO.Path]::GetDirectoryName($Destination)) { throw 'Cross-directory bundle switch refused.' }
    [IO.Directory]::Move($Source,$Destination)
}
function Assert-RenamedBundle($Bundle,[string]$Root) {
    $entries = @(Get-PlainTree $Root)
    if (@($entries | Where-Object { -not $_.PSIsContainer }).Count -ne $Bundle.Files.Count) { throw 'Bundle inventory changed during the exchange.' }
    foreach ($directory in $entries | Where-Object PSIsContainer) {
        $relative = [IO.Path]::GetRelativePath($Root,$directory.FullName).Replace('\','/') + '/'
        if (-not @($Bundle.Files.Keys | Where-Object { $_.StartsWith($relative,[StringComparison]::OrdinalIgnoreCase) }).Count) {
            throw "Unowned directory appeared during the exchange: $($directory.FullName)"
        }
    }
    foreach ($relative in $Bundle.Files.Keys) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $relative) -PathType Leaf)) { throw "Published file missing: $relative" }
        if ((Get-FileHash -LiteralPath (Join-Path $Root $relative) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Bundle.Files[$relative].sha256) {
            throw "Published bytes changed: $relative"
        }
    }
    Assert-PlainPath $Root
}
function Switch-Bundle($New,[string]$Destination,$Old) {
    $backup = Join-Path $script:dist ([IO.Path]::GetFileName($Destination) + '.previous-' + [Guid]::NewGuid().ToString('N'))
    $oldMoved = $false
    $newMoved = $false
    # Directory.Move fails without changing the source if a Windows image/DLL
    # is locked. Never delete files to work around that refusal.
    Close-Bundle $New
    Close-Bundle $Old
    try {
        Assert-RenamedBundle $New $script:stage
        if ($Old) { Assert-RenamedBundle $Old $Destination; Assert-NoRunningImage $Destination }
        if ($Old) { Move-BundleDirectory $Destination $backup; $oldMoved = $true }
        Move-BundleDirectory $script:stage $Destination
        $newMoved = $true
        Assert-RenamedBundle $New $Destination
    } catch {
        $failure = $_
        try {
            if ($newMoved) { Move-BundleDirectory $Destination $script:stage }
            if ($oldMoved) { Move-BundleDirectory $backup $Destination }
        } catch { throw "Bundle switch failed; rollback also failed. Recoverable generations retained at '$backup', '$Destination' and '$script:stage'. $failure ; $_" }
        throw $failure
    }
    # Retain the complete preceding generation. Recursive cleanup can itself
    # become partial if a late foreign reader starts; publication never needs it.
    if ($Old) { Write-Verbose "Previous complete generation retained at '$backup'." }
}

$dist = [IO.Path]::GetFullPath($DistDirectory).TrimEnd('\','/')
$stage = [IO.Path]::GetFullPath($StagingDirectory).TrimEnd('\','/')
if ([IO.Path]::GetDirectoryName($stage) -ne $dist -or
    [IO.Path]::GetFileName($stage) -notmatch '^OkuFlow\.staging-[a-f0-9]{32}$') {
    throw 'Staging must be a uniquely owned OkuFlow.staging-<32-hex-guid> sibling under the exact dist directory.'
}
Assert-PlainPath $dist
if ($Action -eq 'CreateStage') {
    if (Test-Path -LiteralPath $stage) { throw "Refusing a staging name already in use: $stage" }
    $null = [IO.Directory]::CreateDirectory($dist)
    Assert-PlainPath $dist
    $null = New-Item -ItemType Directory -Path $stage -ErrorAction Stop
    return
}
if ($Action -eq 'DiscardStage') {
    if (Test-Path -LiteralPath $stage) {
        $null = Get-PlainTree $stage
        Assert-NoRunningImage $stage
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
    return
}
$mutexName = 'Local\OkuFlow.BundlePublish.' + [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($dist.ToUpperInvariant())))
$mutex = [Threading.Mutex]::new($false,$mutexName)
$acquired = $false
$new = $null
$old = $null
try {
    try { $acquired = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $acquired = $true }
    if (-not $acquired) { throw 'Another bundle publisher owns this dist directory.' }
    $new = Open-VerifiedBundle $stage
    $destination = Join-Path $dist 'OkuFlow'
    if (Test-Path -LiteralPath $destination) {
        try { $old = Open-VerifiedBundle $destination }
        catch {
            Write-Warning "Primary bundle left untouched: $_"
            $destination = Join-Path $dist 'OkuFlow2'
            if (Test-Path -LiteralPath $destination) { $old = Open-VerifiedBundle $destination }
        }
    }
    Switch-Bundle $new $destination $old
    if ($ResultPath) { [IO.File]::WriteAllText($ResultPath,$destination,[Text.UTF8Encoding]::new($false)) }
    Write-Output $destination
} finally {
    Close-Bundle $old
    Close-Bundle $new
    if ($acquired) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
