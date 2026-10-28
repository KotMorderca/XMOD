#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""XMOD's splash screen (240 x 240, 16 colours), drawn here pixel by pixel: a chrome logo over a
striped sun, a starfield and a perspective grid, and on the grid a kitten and a chick making music.

  tools/gen_splash.py OUT.h [PREVIEW.png] [--version TEXT]

OUT.h: a palette (RGB565) and the pixels as 4-bit indexes, two per byte (firmware/splash.c shows it).
"""
import math
import random
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_font import load_bdf  # noqa: E402

W = H = 240
PAL = [
    (0x00, 0x00, 0x00),  # 0 black
    (0x0a, 0x08, 0x22),  # 1 night
    (0x1c, 0x10, 0x48),  # 2 sky
    (0x40, 0x14, 0x68),  # 3 dusk
    (0xb0, 0x20, 0x88),  # 4 magenta
    (0xff, 0x4a, 0xa8),  # 5 pink
    (0xff, 0x86, 0x3a),  # 6 orange
    (0xff, 0xd8, 0x5a),  # 7 yellow
    (0xff, 0xff, 0xff),  # 8 white
    (0x60, 0xe0, 0xff),  # 9 cyan
    (0x2a, 0x6c, 0xff),  # 10 blue
    (0x5c, 0x62, 0x8c),  # 11 fur shade
    (0x90, 0x98, 0xc0),  # 12 grey
    (0x24, 0x0c, 0x3c),  # 13 floor
    (0x04, 0x02, 0x0c),  # 14 shadow
    (0xc0, 0xf4, 0xff),  # 15 ice
]

# the logo's letters, 8 x 10 cells
GLYPHS = {
    "X": ["11....11", "11....11", ".11..11.", "..1111..", "...11...",
          "...11...", "..1111..", ".11..11.", "11....11", "11....11"],
    "M": ["11....11", "111..111", "11111111", "11.11.11", "11....11",
          "11....11", "11....11", "11....11", "11....11", "11....11"],
    "O": [".111111.", "11111111", "11....11", "11....11", "11....11",
          "11....11", "11....11", "11....11", "11111111", ".111111."],
    "D": ["1111111.", "11111111", "11....11", "11....11", "11....11",
          "11....11", "11....11", "11....11", "11111111", "1111111."],
}

# the band on the grid, drawn from shapes: each figure's pixels, then its outline (black)
def ell(cx, cy, rx, ry):
    return {(x, y) for y in range(int(cy - ry) - 1, int(cy + ry) + 2) for x in range(int(cx - rx) - 1, int(cx + rx) + 2)
            if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1}


def tri(a, b, c):
    def side(p, q, r):
        return (r[0] - p[0]) * (q[1] - p[1]) - (r[1] - p[1]) * (q[0] - p[0])
    xs, ys = [p[0] for p in (a, b, c)], [p[1] for p in (a, b, c)]
    out = set()
    for y in range(int(min(ys)), int(max(ys)) + 2):
        for x in range(int(min(xs)), int(max(xs)) + 2):
            d = side(a, b, (x, y)), side(b, c, (x, y)), side(c, a, (x, y))
            if min(d) >= 0 or max(d) <= 0:
                out.add((x, y))
    return out


def line(x0, y0, x1, y1):
    n = max(abs(round(x1) - round(x0)), abs(round(y1) - round(y0)), 1)
    return {(round(x0 + (x1 - x0) * i / n), round(y0 + (y1 - y0) * i / n)) for i in range(n + 1)}


def shaded(base, shade, light, cx, cy, rx, ry):
    """light from the top left: the far side in the shade, dithered between"""
    def colour(x, y):
        d = 0.55 * (x - cx) / rx + 0.85 * (y - cy) / ry
        if d > 0.62 or (d > 0.42 and (x + y) % 2):
            return shade
        if light is not None and (d < -0.92 or (d < -0.82 and (x + y) % 2)):
            return light
        return base
    return colour


class Figure:
    """a figure drawn in its own units around a foot point (x0, y0, scale z)"""
    def __init__(self, x0, y0, z):
        self.px, self.x0, self.y0, self.z = {}, x0, y0, z

    def p(self, x, y):
        return self.x0 + x * self.z, self.y0 + y * self.z

    def ell(self, x, y, rx, ry):
        return ell(*self.p(x, y), rx * self.z, ry * self.z)

    def tri(self, a, b, c):
        return tri(self.p(*a), self.p(*b), self.p(*c))

    def line(self, x0, y0, x1, y1, w=1):
        out = set()
        for i in range(w):
            out |= line(*self.p(x0 + i, y0), *self.p(x1 + i, y1))
        return out

    def dots(self, pts):
        return {tuple(round(v) for v in self.p(x, y)) for x, y in pts}

    def shaded(self, base, shade, light, x, y, rx, ry):
        return shaded(base, shade, light, *self.p(x, y), rx * self.z, ry * self.z)

    def paint(self, shape, colour):
        for p in shape:
            self.px[p] = colour(*p) if callable(colour) else colour

    def draw(self, put):
        for (x, y) in self.px:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                if (x + dx, y + dy) not in self.px:
                    put(x + dx, y + dy, 0)
        for (x, y), c in self.px.items():
            put(x, y, c)


def kitten(put, x0, y0, z):
    """grey with a white chest and paws, cyan headphones on the ears; palette 11 is the fur's shade"""
    k = Figure(x0, y0, z)
    # sitting three-quarters: the haunch at the back (left), the front legs ahead (right), the head to us
    tail = []
    for i in range(40):                                    # the tail on the ground, curled up at the tip
        t = i / 39
        r = 4.2 - 1.6 * t
        tail.append((k.ell(-28 - 20 * t + 4 * t ** 3, -4 - 30 * t ** 2.4, r, r), i in (14, 24, 34)))
    k.paint(set().union(*(p for p, _ in tail)), 12)
    for p, stripe in tail:
        if stripe:
            k.paint(p, 11)
    torso = set()                                          # the torso: from the neck the back slopes down to the rump
    for i in range(31):
        t = i / 30
        r = 13 + 5 * t
        torso |= k.ell(3 - 19 * t, -56 + 39 * t, r, r)
    k.paint(torso, k.shaded(12, 11, 15, -6, -36, 24, 34))
    hx, hy = k.p(-13, -15)                                 # the hind leg folded: the thigh, a crease in front of it,
    thigh = k.ell(-13, -15, 12, 12)
    k.paint({(x, y) for (x, y) in k.ell(-13, -15, 13, 13) - thigh
             if -150 <= math.degrees(math.atan2(y - hy, x - hx)) <= 40}, 0)
    k.paint(thigh, k.shaded(12, 11, 15, -13, -15, 12, 12))
    k.paint(k.line(-19, -21, -13, -24, 2) | k.line(-22, -14, -16, -18, 2), 11)   # (its stripes)
    k.paint(k.ell(-11, -3.5, 10, 4.5), 0)                  # the foot flat on the ground, forward, white toes
    k.paint(k.ell(-11, -3.5, 9, 3.5), k.shaded(12, 11, None, -11, -3.5, 9, 3.5))
    k.paint(k.ell(-5, -3.5, 3.5, 2.8), 8)
    for px, far in ((13, True), (3, False)):               # the front legs, down from the shoulders, the far one first
        leg = k.ell(px, -25, 4.5, 24)
        ring = {(x, y) for (x, y) in k.ell(px, -25, 5.5, 25) - leg if y > y0 - 36 * z}
        k.paint(ring, 0)
        k.paint(leg, 11 if far else k.shaded(12, 11, 15, px - 3, -24, 9, 60))
        k.paint(k.ell(px + 1, -4, 6.5, 4.5), 0)            # the paws, white, toes
        k.paint(k.ell(px + 1, -4, 5.5, 3.5), k.shaded(8, 15, None, px + 1, -4, 5.5, 3.5))
        k.paint(k.line(px - 1, -4, px - 1, -2) | k.line(px + 3, -4, px + 3, -2), 12)
    k.paint(k.ell(7, -47, 11, 16), k.shaded(8, 15, None, 7, -47, 11, 16))       # the chest, white, over the shoulders
    cy = y0 - 92 * z                                       # the headphones' band, close over the head, behind the ears
    band = {(x, y) for x in range(int(x0 - 40 * z), int(x0 + 41 * z)) for y in range(int(y0 - 120 * z), int(y0 - 96 * z))
            if 1 <= ((x - x0) / (34 * z)) ** 2 + ((y - cy) / (18 * z)) ** 2 and
            ((x - x0) / (36.5 * z)) ** 2 + ((y - cy) / (20.5 * z)) ** 2 <= 1}
    k.paint(band, lambda x, y: 10 if ((x - x0) / (35.2 * z)) ** 2 + ((y - cy) / (19.2 * z)) ** 2 < 1 else 9)
    for s in (-1, 1):                                      # ears, pink inside
        k.paint(k.tri((s * 30, -84), (s * 25, -122), (s * 6, -96)), 12)
        k.paint(k.tri((s * 25, -89), (s * 23, -113), (s * 11, -96)), 5)
    k.paint(k.ell(0, -74, 33, 27), k.shaded(12, 11, 15, 0, -74, 33, 27))
    for x, h in ((-7, 4), (0, 6), (7, 4)):                 # stripes on the forehead
        k.paint(k.line(x, -99, x, -99 + h, 2), 11)
    for s in (-1, 1):                                      # the cups side on, against the ears, a light on each
        k.paint(k.ell(s * 32, -92, 5, 11), k.shaded(9, 10, 15, s * 32, -92, 5, 11))
        k.paint(k.ell(s * 29, -92, 2, 9), 10)
        k.paint(k.line(s * 33, -98, s * 33, -86), 5)
    k.paint(k.ell(-7, -62, 9, 7) | k.ell(7, -62, 9, 7), 8)
    for s in (-1, 1):                                      # eyes: big, two glints; blush
        k.paint(k.ell(s * 13, -77, 6, 7), 0)
        k.paint(k.ell(s * 13 - 2, -80, 2, 2), 8)
        k.paint(k.dots([(s * 13 + 2, -73)]), 8)
        k.paint(k.ell(s * 22, -64, 3, 2), 5)
    k.paint(k.tri((-3, -68), (3, -68), (0, -65)), 5)
    k.paint(k.dots([(0, -64), (-3, -62), (-2, -61), (-1, -61), (0, -62), (1, -61), (2, -61), (3, -62)]), 0)
    k.draw(put)
    for s in (-1, 1):                                      # whiskers, over the outline
        for a, b in ((-63, -67), (-60, -58)):
            for (x, y) in k.line(s * 17, a, s * 34, b):
                put(x, y, 15)


