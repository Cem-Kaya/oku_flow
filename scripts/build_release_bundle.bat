@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "STAGING_DIR="
set "BUNDLE_STAGE_CREATED="
set "BUNDLE_FACTS_ENV="
set "BUNDLE_PUBLISH_RESULT="

rem Build, test, stage, validate, and publish a self-contained OkuFlow bundle.
rem Existing bundles and user captures are untouched until staging succeeds.

set "ROOT_DIR=%~dp0.."
pushd "%ROOT_DIR%" >nul
if errorlevel 1 goto :fail_no_popd
set "ROOT_DIR=%CD%"

set "GENERATOR=%CMAKE_GENERATOR%"
if not defined GENERATOR set "GENERATOR=Ninja"
if /I not "%GENERATOR%"=="Ninja" (
    echo ERROR: Release bundles require Ninja and compile_commands.json for shared-source equivalence.
    echo Set CMAKE_GENERATOR=Ninja and OKUFLOW_BUNDLE_BUILD_DIR to a fresh directory.
    echo Existing build caches are never deleted automatically.
    goto :fail
)
set "BUNDLE_CHECK_TOOLCHAIN="
if /I "%~1"=="--check-toolchain" set "BUNDLE_CHECK_TOOLCHAIN=1"
if not "%~1"=="" if not defined BUNDLE_CHECK_TOOLCHAIN (
    echo ERROR: Supported option: --check-toolchain
    goto :fail
)
if not "%~2"=="" (
    echo ERROR: --check-toolchain takes no additional arguments.
    goto :fail
)

set "BUILD_DIR=%ROOT_DIR%\build\release-bundle"
if defined OKUFLOW_BUNDLE_BUILD_DIR for %%I in ("%OKUFLOW_BUNDLE_BUILD_DIR%") do set "BUILD_DIR=%%~fI"
set "DIST_DIR=%ROOT_DIR%\dist"
set "PRIMARY_DIR=%DIST_DIR%\OkuFlow"
set "SECONDARY_DIR=%DIST_DIR%\OkuFlow2"
set "QT_PREFIX_DEFAULT=C:\Qt\6.12.0\msvc2022_64"

call :initialize_toolchain
if errorlevel 1 goto :fail
pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\check_bundle_toolchain.ps1" -RepositoryPath "%ROOT_DIR%" -BuildDirectory "%BUILD_DIR%"
if errorlevel 1 goto :fail

call :resolve_qt
if errorlevel 1 goto :fail
set "Path=%QT_BIN_DIR%;%Path%"
if defined BUNDLE_CHECK_TOOLCHAIN (
    echo PASS: Qt deployment prerequisites are present. Check-only mode made no build or dist changes.
    popd
    endlocal
    exit /b 0
)

set "CMAKE_EXTRA_ARGS=%CMAKE_ARGS%"
if not defined OKUFLOW_ENABLE_CUDA set "OKUFLOW_ENABLE_CUDA=ON"
if not defined OKUFLOW_ENABLE_TEXT_SR set "OKUFLOW_ENABLE_TEXT_SR=ON"
set "TEST_RESUME="
if "%OKUFLOW_TEST_RESUME%"=="1" set "TEST_RESUME=-Resume"

echo ===== CONFIGURE TESTED RELEASE =====
cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -G "%GENERATOR%" ^
    "-DCMAKE_PREFIX_PATH:PATH=%QT_PREFIX%" ^
    "-DQt6_DIR:PATH=%QT_PREFIX%\lib\cmake\Qt6" ^
    -DOKUFLOW_ENABLE_CUDA=%OKUFLOW_ENABLE_CUDA% ^
    -DOKUFLOW_ENABLE_TEXT_SR=%OKUFLOW_ENABLE_TEXT_SR% ^
    %CMAKE_EXTRA_ARGS% ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DOKUFLOW_ENABLE_TESTS=ON
