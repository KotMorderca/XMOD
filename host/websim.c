/* SPDX-License-Identifier: GPL-3.0-only */
/* XMOD in the browser: the FM-1's pages (firmware/pages.c), the player and the flash song slots,
 * built to WebAssembly (tools/build_web.py). web/sim.html draws the panel, sends the input and
 * plays the audio. JS calls:
 *
 *   sim_init(rate, autoplay)  after sim_flash_ptr() was filled (the flash kept from the last visit):
 *                             the splash for 3 s, then the pages (autoplay: and PLAY)
 *   sim_input(pressed, released, held, notes, notes_held), sim_enc(i, steps), then sim_frame()
 *   sim_fb()                  240 x 240 RGB565 pixels;  sim_leds(): 41 bytes (buttons 0..13, keys 14..40)
 *   sim_audio(n)              n stereo frames, float, interleaved
 *   sim_flash_ptr(), sim_flash_writes()   the song slots' areas (0x48000..0xE0000, firmware/layout.h) and a write counter
 *   sim_add_song(p, n, name)  an .xm / .mod as a built-in song, loaded; 0 ok (else sim_message())
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "player.h"
#include "song.h"
#include "fmt.h"
#include "modconv.h"

#define EXPORT(name) __attribute__((export_name(name)))

static volatile uint32_t fm1_ms;

/* ------------------------------------------------------------- the panel --- */
static uint16_t fb[240 * 240];
static uint8_t leds[41];

static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
}

static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++) {
            uint16_t v = px[j * w + i];
            fb[(y + j) * 240u + x + i] = (uint16_t)((v >> 8) | (v << 8));   /* (the strips are byte-swapped) */
        }
}

/* ------------------------------------------------------------- the songs --- */
#define MAX_SONGS 32
static struct { const char *name; const uint8_t *data; uint32_t size; } SONGS[MAX_SONGS];
static int NSONGS;
#include "websim_songs.h"            /* (build_web.py: the songs/ files, raw) */

#include "font.h"
#include "../firmware/gfx.c"

static void lcd_sync(void) {}
#include "../firmware/splash.c"

static void sys_audio_hold(void) {}
static void sys_audio_go(void) {}
static uint32_t sys_lock(void) { return 0; }
static void sys_unlock(uint32_t f) { (void)f; }
static void sys_led(uint32_t id, int on) { if (id < 41u) leds[id] = (uint8_t)(on != 0); }

/* the flash: a NOR in RAM (erase sets 0xFF, programming only clears bits), the slots' area kept
 * by the page in the browser's storage */
#include "../firmware/layout.h"
#define FL_LO XMOD_SONGS_LO
#define FL_HI XMOD_SONGS_HI
#define IN_AREAS(off, n) ((off) >= XMOD_SONGS_LO && (off) + (n) <= XMOD_SONGS_HI)
static uint8_t flash[FL_HI];
static uint32_t flash_writes;
static int stp_ok(void) { return 1; }
static int stp_read(uint32_t off, void *d, uint32_t n) { memcpy(d, flash + off, n); return 0; }
static int stp_erase(uint32_t off)
{
    if (off & 0xFFFu || !IN_AREAS(off, 0x1000u))
        return -8;
    memset(flash + off, 0xFF, 0x1000);
    flash_writes++;
    return 0;
}
static int stp_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (!IN_AREAS(off, n) || n > 256u || (off & 0xFFu) + n > 256u)
        return -8;
    for (i = 0; i < n; i++)
        flash[off + i] &= ((const uint8_t *)src)[i];
    flash_writes++;
    return 0;
}
static const uint8_t *stp_ptr(uint32_t off) { return flash + off; }
static void stp_idle(void) {}

#include "../firmware/pages.c"

static void glo_stats(gline *cpu, gline *usb, int reset)   /* the mixer's FM-1 lines */
{
    if (reset)
        return;
    gl_clear(cpu, ATTR(C_DIM, C_BG));
    gl_puts(cpu, 1, "WEB SIMULATOR", ATTR(C_DIM, C_BG));
    gl_clear(usb, ATTR(C_DIM, C_BG));
}

