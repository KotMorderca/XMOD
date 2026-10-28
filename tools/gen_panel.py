#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The FM-1's panel with XMOD's controls on it, as SVG: the README's picture and the web simulator's
panel (every control has an id: b-NAME buttons, k-NAME knobs, key-N note keys, the screen).

  tools/gen_panel.py OUT.svg [--sim]

The controls are where they are on the FM-1 (their positions follow Felucca's measured panel map),
the rows a little apart to make room for the labels. --sim: no screen picture (the simulator draws
the screen there) and no legend text under the keys.
"""
import base64
import sys
from pathlib import Path

W, H = 1750, 1130
SCREEN = (470, 120, 830, 480)                 # x0, y0, x1, y1

# knobs: name, centre, printed label, XMOD function, second line
KNOBS = [
    ("MASTER", (171, 170), "MASTER", "Volume", ""),
    ("SELECT", (346, 170), "SELECT", "Rows", "playing: position"),
    ("PRESETS", (165, 366), "PRESETS", "Instrument", "EDIT: edit step"),
    ("ALGORITHM", (340, 366), "ALGORITHM", "Field / channel", "EDIT: by channel"),
    ("K1", (969, 170), "KNOB 1", "Instrument", "of the cell"),
    ("K2", (1155, 170), "KNOB 2", "Volume", "volume column"),
    ("K3", (1341, 170), "KNOB 3", "Effect", "0 .. X"),
    ("K4", (1527, 170), "KNOB 4", "Parameter", "00 .. FF"),
]
# buttons: name, column, row, printed label, function, second line
BUTTONS = [
    ("FX", 0, 0, "FX", "FX help", "REC: insert"), ("SEL", 1, 0, "SEL", "Note off", "hold: block"),
    ("ENV", 2, 0, "ENV", "Instr.", "envelopes"), ("LFO", 3, 0, "LFO", "Sample", "loop"),
    ("EDIT", 4, 0, "EDIT", "Shift", "held"), ("GLO", 5, 0, "GLO", "Mixer", "hold: mute"),
    ("HOME", 0, 1, "HOME", "Pattern", "again: 8 ch"), ("SAVE", 1, 1, "SAVE", "Songs", "EDIT: undo"),
    ("ARP", 2, 1, "ARP", "Delete", "hold: copy"), ("SEQ", 3, 1, "SEQ", "Song", "again: list"),
    ("PLAY", 4, 1, "PLAY", "Play", "EDIT: loop"), ("REC", 5, 1, "REC", "Edit", "mode"),
]
BX0, BDX, BY = 962, 104, (330, 490)            # buttons: first column, pitch, the rows' tops
BS = 75
OCTS = [("OCTDN", (154, 528), "OCT-"), ("OCTUP", (284, 528), "OCT+")]
KEYS = (91, 680, 1652, 1000)                   # 27 keys, F3 .. G5: 16 white, 11 black
BLACK_AFTER = [0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14]   # white key indexes a black key follows (F G A, C D, ...)

C = dict(body="#1b1d27", panel="#232634", knob="#3a3f52", knob_hi="#565c76", btn="#2e3244", btn_hi="#3a4058",
         play="#2f6b45", rec="#7a2e34", text="#e8ebf5", dim="#8b91aa", accent="#ffd860", screen="#101424",
         white="#e9e9ef", black="#15161c", line="#3b4057")


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;")


def main():
    out, sim = sys.argv[1], "--sim" in sys.argv
    h = 1060 if sim else H                             # (the simulator: no notes under the keys)
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {h}" width="{W}" height="{h}" '
         f'font-family="Inter, Segoe UI, Helvetica, Arial, sans-serif">',
         f'<rect width="{W}" height="{h}" rx="34" fill="{C["body"]}"/>',
         f'<rect x="20" y="20" width="{W - 40}" height="{h - 40}" rx="26" fill="{C["panel"]}"/>']

    def text(x, y, t, size, fill, weight=400, anchor="middle"):
        if t:
            s.append(f'<text x="{x}" y="{y}" font-size="{size}" fill="{fill}" font-weight="{weight}" '
                     f'text-anchor="{anchor}">{esc(t)}</text>')

    # the screen
    x0, y0, x1, y1 = SCREEN
    s.append(f'<rect x="{x0 - 14}" y="{y0 - 14}" width="{x1 - x0 + 28}" height="{y1 - y0 + 28}" rx="14" fill="#0a0b10"/>')
    if sim:
        s.append(f'<foreignObject id="screen" x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0}">'
                 f'<canvas xmlns="http://www.w3.org/1999/xhtml" id="screen-canvas" width="240" height="240"></canvas>'
                 f'</foreignObject>')
    else:                                             # (embedded: an SVG shown as an image loads no files)
        shot = base64.b64encode((Path(__file__).resolve().parents[1] / "docs" / "screen-pattern.png").read_bytes()).decode()
        s.append(f'<image id="screen" x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0}" href="data:image/png;base64,{shot}" '
                 f'style="image-rendering:pixelated"/>')
    # knobs
    for name, (cx, cy), lbl, fn, alt in KNOBS:
        s.append(f'<g id="k-{name}" class="knob" data-name="{name}">'
                 f'<circle cx="{cx}" cy="{cy}" r="50" fill="{C["knob"]}" stroke="{C["knob_hi"]}" stroke-width="4"/>'
                 f'<g class="dial" transform="rotate(0 {cx} {cy})"><line x1="{cx}" y1="{cy - 16}" x2="{cx}" y2="{cy - 42}" '
                 f'stroke="{C["text"]}" stroke-width="6" stroke-linecap="round"/></g>')
        text(cx, cy + 80, lbl, 18, C["dim"], 600)
        text(cx, cy + 106, fn, 24, C["accent"], 700)
        text(cx, cy + 131, alt, 19, C["dim"])
        s.append('</g>')
    # buttons
    for name, col, row, lbl, fn, alt in BUTTONS:
        x, y = BX0 + col * BDX, BY[row]
        fill = C["play"] if name == "PLAY" else C["rec"] if name == "REC" else C["btn"]
        s.append(f'<g id="b-{name}" class="btn" data-name="{name}"><rect x="{x}" y="{y}" width="{BS}" height="{BS}" rx="12" '
                 f'fill="{fill}" stroke="{C["line"]}" stroke-width="3"/>'
                 f'<rect class="led" x="{x + 26}" y="{y + 10}" width="23" height="6" rx="3" fill="#000" opacity="0.35"/>')
        text(x + BS / 2, y + 50, lbl, 19, C["text"], 700)
        text(x + BS / 2, y + BS + 28, fn, 21, C["accent"], 700)
        text(x + BS / 2, y + BS + 51, alt, 17, C["dim"])
        s.append('</g>')
    for name, (x, y), lbl in OCTS:
        s.append(f'<g id="b-{name}" class="btn" data-name="{name}"><rect x="{x}" y="{y}" width="87" height="48" rx="12" '
                 f'fill="{C["btn"]}" stroke="{C["line"]}" stroke-width="3"/>'
                 f'<rect class="led" x="{x + 32}" y="{y + 7}" width="23" height="5" rx="2.5" fill="#000" opacity="0.35"/>')
        text(x + 43, y + 34, lbl, 19, C["text"], 700)
        s.append('</g>')
    text(262, 606, "Octave", 24, C["accent"], 700)
    text(262, 628, "both held 5 s: update mode", 17, C["dim"])
    # the keys
    kx0, ky0, kx1, ky1 = KEYS
    ww = (kx1 - kx0) / 16
    order = []                                          # key number (0 = F3) of each white / black key
    n = 0
    for w in range(16):
        order.append(("w", w, n))
        n += 1
        if w in BLACK_AFTER:
            order.append(("b", w, n))
            n += 1
    for kind, w, num in order:
        if kind == "w":
            s.append(f'<rect id="key-{num}" class="key white" data-key="{num}" x="{kx0 + w * ww + 2:.1f}" y="{ky0}" '
                     f'width="{ww - 4:.1f}" height="{ky1 - ky0}" rx="8" fill="{C["white"]}"/>')
    for kind, w, num in order:
        if kind == "b":
            s.append(f'<rect id="key-{num}" class="key black" data-key="{num}" x="{kx0 + (w + 1) * ww - ww * 0.3:.1f}" '
                     f'y="{ky0}" width="{ww * 0.6:.1f}" height="{(ky1 - ky0) * 0.58:.1f}" rx="6" fill="{C["black"]}"/>')
    if not sim:
        text(W / 2, 662, "27 keys F3 .. G5: play the cursor's channel; in edit mode (REC) they enter notes. "
             "With GLO held the white keys mute channels 1 .. 16.", 23, C["text"], 600)
        text(W / 2, 1060, "Every page shows its controls in its footer. "
             "HOME, SEQ, ENV, GLO pressed again switch the page's view.", 22, C["dim"])
    s.append("</svg>")
    Path(out).write_text("\n".join(s) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