if errorlevel 1 goto :fail
pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\validate_mcp_release_build.ps1" -BuildDirectory "%BUILD_DIR%" -CacheOnly
if errorlevel 1 goto :fail

echo ===== BUILD APPLICATION AND TESTS =====
cmake --build "%BUILD_DIR%"
if errorlevel 1 goto :fail

rem CMake's effective target/compiler facts, never ambient CUDA variables,
rem select the release capability profile and exact toolkit notice.
for /f "usebackq delims=" %%T in (`pwsh.exe -NoProfile -Command "Join-Path ([IO.Path]::GetTempPath()) ('OkuFlow-build-facts-' + [Guid]::NewGuid().ToString('N') + '.cmd')"`) do set "BUNDLE_FACTS_ENV=%%T"
if not defined BUNDLE_FACTS_ENV goto :fail
pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\get_release_build_facts.ps1" -BuildDirectory "%BUILD_DIR%" -Configuration Release -BatchEnvironmentPath "%BUNDLE_FACTS_ENV%"
if errorlevel 1 goto :fail
call "%BUNDLE_FACTS_ENV%"
if errorlevel 1 goto :fail
del /q "%BUNDLE_FACTS_ENV%"
set "BUNDLE_FACTS_ENV="
set "CAPABILITY_PROFILE=windows-cpu"
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" set "CAPABILITY_PROFILE=windows-cuda"

if "%OKUFLOW_SKIP_BUNDLE_TESTS%"=="1" (
    echo.
    echo WARNING: UNTESTED BUNDLE
    echo OKUFLOW_SKIP_BUNDLE_TESTS=1 bypassed the mandatory CTest gate.
    echo This output must not be described as a tested release.
    echo.
) else (
    echo ===== RUN RELEASE TEST GATE =====
    pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\run_test_gate.ps1" -BuildDirectory "%BUILD_DIR%" -Configuration Release -CapabilityProfile %CAPABILITY_PROFILE% %TEST_RESUME%
    if errorlevel 1 goto :fail
)

call :find_built_executable
if errorlevel 1 goto :fail
pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\validate_mcp_release_build.ps1" -BuildDirectory "%BUILD_DIR%" -Executable "%MCP_EXE_PATH%"
if errorlevel 1 goto :fail
call :sign_release_binary "%EXE_PATH%"
if errorlevel 1 goto :fail
call :sign_release_binary "%APP_EXE_PATH%"
if errorlevel 1 goto :fail
call :sign_release_binary "%PROBE_PATH%"
if errorlevel 1 goto :fail
call :sign_release_binary "%MCP_EXE_PATH%"
if errorlevel 1 goto :fail
rem Signing may change the built application; reseal only this controlled path.
if defined OKUFLOW_SIGN_CERT_SHA1 (
    cmake "-DFactsPath=%BUILD_DIR%\okuflow-build-facts\Release.json" -P "%ROOT_DIR%\cmake\SealReleaseBuildFacts.cmake"
    if errorlevel 1 goto :fail
)

echo ===== ASSEMBLE STAGING BUNDLE =====
for /f "usebackq delims=" %%T in (`pwsh.exe -NoProfile -Command "[Guid]::NewGuid().ToString('N')"`) do set "STAGING_DIR=%DIST_DIR%\OkuFlow.staging-%%T"
if not defined STAGING_DIR goto :fail
if exist "%STAGING_DIR%" (
    echo ERROR: Refusing a staging name already in use: "%STAGING_DIR%".
    goto :fail
)
pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\publish_release_bundle.ps1" -DistDirectory "%DIST_DIR%" -StagingDirectory "%STAGING_DIR%" -Action CreateStage
if errorlevel 1 goto :fail
set "BUNDLE_STAGE_CREATED=1"

