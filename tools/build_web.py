#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The web song manager and the simulator, one file each: build/web/xmod-songs.html, xmod-sim.html.

  python3 tools/build_web.py           (WSL: needs ~/wasi-sdk-24.0-x86_64-linux for the converter)

web/manager.html with the scripts inlined (web/*.mjs, Felucca's web/fm1pkg.js and fm1ota.js), the
converter (host/modconv.c + host/modwrite.c -> WebAssembly) and the firmware package
(build/fw/xmod.fwsc, if built) in base64. The simulator: web/sim.html with the panel
(tools/gen_panel.py --sim) and host/websim.c + the engine + songs/ as WebAssembly.
"""
import base64
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web"
OUT = ROOT / "build" / "web"
WASI = Path(os.environ.get("WASI_SDK", Path.home() / "wasi-sdk-24.0-x86_64-linux"))
FEL_WEB = ROOT / "third_party" / "Felucca" / "web"
MODULES = [WEB / "fm1serial.mjs", WEB / "zip.mjs", WEB / "convert.mjs", FEL_WEB / "fm1pkg.js", FEL_WEB / "fm1ota.js"]


def build_wasm():
    out = OUT / "convert.wasm"
    src = [ROOT / "host" / f for f in ("wasm_api.c", "modconv.c", "modwrite.c")] + [ROOT / "engine" / "ft2_tables.c"]
    subprocess.run([WASI / "bin" / "clang", "--target=wasm32-wasip1", f"--sysroot={WASI / 'share' / 'wasi-sysroot'}",
                    "-O2", "-mexec-model=reactor", "-Wall", "-Wno-unused-parameter", "-Wl,-z,stack-size=1048576",
                    "-Wl,--strip-all", "-o", out, *src], check=True)
    return out.read_bytes()


def fw_version():
    """as tools/build_fw.py: XMOD_VERSION (a release), else the one in firmware/xmod.c"""
    if os.environ.get("XMOD_VERSION"):
        return "XMOD " + os.environ["XMOD_VERSION"]
    m = re.search(r'#define FELUCCA_VERSION "([^"]+)"', (ROOT / "firmware" / "xmod.c").read_text())
    return m.group(1) if m else "XMOD"


def build_sim():
    """host/websim.c (the pages, the player, the song slots) with the songs/ files, to WebAssembly"""
    gen = OUT / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    fonts = ROOT / "third_party" / "fonts"
    subprocess.run(["python3", ROOT / "tools" / "gen_font.py", gen / "font.h", f"font6x12={fonts / 'spleen-6x12.bdf'}",
                    f"font5x8={fonts / 'spleen-5x8.bdf'}"], check=True)
    songs = sorted((p for p in (ROOT / "songs").iterdir() if p.suffix.lower() in (".xm", ".mod")),
                   key=lambda p: (p.stem != "xmod-theme", p.name))        # (the theme: the first song)
    subprocess.run(["python3", ROOT / "tools" / "gen_splash.py", gen / "splash.h", "--version", fw_version()], check=True)
    h = []
    for i, p in enumerate(songs):
        h.append(f"static const uint8_t SONG{i}[] = {{{','.join(str(b) for b in p.read_bytes())}}};")
    h.append("static const struct { const char *name; const uint8_t *data; uint32_t size; } EMBEDDED[] = {")
    h += [f'    {{"{p.stem}", SONG{i}, {p.stat().st_size}u}},' for i, p in enumerate(songs)]
    h.append("};")
    (gen / "websim_songs.h").write_text("\n".join(h) + "\n")
    out = OUT / "sim.wasm"
    src = [ROOT / "host" / f for f in ("websim.c", "modconv.c")] + \
          [ROOT / "engine" / f for f in ("player.c", "song.c", "ft2_tables.c")]
    subprocess.run([WASI / "bin" / "clang", "--target=wasm32-wasip1", f"--sysroot={WASI / 'share' / 'wasi-sysroot'}",
                    "-O2", "-mexec-model=reactor", "-Wall", "-Wno-unused-parameter", "-Wno-unused-function",
                    f"-I{ROOT / 'engine'}", f"-I{gen}", "-Wl,-z,stack-size=1048576", "-Wl,--strip-all", "-o", out, *src],
                   check=True)
    return out.read_bytes()


def build_sim_page():
    panel = OUT / "panel-sim.svg"
    subprocess.run(["python3", ROOT / "tools" / "gen_panel.py", panel, "--sim"], check=True)
    wasm = build_sim()
    page = (WEB / "sim.html").read_text(encoding="utf-8")
    page = page.replace("<!--@PANEL@-->", panel.read_text(encoding="utf-8"))
    page = page.replace('"/*@SIMWASM@*/"', '"' + base64.b64encode(wasm).decode() + '"')
    out = OUT / "xmod-sim.html"
    out.write_text(page, encoding="utf-8")
    print(f"{out}: {out.stat().st_size} B (simulator {len(wasm)} B)")


def inline_module(path):
    """a module in its own scope; its exports become names of the page's script"""
    code = path.read_text(encoding="utf-8")
    code = re.sub(r"^import .*?;\s*$", "", code, flags=re.M)
    names = re.findall(r"^export\s+(?:async\s+)?(?:function|class|const|let)\s+(\w+)", code, flags=re.M)
    code = re.sub(r"^export\s+", "", code, flags=re.M)
    var = "_m_" + re.sub(r"\W", "_", path.stem)
    return f"// ---- {path.relative_to(ROOT)}\nconst {var} = (() => {{\n{code}\nreturn {{ {', '.join(names)} }};\n}})();\n" \
           f"const {{ {', '.join(names)} }} = {var};\n"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    page = (WEB / "manager.html").read_text(encoding="utf-8")
    wasm = build_wasm()
    fw = ROOT / "build" / "fw" / "xmod.fwsc"
    fw_name = fw_version()
    page = page.replace("/*@MODULES@*/", "\n".join(inline_module(p) for p in MODULES))
    page = page.replace('"/*@WASM@*/"', '"' + base64.b64encode(wasm).decode() + '"')
    page = page.replace('"/*@FW@*/"', '"' + (base64.b64encode(fw.read_bytes()).decode() if fw.exists() else "") + '"')
    page = page.replace('"/*@FWNAME@*/"', '"' + fw_name + '"')
    out = OUT / "xmod-songs.html"
    out.write_text(page, encoding="utf-8")
    print(f"{out}: {out.stat().st_size} B (converter {len(wasm)} B, firmware {'yes' if fw.exists() else 'no'})")
    build_sim_page()


if __name__ == "__main__":
    main()
