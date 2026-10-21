#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$File,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$OutputDirectory = (Join-Path $env:LOCALAPPDATA ('OkuFlow/embedded-video-evidence/' + [guid]::NewGuid().ToString('N'))),
    [ValidateSet('Compare','Playback')][string]$Mode = 'Compare'
)
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$File = (Resolve-Path -LiteralPath $File).Path
$RuntimeDirectory = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
if (!(Split-Path -IsAbsolute $OutputDirectory) -or (Test-Path -LiteralPath $OutputDirectory)) {
    throw 'Evidence must use a fresh absolute local directory.'
}
# Share the tracked runner's global lease. Never overlap codec timing with a
# parent gate, another diagnostic or a native desktop/audio fixture.
$lockPath = Join-Path $env:LOCALAPPDATA 'OkuFlow/test-evidence/runner.lock'
[IO.Directory]::CreateDirectory((Split-Path $lockPath)) | Out-Null
$lease = [IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None')
try {
    $active = @(Get-CimInstance Win32_Process | Where-Object {
        $_.ProcessId -ne $PID -and $_.Name -in @('cmd.exe','pwsh.exe','powershell.exe') -and
        $_.CommandLine -match '(?i)(agent_build\.bat|[ /]run_test_gate\.ps1)'
    })
    if ($active.Count) { throw 'An agent_build/run_test_gate process is still active; leave it untouched.' }
    [IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $start.ArgumentList.Add($(if ($Mode -eq 'Compare') {'--compare-embedded'} else {'--native-playback-external'}))
    $start.ArgumentList.Add($File)
    $start.Environment['OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC'] = '1'
    $start.Environment['OKUFLOW_FFMPEG_RUNTIME'] = $RuntimeDirectory
    $start.Environment['OKUFLOW_VIDEO_BACKEND'] = 'ffmpeg'
    $receipt = [ordered]@{
        utc = [DateTime]::UtcNow.ToString('o'); mode = $Mode
        executable = $Executable; executableSha256 = (Get-FileHash -LiteralPath $Executable).Hash
        source = $File; sourceSha256 = (Get-FileHash -LiteralPath $File).Hash
        runtime = @(Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' | ForEach-Object {
            @{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
        })
        deadlineSeconds = 180; noGui = $true; noAudioEndpoint = $true
    }
    $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$OutputDirectory/input.json" -Encoding utf8
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    $clock = [Diagnostics.Stopwatch]::StartNew(); [long]$peak = 0; $timedOut = $false
    try {
        while (!$process.WaitForExit(100)) {
            $process.Refresh(); $peak = [Math]::Max($peak,$process.PeakWorkingSet64)
            if ($clock.Elapsed.TotalSeconds -ge 180) {
                $timedOut = $true; $process.Kill(); $process.WaitForExit(); break
            }
        }
        $stdout.GetAwaiter().GetResult() | Set-Content -LiteralPath "$OutputDirectory/stdout.txt" -Encoding utf8
        $stderr.GetAwaiter().GetResult() | Set-Content -LiteralPath "$OutputDirectory/stderr.txt" -Encoding utf8
        @{exitCode=$process.ExitCode;timedOut=$timedOut;wallMs=$clock.ElapsedMilliseconds;peakWorkingSetBytes=$peak} |
            ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/result.json" -Encoding utf8
        Get-Content -LiteralPath "$OutputDirectory/stdout.txt"
        Write-Host "Private evidence: $OutputDirectory"
        if ($timedOut -or $process.ExitCode) { throw 'Finite comparison failed; original evidence is retained.' }
    } finally { $process.Dispose() }
} finally { $lease.Dispose() }
