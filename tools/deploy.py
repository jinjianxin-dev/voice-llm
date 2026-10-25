"""Flash the app while the board sits in ROM download mode, then reset it and
keep the console drained from the very first byte.

Why not just `esptool write_flash`?  Because the USB CDC console on this board
blocks the firmware as soon as its 64-byte TX FIFO fills up, and the host does
not drain the FIFO until a reader opens the port.  If the reader opens *after*
the app has already started printing, the chip is wedged in that write and no
longer answers USB control requests -- which Windows reports as
"连到系统上的设备没有发挥作用 (error 31)".

So the order matters:
    1. flash with --after no_reset   (chip stays in download mode, responsive)
    2. open the port and start reading *before* releasing reset
    3. pulse EN low->high to boot the app while the FIFO is already draining

Usage:  python deploy.py <image.bin> [offset] [seconds_to_read]
"""
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

PY = r"E:\esp32-tools\idf-tools\python_env\idf5.5_py3.13_env\Scripts\python.exe"
BIN = sys.argv[1] if len(sys.argv) > 1 else r"E:\esp32-tools\projects\voice-llm\build\voice_llm.bin"
OFFSET = sys.argv[2] if len(sys.argv) > 2 else "0x10000"
READ_SEC = float(sys.argv[3]) if len(sys.argv) > 3 else 45.0


def espport():
    ps = [p for p in list_ports.comports() if p.vid == 0x303A]
    ps.sort(key=lambda p: 0 if p.pid == 0x0009 else 1)
    return ps[0].device if ps else None


port = espport()
if not port:
    print("[no ESP32 serial port found]", flush=True)
    sys.exit(1)
print("[port %s]" % port, flush=True)

ok = False
for attempt in range(1, 4):
    r = subprocess.run(
        [PY, "-m", "esptool", "--chip", "esp32s3", "-p", port, "-b", "460800",
         "--before", "no_reset", "--after", "no_reset",
         "write_flash", OFFSET, BIN],
        capture_output=True, text=True)
    out = (r.stdout + r.stderr)
    lines = [l for l in out.strip().splitlines() if l.strip()]
    print("[flash try %d] rc=%d %s" % (attempt, r.returncode, lines[-1][:140] if lines else "-"), flush=True)
    if "Hash of data verified" in out:
        ok = True
        break
    time.sleep(2.0)

if not ok:
    print("[flash failed -- board probably not in download mode]", flush=True)
    sys.exit(2)

# Open and start draining BEFORE the app gets a chance to print.
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.05
s.write_timeout = 1.0
s.dtr = False          # GPIO0 high -> normal boot
s.rts = True           # EN low -> hold in reset
s.open()
print("[released reset, reading]", flush=True)

t0 = time.time()
s.rts = False          # release EN -> chip boots into the new app
buf = b""
total = 0
nlines = 0
while time.time() - t0 < READ_SEC:
    try:
        chunk = s.read(4096)
    except Exception as exc:
        print("%6.2fs [lost %s]" % (time.time() - t0, type(exc).__name__), flush=True)
        break
    if not chunk:
        continue
    total += len(chunk)
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        text = line.decode("utf-8", "replace").rstrip("\r")
        print("%6.2fs  %s" % (time.time() - t0, text), flush=True)
        nlines += 1
        if nlines >= 60:
            # Keep draining but stop spamming once we have the startup sequence.
            time.sleep(0.2)

print("[done: %d bytes, %d lines]" % (total, nlines), flush=True)
try:
    s.close()
except Exception:
    pass
