# Runs only inside the disposable Windows Sandbox fixture created by the host.
# Compatible with its built-in Windows PowerShell; never runs on the owner PC.
param([string]$InputDirectory='C:\OkuFlowFixture',[string]$EvidenceDirectory='C:\OkuFlowEvidence')
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount'){throw 'Acceptance script requires the Windows Sandbox account.'}
$result=[ordered]@{pass=$false;checks=@();failure=$null}
function Assert-Fixture([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message};$result.checks += $Message}
function Invoke-FixtureMsi([string]$Mode,[string]$Path,[string]$Label,[bool]$ShouldPass=$true){
    $log=Join-Path $EvidenceDirectory "$Label.log"
    $process=Start-Process msiexec.exe -ArgumentList @($Mode,"`"$Path`"",'/qn','/norestart','/l*v',"`"$log`"") -WindowStyle Hidden -Wait -PassThru
    if($ShouldPass){Assert-Fixture ($process.ExitCode -eq 0) "$Label exits successfully without a reboot"}
    else{Assert-Fixture ($process.ExitCode -ne 0) "$Label rejects the older version"}
}
try{
    $inputData=Get-Content (Join-Path $InputDirectory 'input.json') -Raw | ConvertFrom-Json
    $install=Join-Path $env:ProgramFiles 'OkuFlow'
    $settings=Join-Path $env:APPDATA 'OkuFlow\OkuFlow\settings.json'
    $capture=Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'OkuFlow\Recordings\owner-sentinel.txt'
    foreach($file in @($settings,$capture)){New-Item -ItemType Directory -Path (Split-Path $file) -Force | Out-Null;[IO.File]::WriteAllText($file,'preserve owner data')}
    $desktop=Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'OkuFlow.lnk'
    $startMenu=Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'OkuFlow\OkuFlow.lnk'
    Invoke-FixtureMsi '/i' (Join-Path $InputDirectory 'previous.msi') 'install-previous'
    Assert-Fixture ((Test-Path $desktop) -and (Test-Path $startMenu)) 'Desktop and Start menu shortcuts are installed'
    Invoke-FixtureMsi '/i' (Join-Path $InputDirectory 'candidate.msi') 'upgrade-candidate'
    $shell=New-Object -ComObject WScript.Shell
    Add-Type -AssemblyName System.Drawing
    foreach($link in @($desktop,$startMenu)){
        $shortcut=$shell.CreateShortcut($link)
        $iconPath=$shortcut.IconLocation -replace ',[^,]*$',''
        Assert-Fixture ((Test-Path -LiteralPath $iconPath) -and [IO.Path]::GetExtension($iconPath) -eq '.exe') 'Shortcut references a real advertised PE icon file'
        $image=[Drawing.Icon]::ExtractAssociatedIcon($iconPath).ToBitmap()
        try{
            $cyan=0;$dark=0
            for($y=0;$y -lt $image.Height;$y++){for($x=0;$x -lt $image.Width;$x++){$pixel=$image.GetPixel($x,$y);if($pixel.A -gt 128){if($pixel.G -gt 100 -and $pixel.B -gt 150 -and $pixel.R -lt 130){$cyan++};if($pixel.R -lt 40 -and $pixel.G -lt 50 -and $pixel.B -lt 70){$dark++}}}}
            Assert-Fixture ($cyan -gt $image.Width*$image.Height/5 -and $dark -gt $image.Width*$image.Height/10) 'Windows renders the cyan magnifier icon from the installed shortcut resource'
            $image.Save((Join-Path $EvidenceDirectory ('shortcut-'+[IO.Path]::GetFileName((Split-Path $link))+'.png')))
        }finally{$image.Dispose()}
    }
    foreach($record in $inputData.files){
        Assert-Fixture ((Get-FileHash -LiteralPath (Join-Path $install $record.path)).Hash.ToLowerInvariant() -eq $record.sha256) "Installed payload matches: $($record.path)"
    }
    Invoke-FixtureMsi '/i' (Join-Path $InputDirectory 'previous.msi') 'block-downgrade' $false
    foreach($name in @('oku_flow.exe','OkuFlowApp.exe')){
        $expected=$inputData.files | Where-Object {$_.path -eq $name}
        Assert-Fixture ((Get-FileHash -LiteralPath (Join-Path $install $name)).Hash.ToLowerInvariant() -eq $expected.sha256) "Downgrade leaves current $name intact"
    }
    Invoke-FixtureMsi '/x' (Join-Path $InputDirectory 'candidate.msi') 'uninstall-candidate'
    Assert-Fixture (-not (Test-Path $desktop) -and -not (Test-Path $startMenu)) 'Uninstall removes both shortcuts'
    Assert-Fixture (-not (Test-Path $install)) 'Uninstall removes the installation folder'
    foreach($file in @($settings,$capture)){Assert-Fixture ([IO.File]::ReadAllText($file) -eq 'preserve owner data') "Uninstall preserves $file"}
    $result.pass=$true
}catch{$result.failure=$_.Exception.Message}
finally{
    $result | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $EvidenceDirectory 'result.tmp') -Encoding UTF8
    Move-Item -LiteralPath (Join-Path $EvidenceDirectory 'result.tmp') -Destination (Join-Path $EvidenceDirectory 'result.json')
    # This shuts down only the disposable guest, ending its owned Sandbox session.
    Start-Process shutdown.exe -ArgumentList '/s /t 0' -WindowStyle Hidden
}
