"""Boot the app from download mode and read its console.

The chip is left in ROM download mode after flashing (write_flash --after
no_reset). To start the app we pulse EN (RTS) while keeping GPIO0 (DTR) high,
then stream the console. The reader is attached *before* the reset, so the
64-byte TX FIFO is drained from the first byte -- that is what wedged the ROM
CDC console before.

If the RTS pulse is rejected by the driver, we fall back to asking the user to
tap RST manually (the reader keeps polling for the port).

Usage:  python bootread.py [port] [seconds]
"""
import sys
import time

import serial
from serial.tools import list_ports

PORT = sys.argv[1] if len(sys.argv) > 1 else None
READ_SEC = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0


def espport():
    ps = [p for p in list_ports.comports() if p.vid == 0x303A]
    ps.sort(key=lambda p: 0 if p.pid == 0x0009 else 1)
    return ps[0].device if ps else None


def open_port(name):
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.timeout = 0.05
    s.write_timeout = 2.0
    s.dtr = False          # GPIO0 high -> normal boot (never touch after this)
    s.rts = False
    s.open()
    # usbser.sys may assert DTR at open time regardless of what we asked for
    # (that leaves GPIO0 low and the chip boots into download mode). Send the
    # deassert explicitly now that the port is open.
    s.setDTR(False)
    s.setRTS(False)
    return s


# If no port given (or the given one is busy), look for any ESP32 port.
s = None
for attempt in range(30):
    name = PORT or espport()
    if name:
        try:
            s = open_port(name)
            print("[opened %s]" % name, flush=True)
            break
        except Exception as exc:
            if PORT is None:
                time.sleep(1.0)
                continue
            print("[open %s failed: %s]" % (name, exc), flush=True)
            sys.exit(1)
    time.sleep(1.0)
if s is None:
    print("[no ESP32 port found]", flush=True)
    sys.exit(1)

# Pulse EN to leave download mode and boot the app.
# On usbser.sys a bare RTS change is sometimes not transmitted; esptool works
# around it by touching DTR right after each RTS change (see esptool/reset.py
# ResetStrategy._setRTS). Replicate that here.
def set_rts(state):
    s.setRTS(state)
    s.setDTR(False)


try:
    set_rts(True)          # EN low, chip in reset
    time.sleep(0.2)
    set_rts(False)         # EN high, chip boots (GPIO0 still high via DTR=False)
    time.sleep(0.1)
    print("[EN pulsed -- app booting]", flush=True)
except Exception as exc:
    print("[RTS pulse rejected (%s) -- tap the RST button now]" % exc, flush=True)

t0 = time.time()
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
        if text:
            print("%6.2fs  %s" % (time.time() - t0, text), flush=True)
            nlines += 1
            if nlines >= 80:
                time.sleep(0.3)

print("[done: %d bytes, %d lines]" % (total, nlines), flush=True)
try:
    s.close()
except Exception:
    pass
