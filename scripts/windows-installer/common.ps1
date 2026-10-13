# First-party packaging helpers. Payloads and build tools stay outside Drive.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'bundle_layout.ps1')

function Get-InstallerStableId([string]$Value) {
    $hash = [Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($Value.ToLowerInvariant()))
    [Convert]::ToHexString($hash).ToLowerInvariant()
}

function Get-InstallerStableGuid([string]$Value) {
    $hex = (Get-InstallerStableId $Value).Substring(0,32)
    # Custom UUIDv8 layout; never reuse between architectures.
    $hex = $hex.Substring(0,12) + '8' + $hex.Substring(13,3) + '8' + $hex.Substring(17)
    ([Guid]::ParseExact($hex,'N')).ToString('B').ToUpperInvariant()
}

function New-VerifiedBundleSnapshot([string]$BundlePath, [string]$Destination) {
    $bundle = (Resolve-Path -LiteralPath $BundlePath).Path
    $manifest = Get-Content -LiteralPath (Join-Path $bundle 'release-manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.schemaVersion -ne 2 -or $manifest.sourceTreeDirty -or $manifest.sourceCommit -notmatch '^[a-f0-9]{40}$') {
        throw 'Installer requires a clean, tracked release bundle with schema-2 provenance.'
    }
    if (Test-Path -LiteralPath $Destination) { throw 'Snapshot destination must be a fresh private directory.' }
    New-Item -ItemType Directory -Path $Destination | Out-Null
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $records = @($manifest.files)
    foreach ($name in @('SHA256SUMS.txt','release-manifest.json','SBOM.spdx.json')) {
        $file = Get-Item -LiteralPath (Join-Path $bundle $name)
        $records += [pscustomobject]@{path=$name;bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()}
    }
    foreach ($record in $records) {
        $relative = [string]$record.path
        if ($relative -match '(^/|\\|:|(^|/)\.\.?(/|$)|(^|/)(output|captures|android-signing)(/|$)|\.(jks|pfx|p12|key)$)' -or
            -not $paths.Add($relative)) { throw "Unsafe or duplicate bundle path: $relative" }
        $source = Get-Item -LiteralPath (Join-Path $bundle $relative)
        if ($source.Attributes -band [IO.FileAttributes]::ReparsePoint -or $source.Length -ne $record.bytes -or
            (Get-FileHash -LiteralPath $source.FullName).Hash.ToLowerInvariant() -ne $record.sha256) {
            throw "Bundle inventory mismatch: $relative"
        }
        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
        Copy-Item -LiteralPath $source.FullName -Destination $target
        if ((Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant() -ne $record.sha256) { throw "Snapshot changed: $relative" }
    }
    $layout = Get-OkuFlowBundleLayout $Destination
    foreach ($required in @('oku_flow.exe', $layout.ApplicationRelativePath,
            ($layout.Prefix + 'platform-tools/adb.exe'),
            ($layout.Prefix + 'sources/adb-37.0.0-okuflow-source.zip'),
            ($layout.Prefix + 'licenses/MICROSOFT_VC_RUNTIME_LICENSE.rtf'))) {
        if (-not $paths.Contains($required)) { throw "Incomplete bundle: $required" }
    }
    [pscustomobject]@{manifest=$manifest;records=$records;path=$Destination;layout=$layout}
}

function Get-WindowsSdkTool([string]$Name) {
    $root = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $path = Get-ChildItem -LiteralPath $root -Directory | Where-Object {$_.Name -match '^10\.'} |
        Sort-Object {[version]$_.Name} -Descending | ForEach-Object {Join-Path $_.FullName "x64\$Name"} |
        Where-Object {Test-Path -LiteralPath $_ -PathType Leaf} | Select-Object -First 1
    if (-not $path) { throw "Windows SDK x64 tool not found: $Name" }
    $path
}

function Set-InstallerSignature([string]$Path, [switch]$AllowUnsignedPreview) {
    if (-not $env:OKUFLOW_SIGN_CERT_SHA1) {
        if (-not $AllowUnsignedPreview) { throw 'A trusted code-signing certificate is required. Use -AllowUnsignedPreview only for an explicitly unsigned preview.' }
        Write-Warning 'UNSIGNED PREVIEW: SmartScreen trust is not established.'
        return 'NotSigned'
    }
    $tool = Get-WindowsSdkTool 'signtool.exe'
    & $tool sign /sha1 $env:OKUFLOW_SIGN_CERT_SHA1 /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $Path
    if ($LASTEXITCODE -ne 0) { throw 'Installer signing failed.' }
    & $tool verify /pa $Path
    if ($LASTEXITCODE -ne 0) { throw 'Installer signature verification failed.' }
    'Valid'
}

function Read-InstallerTable($Database, [string]$Query) {
    $view = $Database.OpenView($Query)
    try {
        [void]$view.Execute()
        while ($record = $view.Fetch()) {
            $count = $record.GetType().InvokeMember('FieldCount',[Reflection.BindingFlags]::GetProperty,$null,$record,$null)
            $cells = for($column=1;$column -le $count;$column++) {
                $record.GetType().InvokeMember('StringData',[Reflection.BindingFlags]::GetProperty,$null,$record,@($column))
            }
            ,@($cells)
        }
    } finally { [void]$view.Close() }
}
