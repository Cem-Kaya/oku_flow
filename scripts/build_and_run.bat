@echo off
setlocal enabledelayedexpansion

set ROOT_DIR=%~dp0..

rem Map UNC paths (e.g. WSL shares) to a temporary drive letter so CUDA/MSBuild behave
pushd "%ROOT_DIR%" >nul
set ROOT_DIR=%CD%

set BUILD_DIR=%ROOT_DIR%\build
set QT_PREFIX_DEFAULT=C:\Qt\6.9.3\msvc2022_64

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

pushd "%BUILD_DIR%" >nul
set BUILD_DIR=%CD%

set CACHE_FILE=%BUILD_DIR%\CMakeCache.txt
if exist "%CACHE_FILE%" (
    set "BUILD_DIR_FWD=%BUILD_DIR:\=/%"
    findstr /I /C:"# For build in directory: !BUILD_DIR_FWD!" "%CACHE_FILE%" >nul
    if errorlevel 1 (
        echo Clearing stale CMake cache at %CACHE_FILE%
        del /f /q "%CACHE_FILE%" >nul 2>nul
        if exist "%BUILD_DIR%\CMakeFiles" (
            rmdir /s /q "%BUILD_DIR%\CMakeFiles"
        )
    )
)

set "QT_BIN_DIR="

if defined QT_PREFIX (
    set "CMAKE_QT_ARGS=-DCMAKE_PREFIX_PATH=%QT_PREFIX%"
    call :resolve_qt_bindir "%QT_PREFIX%"
) else if defined Qt6_DIR (
    set "CMAKE_QT_ARGS=-DQt6_DIR=%Qt6_DIR%"
    call :resolve_qt_bindir "%Qt6_DIR%"
) else if exist "%QT_PREFIX_DEFAULT%" (
    set "CMAKE_QT_ARGS=-DCMAKE_PREFIX_PATH=%QT_PREFIX_DEFAULT%"
    call :resolve_qt_bindir "%QT_PREFIX_DEFAULT%"
) else (
    set "CMAKE_QT_ARGS="
)

if not defined QT_BIN_DIR if exist "%QT_PREFIX_DEFAULT%" call :resolve_qt_bindir "%QT_PREFIX_DEFAULT%"

set "CMAKE_EXTRA_ARGS=%CMAKE_ARGS%"
if not defined OPENZOOM_ENABLE_CUDA set "OPENZOOM_ENABLE_CUDA=ON"
if not defined OPENZOOM_ENABLE_TEXT_SR set "OPENZOOM_ENABLE_TEXT_SR=ON"
set "CMAKE_EXTRA_ARGS=%CMAKE_EXTRA_ARGS% -DOPENZOOM_ENABLE_CUDA=%OPENZOOM_ENABLE_CUDA%"
set "CMAKE_EXTRA_ARGS=%CMAKE_EXTRA_ARGS% -DOPENZOOM_ENABLE_TEXT_SR=%OPENZOOM_ENABLE_TEXT_SR%"

cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 %CMAKE_QT_ARGS% %CMAKE_EXTRA_ARGS%
if errorlevel 1 goto :fail

cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 goto :fail

set EXE_PATH=%BUILD_DIR%\cmake\Release\open_zoom.exe
if not exist "%EXE_PATH%" set EXE_PATH=%BUILD_DIR%\Release\open_zoom.exe
if not exist "%EXE_PATH%" set EXE_PATH=%BUILD_DIR%\open_zoom.exe

if /I "%OPENZOOM_SKIP_RUN%"=="1" goto after_run

if exist "%EXE_PATH%" (
    echo Launching %EXE_PATH%
    call :prepare_qt_runtime "%EXE_PATH%"
    "%EXE_PATH%"
) else (
    echo Executable not found. Build may have failed.
    goto :fail
)

:after_run

popd
popd
endlocal
exit /b 0

:resolve_qt_bindir
if defined QT_BIN_DIR goto :eof
set "QT_ROOT=%~1"
if not defined QT_ROOT goto :eof
for %%I in ("%QT_ROOT%") do set "QT_ROOT=%%~fI"
if exist "%QT_ROOT%\bin" (
    set "QT_BIN_DIR=%QT_ROOT%\bin"
    goto :eof
)
for %%I in ("%QT_ROOT%\..") do if not defined QT_BIN_DIR if exist "%%~fI\bin" set "QT_BIN_DIR=%%~fI\bin"
for %%I in ("%QT_ROOT%\..\..") do if not defined QT_BIN_DIR if exist "%%~fI\bin" set "QT_BIN_DIR=%%~fI\bin"
for %%I in ("%QT_ROOT%\..\..\..") do if not defined QT_BIN_DIR if exist "%%~fI\bin" set "QT_BIN_DIR=%%~fI\bin"
goto :eof

:prepare_qt_runtime
if not defined QT_BIN_DIR goto :prepare_qt_runtime_noqt
if not exist "%QT_BIN_DIR%" (
    echo Warning: Qt runtime directory "%QT_BIN_DIR%" not found.
    set "QT_BIN_DIR="
    goto :prepare_qt_runtime_noqt
)
set "PATH=%QT_BIN_DIR%;%PATH%"
set "WINDEPLOYQT=%QT_BIN_DIR%\windeployqt.exe"
set "EXE_DIR="
for %%I in ("%~1") do (
    for %%J in ("%%~dpI.") do set "EXE_DIR=%%~fJ"
)
if exist "%WINDEPLOYQT%" (
    echo Running Qt deployment tool: %WINDEPLOYQT%
    "%WINDEPLOYQT%" --release "%~1" --dir "%EXE_DIR%"
) else (
    echo windeployqt.exe not found at "%WINDEPLOYQT%"; skipping deployment copy.
)
goto :eof

:prepare_qt_runtime_noqt
echo Qt runtime directory not detected; skipping Qt PATH update and deployment.
goto :eof

:fail
popd
popd
endlocal
exit /b 1
