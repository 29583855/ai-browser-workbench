@echo off
setlocal
rem 2026-11-04: Start CLI with an independent data root.
if not exist "%~dp0data" mkdir "%~dp0data"
"%~dp0workbench-cli.exe" --root "%~dp0data\cli" %*
exit /b %errorlevel%
