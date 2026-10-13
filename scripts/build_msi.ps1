#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [string]$BundlePath = (Join-Path $PSScriptRoot '..\dist\OkuFlow'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\dist\Installers'),
    [switch]$AllowUnsignedPreview
)
. (Join-Path $PSScriptRoot 'windows-installer\common.ps1')
$parsedVersion = [version]$Version
if ($parsedVersion.Major -gt 255 -or $parsedVersion.Minor -gt 255 -or $parsedVersion.Build -gt 65535) { throw 'MSI version fields exceed Windows Installer limits.' }
$repository = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pins = Get-Content (Join-Path $PSScriptRoot 'windows-installer\tools.json') -Raw | ConvertFrom-Json
$toolDirectory = Join-Path $env:LOCALAPPDATA "OkuFlow\installer-tools\wix-$($pins.wix.version)"
New-Item -ItemType Directory -Path $toolDirectory -Force | Out-Null
$archive = Join-Path $toolDirectory "wix.$($pins.wix.version).nupkg"
if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest $pins.wix.url -OutFile $archive }
if ((Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant() -ne $pins.wix.sha256) { throw 'WiX package differs from the reviewed pin.' }
$wix = Join-Path $toolDirectory 'wix.exe'
if (-not (Test-Path -LiteralPath $wix)) {
    & dotnet tool install wix --tool-path $toolDirectory --version $pins.wix.version --no-cache
    if ($LASTEXITCODE -ne 0) { throw 'WiX installation failed.' }
}
$actualVersion = & $wix --version
if ($LASTEXITCODE -ne 0 -or $actualVersion -notlike "$($pins.wix.version)+*") { throw 'Unexpected WiX tool version.' }
$work = Join-Path $env:LOCALAPPDATA "OkuFlow\installer-build\$Version-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work -Force | Out-Null
$snapshot = New-VerifiedBundleSnapshot $BundlePath (Join-Path $work 'payload')
if (-not $AllowUnsignedPreview) {
    foreach ($name in @('oku_flow.exe', $snapshot.layout.ApplicationRelativePath)) {
        if ((Get-AuthenticodeSignature -LiteralPath (Join-Path $snapshot.path $name)).Status -ne 'Valid') { throw "Unsigned application: $name" }
    }
}
$productCode = Get-InstallerStableGuid "OkuFlow.MSI.x64.product.$Version"
$sourcePath = Join-Path $work 'OkuFlow.wxs'
$xml = [Xml.XmlWriter]::Create($sourcePath,[Xml.XmlWriterSettings]@{Indent=$true;Encoding=[Text.UTF8Encoding]::new($false)})
$namespace = 'http://wixtoolset.org/schemas/v4/wxs'
function Start-Element([string]$Name, [hashtable]$Attributes=@{}) {
    $xml.WriteStartElement($Name,$namespace)
    foreach($key in ($Attributes.Keys | Sort-Object)) { $xml.WriteAttributeString($key,[string]$Attributes[$key]) }
}
function Write-Element([string]$Name, [hashtable]$Attributes) { Start-Element $Name $Attributes; $xml.WriteEndElement() }
$componentIds = [Collections.Generic.List[string]]::new()
$directories = @{''='INSTALLFOLDER'}
try {
    Start-Element 'Wix'
    Start-Element 'Package' @{Name='OkuFlow';Manufacturer='OkuFlow';Version=$Version;Language='1033';Scope='perMachine';InstallerVersion='500';ProductCode=$productCode;UpgradeCode=$pins.upgradeCode}
    Write-Element 'MajorUpgrade' @{Schedule='afterInstallInitialize';DowngradeErrorMessage='A newer version of OkuFlow is already installed.'}
    Write-Element 'MediaTemplate' @{EmbedCab='yes';CompressionLevel='high'}
    # Advertised shortcuts require PE icon data and the target's .exe extension.
    # Reuse the verified first-party launcher resource, not a bare ICO stream.
    Write-Element 'Icon' @{Id='OkuFlowIcon.exe';SourceFile=(Join-Path $snapshot.path 'oku_flow.exe')}
    Write-Element 'Property' @{Id='ARPPRODUCTICON';Value='OkuFlowIcon.exe'}
    Write-Element 'Property' @{Id='ARPURLINFOABOUT';Value='https://okuflow.com'}
    Write-Element 'Property' @{Id='ARPNOREPAIR';Value='1'}
    Write-Element 'Property' @{Id='MSIRESTARTMANAGERCONTROL';Value='DisableShutdown'}
    Start-Element 'Property' @{Id='OKUFLOW_WINDOWS_BUILD';Secure='yes'}
    Write-Element 'RegistrySearch' @{Id='WindowsBuildSearch';Root='HKLM';Key='SOFTWARE\Microsoft\Windows NT\CurrentVersion';Name='CurrentBuildNumber';Type='raw';Bitness='always64'}
    $xml.WriteEndElement()
    Write-Element 'Launch' @{Condition='Installed OR (VersionNT64 AND OKUFLOW_WINDOWS_BUILD >= 10240)';Message='OkuFlow requires 64-bit Windows 10 or later.'}
    Start-Element 'StandardDirectory' @{Id='DesktopFolder'}; $xml.WriteEndElement()
    Start-Element 'StandardDirectory' @{Id='ProgramMenuFolder'}
    Start-Element 'Directory' @{Id='ApplicationPrograms';Name='OkuFlow'}; $xml.WriteEndElement(); $xml.WriteEndElement()
    Start-Element 'StandardDirectory' @{Id='ProgramFiles64Folder'}
    Start-Element 'Directory' @{Id='INSTALLFOLDER';Name='OkuFlow'}
    # Add each nested folder in parent-first order. Root files have no parent.
    $allDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($record in $snapshot.records) {
        $parts = $record.path.Split('/')
        for($count=1;$count -lt $parts.Count;$count++) { [void]$allDirectories.Add(($parts[0..($count-1)] -join '/')) }
    }
    $xml.WriteEndElement(); $xml.WriteEndElement()
    foreach ($relative in ($allDirectories | Sort-Object { $_.Split('/').Count }, { $_ })) {
        $parent = ($relative -split '/')[0..(($relative -split '/').Count-2)] -join '/'
        if (-not $relative.Contains('/')) { $parent='' }
        $id = 'D_' + (Get-InstallerStableId "directory.$relative").Substring(0,24)
        $directories[$relative]=$id
        Start-Element 'DirectoryRef' @{Id=$directories[$parent]}
        Start-Element 'Directory' @{Id=$id;Name=($relative.Split('/')[-1])}; $xml.WriteEndElement(); $xml.WriteEndElement()
    }
    foreach ($record in $snapshot.records) {
        $relative = [string]$record.path
        $parent = if($relative.Contains('/')) { $relative.Substring(0,$relative.LastIndexOf('/')) } else { '' }
        $id = (Get-InstallerStableId "file.$relative").Substring(0,24)
        $componentId = 'C_' + $id
        $componentIds.Add($componentId)
        Start-Element 'DirectoryRef' @{Id=$directories[$parent]}
        Start-Element 'Component' @{Id=$componentId;Guid=(Get-InstallerStableGuid "OkuFlow.MSI.x64.component.$relative");Bitness='always64'}
        Start-Element 'File' @{Id=('F_'+$id);Source=(Join-Path $snapshot.path $relative);Name=$relative.Split('/')[-1];KeyPath='yes'}
        if ($relative -eq 'oku_flow.exe') {
            foreach($shortcut in @(@{Id='DesktopShortcut';Directory='DesktopFolder'},@{Id='StartMenuShortcut';Directory='ApplicationPrograms'})) {
                $shortcut += @{Name='OkuFlow';Advertise='yes';Icon='OkuFlowIcon.exe';IconIndex='0';WorkingDirectory='INSTALLFOLDER';Description='OkuFlow camera magnifier'}
                Write-Element 'Shortcut' $shortcut
            }
        }
        $xml.WriteEndElement()
        if ($relative -eq 'oku_flow.exe') { Write-Element 'RemoveFolder' @{Id='RemoveApplicationPrograms';Directory='ApplicationPrograms';On='uninstall'} }
        $xml.WriteEndElement(); $xml.WriteEndElement()
    }
    Start-Element 'Feature' @{Id='Complete';Title='OkuFlow';Level='1'}
    foreach($id in $componentIds) { Write-Element 'ComponentRef' @{Id=$id} }
    $xml.WriteEndElement(); $xml.WriteEndElement(); $xml.WriteEndElement()
} finally { $xml.Dispose() }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) "OkuFlow-$Version-x64.msi"
if (Test-Path -LiteralPath $output) { throw 'Versioned installer already exists; choose a new output folder or version.' }
& $wix build $sourcePath -arch x64 -out $output -intermediateFolder (Join-Path $work 'intermediate')
if ($LASTEXITCODE -ne 0) { throw 'MSI compilation/validation failed.' }
$signature = Set-InstallerSignature $output -AllowUnsignedPreview:$AllowUnsignedPreview
& (Join-Path $PSScriptRoot 'test_msi_package.ps1') -InstallerPath $output -BundlePath $snapshot.path
$receipt = [ordered]@{schema=1;product='OkuFlow';version=$Version;architecture='x64';productCode=$productCode;upgradeCode=$pins.upgradeCode;sourceCommit=$snapshot.manifest.sourceCommit;installer=(Split-Path $output -Leaf);sha256=(Get-FileHash -LiteralPath $output).Hash.ToLowerInvariant();authenticode=$signature;unsignedPreview=[bool]$AllowUnsignedPreview;wixVersion=$pins.wix.version;buildDirectory=$work;validation='MSI tables and extracted payload; VM install/upgrade/uninstall pending'}
$receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$output.json" -Encoding utf8NoBOM
Write-Output "MSI ready: $output"
