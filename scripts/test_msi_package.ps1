#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$InstallerPath,[Parameter(Mandatory)][string]$BundlePath)
. (Join-Path $PSScriptRoot 'windows-installer\common.ps1')
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.OpenDatabase((Resolve-Path -LiteralPath $InstallerPath).Path,0)
$properties = @{}
foreach($row in (Read-InstallerTable $database 'SELECT `Property`, `Value` FROM `Property`')) { $properties[$row[0]]=$row[1] }
if ($properties['ProductName'] -ne 'OkuFlow' -or $properties['ALLUSERS'] -ne '1' -or $properties['MSIRESTARTMANAGERCONTROL'] -ne 'DisableShutdown') { throw 'Incorrect product, scope or application-shutdown policy.' }
$shortcuts = @(Read-InstallerTable $database 'SELECT `Name`, `Directory_`, `Icon_`, `WkDir` FROM `Shortcut`')
if ($shortcuts.Count -ne 2 -or (@($shortcuts | Where-Object {$_[1] -in @('DesktopFolder','ApplicationPrograms') -and $_[2] -eq 'OkuFlowIcon' -and $_[3] -eq 'INSTALLFOLDER'})).Count -ne 2) { throw 'Desktop/Start menu shortcuts are incomplete.' }
$files = @(Read-InstallerTable $database 'SELECT `File`, `FileName`, `Component_` FROM `File`')
$snapshotFiles = @(Get-ChildItem -LiteralPath $BundlePath -File -Recurse)
if ($files.Count -ne $snapshotFiles.Count) { throw 'MSI omitted payload files.' }
$tables = @(Read-InstallerTable $database 'SELECT `Name` FROM `_Tables`' | ForEach-Object {$_[0]})
if ('CustomAction' -in $tables) { throw 'Installer must not contain custom actions or third-party action binaries.' }
$sequence = @{}
foreach($row in (Read-InstallerTable $database 'SELECT `Action`, `Sequence` FROM `InstallExecuteSequence`')) {$sequence[$row[0]]=[int]$row[1]}
if ($sequence['RemoveExistingProducts'] -le $sequence['InstallInitialize'] -or $sequence['RemoveExistingProducts'] -ge $sequence['InstallFinalize']) {throw 'Major upgrade must remain inside the rollback transaction.'}
# WiX extraction reads the cabinet without installation, registry writes or UAC.
$pins = Get-Content (Join-Path $PSScriptRoot 'windows-installer\tools.json') -Raw | ConvertFrom-Json
$wix = Join-Path $env:LOCALAPPDATA "OkuFlow\installer-tools\wix-$($pins.wix.version)\wix.exe"
$extract = Join-Path $env:LOCALAPPDATA "OkuFlow\installer-validation\$([Guid]::NewGuid().ToString('N'))"
& $wix msi decompile (Resolve-Path -LiteralPath $InstallerPath).Path -x $extract -o "$extract.wxs"
if ($LASTEXITCODE -ne 0) {throw 'MSI payload extraction failed.'}
$extracted = @(Get-ChildItem -LiteralPath $extract -File -Recurse)
$hashes = @{}
foreach($file in $extracted) {$hash=(Get-FileHash -LiteralPath $file.FullName).Hash; if(-not $hashes.ContainsKey($hash)){$hashes[$hash]=0};$hashes[$hash]++}
foreach($file in $snapshotFiles) {
    $hash=(Get-FileHash -LiteralPath $file.FullName).Hash
    if(-not $hashes.ContainsKey($hash) -or $hashes[$hash] -lt 1){throw "Missing/changed extracted payload: $($file.Name)"};$hashes[$hash]--
}
Write-Output "PASS: machine scope, two branded shortcuts, rollback upgrade, no custom actions and $($files.Count) byte-identical payload files. Installation is not exercised."
