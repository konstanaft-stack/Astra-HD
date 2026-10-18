@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-VR-CENTR.ps1"
if errorlevel 1 pause
