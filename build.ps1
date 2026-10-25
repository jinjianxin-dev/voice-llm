# Build voice_llm for the ESP-IDF on the ESP32-S3 (run from PowerShell).
# ESP-IDF v5.5.1 at E:\esp32-tools\esp-idf, tools at E:\esp32-tools\idf-tools.
#
# Two environment gotchas on this machine, both handled here:
# 1. The managed Python 3.13 must come first on PATH -- export.ps1 picks the
#    venv by whatever `python` it sees (idf5.5_py3.13_env), and the system
#    python is 3.14, which would point at a venv that does not exist.
# 2. PYTHONPATH carries a host-app Python shim that hooks os.remove() into
#    "move to trash", which fails on E: (SHFileOperationW 0x2) and aborts the
#    component manager mid-configure. The build needs real deletes, so the
#    variable is cleared for this process only.

$env:IDF_TOOLS_PATH = "E:\esp32-tools\idf-tools"
$env:PYTHONPATH = ""
# ccache writes its temp files under %LOCALAPPDATA%\ccache on the (tiny) C:
# drive; a stuck preprocessor there wedged a build for 30+ minutes. Disable.
$env:IDF_CCACHE_ENABLE = "0"
$env:PATH = "C:\Users\aaron\.workbuddy\binaries\python\versions\3.13.12;" + $env:PATH
& "E:\esp32-tools\esp-idf\export.ps1" *> $null
Set-Location "E:\esp32-tools\projects\voice-llm"
idf.py build