copy /y "%EXE_PATH%" "%STAGING_DIR%\oku_flow.exe" >nul
if errorlevel 1 goto :fail
copy /y "%APP_EXE_PATH%" "%STAGING_DIR%\OkuFlowApp.exe" >nul
if errorlevel 1 goto :fail
copy /y "%MCP_EXE_PATH%" "%STAGING_DIR%\okuflow_mcp.exe" >nul
if errorlevel 1 goto :fail
copy /y "%PROBE_PATH%" "%STAGING_DIR%\mf_dxva_minimal.exe" >nul
if errorlevel 1 goto :fail

echo Using Qt runtime from "%QT_BIN_DIR%".
echo Running Qt deployment tool: "%WINDEPLOYQT%"
"%WINDEPLOYQT%" --release "%STAGING_DIR%\OkuFlowApp.exe" ^
    --dir "%STAGING_DIR%" --no-translations
if errorlevel 1 (
    echo ERROR: windeployqt failed; no bundle was published.
    goto :fail
)

rem Proprietary NVIDIA runtimes and Codex CLI are installed
rem separately through Setup Assistant. Qt's software OpenGL fallback is not
rem part of the supported GPU path.
if exist "%STAGING_DIR%\opengl32sw.dll" del /q "%STAGING_DIR%\opengl32sw.dll"
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%\scripts\stage_windows_runtime.ps1" -BundlePath "%STAGING_DIR%"
if errorlevel 1 goto :fail

call :copy_required_file "%ROOT_DIR%\LICENSE" "%STAGING_DIR%\LICENSE"
if errorlevel 1 goto :fail
call :copy_required_file "%ROOT_DIR%\COMMERCIAL.md" "%STAGING_DIR%\COMMERCIAL.md"
if errorlevel 1 goto :fail
call :copy_required_file "%ROOT_DIR%\README.md" "%STAGING_DIR%\README.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%ROOT_DIR%\docs\portable_start_here.txt" "%STAGING_DIR%\START_HERE.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%ROOT_DIR%\docs\THIRD_PARTY_LICENSES.md" "%STAGING_DIR%\THIRD_PARTY_LICENSES.md"
if errorlevel 1 goto :fail

mkdir "%STAGING_DIR%\licenses" 2>nul
call :copy_required_file "%QT_LICENSE_FILE%" "%STAGING_DIR%\licenses\QT_LICENSE.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%QT_FFMPEG_LICENSE_FILE%" "%STAGING_DIR%\licenses\QT_FFMPEG_LGPL_2_1.txt"
if errorlevel 1 goto :fail
mkdir "%STAGING_DIR%\licenses\qt-sbom" 2>nul
for %%M in (qtbase qtimageformats qtmultimedia qtspeech qtsvg) do (
    call :copy_required_file "%QT_SBOM_DIR%\%%M-%QT_RUNTIME_VERSION%.spdx.json" "%STAGING_DIR%\licenses\qt-sbom\%%M-%QT_RUNTIME_VERSION%.spdx.json"
    if errorlevel 1 goto :fail
)
rem Qt PDF is not needed by this desktop build. Preserve its
rem exact notice when installed, and require it if deployment includes PDF.
if exist "%QT_SBOM_DIR%\qtpdf-%QT_RUNTIME_VERSION%.spdx.json" call :copy_required_file "%QT_SBOM_DIR%\qtpdf-%QT_RUNTIME_VERSION%.spdx.json" "%STAGING_DIR%\licenses\qt-sbom\qtpdf-%QT_RUNTIME_VERSION%.spdx.json"
if errorlevel 1 goto :fail
call :copy_required_file "%ROOT_DIR%\assets\icons\lucide\LICENSE" "%STAGING_DIR%\licenses\LUCIDE_LICENSE.txt"
if errorlevel 1 goto :fail
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" call :copy_required_file "%ROOT_DIR%\third_party\amd_fsr1\LICENSE.txt" "%STAGING_DIR%\licenses\AMD_FSR1_LICENSE.txt"
if errorlevel 1 goto :fail
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" call :copy_required_file "%ROOT_DIR%\third_party\nvidia_nis\LICENSE.txt" "%STAGING_DIR%\licenses\NVIDIA_NIS_LICENSE.txt"
if errorlevel 1 goto :fail
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" call :copy_required_file "%ROOT_DIR%\third_party\maxine\Maxine-VFX-SDK\LICENSE" "%STAGING_DIR%\licenses\NVIDIA_MAXINE_SDK_HEADERS_LICENSE.txt"
if errorlevel 1 goto :fail
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" call :copy_required_file "%ROOT_DIR%\third_party\maxine\LICENSE.txt" "%STAGING_DIR%\licenses\NVIDIA_MAXINE_INTEGRATION_NOTICE.txt"
if errorlevel 1 goto :fail
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" call :copy_required_file "%BUNDLE_CUDA_LICENSE_PATH%" "%STAGING_DIR%\licenses\NVIDIA_CUDA_EULA.txt"
if errorlevel 1 goto :fail

