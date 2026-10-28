/* SPDX-License-Identifier: GPL-3.0-only */
/* The tracker's pages (firmware/pages.c) on the PC: scripted input, PNG screenshots.
 *
 *   uisim [--flash FILE] SONG.xm [SONG.mod ...] -- STEP...
 *
 * --flash: the song slots' flash, kept in FILE (read at the start, written at the end).
 *
 * Steps (each runs one 20 ms frame, the player playing underneath):
 *   wait:MS         frames with no input for MS ms
 *   press:BTN       press and release a button (HOME SEQ SAVE FX GLO ENV LFO EDIT SEL ARP PLAY REC OCT- OCT+)
 *   hold:BTN        press a button and keep it down;  release:BTN  let it go
 *   turn:ENC:N      turn an encoder N steps (SELECT ALGO PRESET K1 K2 K3 K4; N may be negative)
 *   key:N           press note key N (0 = F3 .. 26 = G5)
 *   shot:FILE.png   save the screen (2x); shot1:FILE.png 1:1
 *   dump            print the screen's text
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "player.h"
#include "song.h"
#include "fmt.h"
#include "modconv.h"

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

#include "font.h"
#include "../firmware/gfx.c"

static void sys_audio_hold(void) {}
static void sys_audio_go(void) {}
static uint32_t sys_lock(void) { return 0; }
static void sys_unlock(uint32_t f) { (void)f; }
static void sys_led(uint32_t id, int on) { if (id < 41u) leds[id] = (uint8_t)(on != 0); }

/* a NOR flash: erase sets 0xFF, programming only clears bits */
static uint8_t flash[0x100000];
static uint32_t flash_erases, flash_progs;
static int stp_ok(void) { return 1; }
static int stp_read(uint32_t off, void *d, uint32_t n) { memcpy(d, flash + off, n); return 0; }
static int stp_erase(uint32_t off)
{
    if (off & 0xFFFu || off < 0x97000u || off >= 0xE0000u)
        return -8;
    memset(flash + off, 0xFF, 0x1000);
    flash_erases++;
    return 0;
}
static int stp_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (off < 0x97000u || off + n > 0xE0000u || n > 256u || (off & 0xFFu) + n > 256u)
        return -8;
    for (i = 0; i < n; i++)
        flash[off + i] &= ((const uint8_t *)src)[i];
    flash_progs++;
    return 0;
}
static const uint8_t *stp_ptr(uint32_t off) { return flash + off; }
static void stp_idle(void) {}

#include "../firmware/pages.c"

static void glo_stats(gline *cpu, gline *usb, int reset)   /* the mixer's FM-1 lines: none here */
{
    if (reset)
        return;
    gl_clear(cpu, ATTR(C_DIM, C_BG));
    gl_puts(cpu, 1, "PC SIMULATOR", ATTR(C_DIM, C_BG));
    gl_clear(usb, ATTR(C_DIM, C_BG));
}

/* ------------------------------------------------------------------- PNG --- */
static uint32_t crc_tab[256];

static uint32_t crc32(uint32_t c, const uint8_t *p, size_t n)
{
    size_t i;
    if (!crc_tab[1])
        for (i = 0; i < 256; i++) {
            uint32_t v = (uint32_t)i;
            int k;
            for (k = 0; k < 8; k++)
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_tab[i] = v;
        }
    c ^= 0xFFFFFFFFu;
    for (i = 0; i < n; i++)
        c = crc_tab[(c ^ p[i]) & 255u] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t b[8];
    uint32_t c;
    be32(b, n);
    memcpy(b + 4, type, 4);
    fwrite(b, 1, 8, f);
    if (n)
        fwrite(d, 1, n, f);
    c = crc32(crc32(0, (const uint8_t *)type, 4), d, n);
    be32(b, c);
    fwrite(b, 1, 4, f);
}

