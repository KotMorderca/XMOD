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
#define FELUCCA_VERSION "XMOD 1.0.0"     /* (build.sh --release passes the tag's) */
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
#include "fm1_flash.h"
#include "storage_hw.c"
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
