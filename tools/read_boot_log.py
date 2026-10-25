"""Read the board's startup log for a few seconds (COM7, 115200)."""
import serial, time, sys

PORT = "COM7"
SECS = 14

ser = serial.Serial(PORT, 115200, timeout=0.2)
# Pulse EN low the way esptool does, to watch the boot from the first line.
try:
    ser.dtr = False
    ser.rts = True
    time.sleep(0.12)
    ser.dtr = True
    ser.rts = False
    time.sleep(0.06)
    ser.dtr = False
except Exception as e:
    print("reset via DTR/RTS failed:", e)

ser.reset_input_buffer()
t0 = time.time()
out = bytearray()
while time.time() - t0 < SECS:
    d = ser.read(4096)
    if d:
        out.extend(d)

ser.close()
text = out.decode("utf-8", "replace")
sys.stdout.write(text)
print("\n---- %d bytes captured ----" % len(out))