echo ===== STAGE SOURCE-BUILT ANDROID DEBUG BRIDGE =====
rem Phone-camera support needs adb in platform-tools\. The helper copies only
rem the reviewed source build, dependency DLLs, notices and relinking source.
rem There is no bypass that ships a bundle without phone support or source.
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%\scripts\stage_platform_tools.ps1" ^
    -BundlePath "%STAGING_DIR%"
if errorlevel 1 (
    echo ERROR: Reviewed source-built adb could not be staged.
    echo Run scripts\build_adb_from_source.ps1 or set OKUFLOW_PLATFORM_TOOLS_DIR.
    goto :fail
)

echo ===== ORGANIZE PORTABLE LAUNCH PATH =====
pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\organize_release_bundle.ps1" -BundlePath "%STAGING_DIR%"
if errorlevel 1 goto :fail

echo ===== GENERATE RELEASE INTEGRITY METADATA =====
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%\scripts\generate_release_metadata.ps1" ^
    -BundlePath "%STAGING_DIR%" -RepositoryPath "%ROOT_DIR%" -QtVersion "%QT_RUNTIME_VERSION%" ^
    -BuildDirectory "%BUILD_DIR%"
if errorlevel 1 (
    echo ERROR: Release checksums, manifest, or SBOM could not be generated.
    goto :fail
)

call :validate_staging_bundle
if errorlevel 1 goto :fail

echo ===== PUBLISH VALIDATED BUNDLE =====
call :publish_staging_bundle
if errorlevel 1 goto :fail

popd
echo.
if "%PUBLISHED_BUNDLE%"=="%SECONDARY_DIR%" (
    echo The existing dist\OkuFlow bundle remains in use or locked.
    echo The new complete bundle is ready at:
) else (
    echo OkuFlow tested bundle is ready at:
)
echo     %PUBLISHED_BUNDLE%
echo.
echo Contents:
dir /b "%PUBLISHED_BUNDLE%"
echo.
echo Launch oku_flow.exe from that folder or zip the complete directory.
endlocal
exit /b 0

:publish_staging_bundle
goto :publish_staging_bundle_impl

:verify_published_executable
goto :verify_published_executable_impl

:compute_sha256
goto :compute_sha256_impl

:sign_release_binary
goto :sign_release_binary_impl

