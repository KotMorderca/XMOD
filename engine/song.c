/* SPDX-License-Identifier: GPL-3.0-only */
/* The editable song (song.h). */
#ifndef XMOD_FIRMWARE
#include <string.h>                  /* (the firmware: Felucca's libc.c, memmove in xmod.c) */
#endif
#include "song.h"

#ifndef SONG_SECTION
#define SONG_SECTION                 /* the FM-1: the big RAM pool */
#endif

song_t song;
static uint8_t arena[SONG_ARENA] SONG_SECTION __attribute__((aligned(4)));
static song_cell ecell[SONG_MAX_ROWS * FMT_MAX_CHANNELS] SONG_SECTION;   /* [row * 32 + channel] */
static fmt_pattern save_pt[SONG_MAX_PATTERNS + 128 * 16] SONG_SECTION;   /* song_save: the patterns, then the samples' new data_off */
#define EC(row, ch) ecell[(row) * FMT_MAX_CHANNELS + (ch)]

static uint32_t (*lock_fn)(void);
static void (*unlock_fn)(uint32_t);
static uint32_t song_lock(void) { return lock_fn ? lock_fn() : 0; }
static void song_unlock(uint32_t f) { if (unlock_fn) unlock_fn(f); }

void song_set_lock(uint32_t (*l)(void), void (*u)(uint32_t))
{
    lock_fn = l;
    unlock_fn = u;
}

/* ------------------------------------------------------------------ arena --- */
static uint32_t rnd4(uint32_t n) { return (n + 3u) & ~3u; }

/* slide every live block down to the bottom, in address order */
static void arena_compact(void)
{
    uint32_t f = song_lock(), dst = song.tables, p;  /* (the tables stay at the bottom) */
    const uint8_t *last = 0;         /* where the last block moved was (a moved block is at or below it) */
    for (;;) {
        song_pat *best = 0;
        for (p = 0; p < SONG_MAX_PATTERNS; p++) {
            song_pat *s = &song.pat[p];
            if (s->data && (!last || s->data > last) && (!best || s->data < best->data))
                best = s;
        }
        if (!best)
            break;
        last = best->data;
        if (best->data != arena + dst)
            memmove(arena + dst, best->data, rnd4(best->size));
        best->data = arena + dst;
        dst += rnd4(best->size);
    }
    song.top = dst;
    song_unlock(f);
}

static uint8_t *arena_alloc(uint32_t n)
{
    uint8_t *p;
    n = rnd4(n);
    if (song.top + n > SONG_ARENA) {
        if (song.live + n > SONG_ARENA)
            return 0;
        arena_compact();
    }
    p = arena + song.top;
    song.top += n;
    song.live += n;
    return p;
}

/* -------------------------------------------------------------- packing --- */
static uint32_t cell_bytes(const song_cell *c)
{
    uint32_t n = 1;
    if (c->note && c->instr && c->vol && c->efx && c->efx_data)
        return 5;
    n += c->note != 0;
    n += c->instr != 0;
    n += c->vol != 0;
    n += c->efx != 0;
    n += c->efx_data != 0;
    return n;
}

static uint8_t *pack_cell(uint8_t *o, const song_cell *c)
{
    if (c->note && c->instr && c->vol && c->efx && c->efx_data) {
        o[0] = c->note;
        o[1] = c->instr;
        o[2] = c->vol;
        o[3] = c->efx;
        o[4] = c->efx_data;
        return o + 5;
    }
    *o++ = (uint8_t)(0x80u | (c->note != 0) | (c->instr != 0) << 1 | (c->vol != 0) << 2 |
                     (c->efx != 0) << 3 | (c->efx_data != 0) << 4);
    if (c->note) *o++ = c->note;
    if (c->instr) *o++ = c->instr;
    if (c->vol) *o++ = c->vol;
    if (c->efx) *o++ = c->efx;
    if (c->efx_data) *o++ = c->efx_data;
    return o;
}

static const uint8_t *unpack_cell(const uint8_t *p, song_cell *c)
{
    uint8_t b = *p++;
    c->note = c->instr = c->vol = c->efx = c->efx_data = 0;
    if (b & 0x80) {
        if (b & 1) c->note = *p++;
        if (b & 2) c->instr = *p++;
        if (b & 4) c->vol = *p++;
        if (b & 8) c->efx = *p++;
        if (b & 16) c->efx_data = *p++;
    } else {
        c->note = b;
        c->instr = p[0];
        c->vol = p[1];
        c->efx = p[2];
        c->efx_data = p[3];
        p += 4;
    }
    return p;
}

