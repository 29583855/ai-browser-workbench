@echo off
setlocal
rem 2026-11-04: Build native VS2019 projects.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-vs2019.ps1" %*
set "awbResult=%errorlevel%"
if not "%awbResult%"=="0" pause
exit /b %awbResult%
