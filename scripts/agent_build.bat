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
set "AGENT_CUDA_RESULT=SKIP"
set "SCOPED_RESULT=FAIL"
set "SCOPED_MODE="
if /I "%~1"=="--scoped" set "SCOPED_MODE=1"
set "SCOPED_PRESET="
set "AGENT_CUDA_SCOPE=GPU-labeled, CUDA-variant and CUDA-only tests"
set "AGENT_CUDA_SELECTION=-UncoveredByPreset msvc-cpu-tests"
if "%OKUFLOW_FULL_CUDA_SUITE%"=="1" (
    set "AGENT_CUDA_SELECTION="
    set "AGENT_CUDA_SCOPE=full CUDA suite requested by OKUFLOW_FULL_CUDA_SUITE=1"
)
set "TEST_FILTER="
set "TEST_RESUME="
if "%OKUFLOW_TEST_RESUME%"=="1" set "TEST_RESUME=-Resume"
if "%OKUFLOW_AGENT_EXCLUDE_GUI%"=="1" (
    set "TEST_FILTER=-ExcludeGui"
    echo PARTIAL VALIDATION: desktop tests are explicitly excluded.
)
set "VSDEVCMD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

echo ===== TRANSLATION INTEGRITY GATE =====
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%\scripts\check_translations.ps1"
if errorlevel 1 (
    echo FAIL: translation catalogs are incomplete or stale.
    goto :summary
)

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%V in (`call "%VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find **\VsDevCmd.bat`) do (
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

rem Retry only a recorded desktop interruption, in the same VS environment.
rem No rebuild or release publishing occurs in this mode.
if /I "%~1"=="--retry-interrupted" (
    pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\run_test_gate.ps1" -Preset "%~2" -Resume -RetryInterrupted "%~3" %TEST_FILTER%
    if errorlevel 1 goto :retry_failed
    popd
    endlocal
    exit /b 0
)
if /I "%~1"=="--scoped" goto :scoped
if not "%~1"=="" (
    echo FAIL: use --scoped PRESET LABEL-OR-TEST... or --retry-interrupted PRESET EXACT_CTEST_NAME.
    goto :summary
)

echo ===== RELEASE COMPILE GATE =====
rem An isolated release build avoids replacing a running development executable.
set "AGENT_RELEASE_BUILD_DIR="
if defined OKUFLOW_AGENT_RELEASE_BUILD_DIR for %%I in ("%OKUFLOW_AGENT_RELEASE_BUILD_DIR%") do set "AGENT_RELEASE_BUILD_DIR=%%~fI"
if defined AGENT_RELEASE_BUILD_DIR (
    cmake --preset msvc-release -B "%AGENT_RELEASE_BUILD_DIR%"
) else (
    cmake --preset msvc-release
)
if errorlevel 1 (
    set "RELEASE_RESULT=FAIL"
    goto :summary
)
if defined AGENT_RELEASE_BUILD_DIR (
    cmake --build "%AGENT_RELEASE_BUILD_DIR%"
) else (
    cmake --build --preset msvc-release-build
)
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
pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\run_test_gate.ps1" -Preset msvc-cpu-tests %TEST_FILTER% %TEST_RESUME%
if errorlevel 1 (
    set "CPU_RESULT=FAIL"
) else (
    set "CPU_RESULT=PASS"
)

rem Preserve CUDA evidence even when a CPU desktop fixture was interrupted.
echo ===== CUDA TEST GATE =====
echo Scope: %AGENT_CUDA_SCOPE%
cmake --preset msvc-cuda-tests
if errorlevel 1 (
    set "AGENT_CUDA_RESULT=FAIL"
    goto :summary
)
cmake --build --preset msvc-cuda-tests-build
if errorlevel 1 (
    set "AGENT_CUDA_RESULT=FAIL"
    goto :summary
)
pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\run_test_gate.ps1" -Preset msvc-cuda-tests %AGENT_CUDA_SELECTION% %TEST_FILTER% %TEST_RESUME%
if errorlevel 1 (
    set "AGENT_CUDA_RESULT=FAIL"
    goto :summary
)
set "AGENT_CUDA_RESULT=PASS"

:summary
if defined SCOPED_MODE goto :scoped_summary
echo.
echo ===== OKUFLOW BUILD GATE SUMMARY =====
echo %RELEASE_RESULT%: release compile
echo %CPU_RESULT%: CPU test suite
echo %AGENT_CUDA_RESULT%: CUDA test suite - %AGENT_CUDA_SCOPE%
if defined TEST_FILTER echo PARTIAL VALIDATION: GUI checks remain pending; this is not the full release gate.

set "FINAL_RESULT=0"
if not "%RELEASE_RESULT%"=="PASS" set "FINAL_RESULT=1"
if not "%CPU_RESULT%"=="PASS" set "FINAL_RESULT=1"
if not "%AGENT_CUDA_RESULT%"=="PASS" set "FINAL_RESULT=1"

popd
endlocal & exit /b %FINAL_RESULT%

:scoped
set "SCOPED_PRESET=%~2"
if /I "%SCOPED_PRESET%"=="msvc-cpu-tests" set "SCOPED_PRESET=msvc-cpu"
if /I "%SCOPED_PRESET%"=="msvc-cpu" (
    set "SCOPED_TEST_PRESET=msvc-cpu-tests"
    goto :scoped_arguments
)
if /I "%SCOPED_PRESET%"=="msvc-cuda-tests" (
    set "SCOPED_TEST_PRESET=msvc-cuda-tests"
    goto :scoped_arguments
)
echo FAIL: --scoped requires msvc-cpu or msvc-cuda-tests and at least one label or exact test.
goto :scoped_summary

:scoped_arguments
shift
shift
set "SCOPED_SELECTION="
:scoped_next
if "%~1"=="" goto :scoped_build
if defined SCOPED_SELECTION (
    set "SCOPED_SELECTION=%SCOPED_SELECTION%,%~1"
) else (
    set "SCOPED_SELECTION=%~1"
)
shift
goto :scoped_next

:scoped_build
if not defined SCOPED_SELECTION (
    echo FAIL: --scoped requires at least one label or exact test.
    goto :scoped_summary
)
echo ===== SCOPED BUILD AND TEST GATE =====
cmake --preset %SCOPED_PRESET%
if errorlevel 1 goto :scoped_failed
cmake --build --preset %SCOPED_PRESET%-build
if errorlevel 1 goto :scoped_failed
pwsh.exe -NoProfile -File "%ROOT_DIR%\scripts\run_test_gate.ps1" -Preset %SCOPED_TEST_PRESET% %TEST_FILTER% %TEST_RESUME% -Select "%SCOPED_SELECTION%"
if errorlevel 1 goto :scoped_failed
set "SCOPED_RESULT=PASS"
goto :scoped_summary

:scoped_failed
set "SCOPED_RESULT=FAIL"

:scoped_summary
echo.
echo ===== OKUFLOW SCOPED BUILD GATE SUMMARY =====
echo SCOPED %SCOPED_RESULT%: %SCOPED_PRESET% %SCOPED_SELECTION%
echo SCOPED VALIDATION: this does not establish the full release gate.
if defined TEST_FILTER echo PARTIAL VALIDATION: GUI checks remain pending.
set "FINAL_RESULT=1"
if "%SCOPED_RESULT%"=="PASS" set "FINAL_RESULT=0"
popd
endlocal & exit /b %FINAL_RESULT%

:retry_failed
popd
endlocal
exit /b 1