/* the edit buffer -> the arena; the buffer stays as it is */
int song_flush(void)
{
    int32_t p = song.epat;
    uint32_t rows, nch = song.hdr.num_channels, r, c, size = 0, f, any = 0;
    song_pat *s;
    uint8_t *blk = 0;
    if (p < 0)
        return 0;
    s = &song.pat[p];
    rows = s->rows;
    for (r = 0; r < rows; r++)
        for (c = 0; c < nch; c++) {
            const song_cell *e = &EC(r, c);
            size += cell_bytes(e);
            any |= e->note | e->instr | e->vol | e->efx | e->efx_data;
        }
    if (any) {
        uint16_t *ro;
        uint8_t *o;
        size += 2u * rows;
        blk = arena_alloc(size);
        if (!blk)
            return -1;
        ro = (uint16_t *)blk;
        o = blk + 2u * rows;
        for (r = 0; r < rows; r++) {
            ro[r] = (uint16_t)(o - (blk + 2u * rows));
            for (c = 0; c < nch; c++)
                o = pack_cell(o, &EC(r, c));
        }
    }
    f = song_lock();
    if (s->data)
        song.live -= rnd4(s->size);
    s->data = blk;
    s->size = (uint16_t)(any ? size : 0u);
    song.epat = -1;
    song_unlock(f);
    return 0;
}

/* ------------------------------------------------------------------ reading --- */
uint32_t song_rows(uint32_t p)
{
    return p < song.hdr.num_patterns ? song.pat[p].rows : 64u;
}

void song_read_row(uint32_t p, uint32_t row, song_cell *out, uint32_t nch)
{
    uint32_t i, n = song.hdr.num_channels;
    const song_pat *s;
    const uint8_t *d;
    memset(out, 0, sizeof(song_cell) * nch);
    if (p >= song.hdr.num_patterns)
        return;
    s = &song.pat[p];
    if (row >= s->rows)
        return;
    if ((int32_t)p == song.epat) {
        for (i = 0; i < nch && i < n; i++)
            out[i] = EC(row, i);
        return;
    }
    d = s->data;
    if (!d)
        return;
    d += 2u * s->rows + ((const uint16_t *)d)[row];
    for (i = 0; i < n; i++) {
        song_cell c;
        d = unpack_cell(d, &c);
        if (i < nch)
            out[i] = c;
    }
}

/* ------------------------------------------------------------------- loading --- */
static int song_valid(const uint8_t *img, uint32_t size)
{
    const fmt_header *h = (const fmt_header *)img;
    if (size < sizeof *h || h->magic != FMT_MAGIC || h->version != FMT_VERSION || h->size > size)
        return 0;
    if (!h->num_channels || h->num_channels > FMT_MAX_CHANNELS || !h->song_length || h->song_length > 256 ||
        h->num_patterns > SONG_MAX_PATTERNS)
        return 0;
    if (h->orders_off + 256u > size ||
        h->patterns_off + (uint32_t)h->num_patterns * sizeof(fmt_pattern) > size ||
        h->instr_off + (uint32_t)h->num_instr * sizeof(fmt_instr) > size ||
        h->samples_off + (uint32_t)h->num_samples * sizeof(fmt_sample) > size)
        return 0;
    return 1;
}

int song_load(const uint8_t *img, uint32_t size)
{
    const fmt_header *h = (const fmt_header *)img;
    const fmt_pattern *pt;
    uint32_t p;
    memset(&song, 0, sizeof song);
    song.epat = -1;
    if (!song_valid(img, size))
        return -1;
    song.hdr = *h;
    song.img = img;
    song.img_size = size;
    {   /* the instruments and the sample headers to RAM, first: the arena's bottom */
        uint32_t ni = rnd4((uint32_t)sizeof(fmt_instr) * h->num_instr), ns = (uint32_t)sizeof(fmt_sample) * h->num_samples;
        uint8_t *t = arena_alloc(ni + ns + 4u);
        if (!t)
            return -2;
        song.tables = song.top;
        song.ins = (fmt_instr *)t;
        song.smp = (fmt_sample *)(t + ni);
        memcpy(song.ins, img + h->instr_off, (uint32_t)sizeof(fmt_instr) * h->num_instr);
        memcpy(song.smp, img + h->samples_off, ns);
    }
    memcpy(song.orders, img + h->orders_off, 256);
    pt = (const fmt_pattern *)(img + h->patterns_off);
    for (p = 0; p < h->num_patterns; p++) {
        uint32_t rows = pt[p].num_rows, n;
        song_pat *s = &song.pat[p];
        s->rows = (uint16_t)(rows < 1u || rows > SONG_MAX_ROWS ? 64u : rows);
        if (!pt[p].data_off || rows != s->rows)
            continue;
        n = 2u * rows + pt[p].data_bytes;
        if (pt[p].rows_off + 2u * rows > size || pt[p].data_off + pt[p].data_bytes > size || n > 0xFFFFu)
            return -1;
        s->data = arena_alloc(n);
        if (!s->data)
            return -2;
        s->size = (uint16_t)n;
        memcpy(s->data, img + pt[p].rows_off, 2u * rows);
        memcpy(s->data + 2u * rows, img + pt[p].data_off, pt[p].data_bytes);
    }
    return 0;
}

