@echo off
setlocal
rem 2026-11-04: Run offline self-test and preserve exit code.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Self-Test.ps1"
set "awbResult=%errorlevel%"
pause
exit /b %awbResult%
