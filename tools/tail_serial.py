"""Tolerant serial reader: survives the board re-enumerating its native USB
port, so a crash/reboot loop still gets captured."""
import serial, time, sys

PORT = "COM7"
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
deadline = time.time() + SECS
total = 0
reconnects = 0

while time.time() < deadline:
    try:
        ser = serial.Serial()
        ser.port = PORT
        ser.baudrate = 115200
        ser.timeout = 0.2
        ser.dtr = False
        ser.rts = False
        ser.open()
    except Exception:
        time.sleep(0.3)
        continue
    reconnects += 1
    try:
        while time.time() < deadline:
            d = ser.read(4096)
            if d:
                total += len(d)
                sys.stdout.buffer.write(d)
                sys.stdout.flush()
    except Exception:
        pass
    try:
        ser.close()
    except Exception:
        pass
    time.sleep(0.2)

print("\n---- %d bytes, %d port open(s) ----" % (total, reconnects))