/* ------------------------------------------------------------------- editing --- */
static void song_extend(uint32_t p)       /* patterns up to p exist (64 empty rows) */
{
    while (song.hdr.num_patterns <= p) {
        song_pat *s = &song.pat[song.hdr.num_patterns];
        uint32_t f = song_lock();
        if (s->data)                 /* (left from before song_set_num_patterns) */
            song.live -= rnd4(s->size);
        s->rows = 64;
        s->data = 0;
        s->size = 0;
        song.hdr.num_patterns++;
        song_unlock(f);
    }
}

void song_set_num_patterns(uint32_t n)
{
    uint32_t f;
    if (n >= song.hdr.num_patterns) {
        if (n)
            song_extend(n - 1u);
        return;
    }
    f = song_lock();
    if (song.epat >= (int32_t)n)
        song.epat = -1;              /* (dropped, not packed) */
    while (song.hdr.num_patterns > n) {
        song_pat *s = &song.pat[--song.hdr.num_patterns];
        if (s->data)
            song.live -= rnd4(s->size);
        s->data = 0;
        s->size = 0;
    }
    song_unlock(f);
}

int song_edit(uint32_t p)
{
    uint32_t r;
    if (p >= SONG_MAX_PATTERNS)
        return -2;
    if ((int32_t)p == song.epat)
        return 0;
    if (song_flush())
        return -1;
    song_extend(p);
    for (r = 0; r < song.pat[p].rows; r++)
        song_read_row(p, r, &EC(r, 0), FMT_MAX_CHANNELS);
    song.epat = (int16_t)p;          /* (the reader takes the buffer from here on) */
    return 0;
}

song_cell *song_cell_at(uint32_t p, uint32_t row, uint32_t ch)
{
    if ((int32_t)p != song.epat || row >= song.pat[p].rows || ch >= song.hdr.num_channels)
        return 0;
    return &EC(row, ch);
}

int song_set_rows(uint32_t p, uint32_t rows)
{
    uint32_t r, old;
    if (rows < 1u || rows > SONG_MAX_ROWS)
        return -2;
    if (song_edit(p))
        return -1;
    old = song.pat[p].rows;
    for (r = old; r < rows; r++)
        memset(&EC(r, 0), 0, sizeof(song_cell) * FMT_MAX_CHANNELS);
    song.pat[p].rows = (uint16_t)rows;
    for (r = rows; r < old; r++)
        memset(&EC(r, 0), 0, sizeof(song_cell) * FMT_MAX_CHANNELS);
    return 0;
}

int song_new_pattern(void)
{
    uint32_t p = song.hdr.num_patterns;
    if (p >= SONG_MAX_PATTERNS)
        return -1;
    song_extend(p);
    return (int)p;
}

int song_copy_pattern(uint32_t from, uint32_t to)
{
    song_pat *s, *d;
    uint8_t *blk = 0;
    uint32_t f;
    if (from >= SONG_MAX_PATTERNS || to >= SONG_MAX_PATTERNS || from == to)
        return -2;
    if (song_flush())
        return -1;
    song_extend(from > to ? from : to);
    s = &song.pat[from];
    d = &song.pat[to];
    if (s->data) {
        blk = arena_alloc(s->size);
        if (!blk)
            return -1;
        memcpy(blk, s->data, s->size);    /* (s->data is re-read: the alloc may have compacted) */
    }
    f = song_lock();
    if (d->data)
        song.live -= rnd4(d->size);
    d->data = blk;
    d->size = s->size;
    d->rows = s->rows;
    song_unlock(f);
    return 0;
}

