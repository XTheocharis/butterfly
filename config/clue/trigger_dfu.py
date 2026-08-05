#!/usr/bin/env python3
"""Send the CDC "DFU" magic to a connected CLUE firmware.

The firmware (src/serial.cpp) parses the bytes 'D','F','U' anywhere in the
CDC RX buffer and reboots into the Adafruit UF2 bootloader via
GPREGRET=0x57. The firmware force-asserts DTR on CDC PORT_OPEN and only
schedules RX after DTR is asserted, so a bare shell redirect does not work
-- pyserial is required to drive the control lines in time.
"""
import sys
import time

CLUE_USB_ID = "c0ff:eeee"


def find_clue_port():
    try:
        from serial.tools import list_ports
    except ImportError:
        print("pyserial is not installed: pip install pyserial", file=sys.stderr)
        sys.exit(2)
    for port in list_ports.comports():
        if port.subsystem == "usb":
            vid = f"{port.vid:04x}" if port.vid else ""
            pid = f"{port.pid:04x}" if port.pid else ""
        else:
            vid, pid = "", ""
        if f"{vid}:{pid}" == CLUE_USB_ID or CLUE_USB_ID in (port.hwid or ""):
            return port.device
    return None


def main():
    try:
        import serial
    except ImportError:
        print("pyserial is not installed: pip install pyserial", file=sys.stderr)
        sys.exit(2)

    dev = find_clue_port()
    if not dev:
        print(f"No CLUE firmware device (USB {CLUE_USB_ID}) found on /dev/ttyACM*",
              file=sys.stderr)
        sys.exit(1)

    try:
        s = serial.Serial(dev, baudrate=115200, timeout=0.2)
        s.dtr = True
        s.rts = False
        time.sleep(0.15)
        if s.in_waiting:
            s.read(s.in_waiting)
        s.write(b"DFU")
        s.flush()
        time.sleep(0.4)
        s.close()
    except Exception as e:
        print(f"DFU trigger failed on {dev}: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"DFU requested on {dev}; device should reboot into UF2 bootloader.")


if __name__ == "__main__":
    main()
