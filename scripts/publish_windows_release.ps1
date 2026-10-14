#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [Parameter(Mandatory)][string]$NotesFile,
    [string]$PreviousInstallerPath,
    [string]$OutputDirectory,
    [switch]$AllowUnsignedPreview,
    [switch]$Publish
)
. (Join-Path $PSScriptRoot 'windows-installer/common.ps1')
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$notes = (Resolve-Path -LiteralPath $NotesFile).Path
$tag = "v$Version"
$parsedVersion = [version]$Version
if ($parsedVersion.Major -gt 255 -or $parsedVersion.Minor -gt 255 -or $parsedVersion.Build -gt 65535) { throw 'Version exceeds MSI field limits.' }
if ($env:OKUFLOW_AGENT_EXCLUDE_GUI -eq '1' -or $env:OKUFLOW_SKIP_BUNDLE_TESTS -eq '1') { throw 'Release automation refuses excluded GUI or bypassed bundle tests.' }
if ($env:OKUFLOW_ENABLE_CUDA -and $env:OKUFLOW_ENABLE_CUDA -ne 'ON') { throw 'Shipping releases require CUDA enabled.' }
if (-not $AllowUnsignedPreview -and -not $env:OKUFLOW_SIGN_CERT_SHA1) { throw 'Supply a trusted signing identity or explicitly choose -AllowUnsignedPreview.' }
if ($Publish -and -not $PreviousInstallerPath) { throw 'Publication requires a previous installer for disposable-VM upgrade/uninstall acceptance.' }
if ($PreviousInstallerPath) { $PreviousInstallerPath = (Resolve-Path -LiteralPath $PreviousInstallerPath).Path }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $env:LOCALAPPDATA "OkuFlow/releases/$Version" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh output directory; existing release artifacts are never overwritten.' }
function Assert-CleanSource {
    $commit = & git -C $repository rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source commit.' }
    $status = @(& git -C $repository status --porcelain --untracked-files=normal)
    if ($LASTEXITCODE -ne 0 -or $status.Count) { throw 'Release automation requires a clean source checkout.' }
    if ($script:sourceCommit -and $commit -ne $script:sourceCommit) { throw 'Source commit changed during release.' }
    $commit
}
$script:sourceCommit = $null
$script:sourceCommit = Assert-CleanSource
$remoteTag = @(& git -C $repository ls-remote --tags origin "refs/tags/$tag")
if ($LASTEXITCODE -ne 0) { throw 'Cannot check remote version immutability.' }
if ($remoteTag.Count) { throw "Remote $tag already exists; choose a new version." }
if ($Publish) {
    & gh auth status
    if ($LASTEXITCODE -ne 0) { throw 'GitHub CLI authentication is required.' }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsDevCmd = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find '**/VsDevCmd.bat'
if ($LASTEXITCODE -ne 0 -or -not $vsDevCmd) { throw 'Visual Studio 2022 C++ tools are required.' }
$vsDevCmd = @($vsDevCmd)[0]
$work = Join-Path $env:LOCALAPPDATA "OkuFlow/release-runs/$Version-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work -Force | Out-Null
$started = [DateTime]::UtcNow
Push-Location $repository
try {
    & (Join-Path $PSScriptRoot 'agent_build.bat') 2>&1 | Tee-Object -FilePath (Join-Path $work 'matrix.log')
    if ($LASTEXITCODE -ne 0) { throw 'Shipping/CPU/CUDA matrix failed; no release published.' }
    [void](Assert-CleanSource)
    $bundleScript = Join-Path $PSScriptRoot 'build_release_bundle.bat'
    # The tracked batch owns staging and its locked-bundle fallback. Only tool
    # invocations cross into cmd; no generated filesystem commands are used.
    & $env:ComSpec /d /s /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && call `"$bundleScript`"" 2>&1 |
        Tee-Object -FilePath (Join-Path $work 'bundle.log')
    if ($LASTEXITCODE -ne 0) { throw 'Shipping bundle gate/deployment failed; no release published.' }
    [void](Assert-CleanSource)
    $bundle = @('OkuFlow', 'OkuFlow2') | ForEach-Object {
        $path = Join-Path $repository "dist/$_"
        $manifestPath = Join-Path $path 'release-manifest.json'
        if (Test-Path -LiteralPath $manifestPath) {
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            if ($manifest.sourceCommit -eq $sourceCommit -and -not $manifest.sourceTreeDirty -and
                [DateTime]::Parse($manifest.generatedUtc).ToUniversalTime() -ge $started) { $path }
        }
    } | Select-Object -Last 1
    if (-not $bundle) { throw 'No freshly tested, clean bundle matches this source commit.' }
    $snapshot = New-VerifiedBundleSnapshot $bundle (Join-Path $work 'payload')
    New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
    & (Join-Path $PSScriptRoot 'build_msi.ps1') -Version $Version -BundlePath $snapshot.path -OutputDirectory $OutputDirectory -AllowUnsignedPreview:$AllowUnsignedPreview
    $msi = Join-Path $OutputDirectory "OkuFlow-$Version-x64.msi"
    $installerAcceptance = 'MSI tables and every extracted payload file verified; clean-VM installation is not established.'
    if ($PreviousInstallerPath) {
        & (Join-Path $PSScriptRoot 'test_msi_sandbox.ps1') -InstallerPath $msi -PreviousInstallerPath $PreviousInstallerPath -BundlePath $snapshot.path 2>&1 |
            Tee-Object -FilePath (Join-Path $work 'installer-vm.log')
        $installerAcceptance = 'Disposable Windows VM install, upgrade, downgrade rejection, shortcuts, uninstall and user-data preservation passed.'
    }
    $zip = Join-Path $OutputDirectory "OkuFlow-$Version-windows-x64.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory($snapshot.path, $zip)
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entries = @($archive.Entries | Where-Object { $_.Name })
        if ($entries.Count -ne $snapshot.records.Count) { throw 'ZIP payload count differs from the verified bundle.' }
        foreach ($record in $snapshot.records) {
            $entry = $archive.GetEntry($record.path)
            if (-not $entry -or $entry.Length -ne $record.bytes) { throw "Missing/changed ZIP entry: $($record.path)" }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = [Convert]::ToHexString($sha.ComputeHash($stream)).ToLowerInvariant() }
            finally { $sha.Dispose(); $stream.Dispose() }
            if ($hash -ne $record.sha256) { throw "ZIP hash mismatch: $($record.path)" }
        }
    } finally { $archive.Dispose() }
    $assets = @($msi, $zip)
    $checksums = Join-Path $OutputDirectory 'SHA256SUMS.txt'
    $assets | ForEach-Object { '{0} *{1}' -f (Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant(), (Split-Path $_ -Leaf) } |
        Set-Content -LiteralPath $checksums -Encoding utf8NoBOM
    $title = "OkuFlow $Version"
    $publishedNotes = Join-Path $work 'release-notes.md'
    $trust = if ($AllowUnsignedPreview) { $title += ' (unsigned preview)'; 'Unsigned preview: Windows SmartScreen may warn about an unrecognized publisher.' }
        else { 'Application executables and MSI use the configured trusted signing identity.' }
    @((Get-Content -LiteralPath $notes -Raw), '', $trust, '', "Source: $sourceCommit", '',
        'Shipping compilation and complete CPU, CUDA and shipping-bundle CTest gates passed.', '', $installerAcceptance) -join "`n" |
        Set-Content -LiteralPath $publishedNotes -Encoding utf8NoBOM
    [void](Assert-CleanSource)
    $receipt = [ordered]@{version=$Version;sourceCommit=$sourceCommit;bundle=$bundle;workDirectory=$work;installerAcceptance=$installerAcceptance;published=$false}
    if ($Publish) {
        & gh release create $tag @assets $checksums --repo Cem-Kaya/oku_flow --target $sourceCommit --draft --title $title --notes-file $publishedNotes
        if ($LASTEXITCODE -ne 0) { throw 'Draft creation/upload failed; do not replace any existing assets.' }
        $download = Join-Path $work 'download-verification'
        & gh release download $tag --repo Cem-Kaya/oku_flow --dir $download --pattern '*.msi' --pattern '*.zip' --pattern SHA256SUMS.txt
        if ($LASTEXITCODE -ne 0) { throw 'Draft assets could not be verified; release remains a draft.' }
        foreach ($asset in @($assets) + @($checksums)) {
            if ((Get-FileHash -LiteralPath $asset).Hash -ne (Get-FileHash -LiteralPath (Join-Path $download (Split-Path $asset -Leaf))).Hash) { throw 'Uploaded asset hash differs; release remains a draft.' }
        }
        & gh release edit $tag --repo Cem-Kaya/oku_flow --draft=false --latest
        if ($LASTEXITCODE -ne 0) { throw 'Publishing failed; inspect the existing draft before retrying.' }
        $receipt.published = $true
    }
    $receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'publication.json') -Encoding utf8NoBOM
    Write-Output "Release ready: $OutputDirectory; published=$($receipt.published). Logs: $work"
} finally { Pop-Location }