:resolve_qt
set "QT_ROOT="
if defined QT_PREFIX (
    set "QT_ROOT=%QT_PREFIX%"
) else if defined Qt6_DIR (
    for %%I in ("%Qt6_DIR%\..\..\..") do set "QT_ROOT=%%~fI"
) else (
    set "QT_ROOT=%QT_PREFIX_DEFAULT%"
)
for %%I in ("%QT_ROOT%") do set "QT_ROOT=%%~fI"
set "QT_BIN_DIR=%QT_ROOT%\bin"
set "WINDEPLOYQT=%QT_BIN_DIR%\windeployqt.exe"
set "QT_LICENSE_FILE=%QT_ROOT%\LICENSES\LGPL-3.0-only.txt"
if not exist "%QT_LICENSE_FILE%" for %%I in ("%QT_ROOT%\..\..\Licenses\LICENSE") do set "QT_LICENSE_FILE=%%~fI"
if not exist "%QT_BIN_DIR%\qmake.exe" (
    echo ERROR: Qt was not found at "%QT_ROOT%".
    echo Set QT_PREFIX to the Qt msvc2022_64 root, set Qt6_DIR to its
    echo Qt6 CMake package, or install the documented default.
    exit /b 1
)
if not exist "%WINDEPLOYQT%" (
    echo ERROR: windeployqt.exe was not found at "%WINDEPLOYQT%".
    echo Set QT_PREFIX or Qt6_DIR to a complete Qt msvc2022_64 installation.
    exit /b 1
)
if not exist "%QT_LICENSE_FILE%" (
    echo ERROR: Qt license text was not found for "%QT_ROOT%".
    exit /b 1
)
set "QT_RUNTIME_VERSION="
for /f "usebackq delims=" %%V in (`"%QT_BIN_DIR%\qmake.exe" -query QT_VERSION`) do set "QT_RUNTIME_VERSION=%%V"
if not defined QT_RUNTIME_VERSION (
    echo ERROR: The Qt runtime version could not be queried from qmake.exe.
    exit /b 1
)
set "QT_SBOM_DIR=%QT_ROOT%\sbom"
for %%M in (qtbase qtimageformats qtmultimedia qtspeech qtsvg) do if not exist "%QT_SBOM_DIR%\%%M-%QT_RUNTIME_VERSION%.spdx.json" (
    echo ERROR: Required Qt module SBOM is missing: "%QT_SBOM_DIR%\%%M-%QT_RUNTIME_VERSION%.spdx.json".
    exit /b 1
)
if defined QT_SOURCE_ROOT (
    for %%I in ("%QT_SOURCE_ROOT%") do set "QT_SOURCE_ROOT=%%~fI"
) else (
    for %%I in ("%QT_ROOT%\..\Src") do set "QT_SOURCE_ROOT=%%~fI"
)
set "QT_FFMPEG_LICENSE_FILE=%QT_SOURCE_ROOT%\qtmultimedia\src\3rdparty\ffmpeg\LICENSE.LGPL-2.1-or-later.txt"
if not exist "%QT_FFMPEG_LICENSE_FILE%" (
    echo ERROR: The FFmpeg LGPL-2.1-or-later text from the matching Qt source tree is missing.
    echo Install the Qt Sources component or set QT_SOURCE_ROOT to that version's Src directory.
    exit /b 1
)
set "QT_PREFIX=%QT_ROOT%"
exit /b 0

:find_built_executable
rem Select only this generator's output. Old Release subdirectories can remain
rem after a Ninja migration and must never take precedence over its new binary.
set "EXE_PATH=%BUILD_DIR%\cmake\oku_flow.exe"
set "PROBE_PATH=%BUILD_DIR%\sandbox_mf_dxva_minimal\mf_dxva_minimal.exe"
if not exist "%EXE_PATH%" (
    echo ERROR: Built executable was not found under "%BUILD_DIR%".
    exit /b 1
)
set "APP_EXE_PATH=%EXE_PATH%"
for %%I in ("%APP_EXE_PATH%") do set "MCP_EXE_PATH=%%~dpIokuflow_mcp.exe"
if not exist "%MCP_EXE_PATH%" (
    echo ERROR: The first-party MCP bridge was not built.
    exit /b 1
)
for %%I in ("%APP_EXE_PATH%") do set "EXE_PATH=%%~dpIokuflow_launcher.exe"
if not exist "%EXE_PATH%" (
    echo ERROR: Runtime preflight launcher was not built: "%EXE_PATH%".
    exit /b 1
)
if not exist "%PROBE_PATH%" (
    echo ERROR: Isolated camera probe was not found under "%BUILD_DIR%".
    exit /b 1
)
exit /b 0

