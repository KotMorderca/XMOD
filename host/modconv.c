/* SPDX-License-Identifier: GPL-3.0-only */
/* .xm / .mod -> FMTK song image. The parsing and the clean-up of odd files follow ft2-clone's
 * loaders (src/modloaders/ft2_load_xm.c, ft2_load_mod.c, src/ft2_module_loader.c; Copyright (c)
 * 2016-2026 Olav Sorensen, BSD-3-Clause, see THIRD_PARTY.md), so the player sees what FT2 sees. */
#include "modconv.h"
#include "../engine/fmt.h"
#include "../engine/ft2_tables.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ the song in memory --- */

typedef struct { uint8_t note, instr, vol, efx, efx_data; } cell;

typedef struct {
    char name[23];
    int32_t length, loop_start, loop_length;
    uint8_t volume, flags, panning;
    int8_t finetune, relative_note;
    int16_t *pcm16;                  /* decoded, one of the two */
    int8_t *pcm8;
} smp_t;

typedef struct {
    char name[23];
    int num_samples;
    fmt_instr h;                     /* envelopes etc. (name / first_sample / num_samples filled later) */
    smp_t smp[16];
} ins_t;

typedef struct {
    char name[21], tracker[24];
    int linear, from_mod, num_ch, song_length, loop_start, speed, bpm, num_patterns, num_instr;
    uint8_t orders[256];
    int rows[256];
    cell *pat[256];                  /* rows * num_ch, NULL = empty */
    ins_t *ins[128];
} song_t;

