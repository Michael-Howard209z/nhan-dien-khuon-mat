@echo off
chcp 65001 >nul
rem =====================================================================
rem  flash_esp32c3.bat [COMx]
rem  Nap firmware man LCD (ESP32-C3 Super Mini + LCD1602 PCF8574)
rem =====================================================================
set "PROJ=%~dp0"
set "CLI=%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
set "SKETCH=%PROJ%firmware_esp32display\esp32lcd"
set "FQBN=esp32:esp32:esp32c3:CDCOnBoot=cdc"
set "OUTDIR=%PROJ%firmware_esp32display\build"
set "PORT=COM7"

if "%~1" neq "" set "PORT=%~1"

if not exist "%OUTDIR%\esp32lcd.ino.bin" (
  echo [LOI] Chua co file build trong: %OUTDIR%
  echo       Chay Arduino CLI compile truoc.
  pause
  exit /b 1
)

echo Duong dan build : %OUTDIR%
echo Cong COM        : %PORT%
echo Board           : ESP32-C3 Super Mini (FQBN=%FQBN%)
echo.
echo Dang nap firmware vao ESP32-C3...
"%CLI%" upload --fqbn "%FQBN%" -p %PORT% --input-dir "%OUTDIR%" "%SKETCH%"
if errorlevel 1 (
  echo.
  echo [LOI] Nap that fai. Kiem tra: board da cam chua? Dung COM nao? ESP32-C3 can nut BOOT nhan luc reset de vao chedo nap.
  pause
  exit /b 1
)
echo.
echo Nap xong! Board se tu reboot.
echo Mo Serial Monitor 115200 roi go /help de xem lenh.
pause