:copy_required_file
if not exist "%~1" (
    echo ERROR: Required bundle source is missing: "%~1".
    exit /b 1
)
copy /y "%~1" "%~2" >nul
if errorlevel 1 (
    echo ERROR: Could not copy required bundle file to "%~2".
    exit /b 1
)
if not exist "%~2" (
    echo ERROR: Required copied bundle file is missing: "%~2".
    exit /b 1
)
fc /b "%~1" "%~2" >nul
if errorlevel 1 (
    echo ERROR: Required copied bundle file differs from its source: "%~2".
    exit /b 1
)
echo Verified required bundle file: "%~2"
exit /b 0

:validate_staging_bundle
if not exist "%STAGING_DIR%\oku_flow.exe" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\OkuFlowApp.exe" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\okuflow_mcp.exe" goto :deploy_incomplete
if not exist "%STAGING_DIR%\START_HERE.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\vc_redist.x64.exe" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\licenses\MICROSOFT_VC_RUNTIME_LICENSE.rtf" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\mf_dxva_minimal.exe" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\Qt6Core.dll" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\Qt6Gui.dll" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\Qt6Widgets.dll" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\Qt6Network.dll" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\platforms\qwindows.dll" goto :deploy_incomplete
if not exist "%STAGING_DIR%\LICENSE" goto :deploy_incomplete
if not exist "%STAGING_DIR%\COMMERCIAL.md" goto :deploy_incomplete
if not exist "%STAGING_DIR%\THIRD_PARTY_LICENSES.md" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\licenses\QT_LICENSE.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\licenses\QT_FFMPEG_LGPL_2_1.txt" goto :deploy_incomplete
for %%M in (qtbase qtimageformats qtmultimedia qtspeech qtsvg) do if not exist "%STAGING_DIR%\app\licenses\qt-sbom\%%M-%QT_RUNTIME_VERSION%.spdx.json" goto :deploy_incomplete
if exist "%STAGING_DIR%\app\Qt6Pdf.dll" if not exist "%STAGING_DIR%\app\licenses\qt-sbom\qtpdf-%QT_RUNTIME_VERSION%.spdx.json" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\licenses\LUCIDE_LICENSE.txt" goto :deploy_incomplete
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" if not exist "%STAGING_DIR%\app\licenses\AMD_FSR1_LICENSE.txt" goto :deploy_incomplete
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" if not exist "%STAGING_DIR%\app\licenses\NVIDIA_NIS_LICENSE.txt" goto :deploy_incomplete
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" if not exist "%STAGING_DIR%\app\licenses\NVIDIA_MAXINE_SDK_HEADERS_LICENSE.txt" goto :deploy_incomplete
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" if not exist "%STAGING_DIR%\app\licenses\NVIDIA_MAXINE_INTEGRATION_NOTICE.txt" goto :deploy_incomplete
if /I "%BUNDLE_EFFECTIVE_CUDA%"=="ON" if not exist "%STAGING_DIR%\app\licenses\NVIDIA_CUDA_EULA.txt" goto :deploy_incomplete
for %%F in (adb.exe AdbWinApi.dll AdbWinUsbApi.dll NOTICE.txt source.properties adb-build.json) do if not exist "%STAGING_DIR%\app\platform-tools\%%F" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\licenses\adb\LIBUSB_LGPL_2_1.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\app\sources\adb-37.0.0-okuflow-source.zip" goto :deploy_incomplete
if not exist "%STAGING_DIR%\SHA256SUMS.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\release-manifest.json" goto :deploy_incomplete
if not exist "%STAGING_DIR%\SBOM.spdx.json" goto :deploy_incomplete
fc /b "%EXE_PATH%" "%STAGING_DIR%\oku_flow.exe" >nul
if errorlevel 1 (
    echo ERROR: Staged executable does not match the tested release executable.
    exit /b 1
)
fc /b "%APP_EXE_PATH%" "%STAGING_DIR%\app\OkuFlowApp.exe" >nul
if errorlevel 1 (
    echo ERROR: Staged Qt application does not match the tested build.
    exit /b 1
)
fc /b "%MCP_EXE_PATH%" "%STAGING_DIR%\app\okuflow_mcp.exe" >nul
if errorlevel 1 (
    echo ERROR: Staged MCP bridge does not match the tested build.
    exit /b 1
)
exit /b 0

