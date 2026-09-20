@echo off
cd /d "%~dp0"
where node >nul 2>nul
if errorlevel 1 (
  echo Node.js is required. Install Node.js and try again.
  pause
  exit /b 1
)
if not exist node_modules\serialport (
  echo Installing the local USB serial relay dependencies...
  call npm install
  if errorlevel 1 (pause & exit /b 1)
)
set "ESP_PORT="
set /p "ESP_PORT=ESP COM port shown in Device Manager, for example COM5: "
node usb-server.cjs "%ESP_PORT%"
pause
