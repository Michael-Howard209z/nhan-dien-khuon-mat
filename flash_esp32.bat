@echo off
chcp 65001 >nul
rem =====================================================================
rem  flash_esp32.bat [COMx] [noDISC]
rem
rem   flash_esp32.bat           -> nap ban CO discovery (build_esp32)
rem   flash_esp32.bat COM7      -> chon cong COM
rem   flash_esp32.bat COM7 noDISC -> nap ban TAT discovery (de chan doan)
rem
rem  Truoc khi chay: cam ESP32-CAM vao may qua cap USB.
rem  Khong biet COM nao?  Mo Device Manager -> Ports (COM & LPT).
rem
rem  FQBN PHAI DAY DU nhu luc build (sai PartitionScheme -> boot loi).
rem =====================================================================
set "PROJ=%~dp0"
set "CLI=%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
set "SKETCH=%PROJ%firmware_esp32cam"
set "FQBN=esp32:esp32:esp32cam:CPUFreq=240,FlashFreq=80,FlashMode=dio,PartitionScheme=huge_app,DebugLevel=none"
set "OUTDIR=%PROJ%build_esp32"
set "PORT=COM7"

if "%~1" neq "" set "PORT=%~1"
if /i "%~2"=="noDISC" set "OUTDIR=%PROJ%build_esp32_noDISC"
if /i "%~1"=="noDISC" set "OUTDIR=%PROJ%build_esp32_noDISC"

rem Ten file build theo ten thu muc sketch -> doi ten thu muc la doi ten file.
if not exist "%OUTDIR%\firmware_esp32cam.ino.bin" if not exist "%OUTDIR%\CameraWebServer_copy_20261009125906.ino.bin" (
  echo [LOI] Chua co file build trong:
  echo       %OUTDIR%
  echo       Chay Arduino CLI compile truoc, hoac dung "flash_esp32.bat" thuong.
  pause
  exit /b 1
)

echo Duong dan build : %OUTDIR%
echo Cong COM        : %PORT%
echo.
echo Dang nap firmware vao ESP32-CAM...
"%CLI%" upload --fqbn "%FQBN%" -p %PORT% --input-dir "%OUTDIR%" "%SKETCH%"
if errorlevel 1 (
  echo.
  echo [LOI] Nap that bai. Kiem tra: board da cam chua? Dung COM nao?
  pause
  exit /b 1
)
echo.
echo Nap xong! Board se tu reboot.
echo Mo Serial Monitor 115200 roi go  /cam  de kiem tra camera (khong can bam nut).
pause