static void errf(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static uint16_t rd16be(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void copy_name(char *dst, const uint8_t *src, int n)
{
    int i;
    for (i = 0; i < n; i++)
        dst[i] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : (src[i] ? ' ' : 0);
    dst[n] = 0;
    for (i = 0; i < n && dst[i]; i++)
        ;
    while (i > 0 && dst[i - 1] == ' ')
        dst[--i] = 0;                /* trailing spaces */
}

static void free_song(song_t *s)
{
    int i, j;
    for (i = 0; i < 256; i++)
        free(s->pat[i]);
    for (i = 0; i < 128; i++) {
        if (!s->ins[i])
            continue;
        for (j = 0; j < 16; j++) {
            free(s->ins[i]->smp[j].pcm8);
            free(s->ins[i]->smp[j].pcm16);
        }
        free(s->ins[i]);
    }
}

/* ------------------------------------------------------------------------- XM --- */

typedef struct { const uint8_t *p; size_t n, pos; } rd_t;

static int need(rd_t *r, size_t k) { return r->pos + k <= r->n; }

static int xm_patterns(song_t *s, rd_t *r, int version, char *err, size_t en)
{
    int i;
    for (i = 0; i < s->num_patterns; i++) {
        uint32_t hsize;
        int rows, ch, row;
        uint16_t dsize;
        const uint8_t *src, *end;
        if (!need(r, 9))
            goto corrupt;
        hsize = rd32(r->p + r->pos);
        if (version == 0x0102) {
            rows = r->p[r->pos + 5] + 1;
            dsize = rd16(r->p + r->pos + 6);
            r->pos += hsize > 8 ? hsize : 8;
        } else {
            rows = rd16(r->p + r->pos + 5);
            dsize = rd16(r->p + r->pos + 7);
            r->pos += hsize > 9 ? hsize : 9;
        }
        if (rows > FMT_MAX_ROWS)
            rows = FMT_MAX_ROWS;
        if (rows <= 0)
            rows = 64;
        s->rows[i] = rows;
        if (!need(r, dsize))
            goto corrupt;
        if (dsize > 0) {
            int empty = 1, k;
            cell *c = calloc((size_t)rows * (size_t)s->num_ch, sizeof(cell));
            src = r->p + r->pos;
            end = src + dsize;
            for (row = 0; row < rows && src < end; row++) {
                for (ch = 0; ch < s->num_ch && src < end; ch++) {
                    cell x = {0, 0, 0, 0, 0};
                    uint8_t b = *src++;
                    if (b & 0x80) {
                        if ((b & 1) && src < end) x.note = *src++;
                        if ((b & 2) && src < end) x.instr = *src++;
                        if ((b & 4) && src < end) x.vol = *src++;
                        if ((b & 8) && src < end) x.efx = *src++;
                        if ((b & 16) && src < end) x.efx_data = *src++;
                    } else {
                        x.note = b;
                        if (src + 4 <= end) {
                            x.instr = src[0];
                            x.vol = src[1];
                            x.efx = src[2];
                            x.efx_data = src[3];
                        }
                        src += 4;
                    }
                    c[row * s->num_ch + ch] = x;
                }
            }
            for (k = 0; k < rows * s->num_ch; k++)
                if (c[k].note || c[k].instr || c[k].vol || c[k].efx || c[k].efx_data)
                    empty = 0;
            if (empty) {
                free(c);
                s->rows[i] = 64;     /* ft2-clone: an empty pattern is 64 rows */
            } else {
                s->pat[i] = c;
            }
        } else {
            s->rows[i] = 64;
        }
        r->pos += dsize;
    }
    return 0;
corrupt:
    errf(err, en, "XM: pattern data is corrupt");
    return -1;
}

/* instrument header + sample headers; sample data is read by xm_sample_data */
static int xm_instr_header(song_t *s, rd_t *r, int n, uint32_t *extra, char *err, size_t en)
{
    uint8_t ih[263];
    uint32_t isize, rsize;
    int16_t nsmp;
    ins_t *in;
    int j, keep;
    memset(ih, 0, sizeof ih);
    memset(extra, 0, 16 * sizeof(uint32_t));
    if (!need(r, 4))
        goto corrupt;
    isize = rd32(r->p + r->pos);
    rsize = (isize == 0 || isize > 263) ? 263 : isize;
    if (!need(r, rsize))
        goto corrupt;
    memcpy(ih, r->p + r->pos, rsize);
    r->pos += rsize;
    if (isize > 263)
        r->pos += isize - 263;
    nsmp = (int16_t)rd16(ih + 27);
    if (nsmp < 0 || nsmp > 32) {
        errf(err, en, "XM: instrument %d is corrupt", n + 1);
        return -1;
    }
    if (nsmp == 0) {
        if (n < 128) {
            in = calloc(1, sizeof *in);   /* keep the name only; no samples = FT2's empty instrument */
            copy_name(in->name, ih + 4, 22);
            s->ins[n] = in;
        }
        return 0;
    }
    in = calloc(1, sizeof *in);
    copy_name(in->name, ih + 4, 22);
    in->num_samples = nsmp;
    memcpy(in->h.note2sample, ih + 33, 96);
    for (j = 0; j < 12; j++) {
        in->h.vol_env[j][0] = (int16_t)rd16(ih + 129 + j * 4);
        in->h.vol_env[j][1] = (int16_t)rd16(ih + 131 + j * 4);
        in->h.pan_env[j][0] = (int16_t)rd16(ih + 177 + j * 4);
        in->h.pan_env[j][1] = (int16_t)rd16(ih + 179 + j * 4);
    }
    in->h.vol_env_len = ih[225];
    in->h.pan_env_len = ih[226];
    in->h.vol_sus = ih[227];
    in->h.vol_loop_start = ih[228];
    in->h.vol_loop_end = ih[229];
    in->h.pan_sus = ih[230];
    in->h.pan_loop_start = ih[231];
    in->h.pan_loop_end = ih[232];
    in->h.vol_flags = ih[233];
    in->h.pan_flags = ih[234];
    in->h.vib_type = ih[235];
    in->h.vib_sweep = ih[236];
    in->h.vib_depth = ih[237];
    in->h.vib_rate = ih[238];
    in->h.fadeout = rd16(ih + 239);
    in->h.mute = ih[247] == 1;
    keep = nsmp > 16 ? 16 : nsmp;
    for (j = 0; j < 16; j++) {       /* allocated slots: volume 64, centre (ft2-clone allocateInstr) */
        in->smp[j].volume = 64;
        in->smp[j].panning = 128;
    }
    for (j = 0; j < nsmp; j++) {
        const uint8_t *h;
        if (!need(r, 40)) {
            free(in);
            goto corrupt;
        }
        h = r->p + r->pos;
        r->pos += 40;
        if (j >= 16) {
            extra[j - 16] = rd32(h);
            continue;
        }
        in->smp[j].length = (int32_t)rd32(h);
        in->smp[j].loop_start = (int32_t)rd32(h + 4);
        in->smp[j].loop_length = (int32_t)rd32(h + 8);
        in->smp[j].volume = h[12];
        in->smp[j].finetune = (int8_t)h[13];
        in->smp[j].flags = h[14];
        in->smp[j].panning = h[15];
        in->smp[j].relative_note = (int8_t)h[16];
        if (h[17] == 0xAD && !(h[14] & (16 | 32)))
            in->smp[j].flags |= 64;  /* ModPlug 4-bit ADPCM */
        copy_name(in->smp[j].name, h + 18, 22);
    }
    in->num_samples = keep;
    if (n < 128)
        s->ins[n] = in;
    else
        free(in);
    return 0;
corrupt:
    errf(err, en, "XM: instrument %d is truncated", n + 1);
    return -1;
}

static int xm_sample_data(song_t *s, rd_t *r, int n, const uint32_t *extra, int nsmp_file)
{
    ins_t *in = n < 128 ? s->ins[n] : NULL;
    int j;
    if (!in || !in->num_samples)
        return 0;
    for (j = 0; j < in->num_samples; j++) {
        smp_t *m = &in->smp[j];
        int is16 = (m->flags & 16) != 0, stereo = (m->flags & 32) != 0, adpcm = (m->flags & 64) != 0;
        int32_t in_file = m->length, i;
        if (m->length <= 0) {
            m->length = m->loop_start = m->loop_length = 0;
            m->flags = 0;
            continue;
        }
        if (is16) {
            m->length >>= 1;
            m->loop_start >>= 1;
            m->loop_length >>= 1;
        }
        if (adpcm) {
            size_t bytes = 16 + (size_t)(m->length + 1) / 2, k;
            int8_t lut[16], cur = 0;
            int8_t *d = calloc((size_t)m->length + 2, 1);
            const uint8_t *src = r->p + r->pos;
            if (!need(r, bytes))
                bytes = r->n - r->pos;
            memcpy(lut, src, bytes >= 16 ? 16 : bytes);
            for (k = 0; k + 16 < bytes && (int32_t)(2 * k) < m->length; k++) {
                uint8_t nib = src[16 + k];
                cur = (int8_t)(cur + lut[nib & 0x0F]);
                d[2 * k] = cur;
                cur = (int8_t)(cur + lut[nib >> 4]);
                d[2 * k + 1] = cur;
            }
            m->pcm8 = d;
            r->pos += bytes;
        } else {
            size_t bytes = (size_t)in_file;
            size_t avail = r->pos + bytes <= r->n ? bytes : r->n - r->pos;
            if (is16) {
                int16_t *d = calloc((size_t)m->length, 2);
                for (i = 0; i < m->length && (size_t)(2 * i + 1) < avail; i++)
                    d[i] = (int16_t)rd16(r->p + r->pos + 2 * i);
                if (stereo) {
                    int32_t half = m->length >> 1;
                    int16_t ol = 0, orr = 0;
                    for (i = 0; i < half; i++) {
                        ol = (int16_t)(d[i] + ol);
                        orr = (int16_t)(d[half + i] + orr);
                        d[i] = (int16_t)((ol + orr) >> 1);
                    }
                    m->length = half;
                    m->loop_start >>= 1;
                    m->loop_length >>= 1;
                } else {
                    int16_t o = 0;
                    for (i = 0; i < m->length; i++) {
                        o = (int16_t)(d[i] + o);
                        d[i] = o;
                    }
                }
                m->pcm16 = d;
            } else {
                int8_t *d = calloc((size_t)m->length, 1);
                memcpy(d, r->p + r->pos, avail < (size_t)m->length ? avail : (size_t)m->length);
                if (stereo) {
                    int32_t half = m->length >> 1;
                    int8_t ol = 0, orr = 0;
                    for (i = 0; i < half; i++) {
                        ol = (int8_t)(d[i] + ol);
                        orr = (int8_t)(d[half + i] + orr);
                        d[i] = (int8_t)((ol + orr) >> 1);
                    }
                    m->length = half;
                    m->loop_start >>= 1;
                    m->loop_length >>= 1;
                } else {
                    int8_t o = 0;
                    for (i = 0; i < m->length; i++) {
                        o = (int8_t)(d[i] + o);
                        d[i] = o;
                    }
                }
                m->pcm8 = d;
            }
            r->pos += avail;
        }
        m->flags &= (uint8_t)~(32 | 64);
    }
    for (j = 16; j < nsmp_file; j++)
        r->pos += extra[j - 16];
    return 0;
}

static int load_xm(song_t *s, const uint8_t *f, size_t n, char *err, size_t en)
{
    rd_t r = {f, n, 0};
    uint16_t version, nord, nins_file;
    uint32_t hsize;
    int i;
    if (n < 336 || memcmp(f, "Extended Module: ", 17)) {
        errf(err, en, "not an XM file");
        return -1;
    }
    version = rd16(f + 58);
    if (version < 0x0102 || version > 0x0104) {
        errf(err, en, "XM version %x.%02x is not supported", version >> 8, version & 0xFF);
        return -1;
    }
    copy_name(s->name, f + 17, 20);
    copy_name(s->tracker, f + 38, 20);
    hsize = rd32(f + 60);
    nord = rd16(f + 64);
    s->loop_start = rd16(f + 66);
    s->num_ch = rd16(f + 68);
    s->num_patterns = rd16(f + 70);
    nins_file = rd16(f + 72);
    s->linear = rd16(f + 74) & 1;
    s->speed = rd16(f + 76);
    s->bpm = rd16(f + 78);
    if (nord > 256 || s->num_patterns > 256 || s->num_ch == 0 || nins_file > 256) {
        errf(err, en, "XM header is corrupt");
        return -1;
    }
    if (s->num_ch > FMT_MAX_CHANNELS)
        s->num_ch = FMT_MAX_CHANNELS;  /* (the pattern reader would need to skip the rest: rare) */
    s->song_length = nord ? nord : 1;
    if (nord)
        memcpy(s->orders, f + 80, nord);
    for (i = 255; i >= 0; i--) {     /* order list padded with 0xFF */
        if (s->orders[i] != 0xFF)
            break;
        if (s->song_length > i)
            s->song_length = i;
    }
    if (s->song_length > 255)
        s->song_length = 255;
    if (s->song_length < 1)
        s->song_length = 1;
    s->num_instr = nins_file > 128 ? 128 : nins_file;
    r.pos = 60 + hsize;
    if (version < 0x0104) {
        uint32_t extra[128][16];
        int16_t ns[256];
        for (i = 0; i < nins_file; i++) {
            if (xm_instr_header(s, &r, i, i < 128 ? extra[i] : extra[0], err, en))
                return -1;
            ns[i] = i < 128 && s->ins[i] ? (int16_t)s->ins[i]->num_samples : 0;
        }
        if (xm_patterns(s, &r, version, err, en))
            return -1;
        for (i = 0; i < nins_file && i < 128; i++)
            xm_sample_data(s, &r, i, extra[i], ns[i]);
    } else {
        if (xm_patterns(s, &r, version, err, en))
            return -1;
        for (i = 0; i < nins_file; i++) {
            uint32_t extra[16];
            size_t at = r.pos;
            int nsmp_file;
            if (xm_instr_header(s, &r, i, extra, err, en))
                return -1;
            nsmp_file = (int16_t)rd16(f + at + 27);
            if (i >= 128) {          /* skip the data of instruments FT2 discards */
                continue;
            }
            xm_sample_data(s, &r, i, extra, nsmp_file);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ MOD --- */

static void mod_effect(cell *p, int nt)
{
    if (p->efx == 0xC) {
        if (p->efx_data > 64)
            p->efx_data = 64;
    } else if (p->efx == 0x1 || p->efx == 0x2 || p->efx == 0xA) {
        if (p->efx_data == 0)
            p->efx = 0;
    } else if (p->efx == 0x5) {
        if (p->efx_data == 0)
            p->efx = 0x3;
    } else if (p->efx == 0x6) {
        if (p->efx_data == 0)
            p->efx = 0x4;
    } else if (p->efx == 0xE) {
        if (p->efx_data == 0x10 || p->efx_data == 0x20 || p->efx_data == 0xA0 || p->efx_data == 0xB0)
            p->efx = p->efx_data = 0;
    }
    if (nt) {                        /* NoiseTracker: Dxx = D00, F00 does nothing */
        if (p->efx == 0xD)
            p->efx_data = 0;
        if (p->efx == 0xF && p->efx_data == 0)
            p->efx = 0;
    }
}

static int load_mod(song_t *s, const uint8_t *f, size_t n, char *err, size_t en)
{
    const char *id = (const char *)f + 1080;
    int nch = 0, nt = 0, i, npat = 0, row, ch;
    size_t pos;
    if (n < 1084) {
        errf(err, en, "not a MOD file");
        return -1;
    }
    if (!memcmp(id, "M.K.", 4) || !memcmp(id, "M!K!", 4) || !memcmp(id, "NSMS", 4) ||
        !memcmp(id, "LARD", 4) || !memcmp(id, "PATT", 4) || !memcmp(id, "FLT4", 4) || !memcmp(id, "EXO4", 4))
        nch = 4;
    else if (!memcmp(id, "N.T.", 4))
        nch = 4, nt = 1;
    else if (isdigit((unsigned char)id[0]) && !memcmp(id + 1, "CHN", 3))
        nch = id[0] - '0';
    else if (isdigit((unsigned char)id[0]) && isdigit((unsigned char)id[1]) && id[2] == 'C' &&
             (id[3] == 'H' || id[3] == 'N'))
        nch = (id[0] - '0') * 10 + (id[1] - '0');
    else if (!memcmp(id, "CD81", 4) || !memcmp(id, "OKTA", 4) || !memcmp(id, "OCTA", 4))
        nch = 8;
    else if (!memcmp(id, "FA0", 3) && id[3] >= '4' && id[3] <= '8')
        nch = id[3] - '0';
    if (nch <= 0 || nch > FMT_MAX_CHANNELS) {
        errf(err, en, "MOD type '%.4s' is not supported", id);
        return -1;
    }
    s->from_mod = 1;
    s->linear = 0;
    s->num_ch = nch;
    s->speed = 6;
    s->bpm = 125;
    copy_name(s->name, f, 20);
    snprintf(s->tracker, sizeof s->tracker, "MOD %.4s", id);
    s->song_length = f[950];
    s->loop_start = f[951];
    if (s->song_length == 129 && nch == 4)
        s->song_length = 127;
    if (s->song_length < 1 || s->song_length > 128) {
        errf(err, en, "MOD order list is corrupt");
        return -1;
    }
    memcpy(s->orders, f + 952, 128);
    for (i = 0; i < 128; i++)
        if (s->orders[i] > npat)
            npat = s->orders[i];
    npat++;
    s->num_patterns = npat;
    pos = 1084;
    for (i = 0; i < npat; i++) {
        cell *c = calloc((size_t)64 * (size_t)nch, sizeof(cell));
        s->rows[i] = 64;
        for (row = 0; row < 64; row++)
            for (ch = 0; ch < nch; ch++, pos += 4) {
                cell *p = &c[row * nch + ch];
                uint16_t period;
                int note;
                if (pos + 4 > n)
                    continue;
                period = (uint16_t)(((f[pos] & 0x0F) << 8) | f[pos + 1]);
                for (note = 0; note < 8 * 12; note++)
                    if (period >= ft2_mod_periods[note]) {
                        p->note = (uint8_t)(note + 1);
                        break;
                    }
                p->instr = (uint8_t)((f[pos] & 0xF0) | (f[pos + 2] >> 4));
                p->efx = f[pos + 2] & 0x0F;
                p->efx_data = f[pos + 3];
                mod_effect(p, nt);
            }
        s->pat[i] = c;
    }
    s->num_instr = 31;
    for (i = 0; i < 31; i++) {
        const uint8_t *h = f + 20 + i * 30;
        ins_t *in = calloc(1, sizeof *in);
        smp_t *m = &in->smp[0];
        int j;
        copy_name(in->name, h, 22);
        s->ins[i] = in;
        if (rd16be(h + 22) == 0)
            continue;                /* no sample: FT2's empty instrument */
        in->num_samples = 1;
        for (j = 0; j < 16; j++) {
            in->smp[j].volume = 64;
            in->smp[j].panning = 128;
        }
        strcpy(m->name, in->name);
        m->length = 2 * rd16be(h + 22);
        m->finetune = (int8_t)((h[24] & 0x0F) << 4);
        m->volume = h[25];
        m->loop_start = 2 * rd16be(h + 26);
        m->loop_length = 2 * rd16be(h + 28);
        if (m->loop_length > 2 && m->loop_start + m->loop_length > m->length &&
            (m->loop_start >> 1) + m->loop_length <= m->length)
            m->loop_start >>= 1;     /* badly converted STK */
        if (m->loop_start + m->loop_length > m->length) {
            if (m->loop_start >= m->length)
                m->loop_start = m->loop_length = 0;
            else
                m->loop_length = m->length - m->loop_start;
        }
        if (m->loop_start + m->loop_length > 2)
            m->flags |= FMT_LOOP_FWD;
        else
            m->loop_start = m->loop_length = 0;
        m->pcm8 = calloc((size_t)m->length, 1);
        if (pos < n)
            memcpy(m->pcm8, f + pos, n - pos < (size_t)m->length ? n - pos : (size_t)m->length);
        pos += (size_t)m->length;
    }
    return 0;
}

/* -------------------------------------------------------------------- writer --- */

typedef struct { uint8_t *b; size_t n, cap; } buf_t;

static size_t put(buf_t *b, const void *p, size_t k)
{
    size_t at = b->n;
    if (b->n + k > b->cap) {
        while (b->n + k > b->cap)
            b->cap = b->cap ? b->cap * 2 : 65536;
        b->b = realloc(b->b, b->cap);
    }
    if (p)
        memcpy(b->b + b->n, p, k);
    else
        memset(b->b + b->n, 0, k);
    b->n += k;
    return at;
}

static void align4(buf_t *b)
{
    while (b->n & 3)
        put(b, NULL, 1);
}

static size_t pack_pattern(buf_t *b, const cell *c, int rows, int nch, uint16_t *row_off)
{
    size_t start = b->n;
    int r, ch;
    for (r = 0; r < rows; r++) {
        row_off[r] = (uint16_t)(b->n - start);
        for (ch = 0; ch < nch; ch++) {
            const cell *x = &c[r * nch + ch];
            uint8_t m = (uint8_t)((x->note ? 1 : 0) | (x->instr ? 2 : 0) | (x->vol ? 4 : 0) |
                                  (x->efx ? 8 : 0) | (x->efx_data ? 16 : 0));
            if (m == 31) {
                put(b, x, 5);
            } else {
                uint8_t mb = 0x80 | m;
                put(b, &mb, 1);
                if (m & 1) put(b, &x->note, 1);
                if (m & 2) put(b, &x->instr, 1);
                if (m & 4) put(b, &x->vol, 1);
                if (m & 8) put(b, &x->efx, 1);
                if (m & 16) put(b, &x->efx_data, 1);
            }
        }
    }
    return b->n - start;
}

static void sanitize_sample(smp_t *m)
{
    if ((m->flags & 3) == 3)
        m->flags &= (uint8_t)~FMT_LOOP_FWD;
    if (m->volume > 64)
        m->volume = 64;
    if (m->relative_note < -48)
        m->relative_note = -48;
    if (m->relative_note > 71)
        m->relative_note = 71;
    if (m->length < 0)
        m->length = 0;
    if (m->loop_start < 0 || m->loop_length <= 0 || m->loop_start + m->loop_length > m->length) {
        m->loop_start = m->loop_length = 0;
        m->flags &= (uint8_t)~3;
    }
}

static void sanitize_instr(fmt_instr *h)
{
    int i;
    if (h->vib_depth > 0x0F) h->vib_depth = 0x0F;
    if (h->vib_rate > 0x3F) h->vib_rate = 0x3F;
    if (h->vib_type > 3) h->vib_type = 0;
    for (i = 0; i < 96; i++)
        if (h->note2sample[i] >= 16)
            h->note2sample[i] = 15;
    if (h->vol_env_len > 12) h->vol_env_len = 12;
    if (h->vol_loop_start > 11) h->vol_loop_start = 11;
    if (h->vol_loop_end > 11) h->vol_loop_end = 11;
    if (h->vol_sus > 11) h->vol_sus = 11;
    if (h->pan_env_len > 12) h->pan_env_len = 12;
    if (h->pan_loop_start > 11) h->pan_loop_start = 11;
    if (h->pan_loop_end > 11) h->pan_loop_end = 11;
    if (h->pan_sus > 11) h->pan_sus = 11;
    for (i = 0; i < 12; i++) {
        if ((uint16_t)h->vol_env[i][0] > 32767) h->vol_env[i][0] = 32767;
        if ((uint16_t)h->pan_env[i][0] > 32767) h->pan_env[i][0] = 32767;
        if ((uint16_t)h->vol_env[i][1] > 64) h->vol_env[i][1] = 64;
        if ((uint16_t)h->pan_env[i][1] > 63) h->pan_env[i][1] = 63;
    }
}

static int to_8bit;                /* modconv_8bit: 16-bit samples stored as 8-bit */

static int32_t smp_at(const smp_t *m, int32_t i)
{
    if (i < 0 || i >= m->length)
        return 0;
    return m->pcm16 ? m->pcm16[i] : m->pcm8[i];
}

static int32_t smp_at8(const smp_t *m, int32_t i)   /* (a 16-bit sample to 8 bits, rounded) */
{
    int32_t v = smp_at(m, i);
    if (!m->pcm16)
        return v;
    v = (v + 128) >> 8;
    return v > 127 ? 127 : v;
}

static int write_image(song_t *s, uint8_t **out, size_t *out_size)
{
    buf_t b = {0, 0, 0};
    fmt_header h;
    fmt_pattern pt[256];
    fmt_instr fi[128];
    fmt_sample fs[128 * 16];
    int i, j, nsmp = 0, ch;
    size_t hdr_at, ord_at, pat_at, ins_at, smp_at_off;

    memset(&h, 0, sizeof h);
    memset(pt, 0, sizeof pt);
    memset(fi, 0, sizeof fi);
    memset(fs, 0, sizeof fs);
    /* clean the pattern data as ft2-clone does after loading */
    for (i = 0; i < s->num_patterns; i++) {
        if (!s->pat[i])
            continue;
        for (j = 0; j < s->rows[i] * s->num_ch; j++) {
            cell *c = &s->pat[i][j];
            if (c->note > 97) c->note = 0;
            if (c->instr > 128) c->instr = 0;
            if (c->efx > 35) c->efx = c->efx_data = 0;
        }
    }
    hdr_at = put(&b, NULL, sizeof h);
    ord_at = put(&b, s->orders, 256);
    align4(&b);
    pat_at = put(&b, NULL, sizeof(fmt_pattern) * (size_t)s->num_patterns);
    ins_at = put(&b, NULL, sizeof(fmt_instr) * (size_t)s->num_instr);
    /* count samples */
    for (i = 0; i < s->num_instr; i++)
        if (s->ins[i])
            nsmp += s->ins[i]->num_samples;
    smp_at_off = put(&b, NULL, sizeof(fmt_sample) * (size_t)nsmp);
    for (i = 0; i < s->num_patterns; i++) {
        uint16_t row_off[FMT_MAX_ROWS];
        pt[i].num_rows = (uint16_t)s->rows[i];
        if (s->pat[i]) {
            size_t data_at, bytes;
            align4(&b);
            data_at = b.n;
            bytes = pack_pattern(&b, s->pat[i], s->rows[i], s->num_ch, row_off);
            pt[i].data_off = (uint32_t)data_at;
            pt[i].data_bytes = (uint16_t)bytes;
            align4(&b);
            pt[i].rows_off = (uint32_t)put(&b, row_off, sizeof(uint16_t) * (size_t)s->rows[i]);
        }
    }
    nsmp = 0;
    for (i = 0; i < s->num_instr; i++) {
        ins_t *in = s->ins[i];
        if (!in)
            continue;
        fi[i] = in->h;
        memcpy(fi[i].name, in->name, sizeof in->name);
        fi[i].name[23] = 0;
        fi[i].num_samples = (uint8_t)in->num_samples;
        fi[i].first_sample = (uint16_t)nsmp;
        sanitize_instr(&fi[i]);
        for (j = 0; j < in->num_samples; j++, nsmp++) {
            smp_t *m = &in->smp[j];
            fmt_sample *o = &fs[nsmp];
            int is16 = m->pcm16 != NULL && !to_8bit, loop;
            int32_t stored, k, tail0, tail1;
            sanitize_sample(m);
            memcpy(o->name, m->name, sizeof m->name);
            o->flags = (uint8_t)((m->flags & 3) | (is16 ? FMT_16BIT : 0));
            o->volume = m->volume;
            o->finetune = m->finetune;
            o->relative_note = m->relative_note;
            o->panning = m->panning;
            o->loop_start = (uint32_t)m->loop_start;
            o->loop_length = (uint32_t)m->loop_length;
            loop = m->flags & 3;
            stored = loop ? m->loop_start + m->loop_length : m->length;
            o->length = (uint32_t)stored;
            if (stored <= 0 || (!m->pcm8 && !m->pcm16)) {
                o->length = 0;
                continue;
            }
            if (loop == FMT_LOOP_FWD) {
                tail0 = smp_at(m, m->loop_start);
                tail1 = smp_at(m, m->loop_start + 1 < stored ? m->loop_start + 1 : m->loop_start);
            } else if (loop == FMT_LOOP_PINGPONG) {
                tail0 = smp_at(m, stored - 1);
                tail1 = smp_at(m, stored - 2 >= m->loop_start ? stored - 2 : stored - 1);
            } else {
                tail0 = tail1 = 0;
            }
            align4(&b);
            if (is16) {
                int16_t v = (int16_t)smp_at(m, 0);
                put(&b, NULL, 2);    /* keep frame 0 4-aligned: pad, guard */
                put(&b, &v, 2);
                o->data_off = (uint32_t)b.n;
                for (k = 0; k < stored; k++) {
                    v = (int16_t)smp_at(m, k);
                    put(&b, &v, 2);
                }
                v = (int16_t)tail0;
                put(&b, &v, 2);
                v = (int16_t)tail1;
                put(&b, &v, 2);
            } else {
                int8_t v = (int8_t)smp_at8(m, 0);
                if (m->pcm16) {    /* (to_8bit: the tail too) */
                    tail0 = (tail0 + 128) >> 8 > 127 ? 127 : (tail0 + 128) >> 8;
                    tail1 = (tail1 + 128) >> 8 > 127 ? 127 : (tail1 + 128) >> 8;
                }
                put(&b, NULL, 3);
                put(&b, &v, 1);
                o->data_off = (uint32_t)b.n;
                for (k = 0; k < stored; k++) {
                    v = (int8_t)smp_at8(m, k);
                    put(&b, &v, 1);
                }
                v = (int8_t)tail0;
                put(&b, &v, 1);
                v = (int8_t)tail1;
                put(&b, &v, 1);
            }
        }
    }
    align4(&b);
    h.magic = FMT_MAGIC;
    h.version = FMT_VERSION;
    h.size = (uint32_t)b.n;
    snprintf(h.name, sizeof h.name, "%s", s->name);
    snprintf(h.tracker, sizeof h.tracker, "%s", s->tracker);
    h.flags = (uint16_t)((s->linear ? FMT_LINEAR : 0) | (s->from_mod ? FMT_FROM_MOD : 0));
    ch = s->num_ch;
    h.num_channels = (uint8_t)ch;
    h.num_instr = (uint8_t)s->num_instr;
    h.song_length = (uint16_t)s->song_length;
    h.loop_start = (uint16_t)(s->loop_start < s->song_length ? s->loop_start : 0);
    h.speed = (uint16_t)s->speed;
    h.bpm = (uint16_t)s->bpm;
    h.num_patterns = (uint16_t)s->num_patterns;
    h.num_samples = (uint16_t)nsmp;
    h.orders_off = (uint32_t)ord_at;
    h.patterns_off = (uint32_t)pat_at;
    h.instr_off = (uint32_t)ins_at;
    h.samples_off = (uint32_t)smp_at_off;
    memcpy(b.b + hdr_at, &h, sizeof h);
    memcpy(b.b + pat_at, pt, sizeof(fmt_pattern) * (size_t)s->num_patterns);
    memcpy(b.b + ins_at, fi, sizeof(fmt_instr) * (size_t)s->num_instr);
    memcpy(b.b + smp_at_off, fs, sizeof(fmt_sample) * (size_t)nsmp);
    *out = b.b;
    *out_size = b.n;
    return 0;
}

int modconv_8bit(const uint8_t *file, size_t size, uint8_t **out, size_t *out_size, char *err, size_t err_size)
{
    int r;
    to_8bit = 1;
    r = modconv(file, size, out, out_size, err, err_size);
    to_8bit = 0;
    return r;
}

int modconv(const uint8_t *file, size_t size, uint8_t **out, size_t *out_size, char *err, size_t err_size)
{
    song_t *s = calloc(1, sizeof *s);
    int r;
    if (size >= 17 && !memcmp(file, "Extended Module: ", 17))
        r = load_xm(s, file, size, err, err_size);
    else
        r = load_mod(s, file, size, err, err_size);
    if (!r)
        r = write_image(s, out, out_size);
    free_song(s);
    free(s);
    return r;
}
