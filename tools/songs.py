#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The FM-1's song slots over the USB serial port (firmware/console.c), from the command line.

  uv run --python 3.12 --with pyserial python tools/songs.py [--port COMx] COMMAND ...

  info | ls
  get I OUT.fmtk           a slot's song image (.xm / .mod: build/host/convert it, or the web manager)
  put IN.fmtk [NAME]       a song image into a new slot (convert .xm / .mod first)
  del I | ren I NAME | load I [force]
  backup DIR               every slot to DIR/NN name.fmtk
  restore DIR              every .fmtk in DIR into new slots
"""
import base64
import binascii
import os
import re
import sys
import time

import serial
import serial.tools.list_ports


def find_port():
    """the FM-1's port (USB VID 1209); refused when another device shares its name (Windows can give
    two devices the same COM number: whatever answers there may not be the FM-1)"""
    ports = list(serial.tools.list_ports.comports())
    is_fm1 = lambda p: p.vid == 0x1209 or "felucca" in (p.description or "").lower() or "fm-1" in (p.description or "").lower()
    for p in ports:
        if is_fm1(p):
            others = [q for q in ports if q.device == p.device and not is_fm1(q)]
            if others:
                raise SystemExit(f"{p.device} is also another device's port ({others[0].description}, "
                                 f"USB {others[0].vid:04x}:{others[0].pid or 0:04x}): give the FM-1 another "
                                 "COM number in Device Manager (Ports, its USB serial device, Port Settings, Advanced)")
            return p.device
    raise SystemExit("FM-1 serial port not found")


class FM1:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=5)
        self.s.dtr = True
        time.sleep(0.3)
        self.s.reset_input_buffer()
        self.buf = b""
        self.cmd("raw")

    def line(self, timeout=10.0):
        t0 = time.time()
        while b"\n" not in self.buf:
            d = self.s.read(self.s.in_waiting or 1)
            self.buf += d
            if time.time() - t0 > timeout:
                raise SystemExit("FM-1: no answer")
        ln, self.buf = self.buf.split(b"\n", 1)
        return ln.decode(errors="replace").strip()

    def send(self, text):
        self.s.write(text.encode() + b"\r")

    def cmd(self, text):
        """send, collect the lines up to ok / err"""
        self.send(text)
        out = []
        while True:
            ln = self.line()
            if not ln or ln.startswith(">") or ln.startswith("XMOD"):
                continue
            if ln == "ok" or ln.startswith("ok "):
                return out + [ln]
            if ln.startswith("err"):
                raise SystemExit(f"FM-1: {ln}")
            out.append(ln)

    def ls(self):
        slots = []
        for ln in self.cmd("ls"):
            m = re.match(r"slot (\d+) (\d+) (\d+) ?(.*)", ln)
            if m:
                slots.append((int(m[1]), int(m[2]), int(m[3]), m[4]))
        return slots

    def get(self, i):
        self.send(f"get {i}")
        ln = self.line()
        m = re.match(r"data (\d+) ([0-9a-f]+)", ln)
        if not m:
            raise SystemExit(f"FM-1: {ln}")
        size, crc = int(m[1]), int(m[2], 16)
        data = b""
        while True:
            ln = self.line()
            if ln == "ok":
                break
            data += base64.b64decode(ln)
        if len(data) != size or binascii.crc32(data) != crc:
            raise SystemExit("FM-1: the song came back damaged")
        return data

    def put(self, data, name):
        if data[:4] != b"FMTK":
            raise SystemExit("not a song image (.fmtk): convert .xm / .mod first")
        self.send(f"put {len(data)} {binascii.crc32(data):08x} {name[:23]}")
        ln = self.line(30)
        if ln != "ready":
            raise SystemExit(f"FM-1: {ln}")
        for i in range(0, len(data), 96):
            self.send("d " + base64.b64encode(data[i:i + 96]).decode())
        while True:
            ln = self.line(30)
            if ln.startswith("ack"):
                continue
            if ln.startswith("saved"):
                return int(ln.split()[1])
            raise SystemExit(f"FM-1: {ln}")


def main():
    a = sys.argv[1:]
    port = None
    if a[:1] == ["--port"]:
        port, a = a[1], a[2:]
    if not a:
        raise SystemExit(__doc__)
    fm = FM1(port or find_port())
    c = a[0]
    if c == "info":
        print("\n".join(fm.cmd("info")[:-1]))
    elif c == "ls":
        for i, size, seq, name in fm.ls():
            print(f"{i:2}  {size:7} B  #{seq:<4} {name}")
    elif c == "get":
        open(a[2], "wb").write(fm.get(int(a[1])))
    elif c == "put":
        data = open(a[1], "rb").read()
        name = a[2] if len(a) > 2 else os.path.splitext(os.path.basename(a[1]))[0]
        print("slot", fm.put(data, name))
    elif c in ("del", "ren", "load"):
        print(" ".join(fm.cmd(" ".join(a))))
    elif c == "backup":
        os.makedirs(a[1], exist_ok=True)
        for i, size, seq, name in fm.ls():
            safe = re.sub(r"[^\w .()+-]", "_", name) or "song"
            open(os.path.join(a[1], f"{i:02} {safe}.fmtk"), "wb").write(fm.get(i))
            print("saved", i, name)
    elif c == "restore":
        for f in sorted(os.listdir(a[1])):
            if f.endswith(".fmtk"):
                name = re.sub(r"^\d+ ", "", f[:-5])
                print(f, "-> slot", fm.put(open(os.path.join(a[1], f), "rb").read(), name))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
