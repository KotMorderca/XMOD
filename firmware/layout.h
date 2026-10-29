/* SPDX-License-Identifier: GPL-3.0-only */
/* XMOD's flash layout. The FM-1's app area is [0x4000, 0x93000) (the package's flash.bin, the PRCT end):
 * XMOD's app takes only its start, [0x4000, XMOD_APP_HI). The songs take one piece of flash from there
 * to the end of Felucca's data store: the app area's tail, the 16 KB above it (empty: no firmware
 * writes there; update records go to 0xE4F00, the stock firmware's to 0xE8F00) and the store.
 * XMOD's update loader (Felucca's, patched by tools/build_fw.py) never writes [XMOD_APP_HI, 0x93000),
 * refuses a package that has data there, and does not look for update records in the songs. Another
 * firmware's loader (Felucca's, the stock one) rewrites the whole app area: the songs kept there are
 * lost (the web manager's backup first). [0xE0000, ...) is the update staging, then the stock
 * firmware's data (0xE8000..0xFC000: never written).
 * Read by firmware/xmod.c, firmware/store.c, the host tests, tools/build_fw.py (the app slot, the loader patch). */
#ifndef XMOD_LAYOUT_H
#define XMOD_LAYOUT_H

#define XMOD_APP_HI 0x48000u           /* the end of XMOD's app (head, app.bin, cfg_tool, cfg) */
#define XMOD_APP_SLOT 0x43000u         /* app.bin's slot in the package: 0x4120 + slot + ~1.4 KB < XMOD_APP_HI */
#define XMOD_APP_AREA_HI 0x93000u      /* the FM-1's app area ends here (a package's flash.bin) */
#define XMOD_SONGS_LO XMOD_APP_HI      /* the songs */
#define XMOD_SONGS_HI 0xE0000u

#endif
