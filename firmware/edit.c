/* SPDX-License-Identifier: GPL-3.0-only */
/* Editing (part of pages.c): the undo log, the clipboard, changing cells, the order list and the
 * song's settings. Every change goes through here so that EDIT+SAVE can take it back. */

#ifndef UI_POOL
#define UI_POOL                    /* the FM-1: the big RAM pool (ui.c) */
#endif

/* ------------------------------------------------------------------ undo --- */
enum { U_CELL, U_ORDER, U_ORD_INS, U_ORD_DEL, U_HDR, U_ROWS, U_NPAT, U_MEM };
enum { H_LEN, H_RESTART, H_BPM, H_SPEED };
typedef struct {
    uint8_t type, a;               /* a: CELL channel, ORDER / ORD_DEL old pattern, HDR field */
    uint16_t group;                /* one user action */
    uint16_t b, c;                 /* CELL pattern, row; ORDER position; HDR old value; ROWS pattern, old rows */
    song_cell old;
} undo_rec;

#define UNDO_N 2048u
static undo_rec undo_log[UNDO_N] UI_POOL;
static struct {
    uint32_t head, count;          /* the newest is head - 1 */
    uint16_t group;
    /* the last knob change, to fold more turns of the same knob on the same cell into one step */
    int32_t last_p, last_row, last_ch, last_field;
    uint32_t last_ms;
} ud;

static void undo_new(void)         /* the next records are a new action */
{
    ud.group++;
    ud.last_field = -1;
}

static undo_rec *undo_push(uint8_t type)
{
    undo_rec *r;
    if (ud.count == UNDO_N) {      /* full: the oldest action goes, all of it */
        uint32_t tail = (ud.head + UNDO_N - ud.count) % UNDO_N;
        uint16_t g = undo_log[tail].group;
        while (ud.count && undo_log[(ud.head + UNDO_N - ud.count) % UNDO_N].group == g)
            ud.count--;
        (void)tail;
    }
    r = &undo_log[ud.head];
    ud.head = (ud.head + 1u) % UNDO_N;
    ud.count++;
    r->type = type;
    r->group = ud.group;
    r->a = 0;
    r->b = r->c = 0;
    return r;
}

static void undo_cell(uint32_t p, uint32_t row, uint32_t ch, const song_cell *old)
{
    undo_rec *r = undo_push(U_CELL);
    r->a = (uint8_t)ch;
    r->b = (uint16_t)p;
    r->c = (uint16_t)row;
    r->old = *old;
}

static uint16_t *hdr_field(uint32_t f)
{
    switch (f) {
    case H_LEN: return &song.hdr.song_length;
    case H_RESTART: return &song.hdr.loop_start;
    case H_BPM: return &song.hdr.bpm;
    default: return &song.hdr.speed;
    }
}

static void order_insert(uint32_t pos, uint8_t pat)
{
    uint32_t i, n = song.hdr.song_length;
    for (i = n < 255u ? n : 255u; i > pos; i--)
        song.orders[i] = song.orders[i - 1u];
    song.orders[pos] = pat;
    if (n < 256u)
        song.hdr.song_length = (uint16_t)(n + 1u);
}

static void order_delete(uint32_t pos)
{
    uint32_t i, n = song.hdr.song_length;
    for (i = pos; i + 1u < n; i++)
        song.orders[i] = song.orders[i + 1u];
    if (n > 1u)
        song.hdr.song_length = (uint16_t)(n - 1u);
    if (song.hdr.loop_start >= song.hdr.song_length)
        song.hdr.loop_start = 0;
}

