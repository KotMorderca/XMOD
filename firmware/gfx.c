/* SPDX-License-Identifier: GPL-3.0-only */
/* Text lines and bars on the 240x240 panel. The screen is a stack of text lines (6x12 or 5x8
 * cells, a colour pair per cell) and bar graphs; a line is sent to the panel only when it changed.
 * A line can put its columns at any pixel x (xmap) and can have an overlay drawn into it before it
 * goes out (meters, bars). Two strip buffers: one is filled while the DMA sends the other (lcd.c
 * lcd_blit is asynchronous). Needs from the platform: lcd_blit, lcd_fill, font.h. */

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

/* the first 16 can be a cell's colours (ATTR), the rest only in bars and overlays */
enum { C_BG, C_TEXT, C_DIM, C_HI, C_NOTE, C_INS, C_VOL, C_FX, C_ROWBG, C_HEAD, C_PANEL, C_BLACK,
       C_MUTE, C_OK, C_WARN, C_BEAT,
       C_BAR, C_BARHI, C_GRID, C_SHADE, NCOLORS };
static const uint16_t PALETTE[NCOLORS] = {
    RGB(16, 20, 36),     /* C_BG */
    RGB(220, 224, 236),  /* C_TEXT */
    RGB(96, 104, 132),   /* C_DIM */
    RGB(255, 216, 96),   /* C_HI */
    RGB(240, 240, 255),  /* C_NOTE */
    RGB(110, 200, 255),  /* C_INS */
    RGB(120, 230, 140),  /* C_VOL */
    RGB(255, 140, 190),  /* C_FX */
    RGB(52, 60, 112),    /* C_ROWBG: the playing row */
    RGB(40, 92, 160),    /* C_HEAD: title bar */
    RGB(26, 32, 54),     /* C_PANEL: info line, footer */
    RGB(11, 13, 22),     /* C_BLACK: text on a highlight */
    RGB(180, 60, 60),    /* C_MUTE */
    RGB(80, 220, 120),   /* C_OK */
    RGB(255, 96, 64),    /* C_WARN */
    RGB(28, 34, 60),     /* C_BEAT: every 4th row, headers */
    RGB(70, 200, 120),   /* C_BAR */
    RGB(255, 200, 60),   /* C_BARHI */
    RGB(38, 46, 76),     /* C_GRID: empty meters, separators */
    RGB(32, 48, 90),     /* C_SHADE */
};
static uint16_t pal_sw[NCOLORS];               /* byte-swapped: the panel takes the high byte first */

#define ATTR(fg, bg) ((uint8_t)((fg) | ((bg) << 4)))
#define MAXCOLS 48

typedef struct {
    uint8_t font;                  /* 0: 6x12 (40 columns), 1: 5x8 (48 columns) */
    uint8_t y;
    uint8_t ncols;                 /* with xmap: how many columns it has */
    const uint8_t *xmap;           /* pixel x of each column; NULL: column * glyph width */
    char t[MAXCOLS];
    uint8_t a[MAXCOLS];
    uint32_t sum;                  /* of what is on the panel; 0 = redraw */
} gline;
#define GL_UNUSED 0xFFu            /* y of a line that is not on the screen (never drawn) */

/* draws into a line's strip (240 x h pixels) after the text */
typedef void (*gl_overlay)(uint16_t *buf, uint32_t h, const void *ctx);

static uint16_t strip[2][240u * 12u] __attribute__((section(".pool"), aligned(4)));
static uint8_t strip_cur;

static void gfx_init(void)
{
    uint32_t i;
    for (i = 0; i < NCOLORS; i++)
        pal_sw[i] = (uint16_t)((PALETTE[i] >> 8) | (PALETTE[i] << 8));
}

static uint16_t *strip_next(void)
{
    strip_cur ^= 1u;
    return strip[strip_cur];       /* the other one may still be on its way out */
}

static void gl_init(gline *l, uint8_t font, uint8_t y)
{
    l->font = font;
    l->y = y;
    l->xmap = 0;
    l->ncols = 0;
    l->sum = 0;
}

static uint32_t gl_cols(const gline *l) { return l->xmap ? l->ncols : l->font ? 48u : 40u; }

static void gl_clear(gline *l, uint8_t attr)
{
    uint32_t i;
    for (i = 0; i < MAXCOLS; i++) {
        l->t[i] = ' ';
        l->a[i] = attr;
    }
}

/* put a string at column x, returns the column after it */
static uint32_t gl_puts(gline *l, uint32_t x, const char *s, uint8_t attr)
{
    while (*s && x < gl_cols(l)) {
        l->t[x] = *s++;
        l->a[x++] = attr;
    }
    return x;
}

/* at most n characters of s, padded with spaces to n */
static uint32_t gl_putn(gline *l, uint32_t x, const char *s, uint32_t n, uint8_t attr)
{
    uint32_t i;
    for (i = 0; i < n && x < gl_cols(l); i++, x++) {
        l->t[x] = *s ? *s++ : ' ';
        l->a[x] = attr;
    }
    return x;
}

static uint32_t gl_hex(gline *l, uint32_t x, uint32_t v, uint32_t digits, uint8_t attr)
{
    char b[9];
    uint32_t i;
    for (i = 0; i < digits; i++)
        b[i] = "0123456789ABCDEF"[(v >> ((digits - 1u - i) * 4u)) & 15u];
    b[digits] = 0;
    return gl_puts(l, x, b, attr);
}