:deploy_incomplete
echo ERROR: Qt deployment or ancillary-file staging is incomplete.
echo Required files include oku_flow.exe, mf_dxva_minimal.exe,
echo Qt6Core/Gui/Widgets/Network.dll,
echo platforms\qwindows.dll, LICENSE, COMMERCIAL.md, THIRD_PARTY_LICENSES.md,
echo complete Qt/FFmpeg/module-SPDX, Lucide, CUDA-upscaler, Maxine-header,
echo and CUDA Toolkit notices,
echo platform-tools\adb.exe, AdbWinApi.dll, AdbWinUsbApi.dll, NOTICE.txt,
echo source.properties, adb-build.json, full adb dependency notices,
echo sources\adb-37.0.0-okuflow-source.zip,
echo SHA256SUMS.txt, release-manifest.json, and SBOM.spdx.json.
exit /b 1

:sign_release_binary_impl
if not defined OKUFLOW_SIGN_CERT_SHA1 (
    if "%OKUFLOW_PUBLIC_RELEASE%"=="1" (
        echo ERROR: OKUFLOW_PUBLIC_RELEASE=1 requires OKUFLOW_SIGN_CERT_SHA1.
        exit /b 1
    )
    echo WARNING: No Authenticode certificate configured; "%~nx1" remains unsigned.
    exit /b 0
)
set "SIGNTOOL_PATH="
for /f "usebackq delims=" %%S in (`pwsh.exe -NoProfile -Command "$p = Get-ChildItem -Path '${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe' -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName; if ($p) { $p }"`) do if not defined SIGNTOOL_PATH set "SIGNTOOL_PATH=%%S"
if not defined SIGNTOOL_PATH (
    echo ERROR: signtool.exe was not found in the Windows 10 SDK.
    exit /b 1
)
"%SIGNTOOL_PATH%" sign /sha1 "%OKUFLOW_SIGN_CERT_SHA1%" /fd SHA256 ^
    /tr http://timestamp.digicert.com /td SHA256 "%~1"
if errorlevel 1 (
    echo ERROR: Authenticode signing failed for "%~1".
    exit /b 1
)
"%SIGNTOOL_PATH%" verify /pa "%~1"
if errorlevel 1 (
    echo ERROR: Authenticode verification failed for "%~1".
    exit /b 1
)
exit /b 0

:verify_published_executable_impl
call :compute_sha256 "%EXE_PATH%" EXPECTED_EXE_SHA256
if errorlevel 1 exit /b 1
call :compute_sha256 "%PUBLISHED_BUNDLE%\oku_flow.exe" PUBLISHED_EXE_SHA256
if errorlevel 1 exit /b 1
if /I not "%EXPECTED_EXE_SHA256%"=="%PUBLISHED_EXE_SHA256%" (
    echo ERROR: "%PUBLISHED_BUNDLE%\oku_flow.exe" does not match "%EXE_PATH%".
    exit /b 1
)
echo Verified oku_flow.exe SHA-256: %PUBLISHED_EXE_SHA256%
call :compute_sha256 "%APP_EXE_PATH%" EXPECTED_APP_SHA256
if errorlevel 1 exit /b 1
call :compute_sha256 "%PUBLISHED_BUNDLE%\app\OkuFlowApp.exe" PUBLISHED_APP_SHA256
if errorlevel 1 exit /b 1
if /I not "%EXPECTED_APP_SHA256%"=="%PUBLISHED_APP_SHA256%" (
    echo ERROR: Published OkuFlowApp.exe does not match the built Qt application.
    exit /b 1
)
echo Verified OkuFlowApp.exe SHA-256: %PUBLISHED_APP_SHA256%
call :compute_sha256 "%MCP_EXE_PATH%" EXPECTED_MCP_SHA256
if errorlevel 1 exit /b 1
call :compute_sha256 "%PUBLISHED_BUNDLE%\app\okuflow_mcp.exe" PUBLISHED_MCP_SHA256
if errorlevel 1 exit /b 1
if /I not "%EXPECTED_MCP_SHA256%"=="%PUBLISHED_MCP_SHA256%" (
    echo ERROR: Published MCP bridge does not match the built executable.
    exit /b 1
)
echo Verified okuflow_mcp.exe SHA-256: %PUBLISHED_MCP_SHA256%
exit /b 0

