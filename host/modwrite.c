/* SPDX-License-Identifier: GPL-3.0-only */
/* FMTK song image -> .xm (FastTracker 2.04) or .mod (ProTracker): the songs leave the FM-1 as files
 * any tracker reads. What the FMTK image does not keep is not written back: sample data after a
 * loop's end, XM extras FT2 ignores. A .mod takes 31 one-sample instruments, 64-row patterns,
 * 8-bit samples and effects 0..F; more is cut, with a note in warn. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modwrite.h"
#include "../engine/fmt.h"
#include "../engine/ft2_tables.h"

typedef struct { uint8_t *b; size_t n, cap; int oom; } wbuf;

static void wput(wbuf *w, const void *p, size_t k)
{
    if (w->n + k > w->cap) {
        size_t c = w->cap ? w->cap * 2 : 65536;
        uint8_t *nb;
        while (c < w->n + k)
            c *= 2;
        nb = realloc(w->b, c);
        if (!nb) {
            w->oom = 1;
            return;
        }
        w->b = nb;
        w->cap = c;
    }
    if (p)
        memcpy(w->b + w->n, p, k);
    else
        memset(w->b + w->n, 0, k);
    w->n += k;
}

static void w8(wbuf *w, uint32_t v) { uint8_t b = (uint8_t)v; wput(w, &b, 1); }
static void w16(wbuf *w, uint32_t v) { w8(w, v); w8(w, v >> 8); }
static void w32(wbuf *w, uint32_t v) { w16(w, v); w16(w, v >> 16); }
static void w16be(wbuf *w, uint32_t v) { w8(w, v >> 8); w8(w, v); }

static size_t wstrp(wbuf *w, const char *s, size_t n, uint8_t pad)   /* returns the length */
{
    size_t i = 0, len;
    for (; i < n && s[i]; i++)
        w8(w, (uint8_t)s[i]);
    len = i;
    for (; i < n; i++)
        w8(w, pad);
    return len;
}

static void wstr(wbuf *w, const char *s, size_t n) { wstrp(w, s, n, 0); }   /* padded with zeros */
static void wstrs(wbuf *w, const char *s, size_t n) { wstrp(w, s, n, ' '); }   /* with spaces (as FT2) */

typedef struct {
    const uint8_t *img;
    size_t size;
    const fmt_header *h;
    const uint8_t *orders;
    const fmt_pattern *pat;
    const fmt_instr *ins;
    const fmt_sample *smp;
} image_t;

static int open_image(image_t *im, const uint8_t *img, size_t size, char *err, size_t en)
{
    const fmt_header *h = (const fmt_header *)img;
    if (size < sizeof *h || h->magic != FMT_MAGIC || h->version != FMT_VERSION || h->size > size ||
        h->orders_off + 256u > size || h->patterns_off + h->num_patterns * sizeof(fmt_pattern) > size ||
        h->instr_off + h->num_instr * sizeof(fmt_instr) > size ||
        h->samples_off + h->num_samples * sizeof(fmt_sample) > size) {
        snprintf(err, en, "not a valid song image");
        return -1;
    }
    im->img = img;
    im->size = size;
    im->h = h;
    im->orders = img + h->orders_off;
    im->pat = (const fmt_pattern *)(img + h->patterns_off);
    im->ins = (const fmt_instr *)(img + h->instr_off);
    im->smp = (const fmt_sample *)(img + h->samples_off);
    return 0;
}

static int smp_ok(const image_t *im, const fmt_sample *s)
{
    uint32_t bps = (s->flags & FMT_16BIT) ? 2u : 1u;
    return s->length && s->data_off + (uint64_t)s->length * bps <= im->size;
}

