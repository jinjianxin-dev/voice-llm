"""Run an esptool command on this stubborn port without touching line coding.

The ROM USJ CDC on this board accepts only the line coding Windows already has
cached (115200) and STALLs every change, so esptool's own baud handling must be
bypassed. See flash115.py for the full story.

    python espcmd.py <esptool args...>
"""
import sys

import serial
import esptool
import esptool.loader as loader


class QuietPort:
    def __init__(self, real):
        object.__setattr__(self, "_r", real)

    def open(self):
        pass

    def close(self):
        pass

    def __getattr__(self, name):
        return getattr(object.__getattribute__(self, "_r"), name)

    def __setattr__(self, name, value):
        if name == "_r":
            object.__setattr__(self, name, value)
            return
        if name in ("baudrate", "dtr", "rts"):
            return
        setattr(object.__getattribute__(self, "_r"), name, value)

    @property
    def baudrate(self):
        return 115200

    @property
    def dtr(self):
        return False

    @property
    def rts(self):
        return False


def open_real():
    from serial.tools import list_ports
    ps = [p for p in list_ports.comports() if p.vid == 0x303A]
    if not ps:
        raise SystemExit("[no ESP32 port]")
    real = serial.Serial()
    real.port = ps[0].device
    real.baudrate = 115200
    real.timeout = 0.1
    real.write_timeout = 5.0
    real.dtr = False
    real.rts = False
    real.open()
    print("[opened %s]" % real.port, flush=True)
    return real


def patched_main(args, real):
    loader.serial.serial_for_url = lambda *a, **k: QuietPort(real)
    loader.ESPLoader._set_port_baudrate = lambda self, b: None
    loader.ESPLoader.change_baud = lambda self: None
    try:
        esptool.main(args)
    except SystemExit:
        pass


if __name__ == "__main__":
    real = open_real()
    patched_main(
        ["--chip", "esp32s3", "-p", real.port, "-b", "115200",
         "--before", "no_reset", "--after", "no_reset",
         "--connect-attempts", "3"] + sys.argv[1:],
        real)
