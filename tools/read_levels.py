"""Read serial with configurable DTR/RTS levels and an auto-detected port.

Usage: read_levels.py [dtr] [rts] [seconds] [port]

The board enumerates with Espressif VID 0x303A; with the ROM CDC console it
shows up as PID 0x0009 and Windows may hand it a new COM number after every
reflash, so the port is looked up by VID unless one is given explicitly.
Keeps DTR/RTS steady for the whole session: a RTS falling edge makes the ROM
CDC console issue a reboot, which is the last thing we want mid-capture.
"""
import sys
import time

import serial
from serial.tools import list_ports


def pick_port():
    cands = [p for p in list_ports.comports() if p.vid == 0x303A]
    if not cands:
        return "COM7"
    cands.sort(key=lambda p: 0 if p.pid == 0x0009 else 1)
    return cands[0].device


DTR = len(sys.argv) > 1 and sys.argv[1] != "" and sys.argv[1].lower() == "1"
RTS = len(sys.argv) > 2 and sys.argv[2] != "" and sys.argv[2].lower() == "1"
SECS = float(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else 8.0
PORT = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else pick_port()

try:
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = DTR
    ser.rts = RTS
    ser.open()
except Exception as e:
    print("open failed on %s (dtr=%s rts=%s): %s" % (PORT, DTR, RTS, e))
    sys.exit(1)

buf = bytearray()
t0 = time.time()
while time.time() - t0 < SECS:
    try:
        d = ser.read(4096)
    except Exception as e:
        print("[error] %s" % e)
        break
    if d:
        buf.extend(d)
try:
    ser.close()
except Exception:
    pass

sys.stdout.write(buf.decode("utf-8", "replace"))
print("\n---- %s dtr=%s rts=%s -> %d bytes ----" % (PORT, DTR, RTS, len(buf)))
