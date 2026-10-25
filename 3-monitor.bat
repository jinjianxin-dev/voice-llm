@echo off
chcp 65001 >nul
title Voice-LLM monitor - Ctrl+C to quit
cd /d "%~dp0"
set "PY=C:\Users\aaron\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
if exist "%PY%" goto run
echo Python with pyserial not found at:
echo   %PY%
echo Falling back to the py launcher...
py -3 "tools\py_monitor.py"
goto done
:run
"%PY%" "tools\py_monitor.py"
:done
echo.
echo Monitor closed.
pause