/* take back the newest action */
static int undo(void)
{
    uint16_t g;
    if (!ud.count)
        return 0;
    g = undo_log[(ud.head + UNDO_N - 1u) % UNDO_N].group;
    while (ud.count) {
        undo_rec *r = &undo_log[(ud.head + UNDO_N - 1u) % UNDO_N];
        if (r->group != g)
            break;
        ud.head = (ud.head + UNDO_N - 1u) % UNDO_N;
        ud.count--;
        switch (r->type) {
        case U_CELL:
            if (!song_edit(r->b)) {
                song_cell *c = song_cell_at(r->b, r->c, r->a);
                if (c)
                    *c = r->old;
            }
            break;
        case U_ORDER: song.orders[r->b] = r->a; break;
        case U_ORD_INS: order_delete(r->b); break;
        case U_ORD_DEL: order_insert(r->b, r->a); break;
        case U_HDR: *hdr_field(r->a) = r->b; break;
        case U_ROWS: song_set_rows(r->b, r->c); break;
        case U_NPAT: song_set_num_patterns(r->b); break;
        case U_MEM: {                  /* an instrument or sample field: (b | c << 16) into the tables */
            uint8_t *m = (uint8_t *)song.ins + (r->b | (uint32_t)r->c << 16);
            memcpy(m, &r->old, r->a);  /* (1, 2 or 4 bytes) */
            break;
        }
        }
    }
    ud.last_field = -1;
    return 1;
}

static void undo_clear(void)
{
    ud.head = ud.count = 0;
    ud.last_field = -1;
}

/* ------------------------------------------------------------------ cells --- */
static int empty_cell(const song_cell *c) { return !(c->note | c->instr | c->vol | c->efx | c->efx_data); }

/* the cell to change, its old value logged. field >= 0: a knob turn; more turns of the same knob on
 * the same cell within 1.5 s are one undo step. */
static song_cell *cell_edit(uint32_t p, uint32_t row, uint32_t ch, int32_t field)
{
    song_cell *c;
    if (song_edit(p)) {
        ui_message("SONG MEMORY FULL");
        return 0;
    }
    c = song_cell_at(p, row, ch);
    if (!c)
        return 0;
    if (field < 0 || field != ud.last_field || (int32_t)p != ud.last_p || (int32_t)row != ud.last_row ||
        (int32_t)ch != ud.last_ch || fm1_ms - ud.last_ms > 1500u) {
        undo_new();
        undo_cell(p, row, ch, c);
    }
    ud.last_field = field;
    ud.last_p = (int32_t)p;
    ud.last_row = (int32_t)row;
    ud.last_ch = (int32_t)ch;
    ud.last_ms = fm1_ms;
    ui.changed = 1;
    return c;
}

/* the volume column: 00, 10..50 (volume 0..64), 60..FF (its effects); the rest is not valid */
static uint8_t vol_step(uint8_t v, int32_t d)
{
    int32_t i = v == 0 ? 0 : v <= 0x50 ? v - 0x10 + 1 : v >= 0x60 ? v - 0x60 + 66 : 0;
    i = clampi(i + d, 0, 225);
    return (uint8_t)(i == 0 ? 0 : i <= 65 ? i - 1 + 0x10 : i - 66 + 0x60);
}

/* effect number -> its place in FXS and back; a MOD has 0..F only */
static uint8_t fx_step(uint8_t e, int32_t d, int mod)
{
    int i = fx_find(e);
    int32_t n = mod ? 16 : (int32_t)NFX;
    char c;
    i = (int)clampi((i < 0 ? 0 : i) + d, 0, n - 1);
    c = FXS[i].code;
    return (uint8_t)(c <= '9' ? c - '0' : c - 'A' + 10);
}

/* ------------------------------------------------------------ the clipboard --- */
static song_cell clip[SONG_MAX_ROWS * FMT_MAX_CHANNELS] UI_POOL;
static uint16_t clip_rows;
static uint8_t clip_chans;

/* the selected block: rows r0..r1, channels c0..c1 (of the pattern shown) */
static int sel_rect(uint32_t *r0, uint32_t *r1, uint32_t *c0, uint32_t *c1)
{
    if (!ui.sel_on || ui.sel_pat != ui.vpat)
        return 0;
    *r0 = ui.sel_row < ui.row ? ui.sel_row : ui.row;
    *r1 = ui.sel_row < ui.row ? ui.row : ui.sel_row;
    *c0 = ui.sel_ch < ui.chan ? ui.sel_ch : ui.chan;
    *c1 = ui.sel_ch < ui.chan ? ui.chan : ui.sel_ch;
    return 1;
}

