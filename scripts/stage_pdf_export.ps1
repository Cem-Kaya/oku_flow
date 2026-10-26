#requires -Version 7.0
[CmdletBinding()]
param([string]$BundlePath, [string]$RuntimeRoot, [switch]$Install, [string]$ArchivePath)
$ErrorActionPreference = 'Stop'
$pinRoot = Join-Path $PSScriptRoot 'qpdf'
$pin = Get-Content (Join-Path $pinRoot 'pins.json') -Raw | ConvertFrom-Json
if (!$RuntimeRoot) { $RuntimeRoot = $env:OKUFLOW_QPDF_ROOT }
if (!$RuntimeRoot) { $RuntimeRoot = Join-Path $env:LOCALAPPDATA "OkuFlow/qpdf/$($pin.version)" }
if ($Install) {
    if (!$ArchivePath) {
        $ArchivePath = Join-Path ([IO.Path]::GetTempPath()) "okuflow-qpdf-$($pin.version).zip"
        if (!(Test-Path -LiteralPath $ArchivePath)) { Invoke-WebRequest $pin.url -OutFile $ArchivePath }
    }
    if ((Get-FileHash -LiteralPath $ArchivePath).Hash.ToLowerInvariant() -cne $pin.sha256) { throw 'PDF export archive hash mismatch.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ArchivePath).Path)
    try {
        foreach ($record in $pin.files) {
            $entry = $archive.GetEntry($record.archivePath)
            if (!$entry) { throw 'PDF export archive member missing.' }
            $target = Join-Path $RuntimeRoot $record.path
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            if (!(Test-Path -LiteralPath $target)) { [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$false) }
        }
        foreach ($record in $pin.notices) {
            $source = Join-Path $pinRoot ([IO.Path]::GetFileName($record.path))
            if ((Get-FileHash $source).Hash.ToLowerInvariant() -cne $record.sha256) { throw 'PDF export notice pin mismatch.' }
            $target = Join-Path $RuntimeRoot $record.path
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            if (!(Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath $source -Destination $target }
        }
    } finally { $archive.Dispose() }
}
foreach ($record in @($pin.files) + @($pin.notices)) {
    $source = Join-Path $RuntimeRoot $record.path
    if (!(Test-Path -LiteralPath $source -PathType Leaf) -or
        (Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -cne $record.sha256) {
        throw 'Pinned PDF export runtime missing or changed. Run scripts/stage_pdf_export.ps1 -Install first.'
    }
    if ($BundlePath) {
        $target = Join-Path $BundlePath "qpdf/$($record.path)"
        if (Test-Path -LiteralPath $target) { throw 'PDF export staging requires a fresh destination.' }
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target
        if ((Get-FileHash $target).Hash.ToLowerInvariant() -cne $record.sha256) { throw 'Staged PDF export hash mismatch.' }
    }
}
Write-Host "Verified qpdf $($pin.version), two runtime files and complete pinned notices. No vendor CRT DLLs copied."
