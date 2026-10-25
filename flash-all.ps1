# Flash everything to the board (normally called by flash-all.bat -- just
# double-click that; running this .ps1 directly needs a PowerShell that is
# allowed to execute scripts).
#
# What goes where:
#   idf.py flash  -> bootloader + partition table + app (0x10000)
#                    AND srmodels.bin -> 0x5f0000 automatically
#                    (esp-sr's build hooks it into flash_args)
#   model.bin     -> 0x410000, the 1219 KB pin-LLM weights ("llmmodel"
#                    partition). Only needed once; survives every reflash.

param(
    [string]$Port = "",           # empty = auto-detect the Espressif board
    [switch]$SkipModel
)

if (-not $Port) {
    # Find the ESP32-S3 by its Espressif VID (303A) instead of hard-coding
    # a COM number -- Windows reassigns the number after every reflash.
    $dev = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.DeviceID -match 'VID_303A' -and $_.Name -match 'COM\d+' } |
        Select-Object -First 1
    if ($dev -and $dev.Name -match '(COM\d+)') {
        $Port = $Matches[1]
    } else {
        Write-Host "ERROR: no Espressif board found (VID_303A). Plug it into the port labelled USB." -ForegroundColor Red
        exit 1
    }
    Write-Host "auto-detected board on $Port" -ForegroundColor Cyan
}

$env:IDF_TOOLS_PATH = "E:\esp32-tools\idf-tools"
$env:PYTHONPATH = ""            # see build.ps1 -- the host shim breaks deletes
$env:PATH = "C:\Users\aaron\.workbuddy\binaries\python\versions\3.13.12;" + $env:PATH

Write-Host "== importing ESP-IDF environment ==" -ForegroundColor Cyan
& "E:\esp32-tools\esp-idf\export.ps1" *> $null

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: idf.py not found after export -- ESP-IDF install is broken." -ForegroundColor Red
    exit 1
}

Set-Location "E:\esp32-tools\projects\voice-llm"

if (-not (Test-Path "build\voice_llm.bin")) {
    Write-Host "ERROR: build\voice_llm.bin missing -- run build.bat first." -ForegroundColor Red
    exit 1
}

Write-Host "== 1/2 flashing firmware + speech models to $Port ==" -ForegroundColor Cyan
idf.py -p $Port flash
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: flash failed. Is the board on $Port? Try holding BOOT while plugging USB." -ForegroundColor Red
    exit 1
}

if (-not $SkipModel) {
    $model = "E:\gpio_llm\esp32-gpio-llm\firmware\model\model.bin"
    Write-Host "== 2/2 flashing pin-LLM weights (1.2 MB) ==" -ForegroundColor Cyan
    python -m esptool --chip esp32s3 --port $Port --baud 460800 write_flash 0x410000 $model
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: model flash failed." -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "All flashed. Next: double-click monitor.bat to watch the log." -ForegroundColor Green
