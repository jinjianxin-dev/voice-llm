"""esptool, but without ever touching the serial line coding.

The USB-Serial/JTAG port on this board currently STALLs every SET_LINE_CODING
request: opening at 115200 works (Windows reuses its cached DCB) but any
attempt to change the rate -- including esptool's own "set baud" step -- fails
with "Failed to set baud rate".  Flashing does not actually need a different
rate (it is USB, not a real UART), so we neutralise the two places where
esptool would change it and keep the chip's ROM speed.

    python espflash_nowin.py [port] [image] [offset]
"""
import sys

import esptool
import esptool.loader as loader

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
BIN = sys.argv[2] if len(sys.argv) > 2 else r"E:\esp32-tools\projects\voice-llm\build\voice_llm.bin"
OFFSET = sys.argv[3] if len(sys.argv) > 3 else "0x10000"

loader.ESPLoader._set_port_baudrate = lambda self, baud: None
loader.ESPLoader.change_baud = lambda self: None


def no_lower_baud(self):
    return False


loader.ESPLoader._set_lower_baud = no_lower_baud

esptool.main([
    "--chip", "esp32s3",
    "-p", PORT,
    "-b", "115200",
    "--before", "no_reset",
    "--after", "no_reset",
    "--connect-attempts", "5",
    "write_flash", OFFSET, BIN,
])
