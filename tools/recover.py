"""One-shot recovery: wait for the board to be replugged, capture whatever the
current firmware prints, flash the new app, then capture its boot log.

    python recover.py [wait_seconds]

Phase 1 (pre)  -- 8 s of whatever is running right now. If the old test
                  firmware is alive we will see "HELLO n" lines, which tells us
                  the port is fine and the chip was never dead, only silent.
Phase 2 (flash) -- esptool with the default reset sequence.
Phase 3 (post) -- 40 s of boot log, port reopened as soon as it reappears.
"""
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

PY = r"E:\esp32-tools\idf-tools\python_env\idf5.5_py3.13_env\Scripts\python.exe"
BIN = r"E:\esp32-tools\projects\voice-llm\build\voice_llm.bin"
WAIT = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0


def espport():
    ps = [p for p in list_ports.comports() if p.vid == 0x303A]
    ps.sort(key=lambda p: 0 if p.pid == 0x0009 else 1)
    return ps[0].device if ps else None


def try_open(port):
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.05
        s.write_timeout = 1.0
        s.dtr = False
        s.rts = False
        s.open()
        return s
    except Exception:
        return None


def drain(sec, tag):
    """Read the console for `sec` seconds, reconnecting if the port bounces."""
    t0 = time.time()
    total = 0
    buf = b""
    s = None
    while time.time() - t0 < sec:
        if s is None:
            p = espport()
            if not p:
                time.sleep(0.2)
                continue
            s = try_open(p)
            if s is None:
                time.sleep(0.2)
                continue
            print("%6.2fs [%s open %s]" % (time.time() - t0, tag, p), flush=True)
        try:
            chunk = s.read(4096)
        except Exception as exc:
            print("%6.2fs [%s lost %s]" % (time.time() - t0, tag, type(exc).__name__), flush=True)
            try:
                s.close()
            except Exception:
                pass
            s = None
            continue
        if not chunk:
            continue
        total += len(chunk)
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip("\r")
            if text:
                print("%6.2fs  %s" % (time.time() - t0, text), flush=True)
    if s is not None:
        try:
            s.close()
        except Exception:
            pass
    print("[%s: %d bytes]" % (tag, total), flush=True)
    return total


# ---- phase 0: wait for a port that actually accepts a line-coding request ----
t_end = time.time() + WAIT
port = None
while time.time() < t_end:
    p = espport()
    if p:
        s = try_open(p)
        if s is not None:
            s.close()
            port = p
            print("[port %s usable]" % p, flush=True)
            break
    time.sleep(1.0)

if port is None:
    print("[GAVE UP: no usable port during %.0fs]" % WAIT, flush=True)
    sys.exit(1)

# ---- phase 1: what is running now? ----
drain(8.0, "pre")

# ---- phase 2: flash ----
ok = False
for attempt in range(1, 4):
    p = espport() or port
    r = subprocess.run(
        [PY, "-m", "esptool", "--chip", "esp32s3", "-p", p, "-b", "460800",
         "write_flash", "0x10000", BIN],
        capture_output=True, text=True)
    out = r.stdout + r.stderr
    lines = [l for l in out.strip().splitlines() if l.strip()]
    print("[flash %d] rc=%d %s" % (attempt, r.returncode, lines[-1][:140] if lines else "-"), flush=True)
    if "Hash of data verified" in out:
        ok = True
        break
    time.sleep(3.0)

if not ok:
    print("[flash failed]", flush=True)
    sys.exit(2)

# ---- phase 3: boot log ----
time.sleep(0.5)
drain(40.0, "post")
