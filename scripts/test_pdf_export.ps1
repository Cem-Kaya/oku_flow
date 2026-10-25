#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'windows-installer/pdf_bundle.ps1')
$temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$owned = Join-Path $temporary ('okuflow-pdf-test-' + [guid]::NewGuid().ToString('N'))
function Assert-Refused([scriptblock]$Action) {
    $refused = $false
    try { & $Action | Out-Null } catch { $refused = $true }
    if (!$refused) { throw 'Expected the changed PDF runtime inventory to be refused.' }
}
try {
    New-Item -ItemType Directory -Path $owned | Out-Null
    & (Join-Path $PSScriptRoot 'stage_pdf_export.ps1') -BundlePath $owned
    $info = Get-OkuFlowPdfBundleMetadata -PayloadRoot $owned -RepositoryRoot $repository -Prefix 'app/'
    if ($info.Packages.Count -ne 4 -or $info.Files.Count -ne 7 -or
        @($info.Files | Where-Object fileName -notlike './app/qpdf/*').Count -or
        @($info.Relationships | Where-Object relationshipType -eq 'STATIC_LINK').Count -ne 3) { throw 'PDF package/SPDX inventory is incomplete.' }
    $notice = Join-Path $owned 'qpdf/licenses/NOTICE.md'
    $bytes = [IO.File]::ReadAllBytes($notice)
    [IO.File]::AppendAllText($notice,'mutation')
    Assert-Refused { Get-OkuFlowPdfBundleMetadata -PayloadRoot $owned -RepositoryRoot $repository -Prefix 'app/' }
    [IO.File]::WriteAllBytes($notice,$bytes)
    $extra = Join-Path $owned 'qpdf/bin/unlisted.dll'
    [IO.File]::WriteAllText($extra,'unlisted')
    Assert-Refused { Get-OkuFlowPdfBundleMetadata -PayloadRoot $owned -RepositoryRoot $repository -Prefix 'app/' }
    Remove-Item -LiteralPath $extra
    [IO.File]::AppendAllText((Join-Path $owned 'qpdf/bin/qpdf.exe'),'mutation')
    Assert-Refused { Get-OkuFlowPdfBundleMetadata -PayloadRoot $owned -RepositoryRoot $repository -Prefix 'app/' }
    Write-Host 'PASS: pinned PDF runtime staging, SPDX membership, notice/binary tampering and unexpected-file refusal.'
} finally {
    $resolved = [IO.Path]::GetFullPath($owned)
    if (!$resolved.StartsWith($temporary.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^okuflow-pdf-test-[a-f0-9]{32}$') { throw 'Unsafe private fixture cleanup path.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
