@echo off
rem 用途：在独立私有目录启动鉴权回环API，端口自动分配；日期：2026-11-04。
setlocal
if not exist "%~dp0data" mkdir "%~dp0data"
"%~dp0workbench-cli.exe" --root "%~dp0data\api" --http 0 %*
