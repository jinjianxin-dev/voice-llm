@echo off
chcp 65001 >nul
title LED life test - flash and watch the board LED
cd /d "%~dp0"
set "PY=E:\esp32-tools\idf-tools\python_env\idf5.5_py3.13_env\Scripts\python.exe"

echo Step 1/2: flashing LED test app (about 20 s)...
"%PY%" -m esptool --chip esp32s3 -p COM7 --baud 460800 --before default_reset --after hard_reset write_flash 0x10000 "E:\esp32-tools\projects\hello-test\build\hello_test.bin"
if errorlevel 1 goto failed

echo.
echo Step 2/2: DONE. Now watch the board:
echo.
echo   [LED]  the on-board RGB LED should blink GREEN / BLUE
echo          alternating every half second.
echo.
echo   [Serial] double-click 3-monitor.bat -- the window should
echo          print HELLO 0 / HELLO 1 / HELLO 2 ... every second.
echo.
echo Then tell the assistant what you saw (LED on/off, HELLO yes/no).
pause
exit /b 0

:failed
echo.
echo FLASH FAILED.
echo 1) Close the 3-monitor.bat window first (it holds COM7)
echo 2) Then run this bat again
pause