/* ------------------------------------------------------------- the exports --- */
static ui_in in;
static float abuf[2 * 4096];
static int32_t mix[2 * 4096];
static char msg[128];
static uint32_t splash_frames, autoplay;
static int dropped = -1;                     /* a song added during the splash: loaded after it */

EXPORT("buf_alloc") void *buf_alloc(uint32_t n) { return malloc(n ? n : 1); }
EXPORT("buf_free") void buf_free(void *p) { free(p); }
EXPORT("sim_fb") const uint16_t *sim_fb(void) { return fb; }
EXPORT("sim_leds") const uint8_t *sim_leds(void) { return leds; }
EXPORT("sim_flash_ptr") uint8_t *sim_flash_ptr(void) { return flash + FL_LO; }
EXPORT("sim_flash_size") uint32_t sim_flash_size(void) { return FL_HI - FL_LO; }
EXPORT("sim_flash_writes") uint32_t sim_flash_writes(void) { return flash_writes; }
EXPORT("sim_message") const char *sim_message(void) { return msg; }

EXPORT("sim_flash_clear") void sim_flash_clear(void) { memset(flash, 0xFF, sizeof flash); }

EXPORT("sim_init") void sim_init(uint32_t rate, uint32_t play)
{
    uint32_t i;
    player_init(rate, 1);
    gfx_init();
    for (i = 0; i < sizeof EMBEDDED / sizeof EMBEDDED[0] && NSONGS < MAX_SONGS; i++) {
        uint8_t *img;
        size_t n;
        char err[128];
        if (modconv(EMBEDDED[i].data, EMBEDDED[i].size, &img, &n, err, sizeof err))
            continue;
        SONGS[NSONGS].name = EMBEDDED[i].name;
        SONGS[NSONGS].data = img;
        SONGS[NSONGS].size = (uint32_t)n;
        NSONGS++;
    }
    splash_show();
    splash_frames = 150u;                    /* 3 s of 20 ms frames */
    autoplay = play;
}

EXPORT("sim_input") void sim_input(uint32_t pressed, uint32_t released, uint32_t held, uint32_t notes,
                                   uint32_t notes_held)
{
    in.pressed |= pressed;
    in.released |= released;
    in.held = held;
    in.notes |= notes;
    in.notes_held = notes_held;
}

EXPORT("sim_enc") void sim_enc(uint32_t i, int32_t steps)
{
    if (i < NENC)
        in.enc[i] += steps;
}

EXPORT("sim_frame") void sim_frame(void)
{
    uint32_t k;
    fm1_ms += 20u;
    if (splash_frames) {                     /* the splash: the input waits */
        memset(&in, 0, sizeof in);
        if (--splash_frames)
            return;
        pages_init();
        if (dropped >= 0)
            ui_load(dropped);
        else if (autoplay)
            in.pressed = 1u << BTN_PLAY;
    }
    pages_frame(&in);
    in.pressed = in.released = in.notes = 0;
    for (k = 0; k < NENC; k++)
        in.enc[k] = 0;
}

EXPORT("sim_audio") const float *sim_audio(uint32_t n)
{
    uint32_t i;
    if (n > 4096u)
        n = 4096u;
    player_render(mix, n);
    for (i = 0; i < 2u * n; i++)
        abuf[i] = (float)mix[i] * (1.0f / 32768.0f);
    return abuf;
}

EXPORT("sim_add_song") int sim_add_song(const uint8_t *file, uint32_t size, const char *name)
{
    uint8_t *img;
    size_t n;
    char *nm;
    msg[0] = 0;
    if (NSONGS >= MAX_SONGS) {
        strcpy(msg, "too many songs");
        return -1;
    }
    if (modconv(file, size, &img, &n, msg, sizeof msg))
        return -1;
    nm = malloc(strlen(name) + 1);
    strcpy(nm, name);
    SONGS[NSONGS].name = nm;
    SONGS[NSONGS].data = img;
    SONGS[NSONGS].size = (uint32_t)n;
    if (splash_frames)
        dropped = NSONGS++;
    else
        ui_load(NSONGS++);
    return 0;
}
