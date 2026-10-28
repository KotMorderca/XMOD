/* SPDX-License-Identifier: GPL-3.0-only */
/* Song slots in flash. Portable: the FM-1 (ui.c, on Felucca's flash driver) and the simulator
 * (host/uisim.c, a NOR flash in RAM) give the stp_* functions below.
 *
 * The area 0x97000..0xE0000 (Felucca's data store; this firmware does not use Felucca's storage),
 * 73 sectors of 4 KiB. A slot starts on a sector: a 64-byte header, then the song as an FMTK image
 * (engine/song.c song_save), which the player reads in place through the XIP window.
 *
 * Writing a song: the image goes into free sectors first, its header last; only then is the old
 * copy marked deleted (its magic programmed to 0, no erase). A power cut leaves the old copy, or
 * both. When the free sectors are there but not in one run, the slots are moved down first
 * (compacting; a power cut in the middle can lose the slot being moved). */

#define STORE_LO 0x97000u
#define STORE_HI 0xE0000u
#define STORE_SECT 0x1000u
#define STORE_NSECT ((STORE_HI - STORE_LO) / STORE_SECT)
#define STORE_MAX 32
#define SLOT_MAGIC 0x4C534D46u     /* "FMSL" */
#define SLOT_VERSION 1u
#define SLOT_HDR 64u

#ifndef STORE_SECTION
#define STORE_SECTION
#endif

typedef struct {
    uint32_t magic, version, size, crc, seq;    /* size, crc: of the image after the header */
    char name[24];
    uint32_t hcrc;                 /* of the 44 bytes before it */
    uint8_t pad[SLOT_HDR - 48];
} slot_hdr;

typedef struct {
    uint32_t off, size, seq;       /* off: of the header (flash offset) */
    char name[24];
} store_slot;

static store_slot store_slots[STORE_MAX];
static int store_n;
static uint32_t store_seq;
static uint8_t store_buf[STORE_SECT] STORE_SECTION __attribute__((aligned(4)));
static uint32_t store_track;       /* a slot offset that store_compact keeps up to date */
static uint32_t zero4;             /* (in RAM: the flash is programmed with XIP off) */

/* the platform */
static int stp_ok(void);                       /* the flash is there */
static int stp_read(uint32_t off, void *d, uint32_t n);
static int stp_erase(uint32_t off);            /* one sector */
static int stp_prog(uint32_t off, const void *s, uint32_t n);
static const uint8_t *stp_ptr(uint32_t off);   /* read in place */
static void stp_idle(void);                    /* between sectors (the watchdog) */

/* ------------------------------------------------------------------- CRC-32 --- */
static uint32_t crc_tab32[256];

static uint32_t store_crc(uint32_t c, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    if (!crc_tab32[1])
        for (i = 0; i < 256u; i++) {
            uint32_t v = i, k;
            for (k = 0; k < 8u; k++)
                v = v & 1u ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_tab32[i] = v;
        }
    c = ~c;
    for (i = 0; i < n; i++)
        c = crc_tab32[(c ^ p[i]) & 255u] ^ (c >> 8);
    return ~c;
}

/* ---------------------------------------------------------------- the slots --- */
static uint32_t slot_sectors(uint32_t size) { return (SLOT_HDR + size + STORE_SECT - 1u) / STORE_SECT; }

static int hdr_ok(const slot_hdr *h, uint32_t off)
{
    return h->magic == SLOT_MAGIC && h->version == SLOT_VERSION && h->size >= sizeof(fmt_header) &&
           off + SLOT_HDR + h->size <= STORE_HI && h->hcrc == store_crc(0, (const uint8_t *)h, 44);
}

static void store_scan(void)
{
    uint32_t s = 0;
    store_n = 0;
    store_seq = 0;
    if (!stp_ok())
        return;
    while (s < STORE_NSECT) {
        slot_hdr h;
        uint32_t off = STORE_LO + s * STORE_SECT;
        if (stp_read(off, &h, sizeof h) || !hdr_ok(&h, off) || store_n == STORE_MAX) {
            s++;
            continue;
        }
        store_slots[store_n].off = off;
        store_slots[store_n].size = h.size;
        store_slots[store_n].seq = h.seq;
        memcpy(store_slots[store_n].name, h.name, 24);
        store_slots[store_n].name[23] = 0;
        if (h.seq > store_seq)
            store_seq = h.seq;
        store_n++;
        s += slot_sectors(h.size);
    }
}

static const uint8_t *store_image(int i) { return stp_ptr(store_slots[i].off + SLOT_HDR); }

