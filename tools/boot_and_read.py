"""Reset the board and read its boot log, surviving the USB re-enumeration.

The ESP32-S3 USB-Serial/JTAG *is* the auto-reset circuit (see esptool/reset.py):

    DTR=True  -> GPIO0 LOW  (download-mode strap)
    RTS=True  -> EN LOW     (chip held in reset)

Two consequences that bit us:

  * pyserial asserts DTR and RTS when it opens a port, so a naive
    ``serial.Serial(port)`` puts the chip in download mode and holds it in
    reset. Setting dtr/rts BEFORE open() avoids touching the lines at all.
  * pulling RTS low resets the chip, which also resets the USB peripheral,
    so the handle dies. The reset therefore has to be followed by a reopen.

Usage:  python boot_and_read.py [COM7] [seconds]
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


def open_idle(port, tries=40):
    """Open without disturbing the auto-reset circuit."""
    last = None
    for _ in range(tries):
        try:
            ser = serial.Serial()
            ser.port = pick_port(port)
            ser.baudrate = 115200
            ser.timeout = 0.2
            ser.write_timeout = 1.0
            ser.dtr = False        # GPIO0 high: normal boot
            ser.rts = False        # EN high: chip runs
            ser.open()
            return ser
        except Exception as exc:
            last = exc
            time.sleep(0.5)
    raise last


PORT = sys.argv[1] if len(sys.argv) > 1 else None
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

# 1) reset: pull EN low briefly, then let go
ser = open_idle(PORT)
print("[port open, lines idle]", flush=True)
try:
    ser.rts = True
    time.sleep(0.25)
    ser.rts = False
except Exception as exc:
    print("[port died during reset: %r -- expected]" % exc, flush=True)
try:
    ser.close()
except Exception:
    pass

# 2) the chip needs a moment to re-enumerate, then boots the app
time.sleep(1.2)
ser = open_idle(PORT)
print("[reopened after reset -- reading]", flush=True)

t0 = time.time()
buf = b""
total = 0
while time.time() - t0 < SECONDS:
    try:
        chunk = ser.read(4096)
    except Exception as exc:
        print("%6.2fs  [port lost: %r]" % (time.time() - t0, exc), flush=True)
        break
    if not chunk:
        continue
    total += len(chunk)
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        text = line.decode("utf-8", "replace").rstrip("\r")
        if text:
            print("%6.2fs  %s" % (time.time() - t0, text), flush=True)

print("[%d bytes total]" % total, flush=True)
try:
    ser.close()
except Exception:
    pass