:compute_sha256_impl
set "%~2="
for /f "skip=1 tokens=*" %%H in ('certutil -hashfile "%~1" SHA256 2^>nul') do if not defined %~2 set "%~2=%%H"
if not defined %~2 (
    echo ERROR: Could not calculate SHA-256 for "%~1".
    exit /b 1
)
exit /b 0

:publish_staging_bundle_impl
set "PUBLISHED_BUNDLE="
for /f "usebackq delims=" %%T in (`pwsh.exe -NoProfile -Command "[IO.Path]::GetTempFileName()"`) do set "BUNDLE_PUBLISH_RESULT=%%T"
if not defined BUNDLE_PUBLISH_RESULT exit /b 1
rem Preserve staging/recovery evidence if the publication transaction fails.
set "BUNDLE_STAGE_CREATED="
pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\publish_release_bundle.ps1" -DistDirectory "%DIST_DIR%" -StagingDirectory "%STAGING_DIR%" -ResultPath "%BUNDLE_PUBLISH_RESULT%"
if errorlevel 1 exit /b 1
if exist "%STAGING_DIR%" (
    echo ERROR: Transactional publication did not consume the owned staging directory.
    exit /b 1
)
set /p "PUBLISHED_BUNDLE=" < "%BUNDLE_PUBLISH_RESULT%"
del /q "%BUNDLE_PUBLISH_RESULT%"
set "BUNDLE_PUBLISH_RESULT="
if not defined PUBLISHED_BUNDLE exit /b 1
call :verify_published_executable
exit /b %errorlevel%

:fail
if defined BUNDLE_PUBLISH_RESULT if exist "%BUNDLE_PUBLISH_RESULT%" del /q "%BUNDLE_PUBLISH_RESULT%"
if defined BUNDLE_FACTS_ENV if exist "%BUNDLE_FACTS_ENV%" del /q "%BUNDLE_FACTS_ENV%"
if defined BUNDLE_STAGE_CREATED if exist "%STAGING_DIR%" pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\publish_release_bundle.ps1" -DistDirectory "%DIST_DIR%" -StagingDirectory "%STAGING_DIR%" -Action DiscardStage
popd
:fail_no_popd
echo.
echo OkuFlow release bundle failed. No incomplete bundle was declared ready.
endlocal
exit /b 1

:initialize_toolchain
set "BUNDLE_VSDEVCMD="
set "BUNDLE_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
rem VsDevCmd also invokes vswhere by name on some installed SDKs.
set "Path=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%Path%"
if exist "%BUNDLE_VSWHERE%" (
    for /f "usebackq delims=" %%V in (`call "%BUNDLE_VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find **\VsDevCmd.bat`) do if not defined BUNDLE_VSDEVCMD set "BUNDLE_VSDEVCMD=%%V"
)
if not defined BUNDLE_VSDEVCMD if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" set "BUNDLE_VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if not defined BUNDLE_VSDEVCMD (
    echo ERROR: Visual Studio 2022 C++ build tools were not found.
    echo Install the Desktop development with C++ workload and try again.
    exit /b 1
)
call "%BUNDLE_VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
    echo ERROR: Visual Studio 2022 x64 environment could not be initialized.
    exit /b 1
)
exit /b 0
