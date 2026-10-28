/* SPDX-License-Identifier: GPL-3.0-only */
/* engine/song.c against a plain copy: random edits, pattern switches, resizes, copies and new
 * patterns, with a small arena so that it compacts and runs out often.
 *
 *   songtest SONG.xm [ROUNDS]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "song.h"
#include "modconv.h"

static song_cell ref[SONG_MAX_PATTERNS][SONG_MAX_ROWS][FMT_MAX_CHANNELS];
static uint16_t ref_rows[SONG_MAX_PATTERNS];
static uint32_t ref_npat;

static uint32_t rnd(void)
{
    static uint32_t s = 12345;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static int check(const char *when)
{
    uint32_t p, r, c, nch = song.hdr.num_channels;
    song_cell row[FMT_MAX_CHANNELS];
    if (song.hdr.num_patterns != ref_npat) {
        printf("%s: %u patterns, expected %u\n", when, song.hdr.num_patterns, ref_npat);
        return 1;
    }
    for (p = 0; p < ref_npat; p++) {
        if (song_rows(p) != ref_rows[p]) {
            printf("%s: pattern %u has %u rows, expected %u\n", when, p, song_rows(p), ref_rows[p]);
            return 1;
        }
        for (r = 0; r < ref_rows[p]; r++) {
            song_read_row(p, r, row, nch);
            for (c = 0; c < nch; c++)
                if (memcmp(&row[c], &ref[p][r][c], sizeof row[c])) {
                    printf("%s: pattern %u row %u channel %u differs\n", when, p, r, c);
                    return 1;
                }
        }
    }
    return 0;
}

typedef struct { uint8_t *b; uint32_t n; } sink_t;
static int sink(void *ctx, const void *d, uint32_t n)
{
    sink_t *s = ctx;
    if (d)
        memcpy(s->b + s->n, d, n);
    else
        memset(s->b + s->n, 0, n);
    s->n += n;
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f;
    uint8_t *file, *img;
    size_t n, img_n;
    long len;
    char err[128];
    uint32_t p, r, c, i, rounds = argc > 2 ? (uint32_t)atoi(argv[2]) : 20000, oom = 0;
    if (argc < 2 || !(f = fopen(argv[1], "rb")))
        return 2;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc((size_t)len);
    n = fread(file, 1, (size_t)len, f);
    fclose(f);
    if (modconv(file, n, &img, &img_n, err, sizeof err)) {
        printf("%s\n", err);
        return 2;
    }
    if (song_load(img, (uint32_t)img_n)) {
        printf("song_load failed (arena %u B)\n", SONG_ARENA);
        return 1;
    }
    ref_npat = song.hdr.num_patterns;
    for (p = 0; p < ref_npat; p++) {
        ref_rows[p] = (uint16_t)song_rows(p);
        for (r = 0; r < ref_rows[p]; r++)
            song_read_row(p, r, ref[p][r], FMT_MAX_CHANNELS);
    }
    if (check("load"))
        return 1;
    for (i = 0; i < rounds; i++) {
        uint32_t op = rnd() % 100, nch = song.hdr.num_channels;
        p = rnd() % (ref_npat + 1u);
        if (p >= SONG_MAX_PATTERNS)
            p = 0;
        if (op < 80) {                             /* a cell */
            song_cell *e;
            if (song_edit(p)) {
                oom++;
                continue;
            }
            if (p == ref_npat) {
                ref_rows[p] = 64;
                memset(ref[p], 0, sizeof ref[p]);
                ref_npat++;
            }
            r = rnd() % ref_rows[p];
            c = rnd() % nch;
            e = song_cell_at(p, r, c);
            if (!e) {
                printf("no cell %u %u %u\n", p, r, c);
                return 1;
            }
            if (rnd() % 4 == 0)
                memset(e, 0, sizeof *e);
            else {
                e->note = (uint8_t)(rnd() % 98);
                e->instr = (uint8_t)(rnd() % 3 ? rnd() % 128 : 0);
                e->vol = (uint8_t)(rnd() % 2 ? rnd() % 256 : 0);
                e->efx = (uint8_t)(rnd() % 2 ? rnd() % 36 : 0);
                e->efx_data = (uint8_t)(rnd() % 2 ? rnd() % 256 : 0);
            }
            ref[p][r][c] = *e;
        } else if (op < 88 && p < ref_npat) {     /* resize */
            uint32_t rows = 1 + rnd() % SONG_MAX_ROWS;
            if (song_set_rows(p, rows)) {
                oom++;
                continue;
            }
            for (r = ref_rows[p]; r < rows; r++)
                memset(ref[p][r], 0, sizeof ref[p][r]);
            for (r = rows; r < ref_rows[p]; r++)
                memset(ref[p][r], 0, sizeof ref[p][r]);
            ref_rows[p] = (uint16_t)rows;
        } else if (op < 93 && p < ref_npat && ref_npat < SONG_MAX_PATTERNS) {   /* copy to a new one */
            int q = song_new_pattern();
            if (q < 0 || (uint32_t)q != ref_npat) {
                printf("new pattern %d, expected %u\n", q, ref_npat);
                return 1;
            }
            if (song_copy_pattern(p, (uint32_t)q)) {
                song_set_num_patterns(ref_npat);
                oom++;
                continue;
            }
            memcpy(ref[q], ref[p], sizeof ref[q]);
            ref_rows[q] = ref_rows[p];
            ref_npat++;
        } else if (op < 96 && ref_npat > 1) {      /* drop the last */
            ref_npat--;
            song_set_num_patterns(ref_npat);
        } else if (song_flush()) {
            oom++;
        }
        if (i % 97 == 0 && check("edit"))
            return 1;
    }
    if (song_flush() == 0 && check("end"))
        return 1;
    {                                              /* saved and loaded again: the same */
        uint32_t sz;
        sink_t s = {0, 0};
        if (song_save(NULL, NULL, &sz) == 0) {
            s.b = malloc(sz);
            if (song_save(sink, &s, NULL) || s.n != sz || song_load(s.b, sz)) {
                printf("save / load failed\n");
                return 1;
            }
            if (check("saved"))
                return 1;
        }
    }
    printf("ok: %u rounds, %u out of memory, %u patterns, arena %u of %u B\n", rounds, oom, ref_npat,
           song_mem_used(), SONG_ARENA);
    return 0;
}
