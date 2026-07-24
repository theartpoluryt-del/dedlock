@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_release_x64.ps1"
pause
