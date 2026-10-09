[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [string]$OutputDirectory = "build/startup-profile",
    [ValidateRange(1, 5)][int]$Runs = 2,
    [ValidateRange(1, 30)][int]$DurationSeconds = 15,
    [int]$CameraIndex = -1,
    [switch]$CompareLegacy,
    [switch]$AllowNoFrame,
    [string]$SettingsPath
)

$ErrorActionPreference = "Stop"
$repository = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$application = (Resolve-Path -LiteralPath $Executable).Path
$destination = [IO.Path]::GetFullPath((Join-Path $repository $OutputDirectory))
if (Get-Process -Name open_zoom -ErrorAction SilentlyContinue) {
    throw "An OpenZoom instance is already running. Close it before profiling to avoid competing for its camera."
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
if (-not $SettingsPath) {
    $SettingsPath = Join-Path $env:APPDATA "OpenZoom/OpenZoom/settings.json"
}
$settingsText = if (Test-Path -LiteralPath $SettingsPath) {
    Get-Content -LiteralPath $SettingsPath -Raw
} else { '{"version":15}' }
$sourceSettingsHash = if (Test-Path -LiteralPath $SettingsPath) {
    (Get-FileHash -LiteralPath $SettingsPath -Algorithm SHA256).Hash
} else { $null }
$summaries = [Collections.Generic.List[object]]::new()

for ($run = 1; $run -le $Runs; ++$run) {
    $modes = if ($CompareLegacy) { @("legacy", "worker") } else { @("worker") }
    # Alternate ordering so warm camera/driver caches do not always favor one mode.
    if ($CompareLegacy -and $run % 2 -eq 0) { [array]::Reverse($modes) }
    foreach ($mode in $modes) {
        $trial = Join-Path $destination "$mode-$run"
        $settingsDirectory = $trial
        New-Item -ItemType Directory -Path $settingsDirectory -Force | Out-Null
        $config = ConvertFrom-Json $settingsText -AsHashtable
        foreach ($section in @("ui", "capture", "paths", "assistive")) {
            if (-not $config.ContainsKey($section)) { $config[$section] = @{} }
        }
        $config["ui"]["setupAssistantDeclined"] = $true
        $config["capture"]["accelerationAttempt"] = ""
        $config["paths"]["userDataRoot"] = Join-Path $trial "data"
        if ($CameraIndex -ge 0) { $config["cameraIndex"] = $CameraIndex }
        if (-not $config.ContainsKey("assistive")) { $config["assistive"] = @{} }
        $config["assistive"]["aiProvider"] = "openai"
        $config["assistive"]["vlmApiUrl"] = ""
        $config["assistive"]["vlmModel"] = ""
        $config["assistive"]["lectureNotesEnabled"] = $false
        if ($config.ContainsKey("currentConfig")) {
            $config["currentConfig"]["vlmAssistEnabled"] = $false
        }
        $config | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (
            Join-Path $settingsDirectory "settings.json") -Encoding utf8NoBOM

        $report = Join-Path $trial "startup.json"
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $application
        $startInfo.WorkingDirectory = Split-Path $application
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.Environment["QT_FORCE_STDERR_LOGGING"] = "1"
        $startInfo.Environment["OPENZOOM_CAPTURE_DIAGNOSTICS"] = "1"
        # Source-tree builds can use the documented Qt runtime without deployment.
        $qtBin = if ($env:QT_PREFIX) { Join-Path $env:QT_PREFIX "bin" }
                 else { "C:/Qt/6.9.3/msvc2022_64/bin" }
        $startInfo.Environment["PATH"] = $qtBin + ";" + $env:PATH
        $startInfo.Environment.Remove("OPENZOOM_VLM_API_URL") | Out-Null
        $startInfo.Environment.Remove("OPENZOOM_VLM_MODEL") | Out-Null
        $startInfo.Environment.Remove("OPENZOOM_VLM_API_KEY") | Out-Null
        $startInfo.ArgumentList.Add("--startup-profile=$report")
        $startInfo.ArgumentList.Add("--startup-profile-ms=$($DurationSeconds * 1000)")
        if ($mode -eq "legacy") { $startInfo.ArgumentList.Add("--startup-profile-legacy-camera") }

        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        $wallClock = [Diagnostics.Stopwatch]::StartNew()
        if (-not $process.Start()) { throw "Could not start the profiling instance." }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(($DurationSeconds + 20) * 1000)) {
            $process.CloseMainWindow() | Out-Null
            if (-not $process.WaitForExit(10000)) {
                throw "Profiling instance $($process.Id) did not close; it was left running. No further runs started."
            }
        }
        [IO.File]::WriteAllText((Join-Path $trial "stdout.log"), $stdout.GetAwaiter().GetResult())
        [IO.File]::WriteAllText((Join-Path $trial "stderr.log"), $stderr.GetAwaiter().GetResult())
        if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $report)) {
            throw "Profiling run $mode-$run failed (exit $($process.ExitCode)); inspect $trial."
        }
        if ($sourceSettingsHash -and
            (Get-FileHash -LiteralPath $SettingsPath -Algorithm SHA256).Hash -ne $sourceSettingsHash) {
            throw "The source settings changed during profiling; inspect the isolation before continuing."
        }
        $result = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json -AsHashtable
        $result["mode"] = $mode
        $result["run"] = $run
        $result["process_wall_ms"] = $wallClock.ElapsedMilliseconds
        $result["report"] = $report
        $summaries.Add($result)
        $result | ConvertTo-Json -Compress
        $process.Dispose()
        if ($result["first_present_ms"] -lt 0 -and -not $AllowNoFrame) {
            throw "No camera frame was presented in $mode-$run; compare timings only after resolving that camera failure."
        }
    }
}
$summaries | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (
    Join-Path $destination "summary.json") -Encoding utf8NoBOM
