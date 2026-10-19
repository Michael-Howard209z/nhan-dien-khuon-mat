@echo off
chcp 65001 >nul
title ESP32-CAM - May chu diem danh
setlocal

rem =====================================================================
rem  start_server.bat - Khoi dong may chu diem danh khuon mat
rem
rem  LUC TRUOC: chay "python_srv.exe server.py" binh thuong thi stdout
rem  bi BUFFER, nen file server_out.log luon 0 byte va khong bao gio
rem  thay duoc dong:
rem       [BTN] Khung 1: KHOP -> Nguyen The Hoang
rem
rem  GIAI PHAP: chay bang "-u" (unbuffered) -> print() hien NGAY tren
rem  cua so nay. Moi dong cung duoc ghi vao server.log de xem lai.
rem
rem  Cach dung : double-click file nay
rem  De dung   : nhan Ctrl+C hoac dong cua so
rem  Neu port 5001 dang bi chiem -> file nay tu dong dung server cu
rem
rem  LUU Y: chuoi trong file nay la ASCII co y (khong dau) de cmd
rem  doc dung bat ki ma font/codepage nao.
rem =====================================================================

set "PROJ=%~dp0"
cd /d "%PROJ%"

set "EXE=%LOCALAPPDATA%\Programs\Python\Python313\python_srv.exe"
if not exist "%EXE%" set "EXE=%LOCALAPPDATA%\Programs\Python\Python313\python.exe"

if not exist "%EXE%" (
    echo [LOI] Khong tim thay Python tai:
    echo       %EXE%
    echo       Sua bien EXE trong file .bat nay cho phu hop.
    pause
    exit /b 1
)

if not exist "%PROJ%server.py" (
    echo [LOI] Khong tim thay server.py trong thu muc:
    echo       %PROJ%
    pause
    exit /b 1
)

echo ============================================================
echo    MAY CHU DIEM DANH KHUON MAT - ESP32-CAM
echo    Cong dang nhap  : http://localhost:5000
echo    Cong debug (CAM): http://localhost:5001
echo    IP server (tu tim qua UDP): xem dong "Tu tim nhau" duoi day
echo ============================================================
echo.

rem --- Dung server cu (neu co) de khong trung port 5001 ---------------
rem  Vi du nay nam tren MOT DONG vi cmd hong xu ly ky tu ^ dong da.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$p=Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'python_srv.exe' -and $_.CommandLine -like '*server.py*' }; if ($p) { $p | ForEach-Object { Write-Host ('  Dung server cu PID ' + $_.ProcessId) -ForegroundColor Yellow }; Start-Sleep -Seconds 2 } else { Write-Host '  Khong co server cu dang chay.' -ForegroundColor DarkGray }"

rem --- Ghi dau moc vao log cu de khong tron giua cac lan chay ----------
echo. >> "%PROJ%server.log"
echo ============================================================ >> "%PROJ%server.log"
echo  Bo khoi dong luc %date% %time% >> "%PROJ%server.log"
echo ============================================================ >> "%PROJ%server.log"

echo Dang khoi dong... ket qua hien NGAY duoi day.
echo (day lai Ctrl+C de dung server)
echo ------------------------------------------------------------------

rem --- Chay server -----------------------------------------------------
rem  -u     : khong buffer stdout -> hien ngay
rem  2>&1   : gop stderr (HTTP access log) vao cung luong
rem  pipe vao PowerShell de VUA hien man hinh VUA ghi server.log
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%EXE%' -u '%PROJ%server.py' 2>&1 | ForEach-Object { Write-Host $_; Add-Content -LiteralPath '%PROJ%server.log' -Value $_ }"

echo.
echo ------------------------------------------------------------------
echo Server da dung. Nhan phim bat ky de dong cua so...
pause >nul
