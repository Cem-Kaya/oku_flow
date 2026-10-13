#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$InstallerPath,[Parameter(Mandatory)][string]$PreviousInstallerPath,[Parameter(Mandatory)][string]$BundlePath,[ValidateRange(60,1800)][int]$TimeoutSeconds=900)
. (Join-Path $PSScriptRoot 'windows-installer\common.ps1')
$sandbox=Get-Command WindowsSandbox.exe -ErrorAction SilentlyContinue
if(-not $sandbox){throw 'Windows Sandbox is unavailable. Use a clean Windows VM; do not install the fixture on this PC.'}
if(Get-Process WindowsSandbox,WindowsSandboxRemoteSession -ErrorAction SilentlyContinue){throw 'An existing Sandbox session must remain untouched. Close it before this separate acceptance run.'}
$servers=@(Get-Process WindowsSandboxServer -ErrorAction SilentlyContinue)
foreach($server in $servers){
    # The Store Sandbox server can remain idle after its guest shuts down.
    # Query its inventory rather than stopping it or treating the daemon as a VM.
    $inventoryTool=Join-Path (Split-Path $server.Path) 'wsb.exe'
    if(-not (Test-Path -LiteralPath $inventoryTool)){throw 'A Sandbox server is present without a queryable guest inventory. Leave it untouched.'}
    $inventoryText=& $inventoryTool list --raw
    if($LASTEXITCODE -ne 0){throw 'Cannot verify that the existing Sandbox server has no guests. Leave it untouched.'}
    $inventory=$inventoryText | ConvertFrom-Json -ErrorAction Stop
    if(-not $inventory.PSObject.Properties['WindowsSandboxEnvironments'] -or @($inventory.WindowsSandboxEnvironments).Count -ne 0){throw 'An existing Sandbox guest must remain untouched. Close it before this separate acceptance run.'}
}
$work=Join-Path $env:LOCALAPPDATA "OkuFlow\installer-sandbox\$([Guid]::NewGuid().ToString('N'))"
$inputPath=Join-Path $work 'input';$evidence=Join-Path $work 'evidence'
New-Item -ItemType Directory -Path $inputPath,$evidence -Force | Out-Null
Copy-Item -LiteralPath (Resolve-Path -LiteralPath $InstallerPath).Path -Destination (Join-Path $inputPath 'candidate.msi')
Copy-Item -LiteralPath (Resolve-Path -LiteralPath $PreviousInstallerPath).Path -Destination (Join-Path $inputPath 'previous.msi')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-installer\sandbox_acceptance.ps1') -Destination (Join-Path $inputPath 'run.ps1')
$manifest=Get-Content -LiteralPath (Join-Path $BundlePath 'release-manifest.json') -Raw | ConvertFrom-Json
$records=@($manifest.files)
foreach($name in @('SHA256SUMS.txt','release-manifest.json','SBOM.spdx.json')){$records += [pscustomobject]@{path=$name;sha256=(Get-FileHash -LiteralPath (Join-Path $BundlePath $name)).Hash.ToLowerInvariant()}}
@{files=$records} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $inputPath 'input.json') -Encoding utf8NoBOM
$escape={param($value)[Security.SecurityElement]::Escape($value)}
$configuration=@"
<Configuration><VGpu>Disable</VGpu><Networking>Disable</Networking><MemoryInMB>4096</MemoryInMB><ClipboardRedirection>Disable</ClipboardRedirection><AudioInput>Disable</AudioInput><VideoInput>Disable</VideoInput>
<MappedFolders><MappedFolder><HostFolder>$(& $escape $inputPath)</HostFolder><SandboxFolder>C:\OkuFlowFixture</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder><MappedFolder><HostFolder>$(& $escape $evidence)</HostFolder><SandboxFolder>C:\OkuFlowEvidence</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder></MappedFolders>
<LogonCommand><Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\OkuFlowFixture\run.ps1</Command></LogonCommand></Configuration>
"@
$config=Join-Path $work 'acceptance.wsb';$configuration | Set-Content $config -Encoding utf8NoBOM
Write-Output "Starting disposable installer acceptance; evidence: $evidence"
$child=Start-Process -FilePath $sandbox.Source -ArgumentList "`"$config`"" -WindowStyle Hidden -PassThru
$resultPath=Join-Path $evidence 'result.json'
# New Windows Sandbox versions detach their RemoteSession from the launcher.
# Guest evidence, rather than launcher lifetime, determines completion.
$deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
while(-not (Test-Path -LiteralPath $resultPath) -and [DateTime]::UtcNow -lt $deadline){
    Start-Sleep -Seconds 2
    $child.Refresh()
    if($child.HasExited -and $child.ExitCode -ne 0){throw "Sandbox launch failed (exit $($child.ExitCode)); evidence: $evidence"}
}
if(-not (Test-Path -LiteralPath $resultPath)){throw "Sandbox did not produce guest evidence within $TimeoutSeconds seconds. See $evidence"}
$result=Get-Content $resultPath -Raw | ConvertFrom-Json
if(-not $result.pass){throw "Sandbox acceptance failed: $($result.failure). See $evidence"}
Write-Output "PASS: clean-VM install, branded shortcuts, upgrade, downgrade rejection, complete payload, uninstall and preserved user data ($($result.checks.Count) checks). Evidence: $resultPath"
