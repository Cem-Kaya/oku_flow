@echo off
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0lecture_camera.ps1" -Action Start -LaunchOkuFlow %*
if errorlevel 1 pause
