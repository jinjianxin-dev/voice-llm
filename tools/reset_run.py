"""Reset the board using esptool's own line semantics, then print the boot log.

From esptool/reset.py (this is the only reliable source of truth for the
ESP32-S3 USB-Serial/JTAG auto-reset circuit):

    DTR = False  -> GPIO0 HIGH (normal boot)
    RTS = True   -> EN LOW  (chip held in reset)

So a "hold + release" reset is:  RTS=True, then DTR=False, then RTS=False.
Any tool that leaves RTS=True is holding the chip in reset -- which is what
made the old monitor look like a reboot loop.

    python reset_run.py [COM7] [seconds]
"""
import sys
import time

import serial
from serial.tools import list_ports


def pick_port(explicit=None):
    if explicit:
        return explicit
    cands = [p for p in list_ports.comports() if p.vid == 0x303A]
    if not cands:
        return "COM7"
    cands.sort(key=lambda p: 0 if p.pid == 0x0009 else 1)
    return cands[0].device


PORT = pick_port(sys.argv[1] if len(sys.argv) > 1 else None)
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

ser = serial.Serial(PORT, 115200, timeout=0.2)
print("port open %s" % PORT, flush=True)

# EN low (hold in reset), GPIO0 high (normal boot), then release EN.
ser.setRTS(True)
time.sleep(0.2)
ser.setDTR(False)
ser.setRTS(False)          # out of reset -> boots
time.sleep(0.05)
ser.setDTR(False)          # keep GPIO0 high
print("[reset: RTS=True -> DTR=False -> RTS=False]", flush=True)
print("[lines now: DTR=False (GPIO0 high)  RTS=False (EN high)]", flush=True)

t0 = time.time()
buf = b""
while time.time() - t0 < SECONDS:
    try:
        chunk = ser.read(4096)
    except Exception as exc:
        print("%6.2fs  [port lost: %s]" % (time.time() - t0, exc), flush=True)
        break
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        text = line.decode("utf-8", "replace").rstrip("\r")
        if text:
            print("%6.2fs  %s" % (time.time() - t0, text), flush=True)

try:
    ser.close()
except Exception:
    pass
print("done", flush=True)
