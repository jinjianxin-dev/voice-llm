@echo off
chcp 65001 >nul
title Voice-LLM 2/3 : FLASH
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash-all.ps1"
if errorlevel 1 goto failed
echo.
echo ==========================================
echo  FLASH OK - firmware + speech + LLM model
echo  Next: double-click 3-monitor.bat
echo ==========================================
goto end
:failed
echo.
echo ==========================================
echo  FLASH FAILED
echo  1) Board must be plugged into the port
echo     labelled USB (not UART)
echo  2) If it hangs: hold BOOT, tap RST, retry
echo ==========================================
:end
echo.
pause
