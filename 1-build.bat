@echo off
chcp 65001 >nul
title Voice-LLM 1/3 : BUILD
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
if errorlevel 1 goto failed
echo.
echo ==========================================
echo  BUILD OK
echo  Next: double-click 2-flash-all.bat
echo ==========================================
goto end
:failed
echo.
echo ==========================================
echo  BUILD FAILED - read the error above
echo ==========================================
:end
echo.
pause
