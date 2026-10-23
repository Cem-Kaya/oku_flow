#Requires -Version 7.0
# Dot-source from an external build controller that already owns its kernel Job.
# Classification only: this helper never creates, assigns, or terminates a process.

function Initialize-OwnedBuildJobNative {
    if (-not $IsWindows) { throw 'Owned build Job inspection requires Windows.' }
    if ('OkuFlow.BuildCleanup.NativeV1' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace OkuFlow.BuildCleanup {
    public sealed class MemberV1 {
        public uint ProcessId;
        public bool MetadataVerified, JobMembershipVerified, Running;
        public ulong CreationTimeFileTime;
        public string ImagePath = "", CanonicalImagePath = "", ImageSha256 = "", Error = "";
    }
    public sealed class SnapshotV1 {
        public MemberV1[] Members = new MemberV1[0];
        public bool SnapshotVerified, TelemetryPinVerified;
        public string CanonicalTelemetryImagePath = "", Error = "", TelemetryPinError = "";
    }
    public static class NativeV1 {
        const uint QueryLimitedInformation = 0x1000, Synchronize = 0x100000;
        const uint WaitTimeout = 258, DuplicateSameAccess = 2;
        const int MaxMembers = 65536;
        [StructLayout(LayoutKind.Sequential)]
        struct FileTime { public uint Low, High; public ulong Value { get { return ((ulong)High << 32) | Low; } } }
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern SafeFileHandle OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint processId);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool IsProcessInJob(SafeFileHandle process, SafeFileHandle job, [MarshalAs(UnmanagedType.Bool)] out bool member);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool QueryFullProcessImageNameW(SafeFileHandle process, uint flags, StringBuilder path, ref uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool GetProcessTimes(SafeFileHandle process, out FileTime creation, out FileTime exit, out FileTime kernel, out FileTime user);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint WaitForSingleObject(SafeFileHandle handle, uint milliseconds);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool QueryInformationJobObject(SafeFileHandle job, int infoClass, IntPtr buffer, uint size, out uint returned);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern uint GetFinalPathNameByHandleW(SafeFileHandle file, StringBuilder path, uint size, uint flags);
        [DllImport("kernel32.dll")]
        static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool DuplicateHandle(IntPtr sourceProcess, IntPtr source, IntPtr targetProcess,
            out SafeFileHandle target, uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint options);

        static Exception Failed(string operation) {
            return new Win32Exception(Marshal.GetLastWin32Error(), operation);
        }
        static uint[] ProcessIds(SafeFileHandle job) {
            int capacity = 32;
            for (int attempt = 0; attempt < 12; ++attempt) {
                int bytes = checked(8 + capacity * IntPtr.Size);
                IntPtr buffer = Marshal.AllocHGlobal(bytes);
                try {
                    // JOBOBJECT_BASIC_PROCESS_ID_LIST: two DWORDs, then ULONG_PTR[].
                    Marshal.WriteInt32(buffer, 0, 0); Marshal.WriteInt32(buffer, 4, 0);
                    uint returned;
                    bool ok = QueryInformationJobObject(job, 3, buffer, (uint)bytes, out returned);
                    int error = ok ? 0 : Marshal.GetLastWin32Error();
                    uint assigned = unchecked((uint)Marshal.ReadInt32(buffer, 0));
                    uint count = unchecked((uint)Marshal.ReadInt32(buffer, 4));
                    if (assigned > MaxMembers || count > MaxMembers) throw new IOException("Job member limit exceeded.");
                    if (ok && assigned == count && count <= capacity) {
                        var ids = new uint[count];
                        for (int i = 0; i < ids.Length; ++i) {
                            ulong value = unchecked((ulong)Marshal.ReadIntPtr(buffer, 8 + i * IntPtr.Size).ToInt64());
                            if (value == 0 || value > UInt32.MaxValue) throw new IOException("Invalid Job process identifier.");
                            ids[i] = (uint)value;
                        }
                        if (ids.Distinct().Count() != ids.Length) throw new IOException("Duplicate Job process identifier.");
                        return ids.OrderBy(id => id).ToArray();
                    }
                    if (!ok && error != 234) throw new Win32Exception(error, "QueryInformationJobObject");
                    capacity = Math.Min(MaxMembers, Math.Max(capacity * 2, (int)assigned));
                } finally { Marshal.FreeHGlobal(buffer); }
            }
            throw new IOException("Job membership did not stabilize within the bounded query.");
        }
        static string FinalPath(FileStream file) {
            var text = new StringBuilder(32768);
            uint size = GetFinalPathNameByHandleW(file.SafeFileHandle, text, (uint)text.Capacity, 0);
            if (size == 0) throw Failed("GetFinalPathNameByHandleW");
            if (size >= text.Capacity) throw new IOException("Canonical image path exceeds limit.");
            // Retain the normalized extended DOS path for both sides of comparison.
            return text.ToString();
        }
        static string Digest(FileStream file) {
            file.Position = 0;
            using (SHA256 sha = SHA256.Create()) {
                return BitConverter.ToString(sha.ComputeHash(file)).Replace("-", "").ToLowerInvariant();
            }
        }
        static FileStream OpenImage(string path) {
            if (String.IsNullOrEmpty(path) || !Path.IsPathFullyQualified(path)) throw new IOException("Image path must be absolute.");
            // Deny concurrent replacement/writes while observing the exact opened file.
            return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        }
        static ulong Creation(SafeFileHandle process) {
            FileTime creation, exit, kernel, user;
            if (!GetProcessTimes(process, out creation, out exit, out kernel, out user)) throw Failed("GetProcessTimes");
            if (creation.Value == 0 || exit.Value != 0 || WaitForSingleObject(process, 0) != WaitTimeout)
                throw new IOException("Process exited or liveness is unknown.");
            return creation.Value;
        }
        static void CheckMember(SafeFileHandle process, SafeFileHandle job) {
            bool member;
            if (!IsProcessInJob(process, job, out member)) throw Failed("IsProcessInJob");
            if (!member) throw new IOException("Process is not in the exact owned Job.");
        }
        public static SnapshotV1 Capture(IntPtr borrowedJob, uint[] requestedIds, string telemetryPath, string telemetryHash) {
            var snapshot = new SnapshotV1();
            var records = new List<MemberV1>();
            var processes = new List<SafeFileHandle>();
            var images = new List<FileStream>();
            SafeFileHandle job = null;
            FileStream telemetry = null;
            try {
                if (borrowedJob == IntPtr.Zero || borrowedJob == new IntPtr(-1)) throw new IOException("No held Job handle supplied.");
                if (!DuplicateHandle(GetCurrentProcess(), borrowedJob, GetCurrentProcess(), out job, 0, false, DuplicateSameAccess))
                    throw Failed("DuplicateHandle(Job)");
                uint[] before = ProcessIds(job);
                uint[] supplied = requestedIds ?? new uint[0];
                if (supplied.Length > MaxMembers) throw new IOException("Supplied process identifier limit exceeded.");
                bool complete = supplied.Length == supplied.Distinct().Count() && before.SequenceEqual(supplied.OrderBy(id => id));
                if (!complete) snapshot.Error = "Supplied process IDs differ from the kernel Job inventory.";
                // Inspect the union: omitting a real member can never hide it.
                uint[] inspect = before.Union(supplied).OrderBy(id => id).ToArray();
                if (inspect.Length > MaxMembers) throw new IOException("Combined process identifier limit exceeded.");
                if (!String.IsNullOrEmpty(telemetryPath) || !String.IsNullOrEmpty(telemetryHash)) {
                    try {
                        if (String.IsNullOrEmpty(telemetryPath) || String.IsNullOrEmpty(telemetryHash) ||
                            telemetryHash.Length != 64 || telemetryHash.Any(c => !Uri.IsHexDigit(c)))
                            throw new IOException("Telemetry requires an independent absolute path and SHA-256 pin.");
                        telemetry = OpenImage(telemetryPath);
                        snapshot.CanonicalTelemetryImagePath = FinalPath(telemetry);
                        if (!String.Equals(Path.GetFileName(snapshot.CanonicalTelemetryImagePath), "VCTIP.EXE", StringComparison.OrdinalIgnoreCase) ||
                            !String.Equals(Path.GetFileName(telemetryPath), "VCTIP.EXE", StringComparison.OrdinalIgnoreCase))
                            throw new IOException("Only the exact pinned VCTIP.EXE image is eligible.");
                        if (!String.Equals(Digest(telemetry), telemetryHash, StringComparison.OrdinalIgnoreCase))
                            throw new IOException("Current telemetry image bytes do not match the independent pin.");
                        snapshot.TelemetryPinVerified = true;
                    } catch (Exception e) { snapshot.TelemetryPinError = e.Message; }
                } else { snapshot.TelemetryPinError = "No telemetry exception configured."; }
                foreach (uint id in inspect) {
                    var record = new MemberV1 { ProcessId = id };
                    records.Add(record);
                    processes.Add(null);
                    SafeFileHandle process = null;
                    try {
                        // PID is a lookup hint only. No terminate or modify rights are requested.
                        process = OpenProcess(QueryLimitedInformation | Synchronize, false, id);
                        processes[records.Count - 1] = process;
                        if (process.IsInvalid) throw Failed("OpenProcess");
                        CheckMember(process, job); record.JobMembershipVerified = true;
                        record.CreationTimeFileTime = Creation(process);
                        var path = new StringBuilder(32768); uint size = (uint)path.Capacity;
                        if (!QueryFullProcessImageNameW(process, 0, path, ref size)) throw Failed("QueryFullProcessImageNameW");
                        record.ImagePath = path.ToString();
                        FileStream image = OpenImage(record.ImagePath); images.Add(image);
                        record.CanonicalImagePath = FinalPath(image);
                        record.ImageSha256 = Digest(image);
                        CheckMember(process, job);
                        if (Creation(process) != record.CreationTimeFileTime) throw new IOException("Process creation identity changed.");
                        record.Running = true; record.MetadataVerified = true;
                    } catch (Exception e) { record.Error = e.Message; }
                }
                // All process and image handles remain held through the final inventory check.
                for (int i = 0; i < records.Count; ++i) {
                    if (!records[i].MetadataVerified) continue;
                    try {
                        CheckMember(processes[i], job);
                        if (Creation(processes[i]) != records[i].CreationTimeFileTime) throw new IOException("Process creation identity changed.");
                    } catch (Exception e) {
                        records[i].MetadataVerified = false; records[i].Running = false; records[i].Error = e.Message;
                    }
                }
                bool stable = before.SequenceEqual(ProcessIds(job));
                if (!stable) snapshot.Error = "Kernel Job membership changed during inspection.";
                snapshot.SnapshotVerified = complete && stable && records.All(r => r.MetadataVerified);
            } catch (Exception e) {
                snapshot.SnapshotVerified = false; snapshot.Error = e.Message;
                // A failed Job query is explicit unknown metadata for supplied
                // hints, never an apparently empty successful observation.
                var recorded = new HashSet<uint>(records.Select(r => r.ProcessId));
                foreach (uint id in (requestedIds ?? new uint[0]).Take(MaxMembers).Distinct()) {
                    if (recorded.Add(id)) records.Add(new MemberV1 { ProcessId = id, Error = "Job snapshot unavailable: " + e.Message });
                }
            } finally {
                snapshot.Members = records.ToArray();
                foreach (FileStream image in images) image.Dispose();
                foreach (SafeFileHandle process in processes) if (process != null) process.Dispose();
                if (telemetry != null) telemetry.Dispose();
                if (job != null) job.Dispose(); // Never closes the caller's original Job handle.
            }
            return snapshot;
        }
    }
}
'@
}

function Get-OwnedJobCleanupDisposition {
    <# Pure classifier for live metadata, not a source of process ownership.
       The two verification switches default to false; only a complete native
       observation and an independently supplied, verified pin may set them. #>
    [CmdletBinding()]
    param(
        [AllowNull()][AllowEmptyCollection()][object[]]$Members = @(),
        [AllowNull()][string]$TelemetryImagePath,
        [AllowNull()][string]$TelemetryImageSha256,
        [bool]$SnapshotVerified = $false,
        [bool]$TelemetryPinVerified = $false
    )
    $pinEligible = $TelemetryPinVerified -and $TelemetryImageSha256 -cmatch '^[0-9a-fA-F]{64}$' -and
        $TelemetryImagePath -match '^(?:[A-Za-z]:[\\/]|\\\\).*[\\/]VCTIP\.EXE$'
    $classified = @(foreach ($member in $Members) {
        $reason = 'UnverifiedMetadata'
        $classification = 'Unknown'
        try {
            # A dictionary or a native record may be supplied. Missing fields
            # and string-encoded booleans never become verified observations.
            $record = [pscustomobject]$member
            $properties = @{}
            foreach ($property in $record.PSObject.Properties) { $properties[$property.Name] = $property.Value }
            $verified = $true
            foreach ($flag in 'MetadataVerified', 'JobMembershipVerified', 'Running') {
                if ($properties[$flag] -isnot [bool] -or $properties[$flag] -ne $true) { $verified = $false }
            }
            $creation = 0UL
            $id = 0U
            $verified = $verified -and [uint64]::TryParse([string]$properties['CreationTimeFileTime'], [ref]$creation) -and $creation -gt 0 -and
                [uint32]::TryParse([string]$properties['ProcessId'], [ref]$id) -and $id -gt 0 -and
                -not [string]::IsNullOrEmpty([string]$properties['CanonicalImagePath']) -and
                [string]$properties['ImageSha256'] -cmatch '^[0-9a-fA-F]{64}$'
            if ($verified) {
                if (-not $SnapshotVerified) { $reason = 'UnverifiedJobSnapshot' }
                elseif (-not $pinEligible) { $reason = 'TelemetryNotPinned' }
                elseif (-not [string]::Equals([string]$properties['CanonicalImagePath'], $TelemetryImagePath, [StringComparison]::OrdinalIgnoreCase)) { $reason = 'ImagePathNotAllowed' }
                elseif (-not [string]::Equals([string]$properties['ImageSha256'], $TelemetryImageSha256, [StringComparison]::OrdinalIgnoreCase)) { $reason = 'ImageHashNotAllowed' }
                else { $classification = 'VerifiedBuildTelemetry'; $reason = 'ExactPinnedVctip' }
            }
            [pscustomobject]@{ ProcessId = $properties['ProcessId']; Classification = $classification; Reason = $reason; Metadata = $member }
        } catch {
            [pscustomobject]@{ ProcessId = $null; Classification = 'Unknown'; Reason = 'MalformedMetadata'; Metadata = $member }
        }
    })
    # In particular, vacuous truth over an empty Job must never grant an exception.
    $all = $SnapshotVerified -and $pinEligible -and $classified.Count -gt 0 -and
        @($classified | Where-Object Classification -ne 'VerifiedBuildTelemetry').Count -eq 0
    [pscustomobject]@{ Members = $classified; AllVerifiedTelemetry = [bool]$all; SnapshotVerified = $SnapshotVerified }
}

function Get-OwnedBuildJobMembers {
    <# Observe a caller-held kernel Job, with no name/PID-based kill authority.
       Pins must come from the controller's frozen compiler/tools manifest,
       never from the observed process or a hash learned during this call. #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][IntPtr]$Job,
        [Parameter(Mandatory)][AllowEmptyCollection()][uint32[]]$ProcessIds,
        [AllowNull()][string]$TelemetryImagePath,
        [AllowNull()][string]$TelemetryImageSha256
    )
    Initialize-OwnedBuildJobNative
    $snapshot = [OkuFlow.BuildCleanup.NativeV1]::Capture($Job, $ProcessIds, $TelemetryImagePath, $TelemetryImageSha256)
    $result = Get-OwnedJobCleanupDisposition -Members $snapshot.Members -SnapshotVerified $snapshot.SnapshotVerified `
        -TelemetryPinVerified $snapshot.TelemetryPinVerified -TelemetryImagePath $snapshot.CanonicalTelemetryImagePath `
        -TelemetryImageSha256 $TelemetryImageSha256
    $result | Add-Member NoteProperty NativeSnapshot $snapshot
    return $result
}
