/* SPDX-License-Identifier: GPL-3.0-only */
/* XMOD firmware: one compilation unit, like Felucca's felucca.c.
 *
 * Felucca's hardware layer (third_party/Felucca/firmware/hal, header-only) and a few of its
 * source files are included unmodified: the LCD driver, the USB stack (MIDI for the updater,
 * CDC serial for our console), the flash driver and the M-UPGRADE update entry. So the stock
 * installers (Felucca's web installer, tools/fm1_install.py, the official updater) can always put
 * another firmware on the FM-1. Everything after "XMOD" below is ours. */

/* ------------------------------------------------------------ build options --- */
#define FELUCCA_FLASH 1          /* flash driver (needed by the update entry) */
#define FELUCCA_OTA 1            /* M-UPGRADE update entry */
#define FELUCCA_OTA_DRYRUN 0
#define FELUCCA_CDC 1            /* USB serial console */
#define FELUCCA_CDC_DEFAULT 1
#define FELUCCA_UAC 1            /* USB audio: the FM-1 is also an audio input on the computer (audio.c) */
#define FELUCCA_UART 0           /* no TRS MIDI yet */
#define FELUCCA_CPU2 0
#define FM1_CPU1_USED 0
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_666"     /* (build_fw.py passes it too) */
#endif
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "XMOD 1.2.0"     /* (build.sh --release passes the tag's) */
#endif
#define XMOD_FIRMWARE 1

/* ------------------------------------------------------------------- HAL --- */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"

/* --------------------------------------------- Felucca's base, display, USB --- */
#include "libc.c"
#include "lcd.c"
#include "shim.h"                /* what Felucca's usb.c / ota.c expect from its core.h */
#include "usb.c"
/* XMOD keeps songs from the app area's tail on (layout.h): the flash driver may write there, and the
 * storage calls (storage_hw.c: st_erase, st_prog) take it as a storage area */
#include "layout.h"
#define FL_RANGE_OK(off, n) (FL_IN(off, n, XMOD_SONGS_LO, XMOD_SONGS_HI) || FL_STORE_OK(off, n) || \
                             FL_IN(off, n, FL_OTA_LO, FL_OTA_HI))
#include "fm1_flash.h"
#undef FL_STORE_OK
#define FL_STORE_OK(off, n) (FL_IN(off, n, XMOD_SONGS_LO, XMOD_SONGS_HI) || FL_IN(off, n, FL_DATA_LO, FL_DATA_HI) || \
                             FL_IN(off, n, FL_GLOB_LO, FL_GLOB_HI) || FL_IN(off, n, FL_AUTO_LO, FL_VBANK_HI))
#include "storage_hw.c"

/* fl_plain_window_init with the window from the songs on: they are plain (the app below them is
 * SFC-encrypted and stays decrypted) */
static void xmod_plain_window_init(void)
{
    SFCENC_CON &= (uint8_t)~2u;
    SFCENC_UNENC_L = FL_XIP(XMOD_SONGS_LO);
    SFCENC_UNENC_H = 0x07FFFFFFu;
    SFCENC_CON |= 2u;
    __asm__ volatile("cli" ::: "memory");
    fl_inval(XMOD_SONGS_LO, 0x100000u - XMOD_SONGS_LO);   /* drop any decrypted lines */
    __asm__ volatile("csync\n\tsti" ::: "memory");
}
#include "ota.c"

/* ------------------------------------------------------------- XMOD --- */
void *memmove(void *d, const void *s, unsigned n)   /* (libc.c has none) */
{
    uint8_t *a = d;
    const uint8_t *b = s;
    if (a < b)
        while (n--)
            *a++ = *b++;
    else
        while (n--)
            a[n] = b[n];
    return d;
}

#define SONG_SECTION __attribute__((section(".pool")))   /* the arena and the edit buffer: the big RAM */
#include "../engine/song.c"
#include "../engine/player.c"
#include "../engine/ft2_tables.c"
#include "font.h"
#include "songs.h"
#include "audio.c"
#include "gfx.c"
#include "splash.c"
#include "ui.c"
#include "console.c"
#include "main.c"
