"""Flash through the ROM USB-Serial/JTAG port without touching line coding.

Background: this board's ROM CDC accepts one SET_LINE_CODING at open time (the
value Windows already has cached) and STALLs every later change -- including the
one esptool makes right after opening (`_set_port_baudrate`).  A STALLed request
also poisons the usbser state until the device is replugged, so we must open the
port exactly once and never issue another control request.

So:
  * we open pyserial ourselves at 115200 (the cached DCB, no transfer needed)
  * we hand that object to esptool through a wrapper that silently ignores
    baudrate / dtr / rts writes
  * flashing runs at the ROM's own speed; USB is not a real UART, so this only
    costs wall-clock time, not reliability

Usage:  python flash115.py <image> [offset] [port]
        python flash115.py --pairs <off1> <img1> <off2> <img2> ... [port]
"""
import sys

import serial
from serial.tools import list_ports
import esptool
import esptool.loader as loader


def find_esp_port():
    """Auto-detect the Espressif board (VID 303A); Windows reassigns the
    COM number after every reflash, so never hard-code one."""
    cands = [p for p in list_ports.comports() if p.vid == 0x303A]
    if not cands:
        print("ERROR: no Espressif board found. Plug it into the port labelled USB.")
        sys.exit(1)
    cands.sort(key=lambda p: 0 if p.pid == 0x0009 else 1)
    return cands[0].device


args = sys.argv[1:]
PORT = ""
flash_args = []
if args and args[0] == "--pairs":
    rest = args[1:]
    if rest and rest[-1].upper().startswith("COM"):
        PORT = rest[-1]
        rest = rest[:-1]
    flash_args = rest                       # off1 img1 off2 img2 ...
else:
    BIN = args[0] if args else r"E:\esp32-tools\projects\voice-llm\build\voice_llm.bin"
    OFFSET = args[1] if len(args) > 1 else "0x10000"
    if len(args) > 2:
        PORT = args[2]
    flash_args = [OFFSET, BIN]

if not PORT:
    PORT = find_esp_port()


class QuietPort:
    """Delegates data I/O to a real port, swallows line-control writes.

    esptool creates the port with do_not_open=True and calls .open() itself,
    so open() must be a no-op here -- the port is already open.
    """

    def __init__(self, real):
        object.__setattr__(self, "_real", real)

    def open(self):
        pass

    def close(self):
        try:
            self._real.close()
        except Exception:
            pass

    def __getattr__(self, name):
        return getattr(object.__getattribute__(self, "_real"), name)

    def __setattr__(self, name, value):
        if name in ("_real",):
            object.__setattr__(self, name, value)
            return
        real = object.__getattribute__(self, "_real")
        if name in ("baudrate", "dtr", "rts"):
            return                      # never poke the control endpoint
        setattr(real, name, value)

    @property
    def baudrate(self):
        return 115200

    @property
    def dtr(self):
        return False

    @property
    def rts(self):
        return False


real = serial.Serial()
real.port = PORT
real.baudrate = 115200
real.timeout = 0.1
real.write_timeout = 5.0
real.dtr = False
real.rts = False
real.open()
print("[opened %s @115200]" % PORT, flush=True)

wrapper = QuietPort(real)
loader.serial.serial_for_url = lambda *a, **k: QuietPort(real)
loader.ESPLoader._set_port_baudrate = lambda self, baud: None
loader.ESPLoader.change_baud = lambda self: None

try:
    esptool.main([
        "--chip", "esp32s3",
        "-p", PORT,
        "-b", "115200",
        "--before", "no_reset",
        "--after", "no_reset",
        "--connect-attempts", "5",
        "write_flash"] + flash_args,
    )
finally:
    try:
        real.close()
    except Exception:
        pass
