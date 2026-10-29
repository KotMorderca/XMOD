# Third-party material

XMOD is GPL-3.0-only (LICENSE): it links Felucca's GPL-3.0-only hardware layer.

| What | Licence | Where |
| --- | --- | --- |
| Felucca by Hugelton Instruments (Leo Kuroshita): the hardware layer (`firmware/hal`), `libc.c`, `lcd.c`, `usb.c`, `storage_hw.c`, `ota.c`, the package tools, used unmodified; the update loader, patched at build time by `tools/build_fw.py` (it keeps the end of the app area, where XMOD keeps songs; `host/ldrtest.c` reuses its `tests/ldr_test.c`). `firmware/main.c`, `firmware/console.c`, `firmware/audio.c` and `tools/build_fw.py` follow its main.c, console.c, audio.c and build.py. The song manager page inlines its `web/fm1ota.js` and `web/fm1pkg.js` (the firmware installer) | GPL-3.0-only | not in this repository: `tools/get_felucca.sh` fetches github.com/hugelton/Felucca at commit 440e62ce8a33d9bb285ab6bbca9a0fd50e459648 into `third_party/Felucca` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` in every package | Apache-2.0 | read from the SDK checkout at build time |
| ft2-clone by Olav Sorensen: the replay logic ported in `engine/player.c`, the loaders followed in `host/modconv.c`, the XM writer in `host/modwrite.c`, the tables in `engine/ft2_tables.c` | BSD-3-Clause (below) | github.com/8bitbubsy/ft2-clone |
| pt2-clone by Olav Sorensen: the ProTracker mode's Amiga model (panning, stereo separation, the A500 and LED filters' values) in `engine/player.c` | BSD-3-Clause (below) | github.com/8bitbubsy/pt2-clone |
| Spleen by Frederic Cambus: the 6x12 and 5x8 fonts (`build/fw/gen/font.h`, the splash screen) | BSD-2-Clause | `third_party/fonts` (LICENSE there) |
| wasi-sdk / wasi-libc: the C library in the song manager's converter (WebAssembly) | Apache-2.0 WITH LLVM-exception, MIT | github.com/WebAssembly/wasi-sdk |

The example songs in `songs/` are made by `tools/make_demo_songs.py` from nothing (synthesized
samples, notes written there): CC0 1.0.

## ft2-clone and pt2-clone

ft2-clone: Copyright (c) 2016-2026, Olav Sørensen. pt2-clone: Copyright (c) 2010-2026, Olav Sørensen.
All rights reserved. Both under this BSD 3-Clause License:

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions
   and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
   and the following disclaimer in the documentation and/or other materials provided with the
   distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
