@echo off
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0lecture_camera.ps1" -Action Stop
if errorlevel 1 pause