uint32_t song_mem_used(void) { return song.live; }

/* ------------------------------------------------------------------- saving --- */
/* a sample's bytes in an image: from 4 before frame 0 (pad and the guard) to the two guard frames
 * after its end (host/modconv.c write_image), 4-aligned */
static uint32_t smp_block(const fmt_sample *s, uint32_t *start)
{
    uint32_t bps = (s->flags & FMT_16BIT) ? 2u : 1u;
    *start = s->data_off - 4u;
    return rnd4(4u + (s->length + 2u) * bps);
}

typedef struct {
    song_sink sink;
    void *ctx;
    uint32_t n;                      /* bytes so far */
    int err;
} wr_t;

static void wr(wr_t *w, const void *d, uint32_t n)
{
    if (!w->err && n && w->sink)
        w->err = w->sink(w->ctx, d, n);
    w->n += n;
}

static void wr_align(wr_t *w) { if (w->n & 3u) wr(w, 0, 4u - (w->n & 3u)); }

/* one pass: the layout (sink NULL) or the bytes */
static void save_pass(wr_t *w, fmt_header *h, fmt_pattern *pt)
{
    const fmt_header *src = (const fmt_header *)song.img;
    const fmt_instr *ins = song.ins;                 /* (the RAM copies: edited) */
    const fmt_sample *smp = song.smp;
    uint32_t p, i, ns = src->num_samples, np = song.hdr.num_patterns;
    wr(w, h, sizeof *h);
    wr(w, song.orders, 256);
    wr_align(w);
    h->patterns_off = w->n;
    wr(w, pt, (uint32_t)sizeof *pt * np);
    h->instr_off = w->n;
    wr(w, ins, (uint32_t)sizeof *ins * src->num_instr);
    h->samples_off = w->n;
    for (i = 0; i < ns; i++) {       /* the sample table, its data where pass 1 put it */
        fmt_sample s = smp[i];
        if (w->sink)
            s.data_off = pt[np + i].data_off;
        wr(w, &s, sizeof s);
    }
    for (p = 0; p < np; p++) {
        const song_pat *sp = &song.pat[p];
        wr_align(w);
        if (!w->sink) {
            pt[p].num_rows = sp->rows;
            pt[p].data_bytes = (uint16_t)(sp->data ? sp->size - 2u * sp->rows : 0u);
            pt[p].rows_off = sp->data ? w->n : 0u;
            pt[p].data_off = sp->data ? w->n + 2u * sp->rows : 0u;
        }
        if (sp->data)
            wr(w, sp->data, sp->size);   /* rows_off[rows], then the rows: as the image has them */
    }
    for (i = 0; i < ns; i++) {
        uint32_t start, n;
        if (!smp[i].length)
            continue;
        n = smp_block(&smp[i], &start);
        wr_align(w);
        if (!w->sink)
            pt[np + i].data_off = w->n + 4u;   /* (pt has room for it: see song_save) */
        wr(w, song.img + start, n);
    }
    wr_align(w);
}

int song_save(song_sink sink, void *ctx, uint32_t *size)
{

    fmt_header h;
    wr_t w = {0, 0, 0, 0};
    if (song_flush())
        return -1;
    h = song.hdr;
    h.magic = FMT_MAGIC;
    h.version = FMT_VERSION;
    h.num_instr = ((const fmt_header *)song.img)->num_instr;
    h.num_samples = ((const fmt_header *)song.img)->num_samples;
    if (h.num_samples > 128u * 16u)
        return -2;
    h.orders_off = sizeof h;
    save_pass(&w, &h, save_pt);           /* the layout */
    h.size = w.n;
    if (size)
        *size = w.n;
    if (!sink)
        return 0;
    w.sink = sink;
    w.ctx = ctx;
    w.n = 0;
    save_pass(&w, &h, save_pt);
    return w.err;
}

void song_rebind(const uint8_t *img, uint32_t size)
{
    const fmt_header *h = (const fmt_header *)img;
    const fmt_sample *s = (const fmt_sample *)(img + h->samples_off);
    uint32_t i;
    song.img = img;
    song.img_size = size;
    for (i = 0; i < h->num_samples && i < song.hdr.num_samples; i++)
        song.smp[i].data_off = s[i].data_off;   /* (the copy may have the data elsewhere) */
}
