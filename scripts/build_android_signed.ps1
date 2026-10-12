#requires -Version 7.0
[CmdletBinding()]
param(
    [switch]$InitializeKey,
    [string]$Keystore = $(if ($env:OKUFLOW_ANDROID_SIGNING_KEYSTORE) { $env:OKUFLOW_ANDROID_SIGNING_KEYSTORE }
        else { Join-Path $env:LOCALAPPDATA 'OkuFlow\android-signing\upload.jks' }),
    [string]$Alias = $(if ($env:OKUFLOW_ANDROID_SIGNING_KEY_ALIAS) { $env:OKUFLOW_ANDROID_SIGNING_KEY_ALIAS }
        else { 'okuflow-upload' })
)

# Run this in the owner's terminal. Password input stays local and is never
# returned through an agent tool or stored in the checkout/configuration.
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not [IO.Path]::IsPathFullyQualified($Keystore)) { throw 'Use an absolute local keystore path.' }
$keyPath = [IO.Path]::GetFullPath($Keystore)
if ($keyPath.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The upload key must stay on a local disk outside the repository and its synced drive.'
}
if ($InitializeKey) {
    $privateRoot = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'OkuFlow\android-signing'))
    if (-not $keyPath.StartsWith($privateRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'New upload keys are created only in the private local OkuFlow android-signing directory.'
    }
}
if ($InitializeKey -and (Test-Path -LiteralPath $keyPath)) { throw 'An upload key already exists; refusing to replace it.' }
if (-not $InitializeKey -and -not (Test-Path -LiteralPath $keyPath -PathType Leaf)) {
    throw 'Upload key missing. For a first Play release, run again with -InitializeKey.'
}
$jdk = if ($env:OKUFLOW_ANDROID_JDK) { $env:OKUFLOW_ANDROID_JDK } else { Join-Path $env:LOCALAPPDATA 'Programs\jdk-21' }
$keytool = Join-Path $jdk 'bin\keytool.exe'
if (-not (Test-Path -LiteralPath $keytool -PathType Leaf)) { throw 'Configured JDK keytool is unavailable.' }
$names = @('OKUFLOW_ANDROID_UPLOAD_KEYSTORE','OKUFLOW_ANDROID_UPLOAD_KEY_ALIAS',
    'OKUFLOW_ANDROID_UPLOAD_STORE_PASSWORD','OKUFLOW_ANDROID_UPLOAD_KEY_PASSWORD','GRADLE_OPTS')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$password = $null
$keyPassword = $null
$profilePath = if ($env:OKUFLOW_ANDROID_SIGNING_PROFILE -and -not $InitializeKey) { $env:OKUFLOW_ANDROID_SIGNING_PROFILE }
    else { Join-Path (Split-Path $keyPath) 'signing.credentials.xml' }
$savedCredentials = if (-not $InitializeKey -and (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
    Import-Clixml -LiteralPath $profilePath
} else { $null }
try {
    $password = if ($savedCredentials) { $savedCredentials.Store.Password }
        else { Read-Host 'Upload keystore password (keep it in your password manager)' -AsSecureString }
    if ($password.Length -lt 6) { throw 'The keystore password must contain at least six characters.' }
    $storeCredential = [pscredential]::new('upload', $password)
    $env:OKUFLOW_ANDROID_UPLOAD_STORE_PASSWORD = $storeCredential.GetNetworkCredential().Password
    if ($InitializeKey) {
        $confirmation = Read-Host 'Confirm the new keystore password' -AsSecureString
        try {
            if ([pscredential]::new('upload', $confirmation).GetNetworkCredential().Password -cne
                $env:OKUFLOW_ANDROID_UPLOAD_STORE_PASSWORD) { throw 'Password confirmation did not match.' }
        } finally { $confirmation.Dispose() }
        $env:OKUFLOW_ANDROID_UPLOAD_KEY_PASSWORD = $env:OKUFLOW_ANDROID_UPLOAD_STORE_PASSWORD
        New-Item -ItemType Directory -Path (Split-Path $keyPath) -Force | Out-Null
        & $keytool -genkeypair -keystore $keyPath -storetype JKS -alias $Alias -keyalg RSA -keysize 4096 `
            -validity 10000 -dname 'CN=OkuFlow Camera Upload Key' `
            -storepass:env OKUFLOW_ANDROID_UPLOAD_STORE_PASSWORD -keypass:env OKUFLOW_ANDROID_UPLOAD_KEY_PASSWORD
        if ($LASTEXITCODE -ne 0) { throw 'Upload key generation failed. No release was built.' }
        Write-Host "Created private upload key: $keyPath"
        Write-Host 'Back up this encrypted keystore and its password privately; never put them in Drive, Git, or a release ZIP.'
    } else {
        $keyPassword = if ($savedCredentials) { $savedCredentials.Key.Password }
            else { Read-Host 'Upload key password (same password for keys created by this helper)' -AsSecureString }
        $env:OKUFLOW_ANDROID_UPLOAD_KEY_PASSWORD = [pscredential]::new('upload', $keyPassword).GetNetworkCredential().Password
    }
    & $keytool -list -keystore $keyPath -alias $Alias -storepass:env OKUFLOW_ANDROID_UPLOAD_STORE_PASSWORD
    if ($LASTEXITCODE -ne 0) { throw 'Keystore password or upload alias could not be verified.' }
    # Windows Export-Clixml protects SecureString values with this user's
    # DPAPI identity. Future runs decrypt locally into process inputs only.
    if (-not $savedCredentials) {
        [pscustomobject]@{
            Store = [pscredential]::new('upload', $password)
            Key = if ($InitializeKey) { [pscredential]::new('upload', $password) }
                else { [pscredential]::new('upload', $keyPassword) }
        } | Export-Clixml -LiteralPath $profilePath
    }
    [Environment]::SetEnvironmentVariable('OKUFLOW_ANDROID_SIGNING_KEYSTORE', $keyPath, 'User')
    [Environment]::SetEnvironmentVariable('OKUFLOW_ANDROID_SIGNING_KEY_ALIAS', $Alias, 'User')
    [Environment]::SetEnvironmentVariable('OKUFLOW_ANDROID_SIGNING_PROFILE', $profilePath, 'User')
    $certificate = Join-Path (Split-Path $keyPath) 'upload-certificate.pem'
    & $keytool -exportcert -rfc -keystore $keyPath -alias $Alias -file $certificate `
        -storepass:env OKUFLOW_ANDROID_UPLOAD_STORE_PASSWORD
    if ($LASTEXITCODE -ne 0) { throw 'Public upload certificate export failed.' }
    $env:OKUFLOW_ANDROID_UPLOAD_KEYSTORE = $keyPath
    $env:OKUFLOW_ANDROID_UPLOAD_KEY_ALIAS = $Alias
    $env:GRADLE_OPTS = ($previous['GRADLE_OPTS'] + ' -Dorg.gradle.daemon=false').Trim()
    & pwsh.exe -NoProfile -File (Join-Path $PSScriptRoot 'build_android.ps1') -Release
    if ($LASTEXITCODE -ne 0) { throw 'Signed release validation failed; do not upload an earlier artifact.' }
    Write-Host "Public upload certificate: $certificate"
    Write-Host 'Signed local build validated. Play closed-track forms, policy review and submission remain separate.'
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    if ($password) { $password.Dispose() }
    if ($keyPassword) { $keyPassword.Dispose() }
}
