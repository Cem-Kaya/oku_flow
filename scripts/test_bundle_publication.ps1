#requires -Version 7.0
# Private synthetic generations only. No build, GUI, signing or real dist writes.
[CmdletBinding()]
param([ValidateSet('All','Exchange','Refusals','Rollback')][string]$CaseSet = 'All')
$ErrorActionPreference = 'Stop'
$work = Join-Path $env:LOCALAPPDATA ('OkuFlow/bundle-publication-fixtures/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$publisher = Join-Path $PSScriptRoot 'publish_release_bundle.ps1'
$script:caseWatch = $null
$script:caseName = ''
$script:completedCases = 0
function Complete-Case {
    if ($script:caseWatch) {
        $script:caseWatch.Stop()
        ++$script:completedCases
        Write-Host ("PASS bundle publication: {0} ({1:F2} s)" -f $script:caseName,$script:caseWatch.Elapsed.TotalSeconds)
        $script:caseWatch = $null
    }
}
function Assert-Publication([bool]$Value,[string]$Message) { if (-not $Value) { throw $Message } }
function Assert-Refused([scriptblock]$Action,[string]$Message) {
    $refused = $false
    try { & $Action | Out-Null } catch { $refused = $true }
    Assert-Publication $refused $Message
}
function New-Bundle([string]$Root,[string]$Generation) {
    New-Item -ItemType Directory -Path (Join-Path $Root 'app') -Force | Out-Null
    foreach ($name in @('oku_flow.exe','app/OkuFlowApp.exe','app/Qt6Core.dll','app/okuflow_mcp.exe')) {
        [IO.File]::WriteAllText((Join-Path $Root $name),"synthetic $Generation $name")
    }
    $records = @(Get-ChildItem -LiteralPath $Root -File -Recurse | ForEach-Object {
        @{path=[IO.Path]::GetRelativePath($Root,$_.FullName).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
    })
    @{schemaVersion=2;product='OkuFlow';files=$records} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Root 'release-manifest.json')
    foreach ($name in @('SHA256SUMS.txt','SBOM.spdx.json')) { [IO.File]::WriteAllText((Join-Path $Root $name),'synthetic metadata') }
    return $Root
}
function New-Case([string]$Name) {
    Complete-Case
    $script:caseName = $Name
    $script:caseWatch = [Diagnostics.Stopwatch]::StartNew()
    Write-Host "BEGIN bundle publication: $Name"
    $dist = Join-Path $work $Name
    New-Item -ItemType Directory -Path $dist | Out-Null
    return @{dist=$dist;stage=(Join-Path $dist ('OkuFlow.staging-' + [Guid]::NewGuid().ToString('N')));primary=(Join-Path $dist 'OkuFlow');secondary=(Join-Path $dist 'OkuFlow2')}
}
function Get-Inventory([string]$Root) {
    return @(Get-ChildItem -LiteralPath $Root -Force -File -Recurse | Sort-Object FullName | ForEach-Object {
        [IO.Path]::GetRelativePath($Root,$_.FullName) + ':' + (Get-FileHash -LiteralPath $_.FullName).Hash
    }) -join "`n"
}
function Invoke-Publish($Case) { & $publisher -DistDirectory $Case.dist -StagingDirectory $Case.stage }

if ($CaseSet -in @('All','Exchange')) {
$case = New-Case 'fresh-primary'
& $publisher -DistDirectory $case.dist -StagingDirectory $case.stage -Action CreateStage
Assert-Refused { & $publisher -DistDirectory $case.dist -StagingDirectory $case.stage -Action CreateStage } 'Existing stage accepted as newly owned.'
$null = New-Bundle $case.stage 'new'
$new = Get-Inventory $case.stage
Assert-Publication ((Invoke-Publish $case) -eq $case.primary) 'Fresh stage did not publish to primary.'
Assert-Publication ((Get-Inventory $case.primary) -eq $new) 'Fresh publication changed bytes.'
$case.stage = Join-Path $case.dist ('OkuFlow.staging-' + [Guid]::NewGuid().ToString('N'))
$null = New-Bundle $case.stage 'replacement'
$new = Get-Inventory $case.stage
Invoke-Publish $case | Out-Null
Assert-Publication ((Get-Inventory $case.primary) -eq $new) 'Primary replacement changed bytes.'

$case = New-Case 'locked-secondary'
$null = New-Bundle $case.primary 'primary'
[IO.File]::WriteAllText((Join-Path $case.primary 'output'),'owner capture must remain')
$null = New-Bundle $case.secondary 'old'
$null = New-Bundle $case.stage 'new'
$old = Get-Inventory $case.secondary
$primary = Get-Inventory $case.primary
$lock = [IO.File]::Open((Join-Path $case.secondary 'app/Qt6Core.dll'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try { Assert-Refused { Invoke-Publish $case } 'Locked secondary DLL was accepted.' }
finally { $lock.Dispose() }
Assert-Publication ((Get-Inventory $case.secondary) -eq $old) 'Locked secondary was partially removed.'
Assert-Publication ((Get-Inventory $case.primary) -eq $primary) 'Primary owner data was moved/deleted.'
$new = Get-Inventory $case.stage
Assert-Publication ((Invoke-Publish $case) -eq $case.secondary) 'Unlocked replacement did not select OkuFlow2.'
Assert-Publication ((Get-Inventory $case.secondary) -eq $new) 'Secondary replacement changed bytes.'

$case = New-Case 'running-secondary'
$null = New-Bundle $case.primary 'primary'
[IO.File]::WriteAllText((Join-Path $case.primary 'output'),'owner data')
$null = New-Bundle $case.secondary 'old'
$null = New-Bundle $case.stage 'new'
# A private copied Windows command interpreter stays hidden on redirected stdin.
# It opens no GUI, camera or owner process and exits normally via its own stdin.
$image = Join-Path $case.secondary 'app/OkuFlowApp.exe'
Copy-Item -LiteralPath $env:ComSpec -Destination $image -Force
$manifestPath = Join-Path $case.secondary 'release-manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$record = $manifest.files | Where-Object path -eq 'app/OkuFlowApp.exe'
$record.bytes = (Get-Item -LiteralPath $image).Length
$record.sha256 = (Get-FileHash -LiteralPath $image).Hash.ToLowerInvariant()
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
$before = Get-Inventory $case.secondary
$start = [Diagnostics.ProcessStartInfo]::new($image)
$start.ArgumentList.Add('/d'); $start.ArgumentList.Add('/q')
$start.UseShellExecute = $false; $start.CreateNoWindow = $true
$start.RedirectStandardInput = $true; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
$child = [Diagnostics.Process]::Start($start)
try {
    Assert-Publication (-not $child.HasExited) 'Private running-image fixture exited before the probe.'
    Assert-Refused { Invoke-Publish $case } 'Running secondary image accepted.'
    Assert-Publication ((Get-Inventory $case.secondary) -eq $before) 'Running secondary was changed.'
} finally {
    $child.StandardInput.WriteLine('exit')
    Assert-Publication ($child.WaitForExit(5000)) 'Private interpreter did not exit normally.'
    $child.Dispose()
}
}

if ($CaseSet -in @('All','Refusals')) {
foreach ($intrusion in @('output','captures','unknown.txt','empty-directory','tampered-runtime')) {
    $case = New-Case ('refuse-' + $intrusion)
    $null = New-Bundle $case.primary 'primary'
    [IO.File]::WriteAllText((Join-Path $case.primary 'output'),'owner data')
    $null = New-Bundle $case.secondary 'old'
    $null = New-Bundle $case.stage 'new'
    switch ($intrusion) {
        'empty-directory' { New-Item -ItemType Directory -Path (Join-Path $case.secondary 'unknown') | Out-Null }
        'tampered-runtime' { [IO.File]::AppendAllText((Join-Path $case.secondary 'app/Qt6Core.dll'),'changed') }
        default { [IO.File]::WriteAllText((Join-Path $case.secondary $intrusion),'preserve owner bytes') }
    }
    $before = Get-Inventory $case.secondary
    Assert-Refused { Invoke-Publish $case } "Unowned/tampered secondary accepted: $intrusion"
    Assert-Publication ((Get-Inventory $case.secondary) -eq $before) "Refusal changed secondary: $intrusion"
    Assert-Publication (Test-Path -LiteralPath $case.stage) 'Refusal consumed the new stage.'
}
$case = New-Case 'bad-stage'
$null = New-Bundle $case.primary 'old'
$null = New-Bundle $case.stage 'new'
[IO.File]::AppendAllText((Join-Path $case.stage 'app/Qt6Core.dll'),'tampered')
$before = Get-Inventory $case.primary
Assert-Refused { Invoke-Publish $case } 'Tampered stage accepted.'
Assert-Publication ((Get-Inventory $case.primary) -eq $before) 'Invalid stage touched old primary.'
Assert-Refused { & $publisher -DistDirectory $case.dist -StagingDirectory $case.primary -Action DiscardStage } 'Owner directory accepted for staging cleanup.'
$escape = Join-Path $work ('OkuFlow.staging-' + [Guid]::NewGuid().ToString('N'))
Assert-Refused { & $publisher -DistDirectory $case.dist -StagingDirectory $escape } 'Stage outside dist accepted.'

$case = New-Case 'reparse-secondary'
$null = New-Bundle $case.primary 'primary'
[IO.File]::WriteAllText((Join-Path $case.primary 'output'),'owner data')
$null = New-Bundle $case.secondary 'old'
$null = New-Bundle $case.stage 'new'
$foreign = Join-Path $work 'foreign-data'
New-Item -ItemType Directory -Path $foreign | Out-Null
[IO.File]::WriteAllText((Join-Path $foreign 'keep.txt'),'foreign owner bytes')
New-Item -ItemType Junction -Path (Join-Path $case.secondary 'foreign') -Target $foreign | Out-Null
Assert-Refused { Invoke-Publish $case } 'Junction secondary accepted.'
Assert-Publication ((Get-Content -LiteralPath (Join-Path $foreign 'keep.txt') -Raw) -eq 'foreign owner bytes') 'Junction owner data changed.'
$linkedDist = Join-Path $work 'foreign-dist-junction'
New-Item -ItemType Junction -Path $linkedDist -Target $foreign | Out-Null
Assert-Refused { & $publisher -DistDirectory $linkedDist -StagingDirectory (Join-Path $linkedDist ('OkuFlow.staging-' + [Guid]::NewGuid().ToString('N'))) -Action CreateStage } 'Stage creation followed a dist junction.'
Assert-Publication (@(Get-ChildItem -LiteralPath $foreign -Force).Count -eq 1) 'Stage creation wrote through a dist junction.'
}

if ($CaseSet -in @('All','Rollback')) {
# Load the actual functions without invoking production entrypoint. Inject
# failures only in this private fixture, never through a production bypass.
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($publisher,[ref]$tokens,[ref]$parseErrors)
Assert-Publication (-not $parseErrors) 'Publisher parse failed.'
$functions = $ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$false)
. ([scriptblock]::Create(($functions.Extent.Text -join "`n")))
$realMove = ${function:Move-BundleDirectory}
$realVerify = ${function:Assert-RenamedBundle}
foreach ($failure in @('rename-new','verify-new')) {
    $case = New-Case $failure
    $null = New-Bundle $case.secondary 'old'
    $null = New-Bundle $case.stage 'new'
    $oldBytes = Get-Inventory $case.secondary
    $newBytes = Get-Inventory $case.stage
    $script:dist = $case.dist; $script:stage = $case.stage
    $old = Open-VerifiedBundle $case.secondary; $new = Open-VerifiedBundle $case.stage
    try {
        if ($failure -eq 'rename-new') {
            $script:injected = $false
            function Move-BundleDirectory([string]$Source,[string]$Destination) {
                if ($Source -eq $script:stage -and -not $script:injected) { $script:injected = $true; throw 'private injected rename failure' }
                & $realMove $Source $Destination
            }
        } else { function Assert-RenamedBundle { throw 'private injected verification failure' } }
        Assert-Refused { Switch-Bundle $new $case.secondary $old } "Injected failure succeeded: $failure"
    } finally {
        Close-Bundle $old; Close-Bundle $new
        ${function:Move-BundleDirectory} = $realMove
        ${function:Assert-RenamedBundle} = $realVerify
    }
    Assert-Publication ((Get-Inventory $case.secondary) -eq $oldBytes) "Old generation not restored after $failure"
    Assert-Publication ((Get-Inventory $case.stage) -eq $newBytes) "New generation not retained after $failure"
}
$case = New-Case 'late-owner-directory'
$null = New-Bundle $case.secondary 'old'
$null = New-Bundle $case.stage 'new'
$script:dist = $case.dist; $script:stage = $case.stage
$old = Open-VerifiedBundle $case.secondary; $new = Open-VerifiedBundle $case.stage
try {
    New-Item -ItemType Directory -Path (Join-Path $case.secondary 'late-owner-data') | Out-Null
    Assert-Refused { Switch-Bundle $new $case.secondary $old } 'Late owner directory was moved into a backup.'
} finally { Close-Bundle $old; Close-Bundle $new }
Assert-Publication (Test-Path -LiteralPath (Join-Path $case.secondary 'late-owner-data')) 'Late owner directory was not retained at its original path.'
Assert-Publication (Test-Path -LiteralPath $case.stage) 'Late owner directory refusal consumed staging.'
}
Complete-Case
$expectedCases = @{ All=13; Exchange=3; Refusals=7; Rollback=3 }[$CaseSet]
Assert-Publication ($script:completedCases -eq $expectedCases) "Incomplete scenario coverage for $CaseSet : $script:completedCases of $expectedCases"
Write-Output "PASS: bundle publication $CaseSet, all $expectedCases scenarios completed. Synthetic fixtures: $work"
