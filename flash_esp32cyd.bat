@echo off
chcp 65001 >nul
rem =====================================================================
rem  flash_esp32cyd.bat [COMx]
rem  Nap firmware MAN HINH CAN MAT (ESP32-2432S028R - CYD 2.8")
rem =====================================================================
set "PROJ=%~dp0"
set "CLI=%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
set "SKETCH=%PROJ%firmware_esp32display\esp32cyd"
set "FQBN=esp32:esp32:esp32:CPUFreq=240,FlashFreq=80,FlashMode=dio,FlashSize=4M,PartitionScheme=huge_app,DebugLevel=none,PSRAM=disabled"
set "OUTDIR=%PROJ%firmware_esp32display\build_cyd"
set "PORT=COM5"

if "%~1" neq "" set "PORT=%~1"

if not exist "%OUTDIR%\esp32cyd.ino.bin" (
  echo [LOI] Chua co file build trong: %OUTDIR%
  echo       Chay lenh compile Arduino CLI truoc ^(xem README^).
  pause
  exit /b 1
)

echo Duong dan build : %OUTDIR%
echo Cong COM        : %PORT%
echo Board           : ESP32-2432S028R (CYD)
echo.
echo Dang nap firmware vao CYD...
"%CLI%" upload --fqbn "%FQBN%" -p %PORT% --input-dir "%OUTDIR%" "%SKETCH%"
if errorlevel 1 (
  echo.
  echo [LOI] Nap that bai. Kiem tra:
  echo   - Da cam cap USB dung chua? Dung cong COM nao? (xem Device Manager)
  echo   - CYD dung chip USB-serial CH340: can driver CH340.
  echo   - Thu giu nut BOOT tren board trong luc nap.
  pause
  exit /b 1
)
echo.
echo Nap xong! Board se tu reboot.
echo Mo Serial Monitor 115200 (go /help) de xem IP, /find de tim camera.
pause
