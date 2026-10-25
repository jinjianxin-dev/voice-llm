"""Wait for the board to come back, flash the app, then print the boot log.

The USB CDC function on this board wedges when the app dies mid-transfer; a
replug is the only way out. This script sits and waits for that replug, so the
operator only has to pull the cable once.

    python flashwatch.py [seconds_to_wait]
"""
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

BIN = r"E:\esp32-tools\projects\voice-llm\build\voice_llm.bin"
PY = r"E:\esp32-tools\idf-tools\python_env\idf5.5_py3.13_env\Scripts\python.exe"
WAIT = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0


def espport():
    ps = [p for p in list_ports.comports() if p.vid == 0x303A]
    ps.sort(key=lambda p: 0 if p.pid == 0x0009 else 1)
    return ps[0].device if ps else None


def usable(port):
    """True only if the port can be opened -- a wedged CDC port enumerates
    but rejects every line-coding request, so opening is the cheapest probe."""
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.1
        s.write_timeout = 1.0
        s.dtr = False
        s.rts = False
        s.open()
        s.close()
        return True
    except Exception:
        return False


t_end = time.time() + WAIT
flashed = False
last_state = None
while time.time() < t_end:
    p = espport()
    state = "no-device" if not p else ("usable" if usable(p) else "wedged")
    if state != last_state:
        print("[%s %s]" % (state, p or "-"), flush=True)
        last_state = state
    if state != "usable":
        time.sleep(1.5)
        continue
    r = subprocess.run(
        [PY, "-m", "esptool", "--chip", "esp32s3", "-p", p, "-b", "460800",
         "write_flash", "0x10000", BIN],
        capture_output=True, text=True)
    out = r.stdout + r.stderr
    tail = out.strip().splitlines()[-1][:140] if out.strip() else "(no output)"
    print("[flash %s] rc=%d %s" % (p, r.returncode, tail), flush=True)
    if "Hash of data verified" in out:
        flashed = True
        break
    time.sleep(2.0)

if not flashed:
    print("[GAVE UP -- port never became usable]", flush=True)
    sys.exit(1)

print("[flashed, reading boot log]", flush=True)
time.sleep(1.5)

t0 = time.time()
total = 0
buf = b""
while time.time() - t0 < 40:
    p = espport()
    if not p:
        time.sleep(0.3)
        continue
    try:
        s = serial.Serial()
        s.port = p
        s.baudrate = 115200
        s.timeout = 0.2
        s.write_timeout = 1.0
        s.dtr = False
        s.rts = False
        s.open()
    except Exception:
        time.sleep(0.3)
        continue
    print("%6.2fs [open %s]" % (time.time() - t0, p), flush=True)
    try:
        while time.time() - t0 < 40:
            chunk = s.read(2048)
            if not chunk:
                continue
            total += len(chunk)
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                if text:
                    print("%6.2fs  %s" % (time.time() - t0, text), flush=True)
    except Exception as exc:
        print("%6.2fs [lost %s]" % (time.time() - t0, type(exc).__name__), flush=True)
    finally:
        try:
            s.close()
        except Exception:
            pass
print("[done: %d bytes]" % total, flush=True)