static int in_sel(uint32_t row, uint32_t ch)
{
    uint32_t r0, r1, c0, c1;
    return sel_rect(&r0, &r1, &c0, &c1) && row >= r0 && row <= r1 && ch >= c0 && ch <= c1;
}

static void block_copy(void)
{
    uint32_t r0, r1, c0, c1, r, c;
    song_cell row[FMT_MAX_CHANNELS];
    if (!sel_rect(&r0, &r1, &c0, &c1))
        return;
    clip_rows = (uint16_t)(r1 - r0 + 1u);
    clip_chans = (uint8_t)(c1 - c0 + 1u);
    for (r = r0; r <= r1; r++) {
        song_read_row(ui.vpat, r, row, FMT_MAX_CHANNELS);
        for (c = c0; c <= c1; c++)
            clip[(r - r0) * FMT_MAX_CHANNELS + (c - c0)] = row[c];
    }
    ui_message("BLOCK COPIED");
}

static void block_clear(void)
{
    uint32_t r0, r1, c0, c1, r, c;
    if (!sel_rect(&r0, &r1, &c0, &c1))
        return;
    if (song_edit(ui.vpat)) {
        ui_message("SONG MEMORY FULL");
        return;
    }
    undo_new();
    for (r = r0; r <= r1; r++)
        for (c = c0; c <= c1; c++) {
            song_cell *e = song_cell_at(ui.vpat, r, c);
            if (e && !empty_cell(e)) {
                undo_cell(ui.vpat, r, c, e);
                memset(e, 0, sizeof *e);
            }
        }
    ui.changed = 1;
    ui.sel_on = 0;
    ui_message("BLOCK CLEARED");
}

static void block_paste(void)
{
    uint32_t r, c, nch = song.hdr.num_channels;
    if (!clip_rows) {
        ui_message("CLIPBOARD EMPTY");
        return;
    }
    if (song_edit(ui.vpat)) {
        ui_message("SONG MEMORY FULL");
        return;
    }
    undo_new();
    for (r = 0; r < clip_rows; r++)
        for (c = 0; c < clip_chans && ui.chan + c < nch; c++) {
            song_cell *e = song_cell_at(ui.vpat, ui.row + r, ui.chan + c);
            if (!e)
                continue;
            undo_cell(ui.vpat, ui.row + r, ui.chan + c, e);
            *e = clip[r * FMT_MAX_CHANNELS + c];
        }
    ui.changed = 1;
    ui_message("PASTED");
}

/* ------------------------------------------------- order list and settings --- */
static void set_hdr(uint32_t f, int32_t v)
{
    uint16_t *p = hdr_field(f);
    if (*p == (uint16_t)v)
        return;
    if ((int32_t)f + 100 != ud.last_field || fm1_ms - ud.last_ms > 1500u) {   /* (turns of one knob: one step) */
        undo_rec *r;
        undo_new();
        r = undo_push(U_HDR);
        r->a = (uint8_t)f;
        r->b = *p;
    }
    ud.last_field = (int32_t)f + 100;
    ud.last_ms = fm1_ms;
    *p = (uint16_t)v;
    if (song.hdr.loop_start >= song.hdr.song_length)
        song.hdr.loop_start = (uint16_t)(song.hdr.song_length - 1u);
    ui.changed = 1;
}

static void set_order(uint32_t pos, int32_t pat)
{
    if (song.orders[pos] == (uint8_t)pat)
        return;
    if (ud.last_field != 200 || ud.last_p != (int32_t)pos || fm1_ms - ud.last_ms > 1500u) {
        undo_rec *r;
        undo_new();
        r = undo_push(U_ORDER);
        r->a = song.orders[pos];
        r->b = (uint16_t)pos;
    }
    ud.last_field = 200;
    ud.last_p = (int32_t)pos;
    ud.last_ms = fm1_ms;
    song.orders[pos] = (uint8_t)pat;
    ui.changed = 1;
}

