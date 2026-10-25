"""One-shot serial check: boot log + two LLM commands, then exit.

Uses the same line discipline as tools/py_monitor.py (RTS pulse = clean
reset, both lines idle afterwards -- never leave RTS asserted).
"""
import sys, time, threading
import serial
from serial.tools import list_ports

def pick_port():
    for p in list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    return None

port = pick_port()
if not port:
    sys.exit("no ESP32-S3 (VID_303A) found")
print("port:", port)

ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
ser.open()
ser.setRTS(True);  time.sleep(0.15)
ser.setDTR(False); ser.setRTS(False); ser.setDTR(False)

buf = []
def reader():
    while reader.run:
        data = ser.read(4096)
        if data:
            text = data.decode("utf-8", "replace")
            buf.append(text)
            sys.stdout.write(text); sys.stdout.flush()
reader.run = True
threading.Thread(target=reader, daemon=True).start()

time.sleep(10)                     # boot + AFE init + command registration
# re-point the persisted alias (NVS had 台灯=4 from the old wiring), then
# exercise blink + chase on the real LED pins (1, 2, 8)
for cmd in ["把1号脚叫做台灯", "打开台灯", "让台灯闪烁，每500毫秒，闪3次",
            "让1号脚、2号脚、8号脚跑马灯，每500毫秒", "把所有灯都关掉"]:
    ser.write((cmd + "\n").encode("utf-8")); ser.flush()
    time.sleep(3.0)
reader.run = False
time.sleep(0.3)
ser.close()
print("\n--- done ---")
