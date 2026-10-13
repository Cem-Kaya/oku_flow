#requires -Version 7.0
# Offline credential/crypto boundary checks; no live Google account or token.
. (Join-Path $PSScriptRoot 'google-play\publisher_helpers.ps1')
Add-Type -TypeDefinition @'
using System;
using System.Security.Cryptography;
using System.Text;
public static class PlayCredentialFixture {
    public static string MakePem() { using var rsa=RSA.Create(2048); return rsa.ExportPkcs8PrivateKeyPem(); }
    public static bool Verify(string pem,string jwt) {
        var p=jwt.Split('.'); var s=p[2].Replace('-','+').Replace('_','/'); s=s.PadRight((s.Length+3)/4*4,'=');
        using var rsa=RSA.Create(); rsa.ImportFromPem(pem);
        return rsa.VerifyData(Encoding.UTF8.GetBytes(p[0]+"."+p[1]),Convert.FromBase64String(s),HashAlgorithmName.SHA256,RSASignaturePadding.Pkcs1);
    }
}
'@
$fixtureCredential=[pscustomobject]@{type='service_account';client_email='fixture-publisher@fixture-project.iam.gserviceaccount.com';
    private_key_id='fixture-only';private_key=[PlayCredentialFixture]::MakePem();token_uri='https://oauth2.googleapis.com/token'}
$fixtureRoot=Join-Path $env:LOCALAPPDATA ('OkuFlow\google-play\fixtures\'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
$fixtureProfile=Join-Path $fixtureRoot 'fixture.credentials.xml'
$secure=ConvertTo-SecureString ($fixtureCredential | ConvertTo-Json) -AsPlainText -Force
try { [pscredential]::new('fixture',$secure) | Export-Clixml -LiteralPath $fixtureProfile }
finally { $secure.Dispose() }
$restored=Read-AndroidPlayCredential $fixtureProfile
if ($restored.private_key -cne $fixtureCredential.private_key) {throw 'DPAPI credential round-trip failed'}
if ([IO.File]::ReadAllText($fixtureProfile).Contains('BEGIN PRIVATE KEY')) {throw 'Private key leaked into profile text'}
function Invoke-RestMethod {
    param($Method,$Uri,$Body,$TimeoutSec)
    if ($Method -ne 'Post' -or $Uri -ne 'https://oauth2.googleapis.com/token' -or
        $Body.grant_type -ne 'urn:ietf:params:oauth:grant-type:jwt-bearer') {throw 'Unexpected OAuth exchange'}
    if (-not [PlayCredentialFixture]::Verify($fixtureCredential.private_key,$Body.assertion)) {throw 'JWT RSA signature invalid'}
    $payload=$Body.assertion.Split('.')[1].Replace('-','+').Replace('_','/')
    $payload=$payload.PadRight([int]([Math]::Ceiling($payload.Length/4.0)*4),'=')
    $claims=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($payload)) | ConvertFrom-Json
    if ($claims.iss -ne $fixtureCredential.client_email -or
        $claims.scope -ne 'https://www.googleapis.com/auth/androidpublisher' -or
        $claims.exp-$claims.iat -ne 3600) {throw 'JWT scope/expiry/identity invalid'}
    return [pscustomobject]@{access_token='fixture-only-token'}
}
$token=New-AndroidPlayAccessToken $restored
if ($token -ne 'fixture-only-token') {throw 'Token response handling failed'}
if ((ConvertTo-AndroidPlayBase64Url ([byte[]](251,255))) -ne '-_8') {throw 'JWT Base64URL encoding failed'}
try {Get-AndroidPlayProfilePath (Join-Path $PSScriptRoot 'private.credentials.xml') | Out-Null; throw 'Repository profile was accepted'}
catch {if ($_.Exception.Message -notmatch 'must stay') {throw}}
try {& (Join-Path $PSScriptRoot 'publish_android_play.ps1') -CheckAccess -Track production; throw 'Production was accepted'}
catch {if ($_.Exception.Message -notmatch 'testing tracks only') {throw}}
Write-Output 'PASS: DPAPI encryption/round-trip, verified RSA JWT, OAuth scope/expiry, Base64URL, private paths and production refusal. No network/live credentials used.'