/* one cell of pattern p (unpacked) */
static void cell_at(const image_t *im, uint32_t p, uint32_t row, uint32_t ch, uint8_t c[5])
{
    const fmt_pattern *pt;
    const uint8_t *d;
    uint32_t i;
    memset(c, 0, 5);
    if (p >= im->h->num_patterns)
        return;
    pt = &im->pat[p];
    if (!pt->data_off || row >= pt->num_rows)
        return;
    d = im->img + pt->data_off + ((const uint16_t *)(im->img + pt->rows_off))[row];
    for (i = 0; i <= ch; i++) {
        uint8_t b = *d++;
        memset(c, 0, 5);
        if (b & 0x80) {
            if (b & 1) c[0] = *d++;
            if (b & 2) c[1] = *d++;
            if (b & 4) c[2] = *d++;
            if (b & 8) c[3] = *d++;
            if (b & 16) c[4] = *d++;
        } else {
            c[0] = b;
            memcpy(c + 1, d, 4);
            d += 4;
        }
    }
}

/* ------------------------------------------------------------------------ XM --- */
int fmtk_to_xm(const uint8_t *img, size_t size, uint8_t **out, size_t *out_n, char *err, size_t en)
{
    image_t im;
    wbuf w = {0, 0, 0, 0};
    uint32_t i, p, r, c, k, nch;
    if (open_image(&im, img, size, err, en))
        return -1;
    nch = im.h->num_channels;
    wput(&w, "Extended Module: ", 17);
    wstrs(&w, im.h->name, 20);
    w8(&w, 0x1A);
    /* the tracker it came from (OpenMPT plays by it); written the way FT2 writes its files */
    wstrs(&w, im.h->tracker[0] && strncmp(im.h->tracker, "MOD ", 4) ? im.h->tracker : "FastTracker v2.00", 20);
    w16(&w, 0x0104);
    w32(&w, 276);
    w16(&w, im.h->song_length);
    w16(&w, im.h->loop_start);
    w16(&w, nch);
    w16(&w, im.h->num_patterns);
    w16(&w, im.h->num_instr);
    w16(&w, (im.h->flags & FMT_LINEAR) ? 1u : 0u);
    w16(&w, im.h->speed);
    w16(&w, im.h->bpm);
    for (i = 0; i < 256u; i++)
        w8(&w, i < im.h->song_length ? im.orders[i] : 0u);
    for (p = 0; p < im.h->num_patterns; p++) {    /* the packing is XM's, rows in order */
        const fmt_pattern *pt = &im.pat[p];
        w32(&w, 9);
        w8(&w, 0);
        w16(&w, pt->num_rows);
        if (!pt->data_off) {           /* empty (as FT2 saves one) */
            w16(&w, 0);
            continue;
        }
        {   /* (re-packed row by row: the image's rows need not be contiguous) */
            size_t at = w.n;
            w16(&w, 0);
            for (r = 0; r < pt->num_rows; r++)
                for (c = 0; c < nch; c++) {
                    uint8_t e[5];
                    cell_at(&im, p, r, c, e);
                    if (e[0] && e[1] && e[2] && e[3] && e[4]) {
                        wput(&w, e, 5);
                    } else {
                        w8(&w, 0x80u | (e[0] != 0) | (e[1] != 0) << 1 | (e[2] != 0) << 2 | (e[3] != 0) << 3 |
                                   (e[4] != 0) << 4);
                        for (k = 0; k < 5; k++)
                            if (e[k])
                                w8(&w, e[k]);
                    }
                }
            if (!w.oom) {
                size_t bytes = w.n - at - 2;
                if (bytes > 0xFFFF) {
                    snprintf(err, en, "pattern %u is too big for XM", p);
                    free(w.b);
                    return -1;
                }
                w.b[at] = (uint8_t)bytes;
                w.b[at + 1] = (uint8_t)(bytes >> 8);
            }
        }
    }
    for (i = 0; i < im.h->num_instr; i++) {
        const fmt_instr *in = &im.ins[i];
        uint32_t ns = in->num_samples;
        if (in->first_sample + ns > im.h->num_samples)
            ns = 0;
        w32(&w, ns ? 263u : 33u);                  /* (FT2: 33 for an empty one, its sample header size too) */
        wstr(&w, in->name, 22);
        w8(&w, 0);
        w16(&w, ns);
        w32(&w, 40);
        if (!ns)
            continue;
        wput(&w, in->note2sample, 96);
        for (k = 0; k < 12u; k++) {
            w16(&w, (uint16_t)in->vol_env[k][0]);
            w16(&w, (uint16_t)in->vol_env[k][1]);
        }
        for (k = 0; k < 12u; k++) {
            w16(&w, (uint16_t)in->pan_env[k][0]);
            w16(&w, (uint16_t)in->pan_env[k][1]);
        }
        w8(&w, in->vol_env_len);
        w8(&w, in->pan_env_len);
        w8(&w, in->vol_sus);
        w8(&w, in->vol_loop_start);
        w8(&w, in->vol_loop_end);
        w8(&w, in->pan_sus);
        w8(&w, in->pan_loop_start);
        w8(&w, in->pan_loop_end);
        w8(&w, in->vol_flags);
        w8(&w, in->pan_flags);
        w8(&w, in->vib_type);
        w8(&w, in->vib_sweep);
        w8(&w, in->vib_depth);
        w8(&w, in->vib_rate);
        w16(&w, in->fadeout);
        wput(&w, NULL, 22);                        /* reserved (to the 263 bytes) */
        for (k = 0; k < ns; k++) {
            const fmt_sample *s = &im.smp[in->first_sample + k];
            uint32_t bps = (s->flags & FMT_16BIT) ? 2u : 1u, len = smp_ok(&im, s) ? s->length : 0u;
            w32(&w, len * bps);
            w32(&w, len ? s->loop_start * bps : 0u);
            w32(&w, len ? s->loop_length * bps : 0u);
            w8(&w, s->volume);
            w8(&w, (uint8_t)s->finetune);
            w8(&w, len ? s->flags & (3u | FMT_16BIT) : (s->flags & FMT_16BIT));
            w8(&w, s->panning);
            w8(&w, (uint8_t)s->relative_note);
            w8(&w, (uint8_t)(memchr(s->name, 0, 22) ? strlen(s->name) : 22u));   /* (FT2 keeps the name's length here) */
            wstrs(&w, s->name, 22);
        }
        for (k = 0; k < ns; k++) {                 /* delta-coded */
            const fmt_sample *s = &im.smp[in->first_sample + k];
            uint32_t j;
            if (!smp_ok(&im, s))
                continue;
            if (s->flags & FMT_16BIT) {
                const int16_t *d = (const int16_t *)(img + s->data_off);
                int16_t old = 0;
                for (j = 0; j < s->length; j++) {
                    w16(&w, (uint16_t)(int16_t)(d[j] - old));
                    old = d[j];
                }
            } else {
                const int8_t *d = (const int8_t *)(img + s->data_off);
                int8_t old = 0;
                for (j = 0; j < s->length; j++) {
                    w8(&w, (uint8_t)(int8_t)(d[j] - old));
                    old = d[j];
                }
            }
        }
    }
    if (w.oom) {
        free(w.b);
        snprintf(err, en, "out of memory");
        return -1;
    }
    *out = w.b;
    *out_n = w.n;
    return 0;
}

