@echo off
setlocal
cd /d "%~dp0"
if "%~1"=="" (
  echo Usage: Start-Automation.cmd "D:\AI-Browser-Workbench-0.1.8-win64-ui-r1"
  echo Start the website first with Start-Site.cmd.
  pause
  exit /b 2
)
if not exist "%~1\workbench-cli.exe" (
  echo Cannot find workbench-cli.exe in the supplied portable program directory.
  pause
  exit /b 2
)
if not exist "%~1\browser\chrome-win64\chrome.exe" (
  echo Cannot find the bundled browser. Supply the complete portable program directory.
  pause
  exit /b 2
)
set "CONTENT_LAB_RUN=%~dp0runs\run-%RANDOM%-%RANDOM%"
python -X utf8 runner.py --executable "%~1\workbench-cli.exe" --browser "%~1\browser\chrome-win64\chrome.exe" --output "%CONTENT_LAB_RUN%" --headed
set "CONTENT_LAB_RESULT=%ERRORLEVEL%"
echo Report directory: "%CONTENT_LAB_RUN%"
pause
exit /b %CONTENT_LAB_RESULT%
