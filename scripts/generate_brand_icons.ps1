#requires -Version 7.0
[CmdletBinding()]
param([string]$QtPrefix = 'C:\Qt\6.12.0\msvc2022_64')
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$work = Join-Path $env:LOCALAPPDATA 'OkuFlow\icon-build'
New-Item -ItemType Directory -Path $work -Force | Out-Null
$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $compiler) { throw 'Run in the Visual Studio 2022 x64 developer environment.' }
$source = Join-Path $PSScriptRoot 'icon_tools\render_icon.cpp'
$exporter = Join-Path $work 'render_icon.exe'
& $compiler.Source /nologo /std:c++17 /Zc:__cplusplus /permissive- /EHsc /MD "/I$QtPrefix\include" "/I$QtPrefix\include\QtCore" "/I$QtPrefix\include\QtGui" "/I$QtPrefix\include\QtSvg" $source "/Fo$work\render_icon.obj" "/Fe$exporter" /link "/LIBPATH:$QtPrefix\lib" Qt6Core.lib Qt6Gui.lib Qt6Svg.lib
if ($LASTEXITCODE -ne 0) { throw 'Icon exporter compilation failed.' }
$previousPath = $env:PATH
try {
    $env:PATH = "$QtPrefix\bin;$env:PATH"
    & $exporter (Join-Path $repository 'assets\okuflow-icon.svg') $work
    if ($LASTEXITCODE -ne 0) { throw 'Icon export failed.' }
} finally { $env:PATH = $previousPath }
foreach ($name in @('okuflow.ico','okuflow-icon.png')) {
    Copy-Item -LiteralPath (Join-Path $work $name) -Destination (Join-Path $repository "assets\$name")
}
$storeAssets = Join-Path $repository 'assets\store-icons'
New-Item -ItemType Directory -Path $storeAssets -Force | Out-Null
foreach ($name in @('StoreLogo.png','Square44x44Logo.png','Square150x150Logo.png')) {
    Copy-Item -LiteralPath (Join-Path $work $name) -Destination (Join-Path $storeAssets $name)
}
Write-Output 'Exported native/Store icons from the cyan website magnifier SVG.'