static int write_png(const char *path, int scale)
{
    uint32_t W = 240u * (uint32_t)scale, stride = 1u + W * 3u, raw_n = stride * W, x, y, i, a = 1, b = 0;
    uint8_t *raw = malloc(raw_n), *z, hdr[13];
    size_t zn = 0;
    FILE *f;
    for (y = 0; y < W; y++) {
        uint8_t *r = raw + y * stride;
        r[0] = 0;
        for (x = 0; x < W; x++) {
            uint16_t v = fb[(y / (uint32_t)scale) * 240u + x / (uint32_t)scale];
            r[1 + x * 3] = (uint8_t)(((v >> 11) & 31u) * 255u / 31u);
            r[2 + x * 3] = (uint8_t)(((v >> 5) & 63u) * 255u / 63u);
            r[3 + x * 3] = (uint8_t)((v & 31u) * 255u / 31u);
        }
    }
    z = malloc(raw_n + raw_n / 65535u * 5u + 64u);
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    for (i = 0; i < raw_n; i += 65535u) {           /* stored deflate blocks */
        uint32_t n = raw_n - i > 65535u ? 65535u : raw_n - i;
        z[zn++] = (uint8_t)(i + n >= raw_n);
        z[zn++] = (uint8_t)n;
        z[zn++] = (uint8_t)(n >> 8);
        z[zn++] = (uint8_t)~n;
        z[zn++] = (uint8_t)(~n >> 8);
        memcpy(z + zn, raw + i, n);
        zn += n;
    }
    for (i = 0; i < raw_n; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    be32(z + zn, b << 16 | a);
    zn += 4;
    f = fopen(path, "wb");
    if (!f) {
        free(raw);
        free(z);
        return -1;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    be32(hdr, W);
    be32(hdr + 4, W);
    hdr[8] = 8;
    hdr[9] = 2;
    hdr[10] = hdr[11] = hdr[12] = 0;
    chunk(f, "IHDR", hdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}

/* ------------------------------------------------------------- the script --- */
#define RATE 44100u
static uint32_t held;

static void frame(ui_in *in)
{
    static int32_t buf[2 * 882];
    player_render(buf, RATE / 50u);
    fm1_ms += 20u;
    pages_frame(in);
}

static void frame_idle(void)
{
    ui_in in;
    memset(&in, 0, sizeof in);
    in.held = held;
    frame(&in);
}

static int step(const char *s)
{
    ui_in in;
    memset(&in, 0, sizeof in);
    if (!strncmp(s, "wait:", 5)) {
        int ms = atoi(s + 5);
        for (; ms > 0; ms -= 20)
            frame_idle();
    } else if (!strncmp(s, "press:", 6) || !strncmp(s, "hold:", 5) || !strncmp(s, "release:", 8)) {
        const char *n = strchr(s, ':') + 1;
        int id = btn_id(n, (uint32_t)strlen(n));
        if (id < 0)
            return -1;
        if (s[0] != 'r') {
            held |= 1u << id;
            in.pressed = 1u << id;
            in.held = held;
            frame(&in);
            memset(&in, 0, sizeof in);
        }
        if (s[0] != 'h') {
            held &= ~(1u << id);
            in.released = 1u << id;
            in.held = held;
            frame(&in);
        }
    } else if (!strncmp(s, "turn:", 5)) {
        const char *n = s + 5, *c = strchr(n, ':');
        int e = c ? enc_id(n, (uint32_t)(c - n)) : -1;
        if (e < 0)
            return -1;
        in.enc[e] = atoi(c + 1);
        in.held = held;
        frame(&in);
    } else if (!strncmp(s, "key:", 4)) {
        int k = atoi(s + 4);
        if (k < 0 || k > 26)
            return -1;
        in.notes = in.notes_held = 1u << k;
        in.held = held;
        frame(&in);
    } else if (!strncmp(s, "shot1:", 6)) {   /* (1:1, as the panel) */
        if (write_png(s + 6, 1)) {
            fprintf(stderr, "%s: cannot write\n", s + 6);
            return -2;
        }
    } else if (!strncmp(s, "shot:", 5)) {
        if (write_png(s + 5, 2)) {
            fprintf(stderr, "%s: cannot write\n", s + 5);
            return -2;
        }
    } else if (!strncmp(s, "midi:", 5)) {    /* a USB-MIDI event packet, hex (0x00FA000F: Start) */
        ui_midi((uint32_t)strtoul(s + 5, NULL, 16));
        frame_idle();
    } else if (!strcmp(s, "dump")) {         /* the screen's text */
        uint32_t i, k;
        printf("page %u\n", ui.page);
        for (i = 0; i < nl; i++) {
            uint32_t end = gl_cols(&L[i]);
            if (L[i].y == GL_UNUSED)
                continue;
            while (end && L[i].t[end - 1u] == ' ')
                end--;
            printf("%3u|", L[i].y);
            for (k = 0; k < end; k++)
                putchar(L[i].t[k]);
            putchar('\n');
        }
    } else {
        return -1;
    }
    return 0;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long len;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc((size_t)len + 1u);
    *n = fread(d, 1, (size_t)len, f);
    fclose(f);
    return d;
}

int main(int argc, char **argv)
{
    int i = 1;
    const char *flash_file = NULL;
    player_init(RATE, 1);
    gfx_init();
    memset(flash, 0xFF, sizeof flash);
    if (argc > 2 && !strcmp(argv[1], "--flash")) {
        FILE *f = fopen(argv[2], "rb");
        flash_file = argv[2];
        if (f) {
            if (fread(flash, 1, sizeof flash, f) != sizeof flash)
                fprintf(stderr, "%s: short\n", flash_file);
            fclose(f);
        }
        i = 3;
    }
    for (; i < argc && strcmp(argv[i], "--"); i++) {
        size_t n;
        uint8_t *file = read_file(argv[i], &n), *img;
        size_t img_n;
        char err[128];
        const char *base;
        char *name, *dot;
        if (!file) {
            fprintf(stderr, "%s: cannot read\n", argv[i]);
            return 1;
        }
        if (modconv(file, n, &img, &img_n, err, sizeof err)) {
            fprintf(stderr, "%s: %s\n", argv[i], err);
            return 1;
        }
        free(file);
        if (NSONGS == MAX_SONGS)
            break;
        base = strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];
        name = strdup(base);
        dot = strrchr(name, '.');
        if (dot)
            *dot = 0;
        SONGS[NSONGS].name = name;
        SONGS[NSONGS].data = img;
        SONGS[NSONGS].size = (uint32_t)img_n;
        NSONGS++;
    }
    pages_init();
    frame_idle();
    for (i++; i < argc; i++) {
        int r = step(argv[i]);
        if (r == -1) {
            fprintf(stderr, "bad step: %s\n", argv[i]);
            return 1;
        }
        if (r)
            return 1;
    }
    if (flash_file) {
        FILE *f = fopen(flash_file, "wb");
        if (!f || fwrite(flash, 1, sizeof flash, f) != sizeof flash) {
            fprintf(stderr, "%s: cannot write\n", flash_file);
            return 1;
        }
        fclose(f);
    }
    return 0;
}
