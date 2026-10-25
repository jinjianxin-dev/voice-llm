"""Plain serial monitor for the voice-llm board.

Why not `idf.py monitor`: on Windows it prints a red GDB warning that looks
like a fatal error, and it dies when the S3 re-enumerates its native USB
port. This one reconnects silently and lets you type commands straight into
the board's REPL.

    python py_monitor.py [COM7] [115200]
"""
import sys
import time
import threading

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("pyserial is missing. Install it with: pip install pyserial")
    sys.exit(1)


def pick_port(explicit=None):
    """With CONFIG_ESP_CONSOLE_USB_CDC the board enumerates as Espressif
    VID 303A / PID 0009 and Windows may hand it a different COM number after
    every reflash (the old one lingers as a ghost). Find it by VID instead of
    hard-coding a port."""
    if explicit:
        return explicit
    cands = [p for p in list_ports.comports() if p.vid == 0x303A]
    if not cands:
        return "COM7"
    # Prefer the ROM-CDC interface (PID 0009); fall back to anything Espressif.
    cands.sort(key=lambda p: 0 if p.pid == 0x0009 else 1)
    return cands[0].device


PORT = pick_port(sys.argv[1] if len(sys.argv) > 1 else None)
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

stop = False
current = {"ser": None}
lock = threading.Lock()


def reader():
    """Keep the port open and print everything the board says."""
    while not stop:
        try:
            ser = serial.Serial()
            ser.port = PORT
            ser.baudrate = BAUD
            ser.timeout = 0.2
            # NEVER leave RTS asserted. On the ESP32-S3's USB-Serial/JTAG the
            # CDC control lines ARE the auto-reset circuit (esptool/reset.py):
            #   DTR=True  -> GPIO0 LOW  (download-mode strap)
            #   RTS=True  -> EN LOW     (chip held in reset)
            # The old version set rts=True to "enable the console"; that held
            # the chip in reset, and every reconnect looked like a reboot:
            #   close port -> EN released -> ROM prints load:/entry ->
            #   open port  -> RTS=True -> reset again -> port lost -> ...
            # So: hold EN low briefly, release it, and leave both lines idle.
            ser.open()
            ser.setRTS(True)        # EN low -> clean reset
            time.sleep(0.15)
            ser.setDTR(False)       # GPIO0 high -> normal boot, not download
            ser.setRTS(False)       # EN high -> run
            ser.setDTR(False)
        except Exception as exc:
            print("\n[%s not ready: %s -- retrying]" % (PORT, exc), flush=True)
            time.sleep(1.0)
            continue

        with lock:
            current["ser"] = ser
        print("[connected to %s @ %d]" % (PORT, BAUD), flush=True)

        try:
            while not stop:
                data = ser.read(4096)
                if data:
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()
        except Exception as exc:
            if not stop:
                print("\n[port lost: %s -- reconnecting]" % exc, flush=True)
        finally:
            with lock:
                if current["ser"] is ser:
                    current["ser"] = None
            try:
                ser.close()
            except Exception:
                pass
        time.sleep(0.5)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    print("=" * 58)
    print(" voice-llm serial monitor   port=%s  baud=%d" % (PORT, BAUD))
    print(" type a command + Enter to send it to the board")
    print(" e.g.  turn on pin 4   |   alias desk lamp = 4")
    print(" press Ctrl+C to quit")
    print("=" * 58)

    threading.Thread(target=reader, daemon=True).start()

    global stop
    try:
        while True:
            line = input()
            if not line:
                continue
            with lock:
                ser = current["ser"]
            if ser is None:
                print("[not connected yet -- wait a moment]")
                continue
            try:
                ser.write((line + "\r\n").encode("utf-8"))
            except Exception as exc:
                print("[send failed: %s]" % exc)
    except (KeyboardInterrupt, EOFError):
        stop = True
        print("\nbye.")


if __name__ == "__main__":
    main()
