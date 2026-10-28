# Example songs

Built into the firmware (every .xm / .mod in this folder is). Made from nothing by
`tools/make_demo_songs.py`: the samples are synthesized there (single-cycle square, pulse, saw and
triangle waves; drums from a sine sweep and noise) and the notes are written there. No one else's
music or samples.

| Song | Format | What it shows |
| --- | --- | --- |
| `xmod-theme.xm` | XM, 8 channels | instruments with envelopes and auto-vibrato, a pad, an echo channel |
| `amiga-days.mod` | MOD, 4 channels | ProTracker mode: Amiga panning, arpeggio (0xy), vibrato (4xy), Cxx |

To the extent possible under law, the authors have waived all copyright and related rights to these
songs: [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). Use them for anything.

Your own songs: keep them in `songs-local/` (not in git) and build with
`python3 tools/build_fw.py --songs songs-local`, or send them to the FM-1 with the song manager.