/* right-aligned in width (spaces), or zero-padded with width | GL_ZERO */
#define GL_ZERO 0x100u
static uint32_t gl_dec(gline *l, uint32_t x, uint32_t v, uint32_t width, uint8_t attr)
{
    char b[12], pad = (width & GL_ZERO) ? '0' : ' ';
    uint32_t n = 0, i;
    width &= 0xFFu;
    do
        b[n++] = (char)('0' + v % 10u);
    while ((v /= 10u) && n < 10u);
    while (n < width && n < 11u)
        b[n++] = pad;
    for (i = 0; i < n / 2u; i++) {
        char t = b[i];
        b[i] = b[n - 1u - i];
        b[n - 1u - i] = t;
    }
    b[n] = 0;
    return gl_puts(l, x, b, attr);
}

static void gl_attr(gline *l, uint32_t x0, uint32_t x1, uint8_t attr)   /* recolour [x0, x1) */
{
    for (; x0 < x1 && x0 < MAXCOLS; x0++)
        l->a[x0] = attr;
}

/* a filled rectangle in a strip */
static void strip_rect(uint16_t *buf, uint32_t h, int32_t x, int32_t y, int32_t w, int32_t rh, uint32_t col)
{
    int32_t i, j;
    uint16_t c = pal_sw[col];
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + w > 240) w = 240 - x;
    if (y + rh > (int32_t)h) rh = (int32_t)h - y;
    for (j = 0; j < rh; j++)
        for (i = 0; i < w; i++)
            buf[(uint32_t)(y + j) * 240u + (uint32_t)(x + i)] = c;
}

static void gl_render(const gline *l, uint16_t *buf)
{
    uint32_t cols = gl_cols(l), h = l->font ? FONT5X8_H : FONT6X12_H, w = l->font ? FONT5X8_W : FONT6X12_W;
    uint32_t i, r, x;
    if (l->xmap) {                 /* columns may leave gaps: the first cell's background under all */
        uint16_t bg = pal_sw[l->a[0] >> 4];
        for (i = 0; i < 240u * h; i++)
            buf[i] = bg;
    }
    for (i = 0; i < cols; i++) {
        uint8_t c = (uint8_t)l->t[i];
        const uint8_t *g;
        uint16_t fg = pal_sw[l->a[i] & 15u], bg = pal_sw[l->a[i] >> 4];
        uint32_t px = l->xmap ? l->xmap[i] : i * w;
        if (px + w > 240u)
            continue;
        if (c < 32 || c > 126)
            c = '?';
        g = l->font ? font5x8[c - 32] : font6x12[c - 32];
        for (r = 0; r < h; r++) {
            uint16_t *p = buf + r * 240u + px;
            uint8_t bits = g[r];
            for (x = 0; x < w; x++)
                p[x] = (bits & (0x80u >> x)) ? fg : bg;
        }
    }
}

/* send the line if it (or key, what the overlay draws) changed */
static void gl_draw_ov(gline *l, uint32_t key, gl_overlay ov, const void *ctx)
{
    uint32_t cols = gl_cols(l), h = l->font ? FONT5X8_H : FONT6X12_H;
    uint32_t sum = 2166136261u, i;
    uint16_t *buf;
    if (l->y == GL_UNUSED)
        return;
    for (i = 0; i < cols; i++) {
        sum = (sum ^ (uint8_t)l->t[i]) * 16777619u;
        sum = (sum ^ l->a[i]) * 16777619u;
    }
    sum = (sum ^ key) * 16777619u;
    sum = (sum ^ (uint32_t)(uintptr_t)l->xmap) * 16777619u;
    if (!sum)
        sum = 1;
    if (sum == l->sum)
        return;
    l->sum = sum;
    buf = strip_next();
    gl_render(l, buf);
    if (ov)
        ov(buf, h, ctx);
    lcd_blit(0, l->y, 240, h, buf);
}

static void gl_draw(gline *l) { gl_draw_ov(l, 0, 0, 0); }

/* vertical bars, one per value (0..255), across the full width at y, h rows tall */
static __attribute__((unused)) void gfx_bars(uint32_t y, uint32_t h, const uint8_t *v, const uint8_t *col, uint32_t n)
{
    uint32_t bw = n ? 240u / n : 240u, x0 = (240u - bw * n) / 2u, row, y0;
    if (bw > 24u) {
        bw = 24u;
        x0 = (240u - bw * n) / 2u;
    }
    for (y0 = 0; y0 < h; y0 += 12u) {
        uint32_t rows = h - y0 > 12u ? 12u : h - y0;
        uint16_t *buf = strip_next();
        for (row = 0; row < rows; row++) {
            uint32_t level = (h - (y0 + row)) * 255u / h;      /* this row lights for values above it */
            uint16_t *p = buf + row * 240u;
            uint32_t x, k;
            for (x = 0; x < 240u; x++)
                p[x] = pal_sw[C_BG];
            for (k = 0; k < n; k++) {
                uint16_t c = v[k] >= level && v[k] ? pal_sw[col[k]] : pal_sw[C_BEAT];
                for (x = 1; x + 1u < bw; x++)
                    p[x0 + k * bw + x] = c;
            }
        }
        lcd_blit(0, y + y0, 240, rows, buf);
    }
}
