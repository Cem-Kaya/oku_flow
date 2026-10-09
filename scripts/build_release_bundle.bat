@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Build, test, stage, validate, and publish a self-contained OkuFlow bundle.
rem Existing bundles and user captures are untouched until staging succeeds.

set "ROOT_DIR=%~dp0.."
pushd "%ROOT_DIR%" >nul
if errorlevel 1 goto :fail_no_popd
set "ROOT_DIR=%CD%"

set "GENERATOR=%CMAKE_GENERATOR%"
if not defined GENERATOR set "GENERATOR=Visual Studio 17 2022"
set "CMAKE_ARCH_ARGS="
if /I "%GENERATOR%"=="Visual Studio 17 2022" set "CMAKE_ARCH_ARGS=-A x64"

set "BUILD_DIR=%ROOT_DIR%\build\release-bundle"
if defined OKUFLOW_BUNDLE_BUILD_DIR for %%I in ("%OKUFLOW_BUNDLE_BUILD_DIR%") do set "BUILD_DIR=%%~fI"
set "DIST_DIR=%ROOT_DIR%\dist"
set "PRIMARY_DIR=%DIST_DIR%\OkuFlow"
set "SECONDARY_DIR=%DIST_DIR%\OkuFlow2"
set "STAGING_DIR=%DIST_DIR%\OkuFlow.staging"
set "BACKUP_DIR=%DIST_DIR%\OkuFlow.previous"
set "QT_PREFIX_DEFAULT=C:\Qt\6.12.0\msvc2022_64"

if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"
if errorlevel 1 goto :fail

call :resolve_qt
if errorlevel 1 goto :fail
set "Path=%QT_BIN_DIR%;%Path%"

set "CMAKE_EXTRA_ARGS=%CMAKE_ARGS%"
if not defined OKUFLOW_ENABLE_CUDA set "OKUFLOW_ENABLE_CUDA=ON"
if not defined OKUFLOW_ENABLE_TEXT_SR set "OKUFLOW_ENABLE_TEXT_SR=ON"
set "CUDA_LICENSE_PATH="
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" call :resolve_cuda_license
if errorlevel 1 goto :fail

echo ===== CONFIGURE TESTED RELEASE =====
cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -G "%GENERATOR%" %CMAKE_ARCH_ARGS% ^
    -DCMAKE_BUILD_TYPE=Release ^
    "-DCMAKE_PREFIX_PATH:PATH=%QT_PREFIX%" ^
    "-DQt6_DIR:PATH=%QT_PREFIX%\lib\cmake\Qt6" ^
    -DOKUFLOW_ENABLE_CUDA=%OKUFLOW_ENABLE_CUDA% ^
    -DOKUFLOW_ENABLE_TEXT_SR=%OKUFLOW_ENABLE_TEXT_SR% ^
    -DOKUFLOW_ENABLE_TESTS=ON ^
    %CMAKE_EXTRA_ARGS%
if errorlevel 1 goto :fail

echo ===== BUILD APPLICATION AND TESTS =====
if /I "%GENERATOR%"=="Visual Studio 17 2022" (
    cmake --build "%BUILD_DIR%" --config Release
) else (
    cmake --build "%BUILD_DIR%"
)
if errorlevel 1 goto :fail

if "%OKUFLOW_SKIP_BUNDLE_TESTS%"=="1" (
    echo.
    echo WARNING: UNTESTED BUNDLE
    echo OKUFLOW_SKIP_BUNDLE_TESTS=1 bypassed the mandatory CTest gate.
    echo This output must not be described as a tested release.
    echo.
) else (
    echo ===== RUN RELEASE TEST GATE =====
    ctest --test-dir "%BUILD_DIR%" -C Release --output-on-failure --no-tests=error
    if errorlevel 1 goto :fail
)

call :find_built_executable
if errorlevel 1 goto :fail
call :sign_release_binary "%EXE_PATH%"
if errorlevel 1 goto :fail
call :sign_release_binary "%PROBE_PATH%"
if errorlevel 1 goto :fail

echo ===== ASSEMBLE STAGING BUNDLE =====
if exist "%STAGING_DIR%" rmdir /s /q "%STAGING_DIR%"
if exist "%STAGING_DIR%" (
    echo ERROR: Could not clear stale staging directory "%STAGING_DIR%".
    goto :fail
)
mkdir "%STAGING_DIR%"
if errorlevel 1 goto :fail

copy /y "%EXE_PATH%" "%STAGING_DIR%\oku_flow.exe" >nul
if errorlevel 1 goto :fail
copy /y "%PROBE_PATH%" "%STAGING_DIR%\mf_dxva_minimal.exe" >nul
if errorlevel 1 goto :fail