def chick(put, x0, y0, z):
    """round and yellow, facing left, singing"""
    c = Figure(x0, y0, z)
    for x in (-7, 7):
        c.paint(c.line(x, -5, x, -1, 2), 6)
        c.paint(c.line(x - 4, 0, x + 5, 0), 6)
    c.paint(c.ell(2, -28, 27, 24), c.shaded(7, 6, 8, 2, -28, 27, 24))
    for a, b, d in (((-12, -78), (-15, -90), (-8, -79)), ((-9, -79), (-7, -92), (-4, -79)), ((-5, -78), (0, -88), (-1, -77))):
        c.paint(c.tri(a, b, d), 7)
    c.paint(c.ell(-8, -62, 19, 18), c.shaded(7, 6, 8, -8, -62, 21, 26))
    c.paint(c.ell(12, -28, 12, 9), 6)                      # the wing
    c.paint(c.ell(11, -30, 10, 7), c.shaded(7, 6, 8, 11, -30, 10, 7))
    c.paint(c.ell(-17, -65, 3, 4), 0)
    c.paint(c.dots([(-18, -67), (-19, -67), (-18, -68)]), 8)
    c.paint(c.ell(-10, -54, 3, 2), 5)
    c.paint(c.tri((-26, -60), (-37, -57), (-26, -55)), 6)  # the beak, open
    c.paint(c.tri((-26, -55), (-34, -54), (-26, -53)), 4)
    c.paint(c.tri((-26, -53), (-33, -51), (-26, -49)), 6)
    c.draw(put)


