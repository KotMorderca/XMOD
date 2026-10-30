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
#include "../firmware/layout.h"

static uint8_t flash[0x100000];
/* the slots' area (firmware/layout.h) */
#define IN_AREAS(off, n) ((off) >= XMOD_SONGS_LO && (off) + (n) <= XMOD_SONGS_HI)
static int stp_ok(void) { return 1; }
static int stp_read(uint32_t off, void *d, uint32_t n) { memcpy(d, flash + off, n); return 0; }
static int stp_erase(uint32_t off)
{
    if (off & 0xFFFu || !IN_AREAS(off, 0x1000u)) {
        printf("bad erase %x\n", off);
        exit(1);
    }
    memset(flash + off, 0xFF, 0x1000);
    return 0;
}
static int stp_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (!IN_AREAS(off, n) || n > 256u || (off & 0xFFu) + n > 256u) {
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

static int rooms;
static uint32_t state_want;

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
    {                                              /* the tail as after an update from a bigger app: */
        uint32_t k, x = 12345u;                    /* not erased, no slot in it */
        for (k = XMOD_SONGS_LO; k < XMOD_APP_AREA_HI; k++) {
            x = x * 1103515245u + 12345u;
            flash[k] = (uint8_t)(x >> 16);
        }
    }
    store_scan();
    if (store_n) {
        printf("old app code in the tail read as a slot\n");
        return 1;
    }
    {   /* another firmware's slot (xm4midi: the same slot header, an FMTK image of another version) */
        uint32_t off = 0x97000u, n = img_n[0];
        slot_hdr h;
        uint8_t *b = malloc(n);
        memcpy(b, img[0], n);
        ((fmt_header *)b)->version = FMT_VERSION + 1u;
        memset(&h, 0, sizeof h);
        h.magic = SLOT_MAGIC;
        h.version = SLOT_VERSION;
        h.size = n;
        h.crc = store_crc(0, b, n);
        h.seq = 7;
        strcpy(h.name, "foreign");
        h.hcrc = store_crc(0, (const uint8_t *)&h, 44);
        memcpy(flash + off, &h, sizeof h);
        memcpy(flash + off + SLOT_HDR, b, n);
        free(b);
        store_scan();
        if (store_n) {
            printf("a slot with another FMTK version read as a song\n");
            return 1;
        }
    }
    {   /* the state sector: none at first; garbage in it is no state; records; a full sector wraps */
        uint32_t b, sg, k;
        if (state_get(&b, &sg) != 0) {
            printf("state read from an erased sector\n");
            return 1;
        }
        memset(flash + STATE_OFF + 100, 0x5A, 40);
        if (state_get(&b, &sg) != 0 || state_put(11, 22) || state_get(&b, &sg) != 1 || b != 11 || sg != 22) {
            printf("state over garbage: %u %u\n", b, sg);
            return 1;
        }
        for (k = 0; k < 600u; k++)
            if (state_put(11, k)) {
                printf("state write %u failed\n", k);
                return 1;
            }
        if (state_get(&b, &sg) != 1 || b != 11 || sg != 599) {
            printf("state after wrapping: %u %u\n", b, sg);
            return 1;
        }
        state_put(5, 0x80000001u);
    }
    {   /* a slot an older XMOD put over the state sector: it stays, there is no state */
        uint32_t off = STATE_OFF - 0x3000u, n = img_n[0], b, sg;
        int err;
        static uint8_t keep[0x5000];
        memcpy(keep, flash + off, sizeof keep);
        memset(flash + off, 0xFF, sizeof keep);
        if (n < 0x3000u) {
            printf("song 0 is too small for this test\n");
            return 1;
        }
        put_w.base = off;                          /* (as store_put_begin would, at that place) */
        put_w.pos = SLOT_HDR;
        put_w.crc = 0;
        put_w.err = 0;
        put_size = n;
        memset(put_name, 0, sizeof put_name);
        memset(store_buf, 0xFF, SLOT_HDR);
        store_put_data(0, img[0], n);
        store_put_end(0, 0, &err);
        if (err || store_n != 1 || state_get(&b, &sg) != -1 || state_put(1, 2) != -5 || store_n != 1) {
            printf("slot over the state sector: err %d, %d slots\n", err, store_n);
            return 1;
        }
        memcpy(flash + off, keep, sizeof keep);
        store_scan();
        if (store_n || state_get(&b, &sg) != 1 || b != 5 || sg != 0x80000001u) {
            printf("state lost\n");
            return 1;
        }
    }
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
            {
                uint32_t room = store_room();     /* (what the song manager is told) */
                off = store_save("t", replace, moved, &err);
                if (!replace && (off != 0) != (sz <= room)) {
                    printf("step %d: store_room %u, size %u, saved %x err %d\n", step, room, sz, off, err);
                    return 1;
                }
                rooms++;
            }
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
        if (rnd() % 8 == 0) {                      /* the state, between the songs */
            state_want = rnd();
            if (state_put(3, state_want)) {
                printf("step %d: state write failed\n", step);
                return 1;
            }
        }
        store_scan();
        if (check(step))
            return 1;
        {
            uint32_t b, sg;
            int k;
            for (k = 0; k < store_n; k++)
                if (store_slots[k].off + slot_sectors(store_slots[k].size) * STORE_SECT > STATE_OFF) {
                    printf("step %d: a slot over the state sector\n", step);
                    return 1;
                }
            if (state_want && (state_get(&b, &sg) != 1 || b != 3 || sg != state_want)) {
                printf("step %d: state lost\n", step);
                return 1;
            }
        }
    }
    printf("ok: 3000 steps, %d saves, %d with no room, %d slots at the end, %d slot moves (compacting), %d renames\n",
           saves, full, store_n, moves, renames);
    return 0;
}
