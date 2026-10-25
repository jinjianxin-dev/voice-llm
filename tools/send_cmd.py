"""Send a line to the board REPL and print the reply, tolerating the USB
re-enumeration that happens on reset."""
import serial, time, sys

PORT = "COM7"
CMD = sys.argv[1] if len(sys.argv) > 1 else "turn on pin 4"

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()

time.sleep(0.6)
ser.reset_input_buffer()

out = bytearray()
ser.write((CMD + "\r\n").encode())
t0 = time.time()
while time.time() - t0 < 4.0:
    try:
        d = ser.read(4096)
    except Exception as e:
        out.extend(("\n[port error: %s]\n" % e).encode())
        break
    if d:
        out.extend(d)
ser.close()

sys.stdout.write(out.decode("utf-8", "replace"))
print("\n---- sent %r, got %d bytes ----" % (CMD, len(out)))