def note(put, x, y, colour, pair=False):
    n = Figure(x, y, 1)
    heads = [0, 11] if pair else [0]
    for hx in heads:
        n.paint(n.ell(hx, 0, 3.6, 2.7), colour)
        n.paint(n.line(hx + 3, -15, hx + 3, 0), colour)
    if pair:
        n.paint({(x + xx, y + yy) for xx in range(3, 15) for yy in range(-16, -13)}, colour)
    else:
        n.paint(n.line(4, -15, 7, -11, 2), colour)
    n.draw(put)


def main():
    a = sys.argv[1:]
    version = ""
    if "--version" in a:
        i = a.index("--version")
        version = a[i + 1]
        del a[i:i + 2]
    out_h, png = a[0], (a[1] if len(a) > 1 else None)
    img = [[1] * W for _ in range(H)]
    put = lambda x, y, c: 0 <= x < W and 0 <= y < H and img[y].__setitem__(x, c)
    rnd = random.Random(1987)
    HORIZON = 150

    # sky: bands, dithered where they meet
    for y in range(HORIZON):
        t = y / HORIZON
        for x in range(W):
            band = 1 if t < 0.35 else 2 if t < 0.7 else 3
            edge = (t * 3) % 1
            if edge > 0.8 and (x + y) % 2 and band < 3:
                band += 1
            img[y][x] = band
    for _ in range(140):                                   # stars
        x, y = rnd.randrange(W), rnd.randrange(HORIZON - 30)
        put(x, y, rnd.choice([12, 12, 15, 8]))
    for _ in range(8):                                     # a few big ones
        x, y = rnd.randrange(8, W - 8), rnd.randrange(4, 90)
        for d in (-1, 1):
            put(x + d, y, 12)
            put(x, y + d, 12)
        put(x, y, 8)

    # the sun: yellow to magenta, cut by stripes towards the horizon
    cx, cy, r = 120, 146, 48
    for y in range(cy - r, HORIZON):
        for x in range(cx - r, cx + r + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
                continue
            t = (y - (cy - r)) / (HORIZON - (cy - r))
            c = 7 if t < 0.35 else 6 if t < 0.6 else 5 if t < 0.82 else 4
            if 0.25 < t < 0.4 and (x + y) % 2:
                c = 6
            if 0.55 < t < 0.62 and (x + y) % 2:
                c = 5
            gap = y > cy - 26 and (y - (cy - 26)) % 8 < 1 + (y - (cy - 26)) // 8
            if not gap:
                img[y][x] = c

    # the floor: a perspective grid
    for y in range(HORIZON, H):
        for x in range(W):
            img[y][x] = 13
    for x in range(W):
        img[HORIZON][x] = 5
        img[HORIZON + 1][x] = 4
    z = 1.0
    while True:                                            # rows: closer together towards the horizon
        y = HORIZON + int(6 / z * 6)
        if y >= H:
            break
        for x in range(W):
            img[y][x] = 4 if y < HORIZON + 30 else 5
        z *= 0.72
    for k in range(-14, 15):                               # rays from the vanishing point
        for y in range(HORIZON + 2, H):
            x = int(120 + k * 9 * (y - HORIZON) / 18)
            put(x, y, 4 if y < HORIZON + 40 else 5)

    # the logo: chrome, 5x cells, slanted, with an outline and a shadow
    S, GAP = 5, 7
    word = "XMOD"
    lw = len(word) * 8 * S + (len(word) - 1) * GAP
    x0, y0 = (W - lw) // 2 + 6, 14
    cells = set()
    for i, ch in enumerate(word):
        for gy, row in enumerate(GLYPHS[ch]):
            for gx, bit in enumerate(row):
                if bit != "1":
                    continue
                for py in range(S):
                    for px in range(S):
                        y = y0 + gy * S + py
                        x = x0 + i * (8 * S + GAP) + gx * S + px - (y - y0) // 4   # the slant
                        cells.add((x, y))
    for (x, y) in cells:                                   # shadow
        put(x + 4, y + 5, 14)
    for (x, y) in cells:                                   # outline
        for dx, dy in ((-2, 0), (2, 0), (0, -2), (0, 2), (-1, -1), (1, 1), (-1, 1), (1, -1), (-1, 0), (1, 0), (0, -1), (0, 1)):
            if (x + dx, y + dy) not in cells:
                put(x + dx, y + dy, 0)
    top, bot = y0, y0 + 10 * S
    for (x, y) in cells:                                   # chrome: sky above the horizon line, ground below
        t = (y - top) / (bot - top)
        if t < 0.18:
            c = 8
        elif t < 0.32:
            c = 15 if (x + y) % 2 else 8
        elif t < 0.48:
            c = 9
        elif t < 0.52:
            c = 8                                          # the chrome's horizon
        elif t < 0.62:
            c = 10
        elif t < 0.78:
            c = 4 if (x + y) % 2 else 10
        else:
            c = 5
        img[y][x] = c
    for (x, y) in cells:                                   # a highlight on the left edges
        if (x - 1, y) not in cells and (y - top) / (bot - top) < 0.45:
            img[y][x] = 8

    # the words
    root = Path(__file__).resolve().parents[1] / "third_party" / "fonts"
    f12w, f12h, f12 = load_bdf(root / "spleen-6x12.bdf")
    f8w, f8h, f8 = load_bdf(root / "spleen-5x8.bdf")

    def text(s, x, y, c, font=(f12, f12w, f12h), shadow=14):
        g, w, h = font
        for i, ch in enumerate(s):
            rows = g.get(ord(ch), [0] * h)
            for ry, bits in enumerate(rows):
                for rx in range(w):
                    if bits & (0x80 >> rx):
                        if shadow is not None:
                            put(x + i * w + rx + 1, y + ry + 1, shadow)
                        put(x + i * w + rx, y + ry, c)

    def centred(s, y, c, font=(f12, f12w, f12h)):
        text(s, (W - len(s) * font[1]) // 2, y, c, font)

    centred("TRACKER  FOR  THE  FM-1", 72, 15)
    centred("FASTTRACKER 2 + PROTRACKER", 86, 9, (f8, f8w, f8h))
    if version:
        text(version, (W - len(version) * f8w) // 2, H - 10, 12, (f8, f8w, f8h))

    # the band on the grid: a kitten with headphones, a chick singing, the notes between them
    Z = 0.8
    for x, y, rx in ((40, 232, 34), (200, 233, 24)):
        for (px, py) in ell(x, y, rx, 2.5):
            put(px, py, 14)
    kitten(put, 42, 230, Z)
    chick(put, 198, 232, Z)
    note(put, 92, 210, 9)
    note(put, 116, 194, 5, pair=True)
    note(put, 104, 176, 7)

    # out: 4-bit pixels, the palette as RGB565
    rgb565 = [((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for r, g, b in PAL]
    packed = bytearray()
    for y in range(H):
        for x in range(0, W, 2):
            packed.append(img[y][x] << 4 | img[y][x + 1])
    lines = ["/* Generated by tools/gen_splash.py. Do not edit. */", "#pragma once", "#include <stdint.h>", "",
             "static const uint16_t SPLASH_PAL[16] = {" + ", ".join(f"0x{v:04X}" for v in rgb565) + "};",
             f"static const uint8_t SPLASH[{len(packed)}] = {{"]
    for i in range(0, len(packed), 24):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in packed[i:i + 24]) + ",")
    lines.append("};")
    Path(out_h).parent.mkdir(parents=True, exist_ok=True)
    Path(out_h).write_text("\n".join(lines) + "\n")
    if png:
        raw = b"".join(b"\x00" + bytes(v for x in range(W) for v in PAL[img[y][x]]) for y in range(H))
        chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
        Path(png).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
                              chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
