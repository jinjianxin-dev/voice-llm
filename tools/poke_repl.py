"""Poke the board's REPL without touching DTR/RTS (which re-enumerates the
native USB port on the S3). Sends a newline and a harmless command, prints
whatever comes back."""
import serial, time, sys

PORT = "COM7"

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.3
ser.dtr = False          # must be set before open on some drivers
ser.rts = False
ser.open()
time.sleep(0.5)
ser.reset_input_buffer()

ser.write(b"\n")
time.sleep(1.0)
ser.write(b"turn on pin 4\n")
time.sleep(2.5)

buf = bytearray()
t0 = time.time()
while time.time() - t0 < 2.0:
    d = ser.read(4096)
    if d:
        buf.extend(d)
ser.close()

sys.stdout.write(buf.decode("utf-8", "replace"))
print("\n---- %d bytes ----" % len(buf))
