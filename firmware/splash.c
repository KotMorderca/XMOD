/* SPDX-License-Identifier: GPL-3.0-only */
/* The splash screen at power-on (tools/gen_splash.py draws it: 240 x 240, 16 colours, 4 bits a
 * pixel in flash), sent to the panel 12 rows at a time through gfx.c's strip buffers. */
#include "splash.h"

static void splash_show(void)
{
    uint16_t pal[16];
    uint32_t i, y;
    for (i = 0; i < 16u; i++)
        pal[i] = (uint16_t)((SPLASH_PAL[i] >> 8) | (SPLASH_PAL[i] << 8));   /* (the panel: high byte first) */
    for (y = 0; y < 240u; y += 12u) {
        uint16_t *buf = strip_next();
        const uint8_t *src = SPLASH + y * 120u;
        for (i = 0; i < 12u * 120u; i++) {
            buf[2u * i] = pal[src[i] >> 4];
            buf[2u * i + 1u] = pal[src[i] & 15u];
        }
        lcd_blit(0, y, 240, 12, buf);
    }

    lcd_sync();
}
