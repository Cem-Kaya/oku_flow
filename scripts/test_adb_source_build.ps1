#requires -Version 7.0
<# Tests only a newly created adb server on a private loopback port.
   The existing/shared server on 5037 is never queried with a version-changing
   adb client, stopped, or replaced. Only this script's child is cleaned up. #>
[CmdletBinding()]
param([string]$RuntimePath = (Join-Path $env:LOCALAPPDATA 'OkuFlow\adb-source-build\build-output\platform-tools'))
$ErrorActionPreference = 'Stop'
$adb = (Resolve-Path -LiteralPath (Join-Path $RuntimePath 'adb.exe')).Path
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()
$sharedBefore = @(Get-NetTCPConnection -LocalPort 5037 -State Listen -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess -Unique)
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $adb
$start.WorkingDirectory = Split-Path $adb
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WindowStyle = 'Hidden'
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.Environment['ADB_LIBUSB'] = '0'
foreach ($arg in @('-L',"tcp:$port",'server','nodaemon')) { $start.ArgumentList.Add($arg) }
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$started = $false
function Read-Exact([IO.Stream]$Stream,[int]$Count) {
    $bytes = [byte[]]::new($Count)
    $offset = 0
    while ($offset -lt $Count) {
        $read = $Stream.Read($bytes,$offset,$Count-$offset)
        if (-not $read) { throw 'Private adb response ended early.' }
        $offset += $read
    }
    [Text.Encoding]::ASCII.GetString($bytes)
}
function Request-PrivateAdb([string]$Request) {
    $client = [Net.Sockets.TcpClient]::new()
    try {
        $client.Connect('127.0.0.1',$port)
        $stream = $client.GetStream()
        $stream.ReadTimeout = 2000
        $bytes = [Text.Encoding]::ASCII.GetBytes(('{0:x4}{1}' -f $Request.Length,$Request))
        $stream.Write($bytes,0,$bytes.Length)
        if ((Read-Exact $stream 4) -ne 'OKAY') { throw 'Private adb rejected a host request.' }
        $length = [Convert]::ToInt32((Read-Exact $stream 4),16)
        if ($length -gt 1048576) { throw 'Unbounded private adb response.' }
        Read-Exact $stream $length
    } finally { $client.Dispose() }
}
try {
    if (-not $process.Start()) { throw 'Could not start the owned private adb server.' }
    $started = $true
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    $ready = $false
    while ($deadline.ElapsedMilliseconds -lt 10000 -and -not $process.HasExited) {
        $owners = @(Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess -Unique)
        if ($process.Id -in $owners) { $ready = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) {
        if ($process.HasExited) {
            throw "Owned adb exited ($($process.ExitCode)): $($stderr.GetAwaiter().GetResult()) $($stdout.GetAwaiter().GetResult())"
        }
        throw 'Owned adb did not listen on its private port.'
    }
    $addresses = @(Get-NetTCPConnection -LocalPort $port -State Listen | Select-Object -ExpandProperty LocalAddress -Unique)
    if (@($addresses | Where-Object { $_ -notin @('127.0.0.1','::1') }).Count) {
        throw 'Private adb unexpectedly listened beyond loopback.'
    }
    $protocol = [Convert]::ToInt32((Request-PrivateAdb 'host:version'),16)
    if ($protocol -ne 41) { throw "Wrong private adb protocol: $protocol" }
    [void](Request-PrivateAdb 'host:devices-l')
    if ($process.MainWindowHandle -ne [IntPtr]::Zero) { throw 'Hidden adb unexpectedly created a window.' }
    $sharedAfter = @(Get-NetTCPConnection -LocalPort 5037 -State Listen -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess -Unique)
    if ((($sharedBefore | Sort-Object) -join ',') -cne (($sharedAfter | Sort-Object) -join ',')) {
        throw 'The shared adb server ownership changed.'
    }
    Write-Host "PASS: source-built adb started hidden, served protocol $protocol and device listing on a private port; shared server preserved."
} finally {
    # This is the exact process launched above on a private port, never an
    # existing server or another process found by name.
    if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
    $process.Dispose()
}
