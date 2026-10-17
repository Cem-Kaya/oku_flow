#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
# Load only the two production protocol functions. No OBS/process/socket or
# desktop is created by this fixture.
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'lecture_camera.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Lecture-camera script parse failed.'}
foreach($name in @('Invoke-ObsRequest','Wait-ObsCameraReady')) {
    $function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
    if(!$function){throw "Missing production function: $name"}
    Invoke-Expression $function.Extent.Text
}
function Connect-Obs($State) {$script:socket=$true}
function Send-ObsMessage($Message) {$script:last=$Message;$script:calls++}
function Receive-ObsMessage {
    $code=$script:codes.Dequeue()
    return @{op=7;d=@{requestId=$script:last.d.requestId;requestStatus=@{result=$code -eq 100;code=$code;comment='fixture'};responseData=@{outputActive=$false}}}
}
function Reset-Fixture([int[]]$Codes) {
    $script:socket=$true;$script:calls=0;$script:codes=[Collections.Generic.Queue[int]]::new()
    foreach($code in $Codes){$script:codes.Enqueue($code)}
}
Reset-Fixture @(207,207,100)
$result=Wait-ObsCameraReady @{} ([pscustomobject]@{HasExited=$false}) ([DateTime]::UtcNow.AddSeconds(2))
if($script:calls -ne 3 -or $result.outputActive){throw 'NotReady probes did not converge to the actual inactive output.'}
Write-Host 'PASS: authenticated-but-not-ready output waits for typed readiness'
Reset-Fixture @(600,100)
$rejected=$false
try{Wait-ObsCameraReady @{} ([pscustomobject]@{HasExited=$false}) ([DateTime]::UtcNow.AddSeconds(2))|Out-Null}
catch{if($_.Exception.Data['ObsStatusCode'] -ne 600){throw};$rejected=$true}
if(!$rejected -or $script:calls -ne 1){throw 'A permanent output error was retried.'}
Write-Host 'PASS: permanent output errors remain failures'
Reset-Fixture @(100)
$rejected=$false
try{Wait-ObsCameraReady @{} ([pscustomobject]@{HasExited=$true}) ([DateTime]::UtcNow.AddSeconds(2))|Out-Null}
catch{$rejected=$true}
if(!$rejected -or $script:calls){throw 'An exited owner was probed.'}
Reset-Fixture @(100)
$rejected=$false
try{Wait-ObsCameraReady @{} ([pscustomobject]@{HasExited=$false}) ([DateTime]::UtcNow.AddSeconds(-1))|Out-Null}
catch{$rejected=$true}
if(!$rejected -or $script:calls){throw 'An expired deadline was ignored.'}
Write-Host 'PASS: exited owner and expired deadline refuse readiness probes'
