#Requires -Version 7.0
# Standalone model-free native fixtures. No compiler, GUI, admin, owner process,
# or actual VCTIP launch. All children are suspended before private Job assignment.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'Owned build cleanup fixtures require Windows.' }
. (Join-Path $PSScriptRoot 'windows-build/owned_job_cleanup.ps1')
$clock = [Diagnostics.Stopwatch]::StartNew()
$checks = 0
function Assert-Cleanup([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    if ($clock.Elapsed.TotalSeconds -gt 110) { throw 'Owned build cleanup fixture deadline exceeded.' }
    $script:checks++
}

# PowerShell may create console-host descendants even for a hidden launch.
# Await an internally consistent kernel inventory during fixture setup; every
# descendant remains Unknown. Never assume that launch count is Job membership.
function Get-FixtureJobSnapshot([IntPtr]$Job, [uint32[]]$Seed) {
    $until = $clock.ElapsedMilliseconds + 5000
    $current = Get-OwnedBuildJobMembers -Job $Job -ProcessIds $Seed
    do {
        $ids = @($current.NativeSnapshot.Members | ForEach-Object ProcessId)
        $current = Get-OwnedBuildJobMembers -Job $Job -ProcessIds $ids
        if ($current.SnapshotVerified) { return $current }
        [Threading.Thread]::Sleep(25)
    } while ($clock.ElapsedMilliseconds -lt $until)
    throw ('Fixture kernel inventory did not stabilize: ' + ($current | ConvertTo-Json -Depth 8 -Compress))
}

# These records exercise only comparison policy. They are not native evidence.
$pinPath = '\\?\C:\independent-tools\compiler\VCTIP.EXE'
$pinHash = 'a' * 64
$synthetic = @{
    ProcessId = 1234U; MetadataVerified = $true; JobMembershipVerified = $true; Running = $true
    CreationTimeFileTime = 123456UL; CanonicalImagePath = $pinPath; ImageSha256 = $pinHash
}
$policy = @{ SnapshotVerified = $true; TelemetryPinVerified = $true; TelemetryImagePath = $pinPath; TelemetryImageSha256 = $pinHash }
$disposition = Get-OwnedJobCleanupDisposition -Members @($synthetic) @policy
Assert-Cleanup $disposition.AllVerifiedTelemetry 'Exact synthetic comparison policy failed.'
Assert-Cleanup ($disposition.Members[0].Classification -eq 'VerifiedBuildTelemetry') 'Exact synthetic record was not classified.'
foreach ($mutation in @(
    @{ CanonicalImagePath = '\\?\C:\other-compiler\VCTIP.EXE' },
    @{ ImageSha256 = ('b' * 64) }, @{ MetadataVerified = $false },
    @{ JobMembershipVerified = $false }, @{ Running = $false },
    @{ CreationTimeFileTime = 0UL }, @{ ProcessId = 0U }, @{ MetadataVerified = 'true' }
)) {
    $negative = $synthetic.Clone()
    foreach ($key in $mutation.Keys) { $negative[$key] = $mutation[$key] }
    $disposition = Get-OwnedJobCleanupDisposition -Members @($negative) @policy
    Assert-Cleanup (-not $disposition.AllVerifiedTelemetry -and $disposition.Members[0].Classification -eq 'Unknown') 'Unverified or differently pinned VCTIP was accepted.'
}
Assert-Cleanup (-not (Get-OwnedJobCleanupDisposition -Members @() @policy).AllVerifiedTelemetry) 'Empty membership granted a telemetry exception.'
Assert-Cleanup (-not (Get-OwnedJobCleanupDisposition -Members @($null) @policy).AllVerifiedTelemetry) 'Missing metadata granted a telemetry exception.'
Assert-Cleanup (-not (Get-OwnedJobCleanupDisposition -Members @($synthetic)).AllVerifiedTelemetry) 'Default policy granted a telemetry exception.'
$badPolicy = $policy.Clone(); $badPolicy.TelemetryImagePath = '\\?\C:\independent-tools\compiler\other.exe'
Assert-Cleanup (-not (Get-OwnedJobCleanupDisposition -Members @($synthetic) @badPolicy).AllVerifiedTelemetry) 'Non-VCTIP pin was allowed.'
$badPolicy = $policy.Clone(); $badPolicy.TelemetryPinVerified = $false
Assert-Cleanup (-not (Get-OwnedJobCleanupDisposition -Members @($synthetic) @badPolicy).AllVerifiedTelemetry) 'Unverified pin was allowed.'
$badPolicy = $policy.Clone(); $badPolicy.SnapshotVerified = $false
Assert-Cleanup (-not (Get-OwnedJobCleanupDisposition -Members @($synthetic) @badPolicy).AllVerifiedTelemetry) 'Incomplete snapshot was allowed.'
$unknown = $synthetic.Clone(); $unknown.CanonicalImagePath = '\\?\C:\tools\pwsh.exe'
Assert-Cleanup (-not (Get-OwnedJobCleanupDisposition -Members @($synthetic, $unknown) @policy).AllVerifiedTelemetry) 'Mixed telemetry/unknown membership was allowed.'
Write-Host 'PASS owned build cleanup: pure comparison and refusal cases'

Initialize-OwnedBuildJobNative
if (-not ('OkuFlow.BuildCleanup.FixtureV1' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
namespace OkuFlow.BuildCleanup {
    public sealed class ChildV1 : IDisposable {
        public SafeFileHandle Process;
        public uint ProcessId;
        public ulong CreationTimeFileTime;
        public bool IsRunning { get { return FixtureV1.Wait(Process, 0) == 258; } }
        public bool WaitForExit(uint milliseconds) { return FixtureV1.Wait(Process, milliseconds) == 0; }
        public uint ExitCode { get { uint code; if (!FixtureV1.GetExitCodeProcess(Process, out code)) throw new Win32Exception(); return code; } }
        public void Dispose() { if (Process != null) Process.Dispose(); }
    }
    public static class FixtureV1 {
        [StructLayout(LayoutKind.Sequential)]
        struct BasicLimits {
            public long ProcessTime, JobTime; public uint Flags;
            public UIntPtr MinWorkingSet, MaxWorkingSet; public uint ActiveProcessLimit;
            public UIntPtr Affinity; public uint Priority, Scheduling;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct IoCounters { public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes; }
        [StructLayout(LayoutKind.Sequential)]
        struct ExtendedLimits {
            public BasicLimits Basic; public IoCounters Io;
            public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct StartupInfo {
            public uint Size; public string Reserved, Desktop, Title;
            public uint X, Y, Width, Height, XChars, YChars, Fill, Flags;
            public ushort Show, ReservedSize; public IntPtr ReservedBytes, Input, Output, Error;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct ProcessInfo { public IntPtr Process, Thread; public uint ProcessId, ThreadId; }
        [StructLayout(LayoutKind.Sequential)]
        struct FileTime { public uint Low, High; public ulong Value { get { return ((ulong)High << 32) | Low; } } }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern SafeFileHandle CreateJobObjectW(IntPtr attributes, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool SetInformationJobObject(SafeFileHandle job, int infoClass, ref ExtendedLimits limits, uint bytes);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool CreateProcessW(string application, StringBuilder command, IntPtr processAttributes, IntPtr threadAttributes,
            [MarshalAs(UnmanagedType.Bool)] bool inherit, uint flags, IntPtr environment, string directory, ref StartupInfo startup, out ProcessInfo info);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool AssignProcessToJobObject(SafeFileHandle job, SafeFileHandle process);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint ResumeThread(SafeFileHandle thread);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool GetProcessTimes(SafeFileHandle process, out FileTime creation, out FileTime exit, out FileTime kernel, out FileTime user);
        [DllImport("kernel32.dll", EntryPoint = "WaitForSingleObject", SetLastError = true)]
        public static extern uint Wait(SafeFileHandle process, uint milliseconds);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetExitCodeProcess(SafeFileHandle process, out uint code);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool TerminateJobObject(SafeFileHandle job, uint code);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool TerminateProcess(SafeFileHandle process, uint code);

        public static SafeFileHandle NewJob() {
            SafeFileHandle job = CreateJobObjectW(IntPtr.Zero, null);
            if (job.IsInvalid) { job.Dispose(); throw new Win32Exception(); }
            var limits = new ExtendedLimits(); limits.Basic.Flags = 0x2000; // KILL_ON_JOB_CLOSE
            if (!SetInformationJobObject(job, 9, ref limits, (uint)Marshal.SizeOf(typeof(ExtendedLimits)))) {
                var error = new Win32Exception(); job.Dispose(); throw error;
            }
            return job;
        }
        public static ChildV1 Start(SafeFileHandle job, string executable, string encodedCommand) {
            var startup = new StartupInfo { Size = (uint)Marshal.SizeOf(typeof(StartupInfo)), Flags = 1, Show = 0 };
            ProcessInfo info;
            var command = new StringBuilder("\"" + executable + "\" -NoLogo -NoProfile -NonInteractive -EncodedCommand " + encodedCommand);
            if (!CreateProcessW(executable, command, IntPtr.Zero, IntPtr.Zero, false, 0x08000004,
                IntPtr.Zero, null, ref startup, out info)) throw new Win32Exception();
            var process = new SafeFileHandle(info.Process, true);
            using (var thread = new SafeFileHandle(info.Thread, true)) {
                try {
                    if (!AssignProcessToJobObject(job, process)) throw new Win32Exception();
                    FileTime creation, exit, kernel, user;
                    if (!GetProcessTimes(process, out creation, out exit, out kernel, out user)) throw new Win32Exception();
                    if (ResumeThread(thread) == UInt32.MaxValue) throw new Win32Exception();
                    return new ChildV1 { Process = process, ProcessId = info.ProcessId, CreationTimeFileTime = creation.Value };
                } catch {
                    // Only the exact just-created suspended fixture handle, including
                    // failure before assignment. Never open/kill a process by PID.
                    TerminateProcess(process, 99); Wait(process, 5000); process.Dispose(); throw;
                }
            }
        }
        public static void EndJob(SafeFileHandle job) {
            if (!TerminateJobObject(job, 99)) throw new Win32Exception();
        }
    }
}
'@
}

$fixtureRoot = Join-Path $env:LOCALAPPDATA ('OkuFlow/owned-build-cleanup-fixtures/' + [Guid]::NewGuid().ToString('N'))
$job = $null; $sentinelJob = $null; $short = $null; $persistent = $null; $sentinel = $null
$exitGate = $null; $sentinelExitGate = $null
try {
    [void][IO.Directory]::CreateDirectory($fixtureRoot)
    # Deliberately not an executable: a local pin-comparison fixture only.
    $localPin = Join-Path $fixtureRoot 'VCTIP.EXE'
    [IO.File]::WriteAllText($localPin, 'first-party non-executable telemetry pin fixture')
    $localHash = (Get-FileHash -LiteralPath $localPin -Algorithm SHA256).Hash
    $job = [OkuFlow.BuildCleanup.FixtureV1]::NewJob()
    $sentinelJob = [OkuFlow.BuildCleanup.FixtureV1]::NewJob()
    $empty = Get-OwnedBuildJobMembers -Job $job.DangerousGetHandle() -ProcessIds @() -TelemetryImagePath $localPin -TelemetryImageSha256 $localHash
    Assert-Cleanup ($empty.SnapshotVerified -and $empty.Members.Count -eq 0 -and -not $empty.AllVerifiedTelemetry) 'Native empty Job granted an exception or failed inspection.'
    $executable = Join-Path $PSHOME 'pwsh.exe'
    $exitGateName = 'Local\OkuFlow-owned-cleanup-' + [Guid]::NewGuid().ToString('N')
    $exitGate = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $exitGateName)
    # A private bounded wait avoids racing the initial metadata assertions on a
    # slow host. Signalling permits normal exit; it never terminates the child.
    $shortScript = '$gate = [Threading.EventWaitHandle]::OpenExisting(''' + $exitGateName + '''); try { if (-not $gate.WaitOne(90000)) { exit 3 } } finally { $gate.Dispose() }'
    $shortCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($shortScript))
    $sentinelGateName = 'Local\OkuFlow-owned-cleanup-' + [Guid]::NewGuid().ToString('N')
    $sentinelExitGate = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $sentinelGateName)
    $sentinelScript = '$gate = [Threading.EventWaitHandle]::OpenExisting(''' + $sentinelGateName + '''); try { if (-not $gate.WaitOne(90000)) { exit 3 } } finally { $gate.Dispose() }'
    $sentinelCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($sentinelScript))
    $longCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes('[Threading.Thread]::Sleep(90000)'))
    $short = [OkuFlow.BuildCleanup.FixtureV1]::Start($job, $executable, $shortCommand)
    $persistent = [OkuFlow.BuildCleanup.FixtureV1]::Start($job, $executable, $longCommand)
    $sentinel = [OkuFlow.BuildCleanup.FixtureV1]::Start($sentinelJob, $executable, $sentinelCommand)
    $ids = @($short.ProcessId, $persistent.ProcessId)
    $native = Get-FixtureJobSnapshot -Job $job.DangerousGetHandle() -Seed $ids
    $ids = @($native.Members | ForEach-Object ProcessId)
    Assert-Cleanup ($native.SnapshotVerified -and $native.Members.Count -ge 2 -and -not $native.AllVerifiedTelemetry) 'Owned unknown children did not produce a complete non-benign snapshot.'
    foreach ($child in @($short, $persistent)) {
        $row = @($native.Members | Where-Object ProcessId -eq $child.ProcessId)[0]
        Assert-Cleanup ($row.Classification -eq 'Unknown' -and $row.Metadata.MetadataVerified -and $row.Metadata.JobMembershipVerified -and $row.Metadata.Running) 'Owned unknown child metadata was not verified.'
        Assert-Cleanup ($row.Metadata.CreationTimeFileTime -eq $child.CreationTimeFileTime) 'Held process creation identity did not match launch evidence.'
        Assert-Cleanup ($row.Metadata.CanonicalImagePath -match '[\\/]pwsh\.exe$' -and $row.Metadata.ImageSha256 -match '^[0-9a-f]{64}$') 'Native image path/hash metadata was missing.'
    }
    $pinned = Get-OwnedBuildJobMembers -Job $job.DangerousGetHandle() -ProcessIds $ids -TelemetryImagePath $localPin -TelemetryImageSha256 $localHash
    Assert-Cleanup ($pinned.NativeSnapshot.TelemetryPinVerified -and $pinned.SnapshotVerified -and -not $pinned.AllVerifiedTelemetry) 'Unrelated owned image accepted a valid VCTIP file pin.'
    $wrongHash = Get-OwnedBuildJobMembers -Job $job.DangerousGetHandle() -ProcessIds $ids -TelemetryImagePath $localPin -TelemetryImageSha256 ('0' * 64)
    Assert-Cleanup (-not $wrongHash.NativeSnapshot.TelemetryPinVerified -and -not $wrongHash.AllVerifiedTelemetry) 'Wrong native image pin was accepted.'
    $wrongPath = Get-OwnedBuildJobMembers -Job $job.DangerousGetHandle() -ProcessIds $ids -TelemetryImagePath $executable -TelemetryImageSha256 (Get-FileHash -LiteralPath $executable).Hash
    Assert-Cleanup (-not $wrongPath.NativeSnapshot.TelemetryPinVerified -and -not $wrongPath.AllVerifiedTelemetry) 'Non-VCTIP native image pin was accepted.'
    $omitted = Get-OwnedBuildJobMembers -Job $job.DangerousGetHandle() -ProcessIds @($persistent.ProcessId)
    Assert-Cleanup (-not $omitted.SnapshotVerified -and $omitted.Members.Count -eq $native.Members.Count -and -not $omitted.AllVerifiedTelemetry) 'Partial caller inventory concealed an owned member.'
    $emptyInput = Get-OwnedBuildJobMembers -Job $job.DangerousGetHandle() -ProcessIds @()
    Assert-Cleanup (-not $emptyInput.SnapshotVerified -and $emptyInput.Members.Count -eq $native.Members.Count -and -not $emptyInput.AllVerifiedTelemetry) 'Empty caller inventory concealed a nonempty Job.'
    $duplicate = Get-OwnedBuildJobMembers -Job $job.DangerousGetHandle() -ProcessIds @($ids + $persistent.ProcessId)
    Assert-Cleanup (-not $duplicate.SnapshotVerified -and -not $duplicate.AllVerifiedTelemetry) 'Duplicate process identifiers granted a complete snapshot.'
    $outside = Get-OwnedBuildJobMembers -Job $job.DangerousGetHandle() -ProcessIds @($ids + $sentinel.ProcessId)
    $outsideRow = @($outside.Members | Where-Object ProcessId -eq $sentinel.ProcessId)[0]
    Assert-Cleanup (-not $outside.SnapshotVerified -and -not $outside.AllVerifiedTelemetry -and $outsideRow.Classification -eq 'Unknown' -and -not $outsideRow.Metadata.JobMembershipVerified) 'Foreign sentinel was accepted as owned.'
    Assert-Cleanup $sentinel.IsRunning 'Inspection affected the unrelated sentinel.'
    $invalid = Get-OwnedBuildJobMembers -Job ([IntPtr]::Zero) -ProcessIds $ids
    Assert-Cleanup (-not $invalid.SnapshotVerified -and -not $invalid.AllVerifiedTelemetry -and $invalid.NativeSnapshot.Error) 'Missing held Job did not fail closed.'
    Assert-Cleanup ($invalid.Members.Count -eq $ids.Count -and @($invalid.Members | Where-Object Classification -ne 'Unknown').Count -eq 0) 'Missing held Job lost explicit unknown records.'
    Write-Host 'PASS owned build cleanup: held Job, creation identity, native image metadata and refusals'

    [void]$exitGate.Set()
    Assert-Cleanup ($short.WaitForExit(15000) -and $short.ExitCode -eq 0) 'Known short-lived fixture did not exit naturally with code zero.'
    $stale = Get-OwnedBuildJobMembers -Job $job.DangerousGetHandle() -ProcessIds $ids
    Assert-Cleanup (-not $stale.SnapshotVerified -and -not $stale.AllVerifiedTelemetry) 'Exited process lookup was accepted as current membership.'
    $remaining = Get-FixtureJobSnapshot -Job $job.DangerousGetHandle() -Seed @($persistent.ProcessId)
    Assert-Cleanup ($remaining.SnapshotVerified -and $remaining.Members.Count -ge 1 -and @($remaining.Members | Where-Object Classification -ne 'Unknown').Count -eq 0 -and -not $remaining.AllVerifiedTelemetry -and $persistent.IsRunning) 'Persistent unknown child gained a telemetry exception after its sibling exited.'
    # Native fixture teardown owns this exact Job; classification never performs it.
    [OkuFlow.BuildCleanup.FixtureV1]::EndJob($job)
    Assert-Cleanup ($persistent.WaitForExit(5000)) 'Private owned Job did not terminate its fixture child.'
    Assert-Cleanup $sentinel.IsRunning 'Target Job teardown terminated the unrelated sentinel.'
    $drained = Get-FixtureJobSnapshot -Job $job.DangerousGetHandle() -Seed @()
    Assert-Cleanup ($drained.SnapshotVerified -and $drained.Members.Count -eq 0 -and -not $drained.AllVerifiedTelemetry) 'Drained Job granted a telemetry exception.'
    [void]$sentinelExitGate.Set()
    Assert-Cleanup ($sentinel.WaitForExit(15000) -and $sentinel.ExitCode -eq 0) 'Unrelated sentinel did not exit naturally after all survival assertions.'
    Write-Host 'PASS owned build cleanup: natural child exit, persistent unknown and unrelated sentinel survival'
} finally {
    # Closing each private KILL_ON_JOB_CLOSE handle is independent of test outcome.
    if ($job) { $job.Dispose() }
    if ($sentinelJob) { $sentinelJob.Dispose() }
    foreach ($child in @($short, $persistent, $sentinel)) { if ($child) { $child.Dispose() } }
    if ($exitGate) { $exitGate.Dispose() }
    if ($sentinelExitGate) { $sentinelExitGate.Dispose() }
    # Retain tiny fixture files for failure evidence; no recursive deletion needed.
}
Write-Host ("PASS owned build cleanup: {0} assertions in {1:F2}s; fixture files: {2}" -f $checks, $clock.Elapsed.TotalSeconds, $fixtureRoot)
