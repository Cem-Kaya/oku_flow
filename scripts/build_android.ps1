#requires -Version 7.0
[CmdletBinding()]
param(
    [switch]$Lint,
    [switch]$Install,
    [switch]$Release,
    [switch]$AllowUnsignedRelease
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$androidRoot = Join-Path $repoRoot "android"
$originalJavaHome = $env:JAVA_HOME
$originalAndroidHome = $env:ANDROID_HOME
$originalBuildRoot = $env:OKUFLOW_ANDROID_BUILD_DIR
$exitCode = 0

function Assert-AndroidTranslations {
    $resourcesRoot = Join-Path $androidRoot "app\src\main\res"
    $sourceKeys = $null
    foreach ($locale in @("values", "values-tr", "values-de")) {
        $path = Join-Path $resourcesRoot "$locale\strings.xml"
        [xml]$catalog = Get-Content -LiteralPath $path -Raw -Encoding utf8
        $keys = @($catalog.resources.ChildNodes |
            Where-Object { $_.NodeType -eq [Xml.XmlNodeType]::Element } |
            ForEach-Object {
                $name = $_.GetAttribute("name")
                if (-not $name) { throw "Unnamed resource in $path" }
                "$($_.Name):$name"
            } | Sort-Object)
        if (-not $keys.Count) { throw "Empty Android translation catalog: $path" }
        $duplicates = @($keys | Group-Object | Where-Object Count -gt 1)
        if ($duplicates.Count) { throw "Duplicate Android resource keys in ${path}: $($duplicates.Name -join ', ')" }
        if ($null -eq $sourceKeys) {
            $sourceKeys = $keys
        } else {
            $differences = @(Compare-Object -ReferenceObject $sourceKeys -DifferenceObject $keys)
            if ($differences.Count) {
                $details = ($differences | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join ", "
                throw "Android translation key mismatch in ${path}: $details"
            }
        }
    }
    Write-Host "PASS: Android English/Turkish/German resource key parity ($($sourceKeys.Count) keys)"
}

function Read-AdbAscii {
    param([IO.Stream]$Stream, [int]$Count)
    $buffer = [byte[]]::new($Count)
    $offset = 0
    while ($offset -lt $Count) {
        $read = $Stream.Read($buffer, $offset, $Count - $offset)
        if ($read -eq 0) { throw "The running adb server closed its version response." }
        $offset += $read
    }
    return [Text.Encoding]::ASCII.GetString($buffer)
}

function Assert-AdbServerCompatible {
    param([string]$Adb)
    $versionOutput = & $Adb version
    if ($LASTEXITCODE -ne 0) { throw "Unable to read SDK adb version." }
    $match = [regex]::Match(($versionOutput -join "`n"), 'Android Debug Bridge version 1\.0\.(\d+)')
    if (-not $match.Success) { throw "Cannot determine SDK adb server protocol version." }
    $clientVersion = [int]$match.Groups[1].Value
    $client = [Net.Sockets.TcpClient]::new()
    try {
        try {
            [void]$client.ConnectAsync("127.0.0.1", 5037).WaitAsync([TimeSpan]::FromSeconds(3)).GetAwaiter().GetResult()
        } catch {
            $cause = $_.Exception
            while ($cause.InnerException) { $cause = $cause.InnerException }
            if ($cause -is [Net.Sockets.SocketException] -and
                $cause.SocketErrorCode -eq [Net.Sockets.SocketError]::ConnectionRefused) {
                return # No server exists; adb may start its own normally.
            }
            throw "Cannot safely inspect the running adb server: $cause"
        }
        $stream = $client.GetStream()
        $stream.ReadTimeout = 3000
        $stream.WriteTimeout = 3000
        $request = [Text.Encoding]::ASCII.GetBytes("000chost:version")
        $stream.Write($request, 0, $request.Length)
        $status = Read-AdbAscii $stream 4
        if ($status -ne "OKAY") { throw "Running adb server rejected the version probe ($status)." }
        $length = [Convert]::ToInt32((Read-AdbAscii $stream 4), 16)
        if ($length -lt 1 -or $length -gt 16) { throw "Invalid adb server version response length." }
        $serverVersion = [Convert]::ToInt32((Read-AdbAscii $stream $length), 16)
        if ($serverVersion -ne $clientVersion) {
            throw "adb server protocol $serverVersion differs from SDK adb protocol $clientVersion. Installation skipped to preserve the running server. Close the owning tool yourself before retrying."
        }
    } finally {
        $client.Dispose()
    }
}

try {
    if ($AllowUnsignedRelease -and -not $Release) { throw "-AllowUnsignedRelease requires -Release." }
    if ($Release -and $Install) { throw "Release builds are never installed by this helper; use the Debug -Install workflow for device validation." }
    if ($Release) {
        $signingNames = @('OKUFLOW_ANDROID_UPLOAD_KEYSTORE', 'OKUFLOW_ANDROID_UPLOAD_KEY_ALIAS',
            'OKUFLOW_ANDROID_UPLOAD_STORE_PASSWORD', 'OKUFLOW_ANDROID_UPLOAD_KEY_PASSWORD')
        $present = @($signingNames | Where-Object { -not [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($_)) })
        if ($present.Count -ne 0 -and $present.Count -ne $signingNames.Count) { throw "All four OKUFLOW_ANDROID_UPLOAD_* signing inputs are required together." }
        if (-not $AllowUnsignedRelease -and $present.Count -eq 0) { throw "Release signing inputs are missing; no upload key will be created. Use -AllowUnsignedRelease only for a review artifact." }
        if ($AllowUnsignedRelease -and $present.Count) { throw "Clear upload signing inputs for an unsigned review build; -AllowUnsignedRelease must never produce an ambiguous signed artifact." }
    }
    Assert-AndroidTranslations
    $jdk = if ($env:OKUFLOW_ANDROID_JDK) { $env:OKUFLOW_ANDROID_JDK }
        else { Join-Path $env:LOCALAPPDATA "Programs\jdk-21" }
    $sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME }
        else { Join-Path $env:LOCALAPPDATA "Android\Sdk" }
    $buildRoot = if ($env:OKUFLOW_ANDROID_BUILD_DIR) { $env:OKUFLOW_ANDROID_BUILD_DIR }
        else { Join-Path $env:LOCALAPPDATA "OkuFlow\android-build" }
    if (-not [IO.Path]::IsPathFullyQualified($buildRoot)) {
        throw "OKUFLOW_ANDROID_BUILD_DIR must be an absolute local path."
    }
    $buildRoot = [IO.Path]::GetFullPath($buildRoot)
    if ($buildRoot.StartsWith($repoRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $buildRoot -eq $repoRoot -or $buildRoot -match '^[Yy]:[\\/]') {
        throw "Android outputs must be outside the Google Drive checkout; choose a local build directory."
    }
    if (-not (Test-Path (Join-Path $jdk "bin\java.exe"))) { throw "JDK missing at $jdk; set OKUFLOW_ANDROID_JDK." }
    if (-not (Test-Path (Join-Path $sdk "platform-tools\adb.exe"))) { throw "Android SDK missing at $sdk; set ANDROID_HOME." }
    $env:JAVA_HOME = (Resolve-Path $jdk).Path
    $env:ANDROID_HOME = (Resolve-Path $sdk).Path
    $env:OKUFLOW_ANDROID_BUILD_DIR = $buildRoot
    $localProperties = Join-Path $androidRoot "local.properties"
    if (-not (Test-Path $localProperties)) {
        $escapedSdk = $env:ANDROID_HOME.Replace('\', '/').Replace(':', '\:')
        [IO.File]::WriteAllText($localProperties, "sdk.dir=$escapedSdk`n", [Text.UTF8Encoding]::new($false))
    }
    $projectCache = Join-Path $buildRoot "project-cache"
    $kotlinPersistent = Join-Path $buildRoot "kotlin-persistent"
    New-Item -ItemType Directory -Force $projectCache, $kotlinPersistent | Out-Null
    $arguments = @(
        "--project-dir", $androidRoot,
        "--project-cache-dir", $projectCache,
        "-Dorg.gradle.java.home=$env:JAVA_HOME",
        "-Pkotlin.project.persistent.dir=$kotlinPersistent",
        "--console=plain"
    )
    if ($Release) {
        $arguments += @(':app:assembleRelease', ':app:testReleaseUnitTest', ':app:lintRelease',
            ':app:bundleRelease', ':app:writeReleaseDependencyInventory')
        if ($AllowUnsignedRelease) { $arguments += '-Pokuflow.allowUnsignedRelease=true' }
    } else {
        $arguments += @(':app:assembleDebug', ':app:testDebugUnitTest')
        if ($Lint) { $arguments += ':app:lintDebug' }
    }
    Write-Host "JDK: $env:JAVA_HOME"
    Write-Host "SDK: $env:ANDROID_HOME"
    Write-Host "Android outputs: $buildRoot"
    & (Join-Path $androidRoot "gradlew.bat") @arguments
    if ($LASTEXITCODE -ne 0) { throw "Android build/test$(if ($Lint) { '/lint' }) failed (exit $LASTEXITCODE)." }
    if ($Release) {
        $apk = Join-Path $buildRoot $(if ($AllowUnsignedRelease) { 'app\outputs\apk\release\app-release-unsigned.apk' } else { 'app\outputs\apk\release\app-release.apk' })
        $bundle = Join-Path $buildRoot 'app\outputs\bundle\release\app-release.aab'
        & (Join-Path $PSScriptRoot 'validate_android_release.ps1') -Apk $apk -Bundle $bundle -BuildRoot $buildRoot -Sdk $sdk -Jdk $jdk -AllowUnsignedRelease:$AllowUnsignedRelease
        if ($LASTEXITCODE -ne 0) { throw "Android release artifact validation failed." }
        Write-Host "PASS: Release assembly, unit tests, lint, bundle and technical artifact checks."
        Write-Host "Release AAB: $bundle"
        if ($AllowUnsignedRelease) { Write-Host 'REVIEW ONLY: UNSIGNED, NOT UPLOADABLE TO PLAY.' -ForegroundColor Yellow }
        Write-Host 'PENDING: signed tester distribution, public policy publication, Play declarations/account/policy applicability and real tester-device acceptance; production gates remain separate.'
    } else {
        $apk = Join-Path $buildRoot 'app\outputs\apk\debug\app-debug.apk'
        if (-not (Test-Path $apk)) { throw "Build returned success but debug APK is missing: $apk" }
        Write-Host "PASS: assembleDebug and testDebugUnitTest$(if ($Lint) { ' and lintDebug' })"
        Write-Host "Debug APK: $apk"
    }

    if ($Install) {
        $adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
        # ADB_SERVER_SOCKET overrides could bypass the local protocol guard.
        if ($env:ADB_SERVER_SOCKET -or $env:ANDROID_ADB_SERVER_PORT -or $env:ADB_SERVER_PORT) {
            throw "Clear custom adb server endpoint variables before -Install so the running server can be checked safely."
        }
        Assert-AdbServerCompatible $adb
        $deviceOutput = & $adb devices
        if ($LASTEXITCODE -ne 0) { throw "adb devices failed." }
        $devices = @($deviceOutput | Where-Object { $_ -match '^\S+\s+(device|unauthorized|offline|recovery|sideload|bootloader|no permissions)\b' })
        $authorized = @($devices | Where-Object { $_ -match '^\S+\s+device\b' })
        if ($devices.Count -ne 1 -or $authorized.Count -ne 1) {
            Write-Host "Install/launch skipped: $($devices.Count) attached device(s), $($authorized.Count) authorized. Exactly one authorized device and no other attached devices are required."
            $devices | ForEach-Object { Write-Host $_ }
        } else {
            $serial = ($authorized[0] -split '\s+')[0]
            & $adb -s $serial install -r $apk
            if ($LASTEXITCODE -ne 0) { throw "adb install failed." }
            $launchOutput = & $adb -s $serial shell am start -n "com.okuflow/com.okuflow.camera.MainActivity" 2>&1
            $launchCode = $LASTEXITCODE
            $launchOutput | ForEach-Object { Write-Host $_ }
            if ($launchCode -ne 0 -or ($launchOutput -join "`n") -match '(?m)^Error:|Exception') {
                throw "adb activity launch failed."
            }
            Write-Host "PASS: installed and launched on $serial"
        }
    }
} catch {
    Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    $env:JAVA_HOME = $originalJavaHome
    $env:ANDROID_HOME = $originalAndroidHome
    $env:OKUFLOW_ANDROID_BUILD_DIR = $originalBuildRoot
}
exit $exitCode
