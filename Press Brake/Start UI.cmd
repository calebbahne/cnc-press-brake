@echo off
cd /d "%~dp0"
where node >nul 2>nul
if errorlevel 1 (
  echo Node.js is required. Install Node.js and try again.
  pause
  exit /b 1
)
set "ESP_HOST=cnc-press-brake.local"
set /p "ESP_HOST=ESP IP or hostname [cnc-press-brake.local]: "
node server.cjs "%ESP_HOST%"
pause
