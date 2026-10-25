"""Watch the board's boot log for N seconds with timestamps.

Diagnostic for the "load:0x... / entry 0x... then port lost" reboot loop:

  * we open the port first and only THEN touch RTS/DTR, so we can tell
    whether the reset is caused by our line settings or by the chip itself
  * every line is stamped, so we can measure how often it reboots
  * if the port drops, we reconnect -- but record whether it happened
    inside a connection (chip reset) or at open time (driver glitch)

    python watch_boot.py [COM7] [seconds]
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
BAUD = 115200

t0 = time.time()


def stamp():
    return "%6.2fs" % (time.time() - t0)


def say(msg):
    print("%s  %s" % (stamp(), msg), flush=True)


say("watch %s for %.0fs" % (PORT, SECONDS))

deadline = time.time() + SECONDS
boots = 0

while time.time() < deadline:
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.2)
    except Exception as exc:
        say("[open failed: %s]" % exc)
        time.sleep(1.0)
        continue

    say("[port open -- lines untouched]")
    time.sleep(0.7)                     # let the open settle, watch silence
    ser.dtr = False
    ser.rts = True
    say("[dtr=False rts=True]")

    buf = b""
    dropped = None
    try:
        while time.time() < deadline:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                if not text:
                    continue
                if "entry 0x" in text or "load:0x" in text:
                    boots += 1
                say("recv| " + text)
    except Exception as exc:
        dropped = exc
    finally:
        try:
            ser.close()
        except Exception:
            pass

    if dropped is not None:
        say("[port lost inside connection: %s]" % dropped)
    else:
        break

say("done -- rom-load lines seen: %d" % boots)