/* ----------------------------------------------------------------------- MOD --- */
int fmtk_to_mod(const uint8_t *img, size_t size, uint8_t **out, size_t *out_n, char *warn, size_t wn,
                char *err, size_t en)
{
    image_t im;
    wbuf w = {0, 0, 0, 0};
    uint32_t i, p, r, c, nch, npat = 0, len, cut_rows = 0, cut_fx = 0, cut_ins = 0, cut_16 = 0;
    const fmt_sample *ms[31];
    if (warn && wn)
        warn[0] = 0;
    if (open_image(&im, img, size, err, en))
        return -1;
    nch = im.h->num_channels;
    if (nch > 32) {
        snprintf(err, en, "too many channels for MOD");
        return -1;
    }
    len = im.h->song_length > 128u ? 128u : im.h->song_length;
    wstr(&w, im.h->name, 20);
    for (i = 0; i < 31u; i++) {                    /* instrument i+1: its first sample */
        const fmt_instr *in = i < im.h->num_instr ? &im.ins[i] : NULL;
        const fmt_sample *s = in && in->num_samples && in->first_sample < im.h->num_samples ?
                              &im.smp[in->first_sample] : NULL;
        uint32_t words;
        if (s && !smp_ok(&im, s))
            s = NULL;
        if (in && in->num_samples > 1)
            cut_ins++;
        ms[i] = s;
        wstr(&w, in ? in->name : "", 22);
        words = s ? (s->length > 0x1FFFEu ? 0xFFFFu : (s->length + 1u) / 2u) : 0u;
        w16be(&w, words);
        w8(&w, s ? ((uint8_t)s->finetune >> 4) & 15u : 0u);
        w8(&w, s ? (s->volume > 64 ? 64 : s->volume) : 0u);
        if (s && (s->flags & 3) && s->loop_length > 2) {
            w16be(&w, s->loop_start / 2u);
            w16be(&w, s->loop_length / 2u);
        } else {
            w16be(&w, 0);
            w16be(&w, 1);
        }
        if (s && (s->flags & FMT_16BIT))
            cut_16++;
    }
    if (im.h->num_instr > 31u)
        cut_ins += im.h->num_instr - 31u;
    w8(&w, len);
    w8(&w, im.h->loop_start < len ? im.h->loop_start : 0u);
    for (i = 0; i < 128u; i++) {
        uint32_t o = i < len ? im.orders[i] : 0u;
        w8(&w, o);
        if (i < len && o + 1u > npat)
            npat = o + 1u;
    }
    if (nch == 4)
        wput(&w, "M.K.", 4);
    else if (nch < 10) {
        char id[5] = {(char)('0' + nch), 'C', 'H', 'N', 0};
        wput(&w, id, 4);
    } else {
        char id[5] = {(char)('0' + nch / 10), (char)('0' + nch % 10), 'C', 'H', 0};
        wput(&w, id, 4);
    }
    for (p = 0; p < npat; p++) {
        if (p < im.h->num_patterns && im.pat[p].num_rows != 64)
            cut_rows++;
        for (r = 0; r < 64u; r++)
            for (c = 0; c < nch; c++) {
                uint8_t e[5];
                uint32_t period = 0, ins, fx, par;
                cell_at(&im, p, r, c, e);
                if (e[0] >= 1 && e[0] <= 96)
                    period = ft2_mod_periods[e[0] - 1];
                ins = e[1] <= 31 ? e[1] : 0;
                fx = e[3];
                par = e[4];
                if (fx > 15) {
                    cut_fx++;
                    fx = par = 0;
                }
                if (e[2] >= 0x10 && e[2] <= 0x50 && !fx && !par) {   /* volume column -> Cxx */
                    fx = 0xC;
                    par = e[2] - 0x10u;
                } else if (e[2]) {
                    cut_fx++;
                }
                w8(&w, (ins & 0xF0u) | ((period >> 8) & 0x0Fu));
                w8(&w, period & 0xFFu);
                w8(&w, ((ins & 0x0Fu) << 4) | fx);
                w8(&w, par);
            }
    }
    for (i = 0; i < 31u; i++) {
        const fmt_sample *s = ms[i];
        uint32_t j, n;
        if (!s)
            continue;
        n = s->length > 0x1FFFEu ? 0x1FFFEu : s->length;
        for (j = 0; j < n; j++)
            w8(&w, (s->flags & FMT_16BIT) ? (uint8_t)(((const int16_t *)(img + s->data_off))[j] >> 8)
                                          : (uint8_t)((const int8_t *)(img + s->data_off))[j]);
        if (n & 1u)
            w8(&w, 0);
    }
    if (w.oom) {
        free(w.b);
        snprintf(err, en, "out of memory");
        return -1;
    }
    if (warn && wn && (cut_rows || cut_fx || cut_ins || cut_16 || im.h->song_length > 128u))
        snprintf(warn, wn, "MOD: %u patterns not 64 rows, %u effects / volumes, %u instruments, %u 16-bit samples, "
                 "%u positions cut", cut_rows, cut_fx, cut_ins, cut_16,
                 im.h->song_length > 128u ? im.h->song_length - 128u : 0u);
    *out = w.b;
    *out_n = w.n;
    return 0;
}
