#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Fixtures','Prepare','Measure')][string]$Mode,
    [Parameter(Mandatory)][string]$ArtifactRoot,
    [string]$Executable,
    [string]$Sdk,
    [Parameter(Mandatory)][string[]]$Configurations,
    [ValidateSet('selected','neighbours','calibration')][string]$Images = 'selected',
    [string]$FramesRoot = (Join-Path $env:LOCALAPPDATA 'OkuFlow/uct-paused-geometry-20261021/frames'),
    [switch]$Frozen,
    [switch]$ExportCalibration,
    [switch]$AllowContended,
    [ValidateRange(1,3)][int]$Passes = 3,
    [string]$Output
)
$ErrorActionPreference='Stop'
$Configurations=@($Configurations|ForEach-Object {$_.Split(',', [StringSplitOptions]::RemoveEmptyEntries)})
$ArtifactRoot=(Resolve-Path -LiteralPath $ArtifactRoot).Path
if ($Configurations.Count -gt 5 -or !$Configurations.Count -or @($Configurations|Select-Object -Unique).Count -ne $Configurations.Count) {
    throw 'Select one to five distinct explicit configurations.'
}
if ($Mode -eq 'Fixtures') {
    $original=(Get-Content -LiteralPath "$ArtifactRoot/original-audit.json" -Raw|ConvertFrom-Json).records
    $manifestRoot=Join-Path $ArtifactRoot 'manifests'
    [IO.Directory]::CreateDirectory($manifestRoot)|Out-Null
    foreach ($configuration in $Configurations) {
        if ($configuration -ne 'baseline' -and $configuration -notmatch '^(fp16-linear|bf16-linear|int4-woq|fp8-qdq|int8-qdq)-(rec|both)$') {
            throw "Unknown configuration: $configuration"
        }
        $format=if($configuration -eq 'baseline'){'fp32'}else{$Matches[1]}
        $both=$configuration.EndsWith('-both')
        $graphs=@{}
        foreach ($role in @('det','rec')) {
            $record=@($original|Where-Object {$_.role -eq $role -and $_.tier -eq 'medium'})[0]
            if ($format -ne 'fp32' -and ($role -eq 'rec' -or $both)) {
                $derived=Join-Path $ArtifactRoot "models/$format/$($record.sha256)/manifest.json"
                $record=Get-Content -LiteralPath $derived -Raw|ConvertFrom-Json
            }
            if ((Get-FileHash -LiteralPath $record.path).Hash.ToLowerInvariant() -ne $record.sha256) { throw 'Graph hash mismatch.' }
            $graphs[$role]=@{precision=$record.precision;path=$record.path;sourceSha256=$record.sourceSha256;sha256=$record.sha256}
        }
        foreach ($prepare in @($false,$true)) {
            if ($configuration -eq 'baseline' -and $prepare) {continue}
            $suffix=if($prepare){'prepare'}else{'cached'}
            $path=Join-Path $manifestRoot "$configuration-$suffix.json"
            if(Test-Path -LiteralPath $path){throw "Retain existing manifest: $path"}
            @{schema=1;prepare=$prepare;artifactRoot=$ArtifactRoot;receiptRoot="$ArtifactRoot/receipts";
              detector=$graphs.det;recognizer=$graphs.rec}|ConvertTo-Json -Depth 6|
                Set-Content -LiteralPath $path -Encoding utf8NoBOM
        }
    }
    return
}
if (!$Executable -or !$Sdk -or !$Output -or !(Split-Path -IsAbsolute $Output) -or (Test-Path -LiteralPath $Output)) {
    throw 'Explicit executable/SDK and a fresh absolute output directory are required.'
}
if ($Mode -eq 'Prepare' -and $Configurations.Count -ne 1) {throw 'Prepare exactly one explicit pair; no cold-build loop.'}
if ($Mode -eq 'Prepare' -and $Frozen) {throw 'Preparation does not run images.'}
if ($Mode -eq 'Measure' -and $Frozen -and $Configurations[0] -ne 'baseline') {throw 'Frozen work must begin with the baseline.'}
if ($ExportCalibration -and ($Images -ne 'calibration' -or !$Frozen -or $Configurations.Count -ne 1 -or $Configurations[0] -ne 'baseline')) {
    throw 'Calibration export requires separate calibration images and frozen baseline only.'
}
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$Sdk=(Resolve-Path -LiteralPath $Sdk).Path
$suffix=if($Mode -eq 'Prepare'){'prepare'}else{'cached'}
$manifests=@($Configurations|ForEach-Object {(Resolve-Path -LiteralPath "$ArtifactRoot/manifests/$_-$suffix.json").Path})
$paths=@()
if ($Mode -eq 'Measure') {
    $names=if($Images -eq 'selected'){@('menu_001.png','menu_003.png','book_001.png','atlas_0.png')}
        elseif($Images -eq 'neighbours'){@('menu_002.png','menu_004.png','book_002.png','atlas_50.png')}
        else{@('calibration_001.png','calibration_002.png','calibration_003.png','calibration_004.png')}
    $paths=@($names|ForEach-Object {(Resolve-Path -LiteralPath (Join-Path $FramesRoot $_)).Path})
}
# Same global ownership contract as run_test_gate and compare_video_backends.
# Refuse immediately: a parent may own the runner even before acquiring this file.
$lockPath=Join-Path $env:LOCALAPPDATA 'OkuFlow/test-evidence/runner.lock'
[IO.Directory]::CreateDirectory((Split-Path $lockPath))|Out-Null
$lease=[IO.File]::Open($lockPath,'OpenOrCreate','ReadWrite','None')
try {
    $active=@(Get-CimInstance Win32_Process|Where-Object {$_.ProcessId -ne $PID -and
        $_.Name -in @('cmd.exe','pwsh.exe','powershell.exe') -and
        $_.CommandLine -match '(?i)(agent_build\.bat|[ /]run_test_gate\.ps1)'})
    if($active.Count -and !$AllowContended){throw 'A tracked gate is active; do not overlap or stop it.'}
    [IO.Directory]::CreateDirectory($Output)|Out-Null
    $start=[Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.ArgumentList.Add('optionalTensorRtPrecision')
    $start.ArgumentList.Add('-o');$start.ArgumentList.Add("$Output/qttest.txt,txt")
    $values=@{
        QT_QPA_PLATFORM='offscreen';QT_FORCE_STDERR_LOGGING='1';QTEST_FUNCTION_TIMEOUT='1200000'
        OKUFLOW_UCT_PRECISION_DIAGNOSTIC='1';OKUFLOW_UCT_PRECISION_MANIFESTS=($manifests -join ';')
        OKUFLOW_UCT_PRECISION_OUTPUT="$Output/raw";OKUFLOW_UCT_TENSORRT_ROOT=$Sdk
        OKUFLOW_UCT_ROOT=(Join-Path $env:LOCALAPPDATA 'OkuFlow/ocr-runtime')
        OKUFLOW_UCT_RESIDENT_PAIRS='medium:medium';OKUFLOW_UCT_RESIDENT_CAPACITIES='1,8'
        OKUFLOW_UCT_RESIDENT_ROUNDS='1';OKUFLOW_UCT_RESIDENT_PASSES=[string]$Passes
        OKUFLOW_UCT_RESIDENT_IMAGES=($paths -join ';');OKUFLOW_UCT_RESIDENT_PROFILE='1'
        OKUFLOW_UCT_RESIDENT_WIDTH='fixed1024';OKUFLOW_UCT_RESIDENT_STAMP='media'
        OKUFLOW_UCT_RESIDENT_INPUT='single';OKUFLOW_UCT_RESIDENT_FROZEN_CROPS=$(if($Frozen){'1'}else{'0'})
        OKUFLOW_UCT_RESIDENT_SHADOW_PLANNER='0'
        OKUFLOW_UCT_PRECISION_CALIBRATION_EXPORT=$(if($ExportCalibration){'1'}else{'0'})
        OKUFLOW_UCT_PRECISION_NVIDIA_SMI=(Get-Command nvidia-smi -ErrorAction Stop).Source
        OKUFLOW_UCT_PRECISION_CONTENDED=$(if($AllowContended){'1'}else{'0'})
    }
    foreach($key in $values.Keys){$start.Environment[$key]=[string]$values[$key]}
    $start.Environment['PATH']='C:\Qt\6.12.0\msvc2022_64\bin;'+$start.Environment['PATH']
    @{mode=$Mode;configurations=$Configurations;images=$Images;frozen=[bool]$Frozen;passes=$Passes;
      executable=$Executable;executableSha256=(Get-FileHash -LiteralPath $Executable).Hash;
      manifests=@($manifests|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash}});
      sources=@($paths|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash}});
      contended=[bool]$AllowContended;activeTrackedProcesses=@($active|Select-Object ProcessId,Name);
      runnerAcquiredUtc=[DateTime]::UtcNow.ToString('o');
      utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content "$Output/invocation.json" -Encoding utf8NoBOM
    Write-Host ('UCT precision runner acquired '+[DateTime]::UtcNow.ToString('o'))
    $process=[Diagnostics.Process]::Start($start)
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $clock=[Diagnostics.Stopwatch]::StartNew();[long]$peak=0
    while(!$process.WaitForExit(250)){$process.Refresh();$peak=[Math]::Max($peak,$process.PeakWorkingSet64)}
    $stdout.GetAwaiter().GetResult()|Set-Content "$Output/stdout.txt" -Encoding utf8NoBOM
    $stderr.GetAwaiter().GetResult()|Set-Content "$Output/stderr.txt" -Encoding utf8NoBOM
    @{exitCode=$process.ExitCode;wallMs=$clock.ElapsedMilliseconds;peakWorkingSetBytes=$peak;runnerReleasedUtc=[DateTime]::UtcNow.ToString('o')}|
        ConvertTo-Json|Set-Content "$Output/process.json" -Encoding utf8NoBOM
    Get-Content "$Output/qttest.txt" -Tail 8
    if($process.ExitCode){throw 'Precision diagnostic failed; keep all raw evidence and do not retry unchanged work.'}
} finally {
    if($process){$process.Dispose()}
    $lease.Dispose()
    Write-Host 'UCT precision runner lease released.'
}