static int store_find(uint32_t off)            /* the slot at off, -1 */
{
    int i;
    for (i = 0; i < store_n; i++)
        if (store_slots[i].off == off)
            return i;
    return -1;
}

static int store_check(int i)                  /* the image is what was written */
{
    return store_crc(0, store_image(i), store_slots[i].size) == ((const slot_hdr *)stp_ptr(store_slots[i].off))->crc;
}

static void used_map(uint8_t *used)
{
    int i;
    uint32_t k;
    memset(used, 0, STORE_NSECT);
    for (i = 0; i < store_n; i++)
        for (k = 0; k < slot_sectors(store_slots[i].size); k++)
            used[(store_slots[i].off - STORE_LO) / STORE_SECT + k] = 1;
}

static uint32_t store_free_sectors(void)
{
    uint8_t used[STORE_NSECT];
    uint32_t s, n = 0;
    used_map(used);
    for (s = 0; s < STORE_NSECT; s++)
        n += !used[s];
    return n;
}

static int find_run(uint32_t need)             /* the first sector of need free ones, -1 */
{
    uint8_t used[STORE_NSECT];
    uint32_t s, run = 0;
    used_map(used);
    for (s = 0; s < STORE_NSECT; s++) {
        run = used[s] ? 0 : run + 1u;
        if (run == need)
            return (int)(s + 1u - need);
    }
    return -1;
}

static int write_sector(uint32_t off, const uint8_t *b, uint32_t n)   /* erase, then n bytes */
{
    int rc = stp_erase(off);
    uint32_t i;
    for (i = 0; !rc && i < n; i += 256u)
        rc = stp_prog(off + i, b + i, n - i > 256u ? 256u : n - i);
    stp_idle();
    return rc;
}

static int store_delete(int i)
{

    int rc;
    if (i < 0 || i >= store_n)
        return -2;
    rc = stp_prog(store_slots[i].off, &zero4, 4);   /* the magic: 1 -> 0 bits, no erase */
    store_scan();
    return rc;
}

/* move the slots down over the gaps; moved(from, to) for each slot that moved (header offsets) */
static int store_compact(void (*moved)(uint32_t from, uint32_t to))
{
    uint32_t dst = STORE_LO;
    int i, rc = 0;
    for (i = 0; i < store_n && !rc; i++) {     /* (the slots are in offset order) */
        store_slot *sl = &store_slots[i];
        uint32_t n = slot_sectors(sl->size), k, from = sl->off;
        if (from != dst) {
            for (k = 0; k < n && !rc; k++) {
                rc = stp_read(from + k * STORE_SECT, store_buf, STORE_SECT);
                if (!rc)
                    rc = write_sector(dst + k * STORE_SECT, store_buf, STORE_SECT);
            }
            if (rc)
                break;
            if (from >= dst + n * STORE_SECT) {   /* the old copy is whole: mark it deleted */

                rc = stp_prog(from, &zero4, 4);
            }
            sl->off = dst;
            if (store_track == from)
                store_track = dst;
            if (moved)
                moved(from, dst);
        }
        dst += n * STORE_SECT;
    }
    store_scan();
    return rc;
}

/* ------------------------------------------------------------------ writing --- */
typedef struct {
    uint32_t base, pos;            /* the slot's first sector, bytes so far (the header's 64 first) */
    uint32_t crc;
    int err;
} store_wr;

static int store_sink(void *ctx, const void *d, uint32_t n)
{
    store_wr *w = ctx;
    while (n && !w->err) {
        uint32_t at = w->pos % STORE_SECT, k = STORE_SECT - at;
        if (k > n)
            k = n;
        if (d) {
            memcpy(store_buf + at, d, k);
            d = (const uint8_t *)d + k;
        } else {
            memset(store_buf + at, 0, k);
        }
        w->crc = store_crc(w->crc, store_buf + at, k);
        w->pos += k;
        n -= k;
        if (w->pos % STORE_SECT == 0u)
            w->err = write_sector(w->base + w->pos - STORE_SECT, store_buf, STORE_SECT);
    }
    return w->err;
}

/* Writing a slot in three steps (store_save, the console's `put`, rename):
 *   store_put_begin(size, name, moved): room for it (compacting if needed);
 *   store_put_data(...): the image's bytes, in order (any pieces);
 *   store_put_end(crc): the header; the image is read back and checked.
 * Errors: -1 out of memory, -2 bad argument, -3 no room, -4 read back wrong, -5 no flash,
 * -6 not a song image, else the flash's error. */
static store_wr put_w;
static uint32_t put_size;
static char put_name[24];

