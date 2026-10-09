@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "ROOT_DIR=%SCRIPT_DIR%.."
pushd "%ROOT_DIR%" >nul
if errorlevel 1 (
    echo FAIL: repository root could not be opened.
    endlocal
    exit /b 1
)
set "ROOT_DIR=%CD%"

set "RELEASE_RESULT=SKIP"
set "CPU_RESULT=SKIP"
set "CUDA_RESULT=SKIP"
set "VSDEVCMD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

echo ===== TRANSLATION INTEGRITY GATE =====
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%\scripts\check_translations.ps1"
if errorlevel 1 (
    echo FAIL: translation catalogs are incomplete or stale.
    goto :summary
)

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find **\VsDevCmd.bat`) do (
        if not defined VSDEVCMD set "VSDEVCMD=%%V"
    )
)
if not defined VSDEVCMD if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" (
    set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
)
if not defined VSDEVCMD (
    echo FAIL: Visual Studio 2022 C++ build tools were not found.
    echo Install the Desktop development with C++ workload and try again.
    goto :summary
)

call "%VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
    echo FAIL: Visual Studio x64 developer environment could not be initialized.
    goto :summary
)

echo ===== RELEASE COMPILE GATE =====
cmake --preset msvc-release
if errorlevel 1 (
    set "RELEASE_RESULT=FAIL"
    goto :summary
)
cmake --build --preset msvc-release-build
if errorlevel 1 (
    set "RELEASE_RESULT=FAIL"
    goto :summary
)
set "RELEASE_RESULT=PASS"

echo ===== CPU TEST GATE =====
cmake --preset msvc-cpu
if errorlevel 1 (
    set "CPU_RESULT=FAIL"
    goto :summary
)
cmake --build --preset msvc-cpu-build
if errorlevel 1 (
    set "CPU_RESULT=FAIL"
    goto :summary
)
ctest --preset msvc-cpu-tests
if errorlevel 1 (
    set "CPU_RESULT=FAIL"
    goto :summary
)
set "CPU_RESULT=PASS"

echo ===== CUDA TEST GATE =====
cmake --preset msvc-cuda-tests
if errorlevel 1 (
    set "CUDA_RESULT=FAIL"
    goto :summary
)
cmake --build --preset msvc-cuda-tests-build
if errorlevel 1 (
    set "CUDA_RESULT=FAIL"
    goto :summary
)
ctest --preset msvc-cuda-tests
if errorlevel 1 (
    set "CUDA_RESULT=FAIL"
    goto :summary
)
set "CUDA_RESULT=PASS"

:summary
echo.
echo ===== OKUFLOW BUILD GATE SUMMARY =====
echo %RELEASE_RESULT%: release compile
echo %CPU_RESULT%: CPU test suite
echo %CUDA_RESULT%: CUDA test suite

set "FINAL_RESULT=0"
if not "%RELEASE_RESULT%"=="PASS" set "FINAL_RESULT=1"
if not "%CPU_RESULT%"=="PASS" set "FINAL_RESULT=1"
if not "%CUDA_RESULT%"=="PASS" set "FINAL_RESULT=1"

popd
endlocal & exit /b %FINAL_RESULT%
