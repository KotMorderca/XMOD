#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The FM-1's USB serial console (XMOD's firmware/console.c).

  uv run --python 3.12 --with pyserial python tools/console.py [COMx] [command ...]

With commands: sends each, prints the replies and exits (e.g. `tools/console.py stat`).
Without: an interactive terminal (Ctrl+] quits).
"""
import sys
import time

import serial
import serial.tools.list_ports


def find_port():
    from songs import find_port as fp            # (one rule: tools/songs.py)
    return fp()


def main():
    args = sys.argv[1:]
    port = args.pop(0) if args and args[0].upper().startswith("COM") or (args and args[0].startswith("/dev/")) else find_port()
    if not args:
        from serial.tools import miniterm
        sys.argv = ["miniterm", port, "115200", "--eol", "CR"]
        miniterm.main()
        return
    s = serial.Serial(port, 115200, timeout=0.3)
    s.dtr = True
    time.sleep(0.3)
    s.read(4096)
    for cmd in args:
        s.write(cmd.encode() + b"\r")
        time.sleep(0.4)
        out = s.read(65536).decode(errors="replace")
        print(out.replace("\r", "").rstrip("> \n"))
    s.close()


if __name__ == "__main__":
    main()