static int store_put_begin(uint32_t size, const char *name, void (*moved)(uint32_t, uint32_t))
{
    uint32_t need = slot_sectors(size), i;
    int s, rc;
    if (!stp_ok())
        return -5;
    if (size < sizeof(fmt_header) || STORE_LO + SLOT_HDR + size > STORE_HI)
        return -2;
    s = find_run(need);
    if (s < 0 && store_free_sectors() >= need) {
        rc = store_compact(moved);
        if (rc)
            return rc;
        s = find_run(need);
    }
    if (s < 0)
        return -3;
    put_w.base = STORE_LO + (uint32_t)s * STORE_SECT;
    put_w.pos = SLOT_HDR;
    put_w.crc = 0;
    put_w.err = 0;
    put_size = size;
    memset(put_name, 0, sizeof put_name);
    for (i = 0; i < 23u && name[i]; i++)
        put_name[i] = name[i];
    memset(store_buf, 0xFF, SLOT_HDR);         /* the header: programmed last */
    return 0;
}

static int store_put_data(void *ctx, const void *d, uint32_t n)   /* (a song_sink) */
{
    (void)ctx;
    if (put_w.pos - SLOT_HDR + n > put_size)
        return put_w.err = -2;
    return store_sink(&put_w, d, n);
}

/* crc: what the image should have (any value with check 0). Returns the slot's offset, or 0 and *err */
static uint32_t store_put_end(uint32_t crc, int check, int *err)
{
    slot_hdr h;
    const fmt_header *f;
    int rc = put_w.err;
    if (!rc && put_w.pos - SLOT_HDR != put_size)
        rc = -2;
    if (!rc && put_w.pos % STORE_SECT)
        rc = write_sector(put_w.base + put_w.pos - put_w.pos % STORE_SECT, store_buf, put_w.pos % STORE_SECT);
    if (!rc && check && crc != put_w.crc)
        rc = -4;
    f = (const fmt_header *)stp_ptr(put_w.base + SLOT_HDR);
    if (!rc && (f->magic != FMT_MAGIC || f->version != FMT_VERSION || f->size != put_size))
        rc = -6;
    if (!rc) {
        memset(&h, 0, sizeof h);
        h.magic = SLOT_MAGIC;
        h.version = SLOT_VERSION;
        h.size = put_size;
        h.crc = put_w.crc;
        h.seq = store_seq + 1u;
        memcpy(h.name, put_name, sizeof h.name);
        h.hcrc = store_crc(0, (const uint8_t *)&h, 44);
        rc = stp_prog(put_w.base, &h, sizeof h);
        if (!rc && store_crc(0, stp_ptr(put_w.base + SLOT_HDR), put_size) != put_w.crc)
            rc = -4;
        if (rc)
            stp_prog(put_w.base, &zero4, 4);
    }
    store_scan();
    *err = rc;
    return rc ? 0u : put_w.base;
}

/* the song (engine/song.c) into a new slot named name; then the slot at old_off (0: none) is
 * deleted. moved: see store_compact. Returns the new slot's header offset, or 0 and *err. */
static uint32_t store_save(const char *name, uint32_t old_off, void (*moved)(uint32_t, uint32_t), int *err)
{
    uint32_t size, off;
    int rc;
    *err = 0;
    rc = song_save(0, 0, &size);
    if (!rc) {
        store_track = old_off;                 /* (it may move) */
        rc = store_put_begin(size, name, moved);
        old_off = store_track;
    }
    if (!rc)
        rc = song_save(store_put_data, 0, 0);
    if (rc) {
        *err = rc;
        return 0;
    }
    off = store_put_end(0, 0, err);
    if (off && old_off && old_off != off)
        store_delete(store_find(old_off));
    return off;
}

/* slot i under another name: copied to a new slot, the old one deleted. Returns the new offset. */
static __attribute__((unused)) uint32_t store_rename(int i, const char *name, void (*moved)(uint32_t, uint32_t), int *err)
{
    uint32_t old, size, crc, off;
    int rc;
    *err = -2;
    if (i < 0 || i >= store_n)
        return 0;
    old = store_slots[i].off;
    size = store_slots[i].size;
    crc = ((const slot_hdr *)stp_ptr(old))->crc;
    store_track = old;
    rc = store_put_begin(size, name, moved);
    old = store_track;
    if (!rc)
        rc = store_put_data(0, stp_ptr(old + SLOT_HDR), size);
    if (rc) {
        *err = rc;
        return 0;
    }
    off = store_put_end(crc, 1, err);
    if (off) {
        store_delete(store_find(old));
        if (moved)
            moved(old, off);
    }
    return off;
}