echo Using Qt runtime from "%QT_BIN_DIR%".
echo Running Qt deployment tool: "%WINDEPLOYQT%"
"%WINDEPLOYQT%" --release "%STAGING_DIR%\oku_flow.exe" ^
    --dir "%STAGING_DIR%" --no-translations
if errorlevel 1 (
    echo ERROR: windeployqt failed; no bundle was published.
    goto :fail
)

rem Proprietary NVIDIA runtimes and Codex CLI are installed
rem separately through Setup Assistant. Qt's software OpenGL fallback is not
rem part of the supported GPU path.
if exist "%STAGING_DIR%\opengl32sw.dll" del /q "%STAGING_DIR%\opengl32sw.dll"

call :copy_required_file "%ROOT_DIR%\LICENSE" "%STAGING_DIR%\LICENSE"
if errorlevel 1 goto :fail
call :copy_required_file "%ROOT_DIR%\COMMERCIAL.md" "%STAGING_DIR%\COMMERCIAL.md"
if errorlevel 1 goto :fail
call :copy_required_file "%ROOT_DIR%\README.md" "%STAGING_DIR%\README.txt"
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
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" call :copy_required_file "%ROOT_DIR%\third_party\amd_fsr1\LICENSE.txt" "%STAGING_DIR%\licenses\AMD_FSR1_LICENSE.txt"
if errorlevel 1 goto :fail
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" call :copy_required_file "%ROOT_DIR%\third_party\nvidia_nis\LICENSE.txt" "%STAGING_DIR%\licenses\NVIDIA_NIS_LICENSE.txt"
if errorlevel 1 goto :fail
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" call :copy_required_file "%ROOT_DIR%\third_party\maxine\Maxine-VFX-SDK\LICENSE" "%STAGING_DIR%\licenses\NVIDIA_MAXINE_SDK_HEADERS_LICENSE.txt"
if errorlevel 1 goto :fail
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" call :copy_required_file "%ROOT_DIR%\third_party\maxine\LICENSE.txt" "%STAGING_DIR%\licenses\NVIDIA_MAXINE_INTEGRATION_NOTICE.txt"
if errorlevel 1 goto :fail
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" call :copy_required_file "%CUDA_LICENSE_PATH%" "%STAGING_DIR%\licenses\NVIDIA_CUDA_EULA.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%BUILD_DIR%\_deps\libdatachannel-src\LICENSE" "%STAGING_DIR%\licenses\LIBDATACHANNEL_LICENSE.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%BUILD_DIR%\_deps\libdatachannel-src\deps\libjuice\LICENSE" "%STAGING_DIR%\licenses\LIBJUICE_LICENSE.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%BUILD_DIR%\_deps\libdatachannel-src\deps\libsrtp\LICENSE" "%STAGING_DIR%\licenses\LIBSRTP_LICENSE.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%BUILD_DIR%\_deps\libdatachannel-src\deps\usrsctp\LICENSE.md" "%STAGING_DIR%\licenses\USRSCTP_LICENSE.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%BUILD_DIR%\_deps\libdatachannel-src\deps\plog\LICENSE" "%STAGING_DIR%\licenses\PLOG_LICENSE.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%BUILD_DIR%\_deps\libdatachannel-src\deps\json\LICENSE.MIT" "%STAGING_DIR%\licenses\NLOHMANN_JSON_LICENSE.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%BUILD_DIR%\_deps\opus-src\COPYING" "%STAGING_DIR%\licenses\OPUS_COPYING.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%BUILD_DIR%\_deps\opus-src\LICENSE_PLEASE_READ.txt" "%STAGING_DIR%\licenses\OPUS_LICENSE_PLEASE_READ.txt"
if errorlevel 1 goto :fail
call :copy_required_file "%BUILD_DIR%\_deps\mbedtls-src\LICENSE" "%STAGING_DIR%\licenses\MBEDTLS_LICENSE.txt"
if errorlevel 1 goto :fail

echo ===== GENERATE RELEASE INTEGRITY METADATA =====
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%\scripts\generate_release_metadata.ps1" ^
    -BundlePath "%STAGING_DIR%" -RepositoryPath "%ROOT_DIR%" -QtVersion "%QT_RUNTIME_VERSION%" ^
    -CudaEnabled "%OKUFLOW_ENABLE_CUDA%"
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

