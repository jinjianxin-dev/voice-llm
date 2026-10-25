"""Long-running capture that survives resets, writing a timestamped log.

Does NOT touch DTR/RTS: whatever the board does after a physical RST press
is what we want to see, unmodified.

    python capture.py <logfile> [seconds]
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


LOG = sys.argv[1] if len(sys.argv) > 1 else "capture.log"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 240.0

f = open(LOG, "w", encoding="utf-8", buffering=1)
t0 = time.time()


def say(msg, tag="meta"):
    f.write("%6.2fs  [%s] %s\n" % (time.time() - t0, tag, msg))


say("start, duration %.0fs" % SECONDS)

while time.time() - t0 < SECONDS:
    port = pick_port()
    try:
        ser = serial.Serial(port, 115200, timeout=0.2)
    except Exception as exc:
        time.sleep(0.5)
        continue

    say("open %s" % port)
    buf = b""
    try:
        while time.time() - t0 < SECONDS:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                if text:
                    f.write("%6.2fs  %s\n" % (time.time() - t0, text))
    except Exception as exc:
        say("lost: %s" % exc)
    finally:
        try:
            ser.close()
        except Exception:
            pass
    time.sleep(0.3)

say("end")
f.close()
