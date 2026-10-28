#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""XMOD's example songs, made from nothing: every sample is synthesized here (single-cycle waves,
drums from a sine sweep and noise) and the notes are written below. No one else's music or samples:
the songs are CC0 (public domain).

  tools/make_demo_songs.py OUT_DIR      -> xmod-theme.xm, amiga-days.mod

Pitch: a 32-frame cycle played at C-4 (XM, 8363 Hz) or C-2 (MOD, period 428) is about middle C.
"""
import math
import random
import struct
import sys
from pathlib import Path

NOTES = {"C": 0, "C#": 1, "D": 2, "D#": 3, "E": 4, "F": 5, "F#": 6, "G": 7, "G#": 8, "A": 9, "A#": 10, "B": 11}


def xm_note(name):                     # "A-4" / "C#5" -> 1..96 (C-0 = 1)
    if name == "off":
        return 97
    n, o = name[:-1].rstrip("-"), int(name[-1])
    return 12 * o + NOTES[n] + 1


# --------------------------------------------------------------- the sounds
def cycle(kind, n=32):
    out = []
    for i in range(n):
        t = i / n
        if kind == "square":
            v = 1.0 if t < 0.5 else -1.0
        elif kind == "pulse":
            v = 1.0 if t < 0.25 else -1.0
        elif kind == "saw":
            v = 1.0 - 2.0 * t
        elif kind == "pad":            # two saws an octave apart, softened
            v = 0.6 * (1.0 - 2.0 * t) + 0.4 * (1.0 - 2.0 * ((2 * t) % 1.0))
            v = math.tanh(1.5 * v) / math.tanh(1.5)
        elif kind == "tri":
            v = 4 * t - 1 if t < 0.5 else 3 - 4 * t
        out.append(v)
    return out


def drum(kind, rate):
    rnd = random.Random(kind)
    out = []
    if kind == "kick":
        n, ph = int(rate * 0.22), 0.0
        for i in range(n):
            t = i / rate
            f = 45 + 110 * math.exp(-t * 28)
            ph += 2 * math.pi * f / rate
            out.append(math.sin(ph) * math.exp(-t * 11) + (rnd.uniform(-0.3, 0.3) if i < 40 else 0))
    elif kind == "snare":
        n, ph = int(rate * 0.16), 0.0
        for i in range(n):
            t = i / rate
            ph += 2 * math.pi * 185 / rate
            out.append(0.5 * math.sin(ph) * math.exp(-t * 30) + rnd.uniform(-1, 1) * math.exp(-t * 18) * 0.7)
    elif kind == "hat":
        n, prev = int(rate * 0.05), 0.0
        for i in range(n):
            x = rnd.uniform(-1, 1)
            out.append((x - prev) * 0.6 * math.exp(-i / rate * 60))   # (a crude high-pass)
            prev = x
    peak = max(abs(v) for v in out)
    return [v / peak for v in out]


def to8(wave, gain=0.9):
    return bytes(((int(round(v * 127 * gain)) + 256) % 256) for v in wave)


# --------------------------------------------------------------- XM writer
class XM:
    def __init__(self, name, nch, speed, bpm):
        self.name, self.nch, self.speed, self.bpm = name, nch, speed, bpm
        self.patterns, self.instruments, self.orders, self.restart = [], [], [], 0

    def instrument(self, name, data, loop=True, rel=0, fine=0, vol=48, pan=128, env=None, fade=256, vib=None):
        self.instruments.append(dict(name=name, data=data, loop=loop, rel=rel, fine=fine, vol=vol, pan=pan,
                                     env=env, fade=fade, vib=vib))
        return len(self.instruments)

    def pattern(self, rows=64):
        self.patterns.append([[None] * self.nch for _ in range(rows)])
        return self.patterns[-1]

    def write(self, path):
        h = bytearray(b"Extended Module: " + self.name.encode().ljust(20, b" ")[:20] + b"\x1a" +
                      b"FastTracker v2.00   " + struct.pack("<H", 0x0104))
        orders = bytes(self.orders) + bytes(256 - len(self.orders))
        h += struct.pack("<IHHHHHHHH", 276, len(self.orders), self.restart, self.nch, len(self.patterns),
                         len(self.instruments), 1, self.speed, self.bpm) + orders
        for p in self.patterns:
            data = bytearray()
            for row in p:
                for cell in row:
                    note, ins, vol, fx, par = cell or (0, 0, 0, 0, 0)
                    fields = [note, ins, vol, fx, par]
                    if all(fields):
                        data += bytes(fields)
                    else:
                        data.append(0x80 | sum(1 << i for i, v in enumerate(fields) if v))
                        data += bytes(v for v in fields if v)
            h += struct.pack("<IBHH", 9, 0, len(p), len(data)) + data
        for ins in self.instruments:
            d = ins["data"]
            env = ins["env"] or [(0, 64), (1, 64)]
            pts = env + [(0, 0)] * (12 - len(env))
            vt, vs, vd, vr = ins["vib"] or (0, 0, 0, 0)
            h += struct.pack("<I", 263) + ins["name"].encode().ljust(22, b"\0")[:22] + b"\0" + struct.pack("<H", 1)
            h += struct.pack("<I", 40) + bytes(96)
            h += b"".join(struct.pack("<HH", *pt) for pt in pts) + b"".join(struct.pack("<HH", 0, 32) for _ in range(12))
            sus = len(env) - 1 if ins["env"] and len(env) > 2 else 0
            h += bytes([len(env), 0, sus, 0, 0, 0, 0, 0, (1 | 2) if ins["env"] else 0, 0, vt, vs, vd, vr])
            h += struct.pack("<H", ins["fade"]) + bytes(22)
            delta, old = bytearray(), 0
            for b in d:
                v = b - 256 if b > 127 else b
                delta.append((v - old) & 0xFF)
                old = v
            h += struct.pack("<IIIBbBBbB", len(d), 0 if ins["loop"] else 0, len(d) if ins["loop"] else 0,
                             ins["vol"], ins["fine"], 1 if ins["loop"] else 0, ins["pan"], ins["rel"], len(ins["name"]))
            h += ins["name"].encode().ljust(22, b" ")[:22] + bytes(delta)
        Path(path).write_bytes(bytes(h))


def place(p, ch, start, seq, ins, vol=0, fx=0, par=0):
    """a melody: [(note, rows), ...] from row start on channel ch"""
    r = start
    for note, length in seq:
        if note:
            p[r][ch] = (xm_note(note), 0 if note == "off" else ins, vol, fx, par)
        r += length
    return r


# --------------------------------------------------------------- XMOD Theme
def xmod_theme(out):
    s = XM("XMOD Theme", 8, 6, 125)
    rate = 8363 * 2                                  # the drums at 16726 Hz: C-4 with relative note +12
    arp = s.instrument("arp", to8(cycle("pulse")), vol=22, env=[(0, 64), (6, 20), (20, 10)], fade=2048)   # (1: the keys play it at power-on)
    kick = s.instrument("kick", to8(drum("kick", rate)), loop=False, rel=12, vol=64)
    snare = s.instrument("snare", to8(drum("snare", rate)), loop=False, rel=12, vol=48)
    hat = s.instrument("hat", to8(drum("hat", rate)), loop=False, rel=12, vol=32)
    bass = s.instrument("bass", to8(cycle("saw")), vol=56, env=[(0, 64), (8, 40), (40, 30)], fade=1024)
    pad = s.instrument("pad", to8(cycle("pad")), vol=26, env=[(0, 0), (40, 48), (120, 40)], fade=200)
    lead = s.instrument("lead", to8(cycle("square")), vol=36, env=[(0, 64), (10, 44), (60, 36)], fade=600,
                        vib=(0, 24, 3, 18))
    chords = [("A", ["A", "C", "E"]), ("F", ["F", "A", "C"]), ("C", ["C", "E", "G"]), ("G", ["G", "B", "D"])]
    octs = {"A": 2, "F": 2, "C": 3, "G": 2}

    def groove(p, full=True):
        for bar, (root, tones) in enumerate(chords):
            b = bar * 16
            for r in (0, 8) + ((14,) if bar == 3 else ()):
                p[b + r][0] = (xm_note("C-4"), kick, 0, 0, 0)
            for r in (4, 12):
                p[b + r][1] = (xm_note("C-4"), snare, 0, 0, 0)
            for r in range(0, 16, 2):
                p[b + r][2] = (xm_note("C-4"), hat, 0x10 + (0x20 if r % 4 == 0 else 0x14), 0, 0)
            o = octs[root]
            for r, up in zip(range(0, 16, 2), (0, 0, 1, 0, 0, 1, 0, 1)):
                p[b + r][3] = (xm_note(f"{root}-{o + up}" if len(root) == 1 else f"{root}{o + up}"), bass, 0, 0, 0)
            for r in range(16):                          # the arpeggio: chord tones up, sixteenths
                t = tones[r % 3]
                p[b + r][4] = (xm_note(f"{t}-5"), arp, 0, 0, 0)
            if full:
                p[b][5] = (xm_note(f"{tones[0]}-4"), pad, 0, 0, 0)
                p[b][6] = (xm_note(f"{tones[2]}-4"), pad, 0x10 + 28, 0, 0)

    mel_a = [[("E-5", 4), ("A-5", 4), ("G-5", 2), ("E-5", 2), ("D-5", 4)],
             [("C-5", 6), ("D-5", 2), ("E-5", 4), ("F-5", 4)],
             [("G-5", 4), ("E-5", 4), ("C-5", 4), ("E-5", 4)],
             [("D-5", 6), ("B-4", 2), ("D-5", 4), ("G-5", 4)]]
    mel_b = [[("A-5", 4), ("C-6", 4), ("B-5", 2), ("A-5", 2), ("G-5", 4)],
             [("A-5", 8), ("F-5", 4), ("A-5", 4)],
             [("G-5", 6), ("E-5", 2), ("C-5", 4), ("D-5", 4)],
             [("B-4", 6), ("D-5", 2), ("G-5", 4), ("off", 4)]]

    def melody(p, mel):
        seq = [n for bar in mel for n in bar]
        place(p, 7, 0, seq, lead)
        r = 3                                        # the echo: three rows later, quieter, on channel 6
        for note, length in seq:
            if r < 64 and note != "off":
                p[r][6] = (xm_note(note), lead, 0x10 + 14, 0, 0)
            r += length

    intro = s.pattern()
    groove(intro, full=False)
    for r in range(64):                              # (no bass in the intro's first half)
        if r < 32:
            intro[r][3] = None
    groove(s.pattern())
    a = s.pattern()
    groove(a)
    melody(a, mel_a)
    b = s.pattern()
    groove(b)
    melody(b, mel_b)
    s.orders = [0, 1, 2, 3, 2, 3, 1]
    s.restart = 1
    s.write(out)


# --------------------------------------------------------------- Amiga Days (MOD)
PT_PERIODS = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
              428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
              214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113]


def mod_period(name):                  # ProTracker naming: C-1 .. B-3
    n, o = name[:-1].rstrip("-"), int(name[-1])
    return PT_PERIODS[(o - 1) * 12 + NOTES[n]]


def amiga_days(out):
    rate = 8287 * 2                                  # (the drums: an octave up at C-3)
    samples = [
        ("kick", to8(drum("kick", rate)), 64, False),
        ("snare", to8(drum("snare", rate)), 44, False),
        ("hat", to8(drum("hat", rate)), 26, False),
        ("bass", to8(cycle("tri"), 1.0), 60, True),
        ("lead", to8(cycle("square"), 0.8), 34, True),
        ("arp", to8(cycle("pulse"), 0.8), 20, True),
    ]
    prog = [("C", "C-1", ["C", "E", "G"]), ("A", "A-1", ["A", "C", "E"]),
            ("F", "F-1", ["F", "A", "C"]), ("G", "G-1", ["G", "B", "D"])]
    lead = [[("G-2", 2), ("E-2", 2), ("G-2", 2), ("C-3", 4), ("B-2", 2), ("G-2", 4)],
            [("A-2", 4), ("C-3", 2), ("E-3", 4), ("D-3", 2), ("C-3", 4)],
            [("A-2", 2), ("C-3", 2), ("F-2", 4), ("A-2", 4), ("C-3", 4)],
            [("B-2", 6), ("D-3", 2), ("G-2", 8)]]

    def pattern(with_lead):
        cells = [[(0, 0, 0, 0)] * 4 for _ in range(64)]   # (period, sample, effect, parameter)
        for bar, (_, bnote, tones) in enumerate(prog):
            b = bar * 16
            for r in range(0, 16, 2):                     # drums on channel 0: kick, hat, snare, hat
                k = (r // 2) % 4
                smp, per = [(1, "C-3"), (3, "C-3"), (2, "C-3"), (3, "C-3")][k]
                cells[b + r][0] = (mod_period(per), smp, 0, 0)
            for r in range(0, 16, 4):                     # bass, channel 1: root and octave
                cells[b + r][1] = (mod_period(bnote), 4, 0, 0)
                cells[b + r + 2][1] = (mod_period(bnote[:-1] + "2"), 4, 0xC, 0x28)
            for r in range(0, 16, 2):                     # arpeggio, channel 3: 0xy, +3 +7 minor, +4 +7 major
                cells[b + r][3] = (mod_period(f"{tones[0]}-2"), 6, 0x0, 0x37 if tones == ["A", "C", "E"] else 0x47)
            if with_lead:
                r = b
                for note, length in lead[bar]:            # lead, channel 2, with vibrato on held notes
                    cells[r][2] = (mod_period(note), 5, 0, 0)
                    if length >= 4:
                        cells[r + 2][2] = (0, 0, 0x4, 0x34)
                    r += length
        if with_lead:                                     # the phrase's last note ends (C00) before the next
            cells[62][2] = (0, 0, 0xC, 0)
        else:                                             # no lead here: whatever still sounds stops
            cells[0][2] = (0, 0, 0xC, 0)
        return cells

    pats = [pattern(False), pattern(True)]
    order = [0, 1, 1, 0, 1]
    m = bytearray(b"Amiga Days".ljust(20, b"\0"))
    for i in range(31):
        if i < len(samples):
            name, data, vol, loop = samples[i]
            data = data + (b"\0" if len(data) % 2 else b"")
            m += name.encode().ljust(22, b"\0") + struct.pack(">HBBHH", len(data) // 2, 0, vol, 0,
                                                             len(data) // 2 if loop else 1)
        else:
            m += bytes(22) + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
    m += bytes([len(order), 127]) + bytes(order) + bytes(128 - len(order)) + b"M.K."
    for p in pats:
        for row in p:
            for per, smp, fx, par in row:
                m += bytes([(smp & 0xF0) | (per >> 8), per & 0xFF, ((smp & 0x0F) << 4) | fx, par])
    for name, data, vol, loop in samples:
        m += data + (b"\0" if len(data) % 2 else b"")
    Path(out).write_bytes(bytes(m))


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else "songs")
    d.mkdir(parents=True, exist_ok=True)
    xmod_theme(d / "xmod-theme.xm")
    amiga_days(d / "amiga-days.mod")


if __name__ == "__main__":
    main()
