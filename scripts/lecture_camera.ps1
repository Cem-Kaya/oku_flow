#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Start', 'Stop', 'Status', 'Download')][string]$Action = 'Start',
    [string]$VideoPath,
    [string]$ObsPath = "$env:ProgramFiles\obs-studio\bin\64bit\obs64.exe",
    [string]$StateDirectory = "$env:LOCALAPPDATA\OkuFlow\lecture-camera",
    [ValidateRange(0, 86400)][int]$StartSeconds = 1500,
    [string]$Url,
    [string]$DownloaderPath,
    [string]$SevenZipPath = "$env:ProgramFiles\7-Zip\7z.exe",
    [switch]$Replace,
    [switch]$LaunchOkuFlow
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($StateDirectory)
New-Item -ItemType Directory -Path $root -Force | Out-Null
$lock = [IO.File]::Open((Join-Path $root 'operation.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
$statePath = Join-Path $root 'session.json'
$selectionPath = Join-Path $root 'selection.json'
$sample = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'lecture_camera_sample.json') -Raw | ConvertFrom-Json
$socket = $null

function Write-JsonFile($Path, $Value) {
    $Value | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $Path -Encoding utf8NoBOM
}

function Get-OwnedProcess($State) {
    if (-not $State) { return $null }
    $process = Get-Process -Id $State.processId -ErrorAction SilentlyContinue
    if (-not $process) { return $null }
    # A recycled PID must never target an unrelated OBS instance.
    if ($process.Path -ne $State.executable -or
        $process.StartTime.ToUniversalTime().Ticks -ne ([DateTime]$State.startedUtc).ToUniversalTime().Ticks) {
        throw 'Session identity no longer matches. No process was stopped.'
    }
    $expected = [IO.Path]::GetFullPath($State.executable)
    if (-not $expected.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
                                 [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Session executable is outside the test directory. No process was stopped.'
    }
    return $process
}

function Receive-ObsMessage {
    $buffer = [byte[]]::new(65536)
    $stream = [IO.MemoryStream]::new()
    $timeout = [Threading.CancellationTokenSource]::new(5000)
    try {
        do {
            $result = $script:socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $timeout.Token).GetAwaiter().GetResult()
            if ($result.MessageType -eq [Net.WebSockets.WebSocketMessageType]::Close) { throw 'OBS closed its control connection.' }
            $stream.Write($buffer, 0, $result.Count)
            if ($stream.Length -gt 1048576) { throw 'OBS control response exceeded 1 MiB.' }
        } until ($result.EndOfMessage)
        return [Text.Encoding]::UTF8.GetString($stream.ToArray()) | ConvertFrom-Json -AsHashtable
    } finally { $timeout.Dispose(); $stream.Dispose() }
}

function Send-ObsMessage($Message) {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($Message | ConvertTo-Json -Depth 20 -Compress))
    $timeout = [Threading.CancellationTokenSource]::new(5000)
    try {
        $script:socket.SendAsync([ArraySegment[byte]]::new($bytes),
            [Net.WebSockets.WebSocketMessageType]::Text, $true, $timeout.Token).GetAwaiter().GetResult() | Out-Null
    } finally { $timeout.Dispose() }
}

function Get-Base64Sha256([string]$Text) {
    return [Convert]::ToBase64String([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($Text)))
}

function Connect-Obs($State) {
    $script:socket = [Net.WebSockets.ClientWebSocket]::new()
    $timeout = [Threading.CancellationTokenSource]::new(2000)
    try {
        $script:socket.ConnectAsync([Uri]"ws://127.0.0.1:$($State.port)", $timeout.Token).GetAwaiter().GetResult() | Out-Null
        $hello = Receive-ObsMessage
        if ($hello.op -ne 0 -or -not $hello.d.authentication) { throw 'Expected an authenticated OBS WebSocket v5 server.' }
        $secret = Get-Base64Sha256 ($State.password + $hello.d.authentication.salt)
        $auth = Get-Base64Sha256 ($secret + $hello.d.authentication.challenge)
        Send-ObsMessage @{ op = 1; d = @{ rpcVersion = 1; authentication = $auth; eventSubscriptions = 0 } }
        $identified = Receive-ObsMessage
        if ($identified.op -ne 2) { throw 'OBS authentication failed.' }
    } catch {
        $script:socket.Dispose(); $script:socket = $null
        throw
    } finally { $timeout.Dispose() }
}

