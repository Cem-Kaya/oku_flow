#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('all', 'gui-all', 'gui-state', 'gui-layout', 'gui-input', 'gui-idle', 'two-cases')]
    [string]$Scope = 'all',
    [switch]$PrepareDesktop,
    [ValidateNotNullOrEmpty()][string[]]$Test
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
if ($env:GITHUB_ACTIONS -ne 'true' -or !$IsWindows) {
    throw 'This entry point is only for the GitHub Windows VM. Do not run it on the owner desktop.'
}
if (!$env:OKUFLOW_CI_EVIDENCE) { throw 'OKUFLOW_CI_EVIDENCE is required.' }
$repo = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $repo
[void][IO.Directory]::CreateDirectory($env:OKUFLOW_CI_EVIDENCE)

# Inspect the actual input desktop; offscreen Qt would not validate native focus,
# pointer or idle behavior. Do not unlock or alter a runner's Windows session.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class OkuFlowCiDesktop {
    [StructLayout(LayoutKind.Explicit, Size = 220)]
    struct DisplayMode {
        [FieldOffset(68)] public ushort Size;
        [FieldOffset(72)] public uint Fields;
        [FieldOffset(172)] public uint Width;
        [FieldOffset(176)] public uint Height;
    }
    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettings(string device, int mode, ref DisplayMode data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int ChangeDisplaySettings(ref DisplayMode data, uint flags);
    [DllImport("user32.dll", SetLastError = true)]
    static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool GetUserObjectInformation(IntPtr handle, int index, StringBuilder data, uint length, out uint needed);
    [DllImport("user32.dll")]
    static extern bool CloseDesktop(IntPtr handle);
    public static string InputName() {
        var handle = OpenInputDesktop(0, false, 0x0001);
        if (handle == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        try {
            var name = new StringBuilder(256);
            uint needed;
            if (!GetUserObjectInformation(handle, 2, name, 512, out needed))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return name.ToString();
        } finally { CloseDesktop(handle); }
    }
    public static int EnsureDisplaySize() {
        if (GetSystemMetrics(0) >= 1920 && GetSystemMetrics(1) >= 1080) return 0;
        var mode = new DisplayMode { Size = 220 };
        if (!EnumDisplaySettings(null, -1, ref mode)) return -100;
        mode.Fields = 0x180000; // DM_PELSWIDTH | DM_PELSHEIGHT
        mode.Width = 1920;
        mode.Height = 1080;
        int tested = ChangeDisplaySettings(ref mode, 2); // CDS_TEST
        return tested == 0 ? ChangeDisplaySettings(ref mode, 0) : tested;
    }
}
'@
$desktop = [ordered]@{
    runner = $env:RUNNER_NAME
    session = [Diagnostics.Process]::GetCurrentProcess().SessionId
    interactive = [Environment]::UserInteractive
    inputDesktop = [OkuFlowCiDesktop]::InputName()
    scope = $Scope
    width = [OkuFlowCiDesktop]::GetSystemMetrics(0)
    height = [OkuFlowCiDesktop]::GetSystemMetrics(1)
}
$desktop | ConvertTo-Json | Set-Content -LiteralPath "$env:OKUFLOW_CI_EVIDENCE/desktop.json" -Encoding utf8
if (!$desktop.interactive -or $desktop.session -eq 0 -or $desktop.inputDesktop -ne 'Default') {
    throw 'No unlocked interactive input desktop is available. Native GUI checks cannot pass here; use a dedicated remote interactive Windows runner.'
}
if ($env:QT_QPA_PLATFORM -and $env:QT_QPA_PLATFORM -ne 'windows') {
    throw 'Native GUI checks require the Windows Qt platform, not offscreen/minimal.'
}
if ($PrepareDesktop) {
    $desktop.changeResult = [OkuFlowCiDesktop]::EnsureDisplaySize()
    $desktop.width = [OkuFlowCiDesktop]::GetSystemMetrics(0)
    $desktop.height = [OkuFlowCiDesktop]::GetSystemMetrics(1)
    $desktop | ConvertTo-Json | Set-Content -LiteralPath "$env:OKUFLOW_CI_EVIDENCE/desktop-preparation.json" -Encoding utf8
}
if ($desktop.width -lt 1920 -or $desktop.height -lt 1080) {
    throw 'The native GUI fixtures require a 1920x1080 remote desktop. The runner display is too small; do not weaken layout assertions.'
}
if ($PrepareDesktop) { exit 0 }

$inventory = & ctest --preset msvc-cpu-tests --show-only=json-v1
if ($LASTEXITCODE) { throw 'CTest inventory discovery failed.' }
$cases = @(($inventory -join "`n" | ConvertFrom-Json).tests)
if ($Test.Count) {
    $missing = @($Test | Where-Object { $_ -notin $cases.name })
    $nonGui = @($cases | Where-Object {
        $_.name -in $Test -and 'gui' -notin @($_.properties | Where-Object name -eq LABELS | ForEach-Object value)
    })
    if ($missing.Count -or $nonGui.Count) { throw 'Exact GUI selection contains unknown or non-GUI tests.' }
    $Test | Set-Content -LiteralPath "$env:OKUFLOW_CI_EVIDENCE/gui-selection.txt" -Encoding utf8
    Write-Host "SCOPED GUI: $($Test.Count) selected cases; not the release gate."
    & "$PSScriptRoot/run_test_gate.ps1" -Preset msvc-cpu-tests -Test $Test
    exit $LASTEXITCODE
}
$two = @(
    'annotation_overlay_interactions.dockedHeaderRequiresHeldPullAndDoesNotImmediatelyRedock',
    'main_window_interactions.mouseFocusedChromeCanIdleFade'
)
$selected = @($cases | Where-Object {
    $labels = @($_.properties | Where-Object name -eq LABELS | ForEach-Object value)
    if ($Scope -eq 'two-cases') { return $_.name -in $two }
    if ($Scope -in 'all', 'gui-all') { return 'gui' -in $labels }
    return $Scope -in $labels
})
if (!$selected.Count -or ($Scope -eq 'two-cases' -and $selected.Count -ne 2)) {
    throw "Missing registered GUI cases for $Scope."
}
$selected.name | Set-Content -LiteralPath "$env:OKUFLOW_CI_EVIDENCE/gui-selection.txt" -Encoding utf8
if ($Scope -eq 'all') {
    # Full CI runs every background case first, then every native GUI case.
    # Keep both selections serial and retain their independent gate evidence.
    & "$PSScriptRoot/run_test_gate.ps1" -Preset msvc-cpu-tests -Test $selected.name
    exit $LASTEXITCODE
}

# A narrow manual diagnostic retains failures, but gives no full-gate credit.
Write-Host "PARTIAL DIAGNOSTIC: $($selected.Count) GUI cases, CPU build only."
$expression = '^(' + (($selected.name | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'
& ctest --preset msvc-cpu-tests --parallel 1 --output-on-failure --no-tests=error `
    -R $expression --output-junit "$env:OKUFLOW_CI_EVIDENCE/gui.xml" 2>&1 |
    Tee-Object -FilePath "$env:OKUFLOW_CI_EVIDENCE/gui.log" | ForEach-Object { Write-Host $_ }
exit $LASTEXITCODE
