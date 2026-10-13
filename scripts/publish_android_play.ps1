#requires -Version 7.0
[CmdletBinding(DefaultParameterSetName='Check')]
param(
    [Parameter(ParameterSetName='Check')][switch]$CheckAccess,
    [Parameter(Mandatory,ParameterSetName='Release')][string]$BundlePath,
    [Parameter(Mandatory,ParameterSetName='Release')][ValidateRange(1,2100000000)][int]$VersionCode,
    [Parameter(Mandatory,ParameterSetName='Release')][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedSha256,
    [Parameter(Mandatory,ParameterSetName='Release')][ValidateRange(1,2100000000)][int]$ExpectedActiveVersionCode,
    [Parameter(Mandatory,ParameterSetName='Release')][string]$ReleaseName,
    [Parameter(Mandatory,ParameterSetName='Release')][string]$ReleaseNotesPath,
    [Parameter(ParameterSetName='Release')][switch]$Publish,
    [ValidatePattern('^[a-zA-Z0-9_.:-]+$')][string]$Track='alpha',
    [ValidatePattern('^[a-zA-Z0-9_]+(?:\.[a-zA-Z0-9_]+)+$')][string]$PackageName='com.okuflow',
    [string]$Profile=$env:OKUFLOW_ANDROID_PLAY_PROFILE,
    [string]$EvidenceDirectory=(Join-Path $env:LOCALAPPDATA 'OkuFlow\google-play\release-evidence')
)
. (Join-Path $PSScriptRoot 'google-play\publisher_helpers.ps1')
if ($Track -eq 'production' -or $Track.EndsWith(':production')) {
    throw 'This helper publishes testing tracks only; production is not authorized.'
}
$evidence=[IO.Path]::GetFullPath($EvidenceDirectory)
$privateRoot=[IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'OkuFlow'))+'\'
if (-not $evidence.StartsWith($privateRoot,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Publishing evidence must stay under LocalAppData\OkuFlow.'
}
$checkDirectory=$evidence
while ($checkDirectory -and (Test-Path -LiteralPath $checkDirectory)) {
    if ((Get-Item -LiteralPath $checkDirectory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Publishing evidence cannot use redirected/reparse directories.'
    }
    $checkDirectory=Split-Path $checkDirectory
}
$bundle='';$notes=@();$hash=''
if ($PSCmdlet.ParameterSetName -eq 'Release') {
    $bundle=(Resolve-Path -LiteralPath $BundlePath).Path
    if ([IO.Path]::GetExtension($bundle) -ne '.aab') { throw 'Publish a signed Android App Bundle (.aab).' }
    $hash=(Get-FileHash -LiteralPath $bundle -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $ExpectedSha256.ToLowerInvariant()) { throw 'The reviewed bundle SHA-256 differs.' }
    if ($VersionCode -le $ExpectedActiveVersionCode) { throw 'The new version must exceed the current release.' }
    if ($ReleaseName.Length -gt 50) { throw 'Release name exceeds 50 characters.' }
    $notes=@(Get-Content -LiteralPath $ReleaseNotesPath -Raw | ConvertFrom-Json)
    if ($notes.Count -eq 0) { throw 'Release notes are required.' }
    foreach ($note in $notes) {
        if ($note.language -notmatch '^[a-z]{2,3}(?:-[a-zA-Z0-9]+)*$' -or
            -not $note.text -or $note.text.Length -gt 500) { throw 'Invalid localized release notes.' }
    }
}
$credential=Read-AndroidPlayCredential $Profile
$token=New-AndroidPlayAccessToken $credential
$credential=$null
$base='/androidpublisher/v3/applications/'+[Uri]::EscapeDataString($PackageName)+'/edits'
$edit=$null;$committed=$false
try {
    $edit=Invoke-AndroidPlayRequest 'POST' $base $token @{}
    $editPath=$base+'/'+[Uri]::EscapeDataString($edit.id)
    $trackPath=$editPath+'/tracks/'+[Uri]::EscapeDataString($Track)
    $current=Invoke-AndroidPlayRequest 'GET' $trackPath $token
    $active=@($current.releases | Where-Object {$_.status -in @('completed','inProgress')} |
        ForEach-Object {$_.versionCodes} | ForEach-Object {[int]$_})
    Write-Output "Play access verified: $PackageName / $Track; active codes: $($active -join ', ')"
    if ($PSCmdlet.ParameterSetName -eq 'Check') { return }
    if ($active.Count -eq 0 -or ($active | Measure-Object -Maximum).Maximum -ne $ExpectedActiveVersionCode) {
        throw 'Active track changed or is empty; refusing to replace an unexpected release.'
    }
    $inventory=Invoke-AndroidPlayRequest 'GET' ($editPath+'/bundles') $token
    $existing=@($inventory.bundles | Where-Object {[int]$_.versionCode -eq $VersionCode})
    if ($existing.Count -gt 1) { throw 'Duplicate bundle version in Google response.' }
    if ($existing.Count -eq 1) {
        $artifact=$existing[0]
        if ($artifact.sha256.ToLowerInvariant() -ne $hash) { throw 'Existing Play bundle has a different SHA-256.' }
        Write-Output 'Reusing the exact bundle already uploaded to Google Play.'
    } else {
        $artifact=Invoke-AndroidPlayRequest 'POST' ($editPath+'/bundles?uploadType=media') $token $null $bundle
    }
    if ([int]$artifact.versionCode -ne $VersionCode -or $artifact.sha256.ToLowerInvariant() -ne $hash) {
        throw 'Google accepted a different bundle version or digest; no track will be committed.'
    }
    $release=@{name=$ReleaseName;versionCodes=@([string]$VersionCode);status='completed';releaseNotes=$notes}
    Invoke-AndroidPlayRequest 'PUT' $trackPath $token @{track=$Track;releases=@($release)} | Out-Null
    Invoke-AndroidPlayRequest 'POST' ($editPath+':validate') $token | Out-Null
    Write-Output 'PASS: Google validated the testing-track edit.'
    if ($Publish) {
        Invoke-AndroidPlayRequest 'POST' ($editPath+':commit') $token | Out-Null
        $committed=$true
        Write-Output "COMMITTED: $PackageName code $VersionCode to $Track. Google review/availability remain separate."
    } else { Write-Output 'VALIDATE ONLY: no track publication committed; uploaded bundles may remain in Play.' }
    New-Item -ItemType Directory -Path $evidence -Force | Out-Null
    @{package=$PackageName;track=$Track;versionCode=$VersionCode;sha256=$hash;committed=$committed;
        atUtc=[DateTimeOffset]::UtcNow.ToString('o');previousCodes=$active} | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $evidence "code-$VersionCode-$([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')).json") -Encoding utf8NoBOM
} finally {
    if ($edit -and -not $committed) {
        try {Invoke-AndroidPlayRequest 'DELETE' ($base+'/'+[Uri]::EscapeDataString($edit.id)) $token | Out-Null}
        catch {Write-Warning 'Could not discard this uncommitted edit; it will expire. No committed release was removed.'}
    }
    $token=$null
}
