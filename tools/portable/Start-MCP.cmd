@echo off
setlocal
rem 2026-11-04: Start MCP without extra stdout.
if not exist "%~dp0data" mkdir "%~dp0data"
"%~dp0workbench-mcp.exe" --root "%~dp0data\mcp" %*
exit /b %errorlevel%
