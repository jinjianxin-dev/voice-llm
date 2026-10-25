@echo off
chcp 65001 >nul
title ESP-IDF shell - type idf.py commands here
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -NoExit -File "%~dp0start-shell.ps1"
