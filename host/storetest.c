/* SPDX-License-Identifier: GPL-3.0-only */
/* firmware/store.c on a simulated NOR flash: random loads, edits, saves (new, replacing) and
 * deletes; after each step every slot must hold exactly what was saved into it, and the song loaded
 * from a slot must keep its samples when the slots move (compacting).
 *
 *   storetest SONG.xm [SONG.xm ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "song.h"
#include "modconv.h"

static uint8_t flash[0x100000];
static int stp_ok(void) { return 1; }
static int stp_read(uint32_t off, void *d, uint32_t n) { memcpy(d, flash + off, n); return 0; }
static int stp_erase(uint32_t off)
{
    if (off & 0xFFFu || off < 0x97000u || off >= 0xE0000u) {
        printf("bad erase %x\n", off);
        exit(1);
    }
    memset(flash + off, 0xFF, 0x1000);
    return 0;
}
static int stp_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (off < 0x97000u || off + n > 0xE0000u || n > 256u || (off & 0xFFu) + n > 256u) {
        printf("bad prog %x %u\n", off, n);
        exit(1);
    }
    for (i = 0; i < n; i++)
        flash[off + i] &= ((const uint8_t *)src)[i];
    return 0;
}
static const uint8_t *stp_ptr(uint32_t off) { return flash + off; }
static void stp_idle(void) {}

#include "../firmware/store.c"

#define NS 8
static uint8_t *img[NS];
static uint32_t img_n[NS];
static int nimg;

/* what each slot should hold, by header offset */
static struct { uint32_t off, size; uint8_t *data; } want[STORE_MAX * 4];
static int nwant;
static uint32_t src_off;           /* the slot the song's samples come from, 0: a file image */

static uint32_t rnd(void)
{
    static uint32_t s = 2463534242u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

typedef struct { uint8_t *b; uint32_t n; } buf_t;
static int sink(void *ctx, const void *d, uint32_t n)
{
    buf_t *s = ctx;
    if (d)
        memcpy(s->b + s->n, d, n);
    else
        memset(s->b + s->n, 0, n);
    s->n += n;
    return 0;
}

static int moves;
static void moved(uint32_t from, uint32_t to)
{
    int k;
    moves++;
    for (k = 0; k < nwant; k++)
        if (want[k].off == from)
            want[k].off = to;
    if (src_off == from) {
        src_off = to;
        song_rebind(flash + to + SLOT_HDR, ((const fmt_header *)(flash + to + SLOT_HDR))->size);
    }
}

static int check(int step)
{
    int i, k;
    if (store_n != nwant) {
        printf("step %d: %d slots, expected %d\n", step, store_n, nwant);
        return 1;
    }
    for (k = 0; k < nwant; k++) {
        i = store_find(want[k].off);
        if (i < 0 || store_slots[i].size != want[k].size || memcmp(store_image(i), want[k].data, want[k].size) ||
            !store_check(i)) {
            printf("step %d: slot at %x wrong\n", step, want[k].off);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i, step, saves = 0, full = 0, renames = 0;
    for (i = 1; i < argc && nimg < NS; i++) {
        FILE *f = fopen(argv[i], "rb");
        long len;
        uint8_t *file;
        size_t n, out_n;
        char err[128];
        if (!f)
            return 2;
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        file = malloc((size_t)len);
        n = fread(file, 1, (size_t)len, f);
        fclose(f);
        if (modconv(file, n, &img[nimg], &out_n, err, sizeof err))
            return 2;
        img_n[nimg++] = (uint32_t)out_n;
    }
    memset(flash, 0xFF, sizeof flash);
    store_scan();
    song_load(img[0], img_n[0]);
    for (step = 0; step < 3000; step++) {
        uint32_t op = rnd() % 100;
        if (op < 15) {                             /* load a file, or a slot */
            if (store_n && rnd() % 2) {
                int k = (int)(rnd() % (uint32_t)store_n);
                if (song_load(store_image(k), store_slots[k].size)) {
                    printf("step %d: slot does not load\n", step);
                    return 1;
                }
                src_off = store_slots[k].off;
            } else {
                int k = (int)(rnd() % (uint32_t)nimg);
                song_load(img[k], img_n[k]);
                src_off = 0;
            }
        } else if (op < 55) {                      /* edits */
            int k;
            for (k = 0; k < 20; k++) {
                uint32_t p = rnd() % song.hdr.num_patterns;
                song_cell *c;
                if (song_edit(p))
                    break;
                c = song_cell_at(p, rnd() % song_rows(p), rnd() % song.hdr.num_channels);
                if (c)
                    c->note = (uint8_t)(1 + rnd() % 96);
            }
            if (rnd() % 4 == 0)
                song_set_rows(rnd() % song.hdr.num_patterns, 1 + rnd() % 256);
        } else if (op < 85) {                      /* save: replacing the source slot, or new */
            uint32_t replace = (src_off && rnd() % 2) ? src_off : 0, off, sz;
            int err, k = -1;
            buf_t b = {0, 0};

            song_save(NULL, NULL, &sz);
            b.b = malloc(sz);
            song_save(sink, &b, NULL);
            off = store_save("t", replace, moved, &err);
            if (getenv("V")) {
                int q;
                printf("%d save replace=%x size=%u -> %x err %d src %x; slots:", step, replace, sz, off, err, src_off);
                for (q = 0; q < store_n; q++)
                    printf(" %x+%u", store_slots[q].off, store_slots[q].size);
                printf("\n");
            }
            if (!off) {
                free(b.b);
                if (err != -3) {
                    printf("step %d: save error %d\n", step, err);
                    return 1;
                }
                full++;
            } else {
                saves++;
                if (replace)                       /* (src_off followed it if it moved) */
                    for (k = 0; k < nwant; k++)
                        if (want[k].off == src_off) {
                            free(want[k].data);
                            want[k] = want[--nwant];
                            break;
                        }
                want[nwant].off = off;
                want[nwant].size = sz;
                want[nwant].data = b.b;
                nwant++;
                /* the song now reads its samples from the new copy */
                song_rebind(flash + off + SLOT_HDR, sz);
                src_off = off;

            }
        } else if (store_n && op < 92) {           /* rename one (the source too: moved() follows) */
            int k = (int)(rnd() % (uint32_t)store_n), err;
            char nm[24];
            uint32_t off;
            snprintf(nm, sizeof nm, "n%u", (unsigned)(rnd() % 1000u));
            off = store_rename(k, nm, moved, &err);
            if (off) {
                renames++;
                if (strcmp(store_slots[store_find(off)].name, nm)) {
                    printf("step %d: rename lost the name\n", step);
                    return 1;
                }
            } else if (err != -3) {
                printf("step %d: rename error %d\n", step, err);
                return 1;
            }
        } else if (store_n) {                      /* delete one that is not the source */
            int k = (int)(rnd() % (uint32_t)store_n), w;
            if (store_slots[k].off != src_off) {
                uint32_t off = store_slots[k].off;
                if (getenv("V"))
                    printf("%d delete %x\n", step, store_slots[k].off);
                store_delete(k);
                for (w = 0; w < nwant; w++)
                    if (want[w].off == off) {
                        free(want[w].data);
                        want[w] = want[--nwant];
                        break;
                    }
            }
        }
        store_scan();
        if (check(step))
            return 1;
    }
    printf("ok: 3000 steps, %d saves, %d with no room, %d slots at the end, %d slot moves (compacting), %d renames\n",
           saves, full, store_n, moves, renames);
    return 0;
}
