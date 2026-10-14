#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('all', 'gui-all', 'gui-state', 'gui-layout', 'gui-input', 'gui-idle', 'two-cases')]
    [string]$Scope = 'all'
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
}
'@
$desktop = [ordered]@{
    runner = $env:RUNNER_NAME
    session = [Diagnostics.Process]::GetCurrentProcess().SessionId
    interactive = [Environment]::UserInteractive
    inputDesktop = [OkuFlowCiDesktop]::InputName()
    scope = $Scope
}
$desktop | ConvertTo-Json | Set-Content -LiteralPath "$env:OKUFLOW_CI_EVIDENCE/desktop.json" -Encoding utf8
if (!$desktop.interactive -or $desktop.session -eq 0 -or $desktop.inputDesktop -ne 'Default') {
    throw 'No unlocked interactive input desktop is available. Native GUI checks cannot pass here; use a dedicated remote interactive Windows runner.'
}
if ($env:QT_QPA_PLATFORM -and $env:QT_QPA_PLATFORM -ne 'windows') {
    throw 'Native GUI checks require the Windows Qt platform, not offscreen/minimal.'
}

$inventory = & ctest --preset msvc-cpu-tests --show-only=json-v1
if ($LASTEXITCODE) { throw 'CTest inventory discovery failed.' }
$cases = @(($inventory -join "`n" | ConvertFrom-Json).tests)
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
    # The earlier background invocation retains results for the identical CPU
    # inventory. Fill only its GUI entries; require every CPU entry to resolve.
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
