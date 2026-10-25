"""Read whatever the board prints for N seconds (no reset, no write)."""
import serial, time, sys

PORT = "COM7"
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()

buf = bytearray()
t0 = time.time()
while time.time() - t0 < SECS:
    d = ser.read(4096)
    if d:
        buf.extend(d)
ser.close()

sys.stdout.write(buf.decode("utf-8", "replace"))
print("\n---- %d bytes ----" % len(buf))
