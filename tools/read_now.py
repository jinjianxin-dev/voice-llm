"""Open whichever Espressif port is free and print what the board says.

Port identity drifts on this board: the native USB re-enumerates between
VID 303A PID 1001 (USB-Serial/JTAG) and PID 0009 (CDC interface), and Windows
hands out a new COM number each time. Never hard-code the port.

    python read_now.py [seconds]
"""
import sys
import time

import serial
from serial.tools import list_ports


SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0


def candidates():
    ports = [p for p in list_ports.comports() if p.vid == 0x303A]
    ports.sort(key=lambda p: 0 if p.pid == 0x0009 else 1)
    return ports


def open_idle():
    """Open without touching DTR/RTS (pyserial asserts both by default, which
    on this chip means GPIO0 low + EN low = download mode, held in reset)."""
    for p in candidates():
        try:
            ser = serial.Serial()
            ser.port = p.device
            ser.baudrate = 115200
            ser.timeout = 0.2
            ser.write_timeout = 1.0
            ser.dtr = False
            ser.rts = False
            ser.open()
            return ser, p
        except Exception:
            continue
    return None, None


ser, p = open_idle()
if ser is None:
    print("[no free Espressif port] ports=%r" % [(x.device, hex(x.pid or 0)) for x in candidates()])
    sys.exit(1)

print("[open %s pid=0x%04x %s]" % (p.device, p.pid or 0, p.description), flush=True)
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

print("[%d bytes]" % total, flush=True)
try:
    ser.close()
except Exception:
    pass
