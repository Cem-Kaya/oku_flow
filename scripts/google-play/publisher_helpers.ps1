# First-party Google Play authentication. Credentials never belong in Drive/Git.
#requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-AndroidPlayProfilePath([string]$Path) {
    if (-not $IsWindows) { throw 'The private publishing profile requires Windows DPAPI.' }
    $root = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'OkuFlow\google-play'))
    if (-not $Path) { $Path = Join-Path $root 'okuflow-test-publisher.credentials.xml' }
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Publishing profiles must stay in LocalAppData\OkuFlow\google-play.'
    }
    $current = Split-Path $full
    while ($current -and (Test-Path -LiteralPath $current)) {
        if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Publishing profiles cannot use a redirected/reparse directory.'
        }
        $current = Split-Path $current
    }
    return $full
}

function Test-AndroidPlayCredential($Credential) {
    if ($Credential.type -ne 'service_account' -or
        $Credential.client_email -notmatch '^[a-z0-9._-]+@[a-z0-9-]+\.iam\.gserviceaccount\.com$' -or
        $Credential.token_uri -ne 'https://oauth2.googleapis.com/token' -or
        $Credential.private_key -notmatch '^-----BEGIN PRIVATE KEY-----') {
        throw 'Expected a Google service-account JSON with the official token endpoint.'
    }
}

function Read-AndroidPlayCredential([string]$Profile) {
    $path = Get-AndroidPlayProfilePath $Profile
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw 'Configure the private Google Play profile before publishing.'
    }
    if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'The credential profile cannot be a reparse point.'
    }
    $protected = Import-Clixml -LiteralPath $path
    if ($protected -isnot [pscredential]) { throw 'Invalid DPAPI publishing profile.' }
    $credential = $protected.GetNetworkCredential().Password | ConvertFrom-Json
    Test-AndroidPlayCredential $credential
    return $credential
}

function ConvertTo-AndroidPlayBase64Url([byte[]]$Bytes) {
    return [Convert]::ToBase64String($Bytes).TrimEnd('=').Replace('+','-').Replace('/','_')
}

function New-AndroidPlayAccessToken($Credential) {
    Test-AndroidPlayCredential $Credential
    if (-not ('OkuFlow.PlayPublisherCrypto' -as [type])) {
        Add-Type -TypeDefinition @'
using System.Security.Cryptography;
namespace OkuFlow {
    public static class PlayPublisherCrypto {
        public static byte[] Sign(string pem, byte[] data) {
            using var rsa = RSA.Create();
            rsa.ImportFromPem(pem);
            return rsa.SignData(data, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        }
    }
}
'@
    }
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $header = @{alg='RS256';typ='JWT';kid=$Credential.private_key_id} | ConvertTo-Json -Compress
    $claims = @{iss=$Credential.client_email;scope='https://www.googleapis.com/auth/androidpublisher';
        aud='https://oauth2.googleapis.com/token';iat=$now;exp=$now+3600} | ConvertTo-Json -Compress
    $unsigned = (ConvertTo-AndroidPlayBase64Url ([Text.Encoding]::UTF8.GetBytes($header))) + '.' +
        (ConvertTo-AndroidPlayBase64Url ([Text.Encoding]::UTF8.GetBytes($claims)))
    $signature = [OkuFlow.PlayPublisherCrypto]::Sign($Credential.private_key,
        [Text.Encoding]::UTF8.GetBytes($unsigned))
    $assertion = $unsigned + '.' + (ConvertTo-AndroidPlayBase64Url $signature)
    # Never log the assertion, private JSON, token response or Authorization header.
    $response = Invoke-RestMethod -Method Post -Uri 'https://oauth2.googleapis.com/token' `
        -Body @{grant_type='urn:ietf:params:oauth:grant-type:jwt-bearer';assertion=$assertion} `
        -TimeoutSec 60
    if (-not $response.access_token) { throw 'Google returned no publishing access token.' }
    return $response.access_token
}

function Invoke-AndroidPlayRequest([string]$Method, [string]$Path, [string]$Token,
    $Body = $null, [string]$BinaryFile = '') {
    if ($Path -notmatch '^/androidpublisher/v3/applications/[a-zA-Z0-9._]+/edits(?:/|$|\?)') {
        throw 'Only transactional app edits are supported by this publisher.'
    }
    $parameters = @{Method=$Method;Headers=@{Authorization="Bearer $Token"};TimeoutSec=300}
    if ($BinaryFile) {
        $parameters.Uri = 'https://androidpublisher.googleapis.com/upload' + $Path
        $parameters.InFile = $BinaryFile
        $parameters.ContentType = 'application/octet-stream'
    } else {
        $parameters.Uri = 'https://androidpublisher.googleapis.com' + $Path
        if ($null -ne $Body) {
            $parameters.Body = [Text.Encoding]::UTF8.GetBytes(($Body | ConvertTo-Json -Depth 20 -Compress))
            $parameters.ContentType = 'application/json; charset=utf-8'
        }
    }
    Invoke-RestMethod @parameters
}