function Invoke-ObsRequest([string]$Type, [hashtable]$Data = @{}) {
    $id = [Guid]::NewGuid().ToString()
    Send-ObsMessage @{ op = 6; d = @{ requestType = $Type; requestId = $id; requestData = $Data } }
    $response = Receive-ObsMessage
    if ($response.op -ne 7 -or $response.d.requestId -ne $id) { throw "Unexpected response to $Type." }
    if (-not $response.d.requestStatus.result) { throw "OBS $Type failed: $($response.d.requestStatus.comment)" }
    return $response.d.responseData
}

function Stop-OwnedObs($State, $Process) {
    try {
        if (-not $script:socket) { Connect-Obs $State }
        if ((Invoke-ObsRequest 'GetVirtualCamStatus').outputActive) { Invoke-ObsRequest 'StopVirtualCam' | Out-Null }
    } catch { Write-Warning "OBS control unavailable; closing the owned test instance: $($_.Exception.Message)" }
    if ($script:socket) { $script:socket.Dispose(); $script:socket = $null }
    $Process.CloseMainWindow() | Out-Null
    if (-not $Process.WaitForExit(10000)) {
        # Recheck identity immediately before the explicitly requested kill fallback.
        $verified = Get-OwnedProcess $State
        if ($verified) { Stop-Process -Id $verified.Id -Force; $verified.WaitForExit(5000) | Out-Null }
    }
    if (Get-OwnedProcess $State) { throw 'The test OBS instance did not stop.' }
    Write-Host 'Lecture camera stopped. Your video is kept for next time.'
}

