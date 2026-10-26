#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
# Load only the production protocol functions. No OBS/process/socket or
# desktop is created by this fixture.
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'lecture_camera.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Lecture-camera script parse failed.'}
foreach($name in @('Invoke-ObsRequest','Wait-ObsCameraReady','Wait-ObsMediaReady')) {
    $function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
    if(!$function){throw "Missing production function: $name"}
    Invoke-Expression $function.Extent.Text
}
function Connect-Obs($State) {$script:socket=$true}
function Send-ObsMessage($Message) {$script:last=$Message;$script:calls++}
function Receive-ObsMessage {
    $code=$script:codes.Dequeue()
    $data=@{outputActive=$false}
    if($script:last.d.requestType -eq 'GetMediaInputStatus' -and $code -eq 100){$data=$script:media.Dequeue()}
    return @{op=7;d=@{requestId=$script:last.d.requestId;requestStatus=@{result=$code -eq 100;code=$code;comment='fixture'};responseData=$data}}
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

Reset-Fixture @(207,100,100)
$script:media=[Collections.Generic.Queue[object]]::new()
$script:media.Enqueue(@{mediaDuration=0;mediaState='OBS_MEDIA_STATE_OPENING'})
$script:media.Enqueue(@{mediaDuration=12000;mediaState='OBS_MEDIA_STATE_PLAYING'})
$result=Wait-ObsMediaReady ([pscustomobject]@{HasExited=$false}) ([DateTime]::UtcNow.AddSeconds(2))
if($script:calls -ne 3 -or $result.mediaDuration -ne 12000){throw 'Media readiness did not wait for the actual decoder duration.'}
Write-Host 'PASS: typed NotReady and opening media wait for positive duration'

Reset-Fixture @(100,100)
$script:media=[Collections.Generic.Queue[object]]::new()
$script:media.Enqueue(@{mediaDuration=0;mediaState='OBS_MEDIA_STATE_ERROR'})
$rejected=$false
try{Wait-ObsMediaReady ([pscustomobject]@{HasExited=$false}) ([DateTime]::UtcNow.AddSeconds(2))|Out-Null}
catch{if($_.Exception.Message -ne 'OBS could not decode the video.'){throw};$rejected=$true}
if(!$rejected -or $script:calls -ne 1){throw 'A decoder error was retried.'}
Reset-Fixture @(600,100)
$rejected=$false
try{Wait-ObsMediaReady ([pscustomobject]@{HasExited=$false}) ([DateTime]::UtcNow.AddSeconds(2))|Out-Null}
catch{if($_.Exception.Data['ObsStatusCode'] -ne 600){throw};$rejected=$true}
if(!$rejected -or $script:calls -ne 1){throw 'A permanent media error was retried.'}
Write-Host 'PASS: decoder and permanent media errors remain failures'

foreach($expired in @($false,$true)) {
    Reset-Fixture @(100)
    $rejected=$false
    $deadline=if($expired){[DateTime]::UtcNow.AddSeconds(-1)}else{[DateTime]::UtcNow.AddSeconds(2)}
    try{Wait-ObsMediaReady ([pscustomobject]@{HasExited=!$expired}) $deadline|Out-Null}
    catch{$rejected=$true}
    if(!$rejected -or $script:calls){throw 'An exited owner or expired deadline was ignored by media readiness.'}
}
Write-Host 'PASS: media readiness preserves owner and deadline refusals'
