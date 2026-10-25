# Open the serial log of the board (normally called by monitor.bat).
# Exit the monitor with  Ctrl + ]

param(
    [string]$Port = "COM7"
)

$env:IDF_TOOLS_PATH = "E:\esp32-tools\idf-tools"
$env:PYTHONPATH = ""
$env:PATH = "C:\Users\aaron\.workbuddy\binaries\python\versions\3.13.12;" + $env:PATH

Write-Host "== importing ESP-IDF environment ==" -ForegroundColor Cyan
& "E:\esp32-tools\esp-idf\export.ps1" *> $null

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: idf.py not found after export." -ForegroundColor Red
    exit 1
}

Set-Location "E:\esp32-tools\projects\voice-llm"

Write-Host ""
Write-Host "  Say:  ni hao xiao zhi   (Hello Xiaozhi)  -> wake word" -ForegroundColor Yellow
Write-Host "  Then: da kai si hao yin jiao  -> 'open pin 4'" -ForegroundColor Yellow
Write-Host "  Quit this window with  Ctrl + ]" -ForegroundColor Yellow
Write-Host ""

idf.py -p $Port monitor