function Download-Lecture([string]$Destination) {
    if (Test-Path -LiteralPath $Destination) {
        if (-not $Replace) { throw "Video already exists: $Destination. Use -Replace to overwrite it, or choose another -VideoPath." }
    }
    if (-not $script:DownloaderPath) {
        $command = Get-Command yt-dlp -ErrorAction SilentlyContinue
        if ($command) { $script:DownloaderPath = $command.Source }
        elseif (Test-Path -LiteralPath "$env:USERPROFILE\.oz-tools\venv\Scripts\yt-dlp.exe") {
            $script:DownloaderPath = "$env:USERPROFILE\.oz-tools\venv\Scripts\yt-dlp.exe"
        } elseif ($Url) { throw 'Install yt-dlp or pass -DownloaderPath. A downloaded local file can also be supplied with -VideoPath.' }
    }
    $source = if ($Url) { $Url } else { $sample.youtube }
    New-Item -ItemType Directory -Path (Split-Path $Destination) -Force | Out-Null
    $temporary = Join-Path (Split-Path $Destination) ("lecture-download-$([Guid]::NewGuid().ToString('N')).mp4")
    $publisherCopy = -not $script:DownloaderPath
    if ($publisherCopy) {
        # Download the publisher's offline copy of the same YouTube lecture.
        # A distinct query avoids stale partial Range responses from HTTP caches.
        $downloadUri = $sample.download + '&okuflow=' + [Guid]::NewGuid().ToString('N')
        & "$env:SystemRoot\System32\curl.exe" --fail --location --silent --show-error `
            --retry 2 --connect-timeout 15 --max-time 240 --output $temporary $downloadUri
        if ($LASTEXITCODE -ne 0) { throw 'Publisher download failed; the existing video was preserved.' }
        if ((Get-Item -LiteralPath $temporary).Length -ne $sample.bytes -or
            (Get-FileHash -LiteralPath $temporary -Algorithm SHA1).Hash -ne $sample.sha1) {
            throw 'Publisher download length/checksum does not match. Existing video preserved.'
        }
    } else {
        # HLS avoids unavailable signed progressive formats on some public lectures.
        # Camera output is video-only; no audio merge is needed.
        $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
        if (-not $ffmpeg) { throw 'FFmpeg must be on PATH to remux YouTube HLS into a seekable MP4.' }
        & $script:DownloaderPath --no-playlist --no-overwrites --retries 3 --socket-timeout 30 `
            --no-progress --concurrent-fragments 4 --fragment-retries 2 --abort-on-unavailable-fragments `
            --ffmpeg-location $ffmpeg.Source `
            --format 'bv[protocol^=m3u8][ext=mp4][height<=480]/bv[ext=mp4][height<=480]/b[ext=mp4][height<=480]' `
            --output $temporary $source
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $temporary)) { throw 'Lecture download failed; the existing video was preserved.' }
    }
    if ((Get-Item -LiteralPath $temporary).Length -lt 1024) { throw 'Downloaded video is unexpectedly small.' }
    Move-Item -LiteralPath $temporary -Destination $Destination -Force:$Replace
    Write-JsonFile ($Destination + '.source.json') @{
        youtube = $source; downloadedUtc = [DateTime]::UtcNow.ToString('o')
        sha256 = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
        attribution = if ($Url) { 'User-selected video; retain its applicable attribution and license.' } else { $sample.attribution }
        license = if ($Url) { 'See source publisher.' } else { $sample.license }
        publisherPage = if ($Url) { $null } else { $sample.publisherPage }
        modification = if (-not $publisherCopy) { 'Downloaded available MP4 video up to 480p; audio omitted. No added blur.' } else { 'Unmodified publisher MP4; audio muted in OBS. No added blur.' }
        downloadMethod = if ($publisherCopy) { 'Publisher offline MP4, length and SHA-1 verified.' } else { 'YouTube via yt-dlp.' }
    }
    Write-Host "Downloaded: $Destination"
}

function Install-MediaFoundationOutput([string]$PortableRoot) {
    $plugin = Join-Path $PortableRoot 'obs-plugins\64bit\droidcam-virtual-output.dll'
    $driver = Get-PnpDevice -FriendlyName 'DroidCam Video' -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -eq 'DroidCam Video' -and $_.Status -eq 'OK' }
    # The existing kernel driver exposes OBS frames to Media Foundation apps.
    if (-not $driver) {
        throw 'OkuFlow needs the DroidCam Video Media Foundation driver. Install the DroidCam Virtual Output driver from its official releases; the ordinary OBS camera is DirectShow-only.'
    }
    if (Test-Path -LiteralPath $plugin) { return }
    if (-not (Test-Path -LiteralPath $SevenZipPath)) {
        throw '7-Zip is required to extract the private OBS output plugin. Pass -SevenZipPath for another installation.'
    }
    $package = Join-Path $root 'DroidCam.OBSVirtualOut.Plugin.0.2.2.exe'
    $expectedHash = '49CBEFF0037046336E04BFFD0E4499A5419BE74580C1A9BD882E0CB197B6E527'
    if (-not (Test-Path -LiteralPath $package)) {
        & "$env:SystemRoot\System32\curl.exe" --fail --location --silent --show-error `
            --connect-timeout 15 --max-time 60 --output $package `
            'https://github.com/dev47apps/droidcam-obs-virtual-output/releases/download/0.2.2/DroidCam.OBSVirtualOut.Plugin.0.2.2.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Could not download the Media Foundation output plugin.' }
    }
    if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne $expectedHash) { throw 'DroidCam plugin package checksum mismatch.' }
    $extract = Join-Path $root 'droidcam-plugin-extracted'
    New-Item -ItemType Directory -Path $extract -Force | Out-Null
    # The NSIS package has x64 and ARM64 DLLs with the same name. Keep both,
    # then select the actual PE x64 machine type instead of guessing order.
    & $SevenZipPath e $package '-r' '-aou' "-o$extract" 'droidcam-virtual-output.dll' 'en-US.ini' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract the DroidCam plugin.' }
    $x64 = Get-ChildItem -LiteralPath $extract -Filter 'droidcam-virtual-output*.dll' | Where-Object {
        $bytes = [IO.File]::ReadAllBytes($_.FullName)
        $offset = [BitConverter]::ToInt32($bytes, 0x3c)
        [BitConverter]::ToUInt16($bytes, $offset + 4) -eq 0x8664
    } | Select-Object -First 1
    if (-not $x64) { throw 'The plugin package contains no x64 DLL.' }
    Copy-Item -LiteralPath $x64.FullName -Destination $plugin
    $locale = Join-Path $PortableRoot 'data\obs-plugins\droidcam-virtual-output\locale'
    New-Item -ItemType Directory -Path $locale -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $extract 'en-US.ini') -Destination (Join-Path $locale 'en-US.ini')
    Write-Host 'Prepared private OBS output plugin for the installed DroidCam Video driver.'
}

function Start-OkuFlow {
    $app = Join-Path $PSScriptRoot '..\dist\OkuFlow\oku_flow.exe'
    if (-not (Test-Path -LiteralPath $app)) { throw "OkuFlow bundle not found: $app" }
    if (-not (Get-Process -Name oku_flow -ErrorAction SilentlyContinue)) {
        Start-Process -FilePath $app -WorkingDirectory (Split-Path $app) | Out-Null
    }
}

try {
    $state = if (Test-Path -LiteralPath $statePath) { Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json } else { $null }
    $owned = Get-OwnedProcess $state
    if ($Action -eq 'Stop') {
        if ($owned) { Stop-OwnedObs $state $owned } else { Write-Host 'Lecture camera is already stopped.' }
        return
    }
    if ($Action -eq 'Status' -or ($Action -eq 'Start' -and $owned)) {
        if (-not $owned) { Write-Host 'Lecture camera: stopped'; return }
        Connect-Obs $state
        $camera = Invoke-ObsRequest 'GetVirtualCamStatus'
        $media = Invoke-ObsRequest 'GetMediaInputStatus' @{ inputName = 'Lecture video' }
        [pscustomobject]@{ Running = $true; VirtualCameraActive = $camera.outputActive; VideoPath = $state.videoPath;
            Playback = $media.mediaState; PositionSeconds = [math]::Round($media.mediaCursor / 1000, 1); ProcessId = $owned.Id }
        if ($Action -eq 'Start') {
            Write-Host 'Already running. Stop before changing the video.'
            if ($LaunchOkuFlow) { Start-OkuFlow }
        }
        return
    }
    if ($owned) { throw 'Stop the lecture camera before replacing its video.' }
    if (-not $VideoPath) {
        if ($Action -eq 'Start' -and (Test-Path -LiteralPath $selectionPath)) {
            $selection = Get-Content -LiteralPath $selectionPath -Raw | ConvertFrom-Json
            $VideoPath = $selection.videoPath
            if (-not $PSBoundParameters.ContainsKey('StartSeconds') -and $null -ne $selection.startSeconds) {
                $StartSeconds = $selection.startSeconds
            }
        } else { $VideoPath = Join-Path $root 'media\lecture.mp4' }
    }
    $VideoPath = [IO.Path]::GetFullPath($VideoPath)
    if ($Action -eq 'Download') { Download-Lecture $VideoPath; return }
    if (Get-Process -Name obs64 -ErrorAction SilentlyContinue) { throw 'Another OBS instance is running. Close it before starting the lecture camera; the virtual camera driver has one shared output.' }
    if (-not (Test-Path -LiteralPath $VideoPath)) { Download-Lecture $VideoPath }
    if (-not (Test-Path -LiteralPath $ObsPath)) { throw "OBS not found at $ObsPath. Install OBS Studio 28+ or pass -ObsPath." }
    $ObsPath = (Resolve-Path -LiteralPath $ObsPath).Path
    $obsRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path $ObsPath) '..\..'))
    $hash = (Get-FileHash -LiteralPath $ObsPath -Algorithm SHA256).Hash.Substring(0, 12)
    $portable = Join-Path $root "obs-$hash"
    $executable = Join-Path $portable 'bin\64bit\obs64.exe'
    if (-not (Test-Path -LiteralPath (Join-Path $portable 'copy-complete.txt'))) {
        Write-Host 'Preparing a separate portable copy of your installed OBS (first run only)...'
        foreach ($folder in @('bin', 'data', 'obs-plugins')) {
            & robocopy (Join-Path $obsRoot $folder) (Join-Path $portable $folder) /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) { throw "Could not copy OBS $folder (robocopy exit $LASTEXITCODE)." }
        }
        if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $ObsPath -Algorithm SHA256).Hash) { throw 'Portable OBS executable does not match the installed copy.' }
        Set-Content -LiteralPath (Join-Path $portable 'copy-complete.txt') -Value $hash
    }
    Install-MediaFoundationOutput $portable
    $config = Join-Path $portable 'config\obs-studio'
    foreach ($folder in @('basic\profiles\OkuFlowLecture', 'basic\scenes', 'plugin_config\obs-websocket')) {
        New-Item -ItemType Directory -Path (Join-Path $config $folder) -Force | Out-Null
    }
    $globalConfig = @'
[General]
FirstRun=true
EnableAutoUpdates=false
ConfirmOnExit=false
Pre31Migrated=true
[Basic]
Profile=OkuFlow Lecture Test
ProfileDir=OkuFlowLecture
SceneCollection=OkuFlow Lecture Test
SceneCollectionFile=OkuFlowLecture
ConfigOnNewProfile=false
[BasicWindow]
SysTrayEnabled=true
SysTrayWhenStarted=true
PreviewEnabled=true
'@
    Set-Content -LiteralPath (Join-Path $config 'global.ini') -Value $globalConfig -Encoding utf8NoBOM
    # OBS 31+ splits app and user settings; seed both for fresh portable setups.
    Set-Content -LiteralPath (Join-Path $config 'user.ini') -Value $globalConfig -Encoding utf8NoBOM
    Set-Content -LiteralPath (Join-Path $config 'basic\profiles\OkuFlowLecture\basic.ini') -Encoding utf8NoBOM -Value @'
[General]
Name=OkuFlow Lecture Test
[Video]
BaseCX=1280
BaseCY=720
OutputCX=1280
OutputCY=720
FPSType=0
FPSCommon=30
ColorFormat=NV12
ColorSpace=709
ColorRange=Partial
[Audio]
SampleRate=48000
ChannelSetup=Stereo
[Output]
Mode=Simple
[DroidCamVirtualOutput]
AutoStart=true
'@
    $inputId = [Guid]::NewGuid().ToString()
    $sceneId = [Guid]::NewGuid().ToString()
    Write-JsonFile (Join-Path $config 'basic\scenes\OkuFlowLecture.json') @{
        name = 'OkuFlow Lecture Test'; current_scene = 'Lecture'; current_program_scene = 'Lecture';
        scene_order = @(@{ name = 'Lecture' }); groups = @(); quick_transitions = @();
        sources = @(
            @{ name = 'Lecture video'; uuid = $inputId; id = 'ffmpeg_source'; versioned_id = 'ffmpeg_source';
               settings = @{ local_file = $VideoPath; is_local_file = $true; looping = $true;
                   restart_on_activate = $true; close_when_inactive = $false; hw_decode = $false; clear_on_media_end = $false };
               volume = 0.0; muted = $true; mixers = 0 },
            @{ name = 'Lecture'; uuid = $sceneId; id = 'scene'; versioned_id = 'scene';
               settings = @{ items = @(@{ name = 'Lecture video'; source_uuid = $inputId; id = 1;
                   visible = $true; locked = $true; pos = @{ x = 640.0; y = 360.0 }; align = 0;
                   scale = @{ x = 1.0; y = 1.0 }; rot = 0.0; bounds_type = 2; bounds_align = 0;
                   bounds = @{ x = 1280.0; y = 720.0 }; crop_left = 0; crop_right = 0; crop_top = 0; crop_bottom = 0 }) } }
        ); 'virtual-camera' = @{ type = 0 }
    }
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $password = [Convert]::ToBase64String([Security.Cryptography.RandomNumberGenerator]::GetBytes(32))
    Write-JsonFile (Join-Path $config 'plugin_config\obs-websocket\config.json') @{
        server_enabled = $true; server_port = $port; auth_required = $true;
        server_password = $password; first_load = $false; alerts_enabled = $false
    }
    $process = Start-Process -FilePath $executable -WorkingDirectory (Split-Path $executable) -WindowStyle Hidden -PassThru `
        -ArgumentList '--portable --multi --disable-updater --minimize-to-tray --collection "OkuFlow Lecture Test" --profile "OkuFlow Lecture Test" --scene "Lecture"'
    $state = @{ processId = $process.Id; startedUtc = $process.StartTime.ToUniversalTime().ToString('o');
        executable = $executable; port = $port; password = $password; videoPath = $VideoPath }
    Write-JsonFile $statePath $state
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(40)
        do {
            try { Connect-Obs $state; break } catch {
                if ($process.HasExited) { throw "OBS exited during startup. Inspect $config\logs." }
                if ([DateTime]::UtcNow -ge $deadline) { throw "OBS control did not become ready. Inspect $config\logs." }
                Start-Sleep -Milliseconds 500
            }
        } while ($true)
        $camera = Invoke-ObsRequest 'GetVirtualCamStatus'
        if (-not $camera.outputActive) { Invoke-ObsRequest 'StartVirtualCam' | Out-Null }
        $media = Invoke-ObsRequest 'GetMediaInputStatus' @{ inputName = 'Lecture video' }
        if ($media.mediaDuration -le 0) { throw 'OBS could not decode the video.' }
        if ($StartSeconds * 1000 -ge $media.mediaDuration) { throw 'StartSeconds exceeds the video duration. Use -StartSeconds 0 for short clips.' }
        Invoke-ObsRequest 'SetMediaInputCursor' @{ inputName = 'Lecture video'; mediaCursor = $StartSeconds * 1000 } | Out-Null
        $cameraDeadline = [DateTime]::UtcNow.AddSeconds(10)
        while (-not (Invoke-ObsRequest 'GetVirtualCamStatus').outputActive) {
            if ([DateTime]::UtcNow -ge $cameraDeadline) { throw 'OBS Virtual Camera did not start. Check the installed OBS virtual-camera driver.' }
            Start-Sleep -Milliseconds 200
        }
        $outputs = Invoke-ObsRequest 'GetOutputList'
        $mfOutput = $outputs.outputs | Where-Object outputName -eq 'DroidCamVirtualOutput'
        if (-not $mfOutput) { throw 'The Media Foundation camera output was not created.' }
        Write-JsonFile $selectionPath @{ videoPath = $VideoPath; startSeconds = $StartSeconds }
        Write-Host "Lecture camera ready: $VideoPath"
        Write-Host 'In OkuFlow, select Advanced > Image > Device > Camera > DroidCam Video.'
        Write-Host 'OBS Virtual Camera also runs for DirectShow apps; DroidCam Video carries the same scene to OkuFlow.'
        Write-Host 'The video loops at 1280x720 / 30 FPS with its aspect ratio preserved. Virtual Camera carries video only.'
        if ($LaunchOkuFlow) { Start-OkuFlow }
    } catch {
        $failure = $_
        Stop-OwnedObs $state $process
        throw $failure
    }
} finally {
    if ($socket) { $socket.Dispose() }
    $lock.Dispose()
}
