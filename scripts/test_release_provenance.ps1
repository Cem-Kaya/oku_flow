#requires -Version 7.0
# Private, desktop-free provenance fixtures. No application build, install,
# signing, external tool resolution, account access or publication is performed.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CMake,
    [Parameter(Mandatory)][string]$Ninja,
    [string]$WixPackage = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-installer/wix_tool.ps1')
. (Join-Path $PSScriptRoot 'windows-installer/release_build_facts.ps1')
$work = Join-Path $env:LOCALAPPDATA ('OkuFlow/release-provenance-fixtures/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
function Assert-Provenance([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Assert-Refused([scriptblock]$Action, [string]$Message) {
    $refused = $false
    try { & $Action | Out-Null } catch { $refused = $true }
    Assert-Provenance $refused $Message
}
function Write-FixtureFile([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path (Split-Path $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text)
}
# Metadata needs Authenticode results; use explicit synthetic results only for
# paths in this fresh private fixture root. No certificate/key is accessed.
function Get-AuthenticodeSignature([string]$LiteralPath) {
    if (-not [IO.Path]::GetFullPath($LiteralPath).StartsWith($work + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Synthetic signature fixture refuses a path outside its private root.'
    }
    [pscustomobject]@{Status='Valid';SignerCertificate=[pscustomobject]@{Subject='CN=Microsoft Corporation, O=Synthetic fixture';Thumbprint='synthetic'}}
}
function New-MetadataFixture([string]$Bundle, [string]$Build, [string]$Source, [bool]$Cuda) {
    New-Item -ItemType Directory -Path $Bundle | Out-Null
    Copy-Item -LiteralPath (Join-Path $Build 'OkuFlowApp.exe') -Destination $Bundle
    foreach ($name in @('oku_flow.exe','vc_redist.x64.exe','licenses/MICROSOFT_VC_RUNTIME_LICENSE.rtf',
            'platform-tools/adb.exe','platform-tools/AdbWinApi.dll','platform-tools/AdbWinUsbApi.dll',
            'platform-tools/NOTICE.txt','licenses/adb/BORINGSSL_LICENSE.txt')) {
        Write-FixtureFile (Join-Path $Bundle $name) "synthetic fixture: $name"
    }
    Write-FixtureFile (Join-Path $Bundle 'platform-tools/source.properties') 'Pkg.Revision=37.0.0-fixture'
    Write-FixtureFile (Join-Path $Bundle 'platform-tools/adb-build.json') '{"sourceBuilt":true,"protocol":41,"files":[],"components":[]}'
    foreach ($name in @('qtbase','qtimageformats','qtmultimedia','qtspeech','qtsvg')) {
        Write-FixtureFile (Join-Path $Bundle "licenses/qt-sbom/$name-6.12.0.spdx.json") (@{
            documentNamespace="https://fixture.invalid/$name";packages=@(@{name=$name;SPDXID="SPDXRef-Package-$name"})} | ConvertTo-Json -Depth 4)
    }
    $pin = @{version='0.0.0-fixture';vendorPage='https://fixture.invalid';licensePage='https://fixture.invalid';
        installer=@{name='vc_redist.x64.exe';sha256=(Get-FileHash -LiteralPath (Join-Path $Bundle 'vc_redist.x64.exe')).Hash.ToLowerInvariant()};
        licenseSha256=(Get-FileHash -LiteralPath (Join-Path $Bundle 'licenses/MICROSOFT_VC_RUNTIME_LICENSE.rtf')).Hash.ToLowerInvariant()}
    Write-FixtureFile (Join-Path $Source 'scripts/vc-runtime/pins.json') ($pin | ConvertTo-Json)
    if ($Cuda) { Copy-Item -LiteralPath (Join-Path $toolkit 'LICENSE') -Destination (Join-Path $Bundle 'licenses/NVIDIA_CUDA_EULA.txt') }
}
function New-SyntheticWix([string]$Name) {
    $root = Join-Path $work $Name
    New-Item -ItemType Directory -Path $root | Out-Null
    $archive = Join-Path $root 'wix.4.0.6.nupkg'
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in @('tools/net6.0/any/wix.exe','tools/net6.0/any/wix.dll','tools/net6.0/any/wix.deps.json')) {
            $writer = [IO.StreamWriter]::new($zip.CreateEntry($name).Open())
            try { $writer.Write("synthetic package bytes: $name") } finally { $writer.Dispose() }
        }
    } finally { $zip.Dispose() }
    return [pscustomobject]@{root=$root;pin=[pscustomobject]@{version='4.0.6';sha256=(Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant();url='https://invalid.invalid/no-network'}}
}
$package = New-SyntheticWix 'wix-tree'
[IO.File]::WriteAllText((Join-Path $package.root 'wix.exe'), 'untrusted preinstalled shim')
$tool = Get-VerifiedWixTool $package.pin $package.root
Assert-Provenance ($tool.Executable -ne (Join-Path $package.root 'wix.exe')) 'A preinstalled shim was selected.'
Assert-Provenance ((Get-Content -LiteralPath $tool.Executable -Raw) -like 'synthetic package bytes:*') 'Tool did not come from the verified package.'
$dependency = Join-Path $tool.Root 'tools/net6.0/any/wix.dll'
[IO.File]::WriteAllText($dependency, 'replaced cached dependency')
Assert-Refused { Assert-VerifiedWixTree $tool } 'Changed installed WiX dependency was accepted.'
Assert-Refused { Invoke-VerifiedWix $tool -Arguments @('--version') } 'Changed tree reached invocation.'
Assert-Refused { Get-VerifiedWixTool $package.pin $package.root } 'Corrupt cached WiX tree was silently reused/repaired.'
$package2 = New-SyntheticWix 'wix-extra'
$tool2 = Get-VerifiedWixTool $package2.pin $package2.root
[IO.File]::WriteAllText((Join-Path $tool2.Root 'tools/net6.0/any/unreviewed.dll'), 'extra dependency')
Assert-Refused { Assert-VerifiedWixTree $tool2 } 'Added WiX dependency was accepted.'
$package3 = New-SyntheticWix 'wix-archive'
[IO.File]::AppendAllText((Join-Path $package3.root 'wix.4.0.6.nupkg'), 'changed archive')
Assert-Refused { Get-VerifiedWixTool $package3.pin $package3.root } 'Changed WiX archive was accepted.'

# Configure only a synthetic imported target: this exercises the production
# CMake facts generator without invoking a compiler or building OkuFlow.
$source = Join-Path $work 'source'
$toolkit = Join-Path $work 'selected toolkit'
New-Item -ItemType Directory -Path $source, (Join-Path $toolkit 'bin'), (Join-Path $toolkit 'lib/x64') | Out-Null
[IO.File]::WriteAllText((Join-Path $toolkit 'bin/nvcc.exe'), 'synthetic effective compiler')
[IO.File]::WriteAllText((Join-Path $toolkit 'lib/x64/cudart_static.lib'), 'synthetic effective runtime')
[IO.File]::WriteAllText((Join-Path $toolkit 'LICENSE'), 'synthetic selected toolkit license')
$module = (Join-Path $PSScriptRoot '../cmake/ReleaseBuildFacts.cmake').Replace('\','/')
$sealScript = Join-Path $PSScriptRoot '../cmake/SealReleaseBuildFacts.cmake'
$project = @'
cmake_minimum_required(VERSION 3.25)
project(ProvenanceFixture NONE)
option(OKUFLOW_ENABLE_CUDA "fixture" OFF)
set(fixture_toolkit "${FIXTURE_TOOLKIT}")
add_executable(oku_flow IMPORTED GLOBAL)
set_target_properties(oku_flow PROPERTIES IMPORTED_LOCATION "${CMAKE_BINARY_DIR}/OkuFlowApp.exe")
if(NOT EXISTS "${CMAKE_BINARY_DIR}/OkuFlowApp.exe")
  file(WRITE "${CMAKE_BINARY_DIR}/OkuFlowApp.exe" "synthetic application ${OKUFLOW_ENABLE_CUDA}")
endif()
if(OKUFLOW_ENABLE_CUDA)
  set(CMAKE_CUDA_COMPILER "${FIXTURE_TOOLKIT}/bin/nvcc.exe" CACHE FILEPATH "fixture")
  set(CMAKE_CUDA_COMPILER_VERSION "12.9.86")
  set(CUDAToolkit_NVCC_EXECUTABLE "${CMAKE_CUDA_COMPILER}")
  set(CUDAToolkit_VERSION "12.9.86")
  set_target_properties(oku_flow PROPERTIES CUDA_ARCHITECTURES "61;75;86" CUDA_RUNTIME_LIBRARY Static)
  add_library(CUDA::cudart_static STATIC IMPORTED GLOBAL)
  set_target_properties(CUDA::cudart_static PROPERTIES IMPORTED_LOCATION "${FIXTURE_TOOLKIT}/lib/x64/cudart_static.lib")
  set_property(TARGET oku_flow PROPERTY LINK_LIBRARIES CUDA::cudart_static)
endif()
include("@MODULE@")
okuflow_generate_release_build_facts(oku_flow)
'@
[IO.File]::WriteAllText((Join-Path $source 'CMakeLists.txt'), $project.Replace('@MODULE@',$module))
$oldCudaPath = $env:CUDA_PATH
$oldCudaRoot = $env:OKUFLOW_CUDA_TOOLKIT_ROOT
try {
    $env:CUDA_PATH = Join-Path $work 'ambient-wrong-cuda-v99'
    $env:OKUFLOW_CUDA_TOOLKIT_ROOT = $env:CUDA_PATH
    foreach ($mode in @('OFF','ON')) {
        $build = Join-Path $work "build-$mode"
        & $CMake -S $source -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" -DCMAKE_BUILD_TYPE=Release "-DOKUFLOW_ENABLE_CUDA=$mode" "-DFIXTURE_TOOLKIT=$($toolkit.Replace('\','/'))" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Synthetic CMake configuration failed: $mode" }
        Assert-Refused { Get-OkuFlowReleaseBuildFacts $build } 'Configure-only target without a build seal was accepted.'
        # Imported fixture targets cannot link. Explicitly run the same seal
        # command that production invokes POST_BUILD, against synthetic bytes.
        & $CMake "-DFactsPath=$(Join-Path $build 'okuflow-build-facts/Release.json')" -P $sealScript
        if ($LASTEXITCODE -ne 0) { throw "Synthetic successful-build seal failed: $mode" }
        $facts = Get-OkuFlowReleaseBuildFacts $build
        Assert-Provenance ($facts.cuda.enabled -eq ($mode -eq 'ON')) 'CUDA state came from ambient environment.'
        if ($mode -eq 'ON') {
            Assert-Provenance ($facts.cuda.toolkitVersion -eq '12.9.86' -and
                ($facts.cuda.architectures -join ';') -eq '61;75;86' -and
                [IO.Path]::GetFullPath($facts.cuda.toolkitRoot) -eq $toolkit) 'Effective toolkit/target facts changed.'
        }
        $cmd = Join-Path $work "facts-$mode.cmd"
        & (Join-Path $PSScriptRoot 'get_release_build_facts.ps1') -BuildDirectory $build -BatchEnvironmentPath $cmd
        Assert-Provenance ((Get-Content -LiteralPath $cmd -Raw).Contains("BUNDLE_EFFECTIVE_CUDA=$mode")) 'Batch facts disagree with configured target.'
        $bundle = Join-Path $work "bundle-$mode"
        New-MetadataFixture $bundle $build $source ($mode -eq 'ON')
        & (Join-Path $PSScriptRoot 'generate_release_metadata.ps1') -BundlePath $bundle -RepositoryPath $source -BuildDirectory $build -QtVersion '6.12.0'
        $manifest = Get-Content -LiteralPath (Join-Path $bundle 'release-manifest.json') -Raw | ConvertFrom-Json
        $sbom = Get-Content -LiteralPath (Join-Path $bundle 'SBOM.spdx.json') -Raw | ConvertFrom-Json
        $cudaPackages = @($sbom.packages | Where-Object SPDXID -eq 'SPDXRef-Package-CUDA')
        Assert-Provenance ($manifest.buildFacts.cuda.enabled -eq ($mode -eq 'ON') -and
            $cudaPackages.Count -eq $(if($mode -eq 'ON'){1}else{0})) 'Generated metadata claimed the wrong CUDA tier.'
        if ($mode -eq 'ON') {
            Assert-Provenance ($cudaPackages[0].versionInfo -eq '12.9.86') 'Metadata used ambient CUDA_PATH instead of the configured toolkit.'
            $notice = Join-Path $bundle 'licenses/NVIDIA_CUDA_EULA.txt'
            [IO.File]::AppendAllText($notice, 'different staged toolkit notice')
            Assert-Refused { & (Join-Path $PSScriptRoot 'generate_release_metadata.ps1') -BundlePath $bundle -RepositoryPath $source -BuildDirectory $build } 'Different staged CUDA notice was accepted.'
            Copy-Item -LiteralPath (Join-Path $toolkit 'LICENSE') -Destination $notice -Force
        }
        Assert-Refused { & (Join-Path $PSScriptRoot 'generate_release_metadata.ps1') -BundlePath $bundle -RepositoryPath $source -BuildDirectory $build -CudaEnabled $(if($mode -eq 'ON'){'OFF'}else{'ON'}) } 'Contradictory requested CUDA metadata was accepted.'
        Assert-Refused { Get-OkuFlowReleaseBuildFacts $build Debug } 'Wrong build configuration was accepted.'
        $other = Join-Path $work "other-$mode.exe"
        [IO.File]::WriteAllText($other, 'different staged application')
        Assert-Refused { Get-OkuFlowReleaseBuildFacts $build Release $other } 'Different staged application was accepted.'
        $application = Join-Path $build 'OkuFlowApp.exe'
        $originalApplication = [IO.File]::ReadAllBytes($application)
        [IO.File]::AppendAllText($application, 'changed after successful build')
        Assert-Refused { Get-OkuFlowReleaseBuildFacts $build } 'Changed application bytes retained their old build seal.'
        [IO.File]::WriteAllBytes($application, $originalApplication)
        $cache = Join-Path $build 'CMakeCache.txt'
        $original = [IO.File]::ReadAllText($cache)
        [IO.File]::WriteAllText($cache, $original.Replace("OKUFLOW_ENABLE_CUDA:BOOL=$mode", "OKUFLOW_ENABLE_CUDA:BOOL=$(if($mode -eq 'ON'){'OFF'}else{'ON'})"))
        Assert-Refused { Get-OkuFlowReleaseBuildFacts $build } 'Stale facts/cache mismatch was accepted.'
        [IO.File]::WriteAllText($cache, $original)
    }
    $reconfigured = Join-Path $work 'build-OFF'
    & $CMake -S $source -B $reconfigured -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" -DCMAKE_BUILD_TYPE=Release -DOKUFLOW_ENABLE_CUDA=ON "-DFIXTURE_TOOLKIT=$($toolkit.Replace('\','/'))" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Synthetic CPU-to-CUDA reconfiguration failed.' }
    Assert-Provenance ((Get-Content -LiteralPath (Join-Path $reconfigured 'OkuFlowApp.exe') -Raw) -eq 'synthetic application OFF') 'Reconfigure fixture replaced the prior CPU application.'
    Assert-Refused { Get-OkuFlowReleaseBuildFacts $reconfigured } 'CUDA reconfiguration relabelled old CPU bytes using a stale successful-build seal.'
    Assert-Refused { & (Join-Path $PSScriptRoot 'generate_release_metadata.ps1') -BundlePath (Join-Path $work 'bundle-OFF') -RepositoryPath $source -BuildDirectory $reconfigured } 'Standalone metadata labelled old CPU bytes as the reconfigured CUDA target.'
    [IO.File]::AppendAllText((Join-Path $toolkit 'LICENSE'), 'changed notice')
    Assert-Refused { Get-OkuFlowReleaseBuildFacts (Join-Path $work 'build-ON') } 'Changed effective CUDA license was accepted.'
    [IO.File]::WriteAllText((Join-Path $toolkit 'LICENSE'), 'synthetic selected toolkit license')
    [IO.File]::AppendAllText((Join-Path $toolkit 'lib/x64/cudart_static.lib'), 'changed runtime')
    Assert-Refused { Get-OkuFlowReleaseBuildFacts (Join-Path $work 'build-ON') } 'Changed effective CUDA runtime was accepted.'
    [IO.File]::WriteAllText((Join-Path $toolkit 'lib/x64/cudart_static.lib'), 'synthetic effective runtime')
    [IO.File]::AppendAllText((Join-Path $toolkit 'bin/nvcc.exe'), 'changed compiler')
    Assert-Refused { Get-OkuFlowReleaseBuildFacts (Join-Path $work 'build-ON') } 'Changed effective CUDA compiler was accepted.'
} finally {
    $env:CUDA_PATH = $oldCudaPath
    $env:OKUFLOW_CUDA_TOOLKIT_ROOT = $oldCudaRoot
}
if ($WixPackage) {
    $pin = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'windows-installer/tools.json') -Raw | ConvertFrom-Json).wix
    $real = Join-Path $work 'reviewed-wix'
    New-Item -ItemType Directory -Path $real | Out-Null
    Copy-Item -LiteralPath $WixPackage -Destination (Join-Path $real "wix.$($pin.version).nupkg")
    $verified = Get-VerifiedWixTool $pin $real
    $version = Invoke-VerifiedWix $verified -Arguments @('--version')
    Assert-Provenance ($version -like "$($pin.version)+*") 'Verified WiX apphost returned an unexpected version.'
    $payload = Join-Path $real 'synthetic.txt'
    [IO.File]::WriteAllText($payload, 'private synthetic MSI payload; never installed')
    $authoring = Join-Path $real 'fixture.wxs'
    $escapedPayload = [Security.SecurityElement]::Escape($payload)
    [IO.File]::WriteAllText($authoring, @"
<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">
  <Package Name="OkuFlow provenance fixture" Manufacturer="OkuFlow" Version="0.0.0" UpgradeCode="F5C607C2-71BB-4B1A-83ED-12AE4B3595E2" Scope="perMachine">
    <MediaTemplate EmbedCab="yes" />
    <StandardDirectory Id="ProgramFiles64Folder"><Directory Id="INSTALLFOLDER" Name="OkuFlowProvenanceFixture" /></StandardDirectory>
    <Component Id="FixtureComponent" Directory="INSTALLFOLDER" Guid="A0A29665-DA8F-4CA1-B85F-483527D4587F" Bitness="always64"><File Id="FixtureFile" Source="$escapedPayload" /></Component>
    <Feature Id="Complete"><ComponentRef Id="FixtureComponent" /></Feature>
  </Package>
</Wix>
"@)
    $msi = Join-Path $real 'private-fixture.msi'
    Invoke-VerifiedWix $verified -Arguments @('build', $authoring, '-arch', 'x64', '-out', $msi, '-intermediateFolder', (Join-Path $real 'intermediate'))
    $extracted = Join-Path $real 'extracted'
    Invoke-VerifiedWix $verified -Arguments @('msi', 'decompile', $msi, '-x', $extracted, '-o', (Join-Path $real 'extracted.wxs'))
    $match = @(Get-ChildItem -LiteralPath $extracted -File -Recurse | Where-Object { (Get-FileHash -LiteralPath $_.FullName).Hash -eq (Get-FileHash -LiteralPath $payload).Hash })
    Assert-Provenance ($match.Count -eq 1) 'Direct verified WiX could not compile/extract its synthetic payload.'
}
Write-Output "PASS: WiX package/tree/invocation refusals and effective CPU/CUDA CMake provenance. Evidence: $work"
