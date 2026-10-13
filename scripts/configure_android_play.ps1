#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ServiceAccountJson,
    [string]$Profile)
. (Join-Path $PSScriptRoot 'google-play\publisher_helpers.ps1')
$source = (Resolve-Path -LiteralPath $ServiceAccountJson).Path
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([IO.Path]::GetPathRoot($source) -eq [IO.Path]::GetPathRoot($repo)) {
    throw 'The service-account key must stay outside the synced repository drive.'
}
$destination = Get-AndroidPlayProfilePath $Profile
if (Test-Path -LiteralPath $destination) { throw 'An existing publishing profile will not be replaced.' }
$json = [IO.File]::ReadAllText($source)
$credential = $json | ConvertFrom-Json
Test-AndroidPlayCredential $credential
New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
$secure = ConvertTo-SecureString $json -AsPlainText -Force
try {
    [pscredential]::new('okuflow-test-publisher', $secure) | Export-Clixml -LiteralPath $destination
    $verified = Read-AndroidPlayCredential $destination
    if ($verified.private_key -cne $credential.private_key -or
        $verified.client_email -cne $credential.client_email) { throw 'Private profile verification failed.' }
    [Environment]::SetEnvironmentVariable('OKUFLOW_ANDROID_PLAY_PROFILE', $destination, 'User')
    Write-Output "PASS: Windows-protected publishing profile configured at $destination"
    Write-Output 'Only the profile path is stored in the user environment. Keep the original key private.'
} finally { $secure.Dispose(); $json=$null; $credential=$null; $verified=$null }
