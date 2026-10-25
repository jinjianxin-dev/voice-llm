# Open an interactive PowerShell that already has ESP-IDF on PATH.
# Normally called by idf-shell.bat. Inside that window you can type any
# idf.py command, e.g.:  idf.py -p COM7 monitor

$env:IDF_TOOLS_PATH = "E:\esp32-tools\idf-tools"
$env:PYTHONPATH = ""
$env:PATH = "C:\Users\aaron\.workbuddy\binaries\python\versions\3.13.12;" + $env:PATH

& "E:\esp32-tools\esp-idf\export.ps1" *> $null

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: idf.py not found after export." -ForegroundColor Red
    return
}

Set-Location "E:\esp32-tools\projects\voice-llm"

Write-Host ""
Write-Host "ESP-IDF ready, current folder: $(Get-Location)" -ForegroundColor Green
Write-Host "Useful commands:" -ForegroundColor Cyan
Write-Host "  idf.py build                 # compile"
Write-Host "  idf.py -p COM7 flash         # flash firmware only"
Write-Host "  idf.py -p COM7 monitor       # serial log, quit with Ctrl+]"
Write-Host "  idf.py -p COM7 flash monitor # flash then watch"
Write-Host ""
