#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$InstallerPath,[Parameter(Mandatory)][string]$PreviousInstallerPath,[Parameter(Mandatory)][string]$BundlePath)
. (Join-Path $PSScriptRoot 'windows-installer\common.ps1')
$sandbox=Get-Command WindowsSandbox.exe -ErrorAction SilentlyContinue
if(-not $sandbox){throw 'Windows Sandbox is unavailable. Use a clean Windows VM; do not install the fixture on this PC.'}
if(Get-Process WindowsSandbox -ErrorAction SilentlyContinue){throw 'An existing Sandbox session must remain untouched. Close it before this separate acceptance run.'}
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
if(-not $child.WaitForExit(240000)){throw "Owned Sandbox did not finish within four minutes; evidence: $evidence"}
$resultPath=Join-Path $evidence 'result.json'
if(-not (Test-Path -LiteralPath $resultPath)){throw "Sandbox did not produce acceptance evidence (feature may be disabled). See $evidence"}
$result=Get-Content $resultPath -Raw | ConvertFrom-Json
if(-not $result.pass){throw "Sandbox acceptance failed: $($result.failure). See $evidence"}
Write-Output "PASS: clean-VM install, branded shortcuts, upgrade, downgrade rejection, complete payload, uninstall and preserved user data ($($result.checks.Count) checks). Evidence: $resultPath"