:resolve_cuda_license
if defined CUDA_PATH if exist "%CUDA_PATH%\EULA.txt" set "CUDA_LICENSE_PATH=%CUDA_PATH%\EULA.txt"
if not defined CUDA_LICENSE_PATH if defined CUDAToolkit_ROOT if exist "%CUDAToolkit_ROOT%\EULA.txt" set "CUDA_LICENSE_PATH=%CUDAToolkit_ROOT%\EULA.txt"
rem Recent toolkits install the same license material as LICENSE.
if not defined CUDA_LICENSE_PATH if defined CUDA_PATH if exist "%CUDA_PATH%\LICENSE" set "CUDA_LICENSE_PATH=%CUDA_PATH%\LICENSE"
if not defined CUDA_LICENSE_PATH if defined CUDAToolkit_ROOT if exist "%CUDAToolkit_ROOT%\LICENSE" set "CUDA_LICENSE_PATH=%CUDAToolkit_ROOT%\LICENSE"
if not defined CUDA_LICENSE_PATH (
    echo ERROR: CUDA is enabled but the CUDA Toolkit EULA.txt or LICENSE was not found.
    echo Set CUDA_PATH or CUDAToolkit_ROOT to the toolkit used for this build.
    exit /b 1
)
exit /b 0

:find_built_executable
set "EXE_PATH=%BUILD_DIR%\cmake\Release\oku_flow.exe"
if not exist "%EXE_PATH%" set "EXE_PATH=%BUILD_DIR%\Release\oku_flow.exe"
if not exist "%EXE_PATH%" set "EXE_PATH=%BUILD_DIR%\cmake\oku_flow.exe"
if not exist "%EXE_PATH%" set "EXE_PATH=%BUILD_DIR%\oku_flow.exe"
if not exist "%EXE_PATH%" (
    echo ERROR: Built executable was not found under "%BUILD_DIR%".
    exit /b 1
)
set "PROBE_PATH=%BUILD_DIR%\sandbox_mf_dxva_minimal\Release\mf_dxva_minimal.exe"
if not exist "%PROBE_PATH%" set "PROBE_PATH=%BUILD_DIR%\sandbox_mf_dxva_minimal\mf_dxva_minimal.exe"
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
exit /b 0

:validate_staging_bundle
if not exist "%STAGING_DIR%\oku_flow.exe" goto :deploy_incomplete
if not exist "%STAGING_DIR%\mf_dxva_minimal.exe" goto :deploy_incomplete
if not exist "%STAGING_DIR%\Qt6Core.dll" goto :deploy_incomplete
if not exist "%STAGING_DIR%\Qt6Gui.dll" goto :deploy_incomplete
if not exist "%STAGING_DIR%\Qt6Widgets.dll" goto :deploy_incomplete
if not exist "%STAGING_DIR%\Qt6Network.dll" goto :deploy_incomplete
if not exist "%STAGING_DIR%\platforms\qwindows.dll" goto :deploy_incomplete
if not exist "%STAGING_DIR%\LICENSE" goto :deploy_incomplete
if not exist "%STAGING_DIR%\COMMERCIAL.md" goto :deploy_incomplete
if not exist "%STAGING_DIR%\THIRD_PARTY_LICENSES.md" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\QT_LICENSE.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\QT_FFMPEG_LGPL_2_1.txt" goto :deploy_incomplete
for %%M in (qtbase qtimageformats qtmultimedia qtspeech qtsvg) do if not exist "%STAGING_DIR%\licenses\qt-sbom\%%M-%QT_RUNTIME_VERSION%.spdx.json" goto :deploy_incomplete
if exist "%STAGING_DIR%\Qt6Pdf.dll" if not exist "%STAGING_DIR%\licenses\qt-sbom\qtpdf-%QT_RUNTIME_VERSION%.spdx.json" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\LUCIDE_LICENSE.txt" goto :deploy_incomplete
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" if not exist "%STAGING_DIR%\licenses\AMD_FSR1_LICENSE.txt" goto :deploy_incomplete
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" if not exist "%STAGING_DIR%\licenses\NVIDIA_NIS_LICENSE.txt" goto :deploy_incomplete
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" if not exist "%STAGING_DIR%\licenses\NVIDIA_MAXINE_SDK_HEADERS_LICENSE.txt" goto :deploy_incomplete
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" if not exist "%STAGING_DIR%\licenses\NVIDIA_MAXINE_INTEGRATION_NOTICE.txt" goto :deploy_incomplete
if /I "%OKUFLOW_ENABLE_CUDA%"=="ON" if not exist "%STAGING_DIR%\licenses\NVIDIA_CUDA_EULA.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\LIBDATACHANNEL_LICENSE.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\LIBJUICE_LICENSE.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\LIBSRTP_LICENSE.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\USRSCTP_LICENSE.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\PLOG_LICENSE.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\NLOHMANN_JSON_LICENSE.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\OPUS_COPYING.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\OPUS_LICENSE_PLEASE_READ.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\licenses\MBEDTLS_LICENSE.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\SHA256SUMS.txt" goto :deploy_incomplete
if not exist "%STAGING_DIR%\release-manifest.json" goto :deploy_incomplete
if not exist "%STAGING_DIR%\SBOM.spdx.json" goto :deploy_incomplete
fc /b "%EXE_PATH%" "%STAGING_DIR%\oku_flow.exe" >nul
if errorlevel 1 (
    echo ERROR: Staged executable does not match the tested release executable.
    exit /b 1
)
exit /b 0