static void insert_pos(uint32_t pos)
{
    undo_rec *r;
    if (song.hdr.song_length >= 256u) {
        ui_message("ORDER LIST FULL");
        return;
    }
    undo_new();
    r = undo_push(U_ORD_INS);
    r->b = (uint16_t)pos;
    order_insert(pos, song.orders[pos]);
    ui.changed = 1;
}

static void delete_pos(uint32_t pos)
{
    undo_rec *r;
    if (song.hdr.song_length <= 1u)
        return;
    undo_new();
    r = undo_push(U_ORD_DEL);
    r->a = song.orders[pos];
    r->b = (uint16_t)pos;
    order_delete(pos);
    ui.changed = 1;
}

static void set_rows(uint32_t p, int32_t rows)
{
    uint32_t old = song_rows(p), r, c;
    undo_rec *u;
    if ((uint32_t)rows == old || rows < 1 || rows > SONG_MAX_ROWS)
        return;
    if (song_edit(p)) {
        ui_message("SONG MEMORY FULL");
        return;
    }
    undo_new();
    for (r = (uint32_t)rows; r < old; r++)        /* the rows cut off */
        for (c = 0; c < song.hdr.num_channels; c++) {
            song_cell *e = song_cell_at(p, r, c);
            if (e && !empty_cell(e))
                undo_cell(p, r, c, e);
        }
    u = undo_push(U_ROWS);         /* (undone first: the rows come back, then their cells) */
    u->b = (uint16_t)p;
    u->c = (uint16_t)old;
    song_set_rows(p, (uint32_t)rows);
    ui.changed = 1;
}

static int new_pattern(int copy_from)   /* -1: empty; the new number, or -1 */
{
    uint32_t old = song.hdr.num_patterns;
    int p;
    undo_rec *r;
    if (old >= SONG_MAX_PATTERNS) {
        ui_message("NO PATTERN NUMBERS LEFT");
        return -1;
    }
    undo_new();
    r = undo_push(U_NPAT);
    r->b = (uint16_t)old;
    p = song_new_pattern();
    if (p >= 0 && copy_from >= 0 && song_copy_pattern((uint32_t)copy_from, (uint32_t)p)) {
        ui_message("SONG MEMORY FULL");
        song_set_num_patterns(old);
        ud.count--;
        ud.head = (ud.head + UNDO_N - 1u) % UNDO_N;
        return -1;
    }
    ui.changed = 1;
    return p;
}

/* ------------------------------------------------- instruments and samples --- */
/* a field of song.ins / song.smp (1, 2 or 4 bytes, little-endian) set to v; turns of one knob on
 * one field within 1.5 s are one undo step */
static void edit_mem(void *field, uint32_t size, int32_t v)
{
    uint8_t *m = field, nv[4];
    uint32_t off = (uint32_t)(m - (uint8_t *)song.ins), i;
    int32_t key = 1000 + (int32_t)off;
    for (i = 0; i < 4u; i++)
        nv[i] = (uint8_t)((uint32_t)v >> (8u * i));
    if (!memcmp(m, nv, size))
        return;
    if (key != ud.last_field || fm1_ms - ud.last_ms > 1500u) {
        undo_rec *r;
        undo_new();
        r = undo_push(U_MEM);
        r->a = (uint8_t)size;
        r->b = (uint16_t)off;
        r->c = (uint16_t)(off >> 16);
        memcpy(&r->old, m, size);
    }
    ud.last_field = key;
    ud.last_ms = fm1_ms;
    memcpy(m, nv, size);
    ui.changed = 1;
}
#define EDIT(field, v) edit_mem(&(field), sizeof(field), (int32_t)(v))