:deploy_incomplete
echo ERROR: Qt deployment or ancillary-file staging is incomplete.
echo Required files include oku_flow.exe, mf_dxva_minimal.exe,
echo Qt6Core/Gui/Widgets/Network.dll,
echo platforms\qwindows.dll, LICENSE, COMMERCIAL.md, THIRD_PARTY_LICENSES.md,
echo complete Qt/FFmpeg/module-SPDX, Lucide, CUDA-upscaler, Maxine-header,
echo native-WebRTC, and CUDA Toolkit notices,
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
if exist "%BACKUP_DIR%" (
    rmdir /s /q "%BACKUP_DIR%"
    if exist "%BACKUP_DIR%" (
        echo ERROR: Stale bundle backup could not be removed: "%BACKUP_DIR%".
        exit /b 1
    )
)

if not exist "%PRIMARY_DIR%" (
    move "%STAGING_DIR%" "%PRIMARY_DIR%" >nul
    if errorlevel 1 exit /b 1
    set "PUBLISHED_BUNDLE=%PRIMARY_DIR%"
    call :verify_published_executable
    if errorlevel 1 exit /b 1
    exit /b 0
)

rem Legacy output is user-owned. Never move it into a disposable bundle
rem backup; leave the primary tree untouched and publish the new app beside it.
if exist "%PRIMARY_DIR%\output" (
    echo Existing dist\OkuFlow contains legacy user output; leaving it untouched.
    goto :publish_secondary
)

if exist "%PRIMARY_DIR%\oku_flow.exe" (
    ren "%PRIMARY_DIR%\oku_flow.exe" "oku_flow.bundle-lock-check.exe" >nul 2>&1
    if errorlevel 1 goto :publish_secondary
    ren "%PRIMARY_DIR%\oku_flow.bundle-lock-check.exe" "oku_flow.exe" >nul 2>&1
    if errorlevel 1 (
        echo ERROR: Bundle lock check could not restore oku_flow.exe.
        exit /b 1
    )
)

move "%PRIMARY_DIR%" "%BACKUP_DIR%" >nul
if errorlevel 1 goto :publish_secondary

move "%STAGING_DIR%" "%PRIMARY_DIR%" >nul
if errorlevel 1 (
    echo ERROR: Validated staging bundle could not replace dist\OkuFlow.
    goto :rollback_primary
)

set "PUBLISHED_BUNDLE=%PRIMARY_DIR%"
call :verify_published_executable
if errorlevel 1 (
    echo ERROR: Published executable verification failed.
    exit /b 1
)
rmdir /s /q "%BACKUP_DIR%"
if exist "%BACKUP_DIR%" (
    echo WARNING: The old runtime backup remains at "%BACKUP_DIR%".
)
exit /b 0

:rollback_primary
if not exist "%PRIMARY_DIR%" move "%BACKUP_DIR%" "%PRIMARY_DIR%" >nul
exit /b 1

:publish_secondary
echo Existing dist\OkuFlow cannot be replaced safely; publishing to dist\OkuFlow2.
if exist "%SECONDARY_DIR%" (
    if exist "%SECONDARY_DIR%\output" (
        echo ERROR: dist\OkuFlow2 contains legacy user output and was left untouched.
        echo Open that bundle once to copy its files, then retry packaging.
        exit /b 1
    )
    rmdir /s /q "%SECONDARY_DIR%"
    if exist "%SECONDARY_DIR%" (
        echo ERROR: Existing dist\OkuFlow2 is also in use.
        exit /b 1
    )
)
move "%STAGING_DIR%" "%SECONDARY_DIR%" >nul
if errorlevel 1 exit /b 1
set "PUBLISHED_BUNDLE=%SECONDARY_DIR%"
call :verify_published_executable
if errorlevel 1 exit /b 1
exit /b 0

:fail
if exist "%STAGING_DIR%" rmdir /s /q "%STAGING_DIR%"
popd
:fail_no_popd
echo.
echo OkuFlow release bundle failed. No incomplete bundle was declared ready.
endlocal
exit /b 1
