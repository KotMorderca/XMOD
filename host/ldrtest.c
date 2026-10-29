/* SPDX-License-Identifier: GPL-3.0-only */
/* XMOD's update loader (Felucca's firmware/loader/ldr_core.c as tools/build_fw.py patches it, in
 * build/fw/loader/src) on the PC: a fake host serves a package over the SysEx protocol to a "device"
 * flash. The SysEx host and the flash hooks are Felucca's tests/ldr_test.c (Copyright (C) 2026 Leo
 * Kuroshita, Hugelton Instruments, GPL-3.0-only).
 *   ldrtest OLD.fwsc NEW.fwsc   (OLD: a package with data in the tail, e.g. XMOD 1.0.0's)
 * The device runs OLD, with songs in the tail (firmware/layout.h) and in the store; NEW must install
 * without touching them, and a package with data in the tail must be refused before any erase. */
#include "../firmware/layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __attribute__(x)   /* (the sources' .pool sections: no Mach-O section names) */

#define FELUCCA_ID "ota-FM-1_900"
#define FELUCCA_OTA_DRYRUN 0

static uint8_t nor[0x100000], *logical;
static size_t logical_len;
static uint8_t rx[1024];
static uint32_t rx_len, rx_full, now_ms, requests, erases, bad_range, record_cleared, f0_asked, tail_writes;
static uint32_t flip_addr = 0xFFFFFFFFu, flip_seen, flip_on;   /* serve addr's flip_on-th reply with a byte changed */

static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8;
        while (nb >= 7) { out[o++] = acc & 0x7F; acc >>= 7; nb -= 7; }
    }
    if (nb) out[o++] = acc & 0x7F;
    return o;
}
static uint32_t unpack7h(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 7;
        if (nb >= 8) { out[o++] = (uint8_t)acc; acc >>= 8; nb -= 8; }
    }
    return o;
}

static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint8_t u[64], m[600];
    uint32_t addr, len, i, s = 0;
    unpack7h(p + 1, n - 2, u);
    if (u[2] == 0x11) return 0;
    addr = u[7] | u[8] << 8 | u[9] << 16 | (uint32_t)u[10] << 24;
    len = u[11] | u[12] << 8 | u[13] << 16;
    requests++;
    memcpy(m, "\x00\x59\x30", 3);
    m[3] = (uint8_t)(len + 8); m[4] = (uint8_t)((len + 8) >> 8); m[5] = 0; m[6] = 0;
    memcpy(m + 7, &addr, 4);
    m[11] = (uint8_t)len; m[12] = (uint8_t)(len >> 8); m[13] = 0;
    if (addr >= 0xE0000000u) {
        f0_asked += addr == 0xF0000000u;
        memset(m + 14, 0, len);
        memcpy(m + 14, "success", 8);
    } else {
        if (addr + len > logical_len) { printf("read past the package %#x\n", addr); exit(1); }
        memcpy(m + 14, logical + addr, len);
        if (addr == flip_addr && ++flip_seen == flip_on)
            m[14 + len / 2u] ^= 0x10;                /* a transfer error the frame checksum does not see */
    }
    for (i = 6; i < 14 + len; i++) s += m[i];
    m[14 + len] = (uint8_t)~s;
    rx_len = pack7(m, 15 + len, rx);
    rx_full = 1;
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n) { if (!rx_full) return 0; *p = rx; *n = rx_len; return 1; }
static void ota_frame_done(void) { rx_full = 0; }
static uint32_t ota_now_ms(void) { return now_ms; }
static void ota_idle(void) { now_ms++; }
static int ota_erase(uint32_t off) { (void)off; return -1; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { memcpy(p, nor + off, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }
#include "../third_party/Felucca/firmware/src/ota.c"

static int ldr_fread(uint32_t off, void *p, uint32_t n) { memcpy(p, nor + off, n); return 0; }
static int ldr_erase(uint32_t off)
{
    if (off < 0x4000u || off >= 0xFC000u || (off & 0xFFFu) || (off >= 0x97000u && off < 0xE0000u)) bad_range++;
    if (off < 0x4000u) { printf("ERASE IN THE HEAD %#x\n", off); exit(1); }
    erases++;
    if (off >= XMOD_SONGS_LO && off < XMOD_APP_AREA_HI) tail_writes++;
    memset(nor + off, 0xFF, 0x1000);
    return 0;
}
static int ldr_prog(uint32_t off, const void *p, uint32_t n)
{
    const uint8_t *s = p;
    uint32_t i;
    if (off < 0x4000u || off + n > 0x93000u || ((off & 0xFFu) + n) > 256u) bad_range++;
    if (off + n > XMOD_SONGS_LO && off < XMOD_APP_AREA_HI) tail_writes++;
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static void ldr_record_clear(void) { record_cleared++; }
static void ldr_progress(uint32_t done, uint32_t total) { (void)done; (void)total; }
#include "../build/fw/loader/src/ldr_core.c"   /* (tools/build_fw.py: Felucca's, patched) */

static uint8_t *load_logical(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *raw = malloc(0x200000), *lg;
    size_t n, i;
    if (!f) { perror(path); exit(2); }
    n = fread(raw, 1, 0x200000, f);
    fclose(f);
    if (n < 20 * 48 + 0x400) { printf("%s: too short for a package\n", path); exit(2); }
    lg = malloc(n);
    for (i = 0; i < 20; i++) memcpy(lg + i * 47, raw + i * 48, 47);
    memcpy(lg + 20 * 47, raw + 20 * 48, n - 20 * 48);
    *len = n - 20;
    free(raw);
    return lg;
}

static uint32_t flash_off(const uint8_t *lg)        /* flash.bin offset in the logical image */
{
    uint8_t h[0x400];
    uint32_t i, n;
    memcpy(h, lg, sizeof h);
    ota_jl_enc(h, 0x40);
    n = ota_rd16(h + 8);
    for (i = 0; i < n; i++) {
        uint8_t *e = h + 0x40 + i * 0x50u;
        ota_jl_enc(e, 0x50);
        if (ota_rd16(e) == 0) return ota_rd32(e + 8);
    }
    return 0;
}

static int check(const char *what, int ok) { printf("%-64s %s\n", what, ok ? "ok" : "FAIL"); return ok ? 0 : 1; }

int main(int argc, char **argv)
{
    size_t olen;
    static uint8_t tail[XMOD_APP_AREA_HI - XMOD_SONGS_LO], store[0x1000], gap[0x4000], head[0x4000];
    uint8_t *old;
    uint32_t ofo, nfo, i, x = 1u;
    int bad = 0, rc;
    if (argc < 3) { printf("usage: ldrtest OLD.fwsc NEW.fwsc\n"); return 2; }
    old = load_logical(argv[1], &olen);
    logical = load_logical(argv[2], &logical_len);
    ofo = flash_off(old);
    nfo = flash_off(logical);
    /* the device: OLD installed; songs in the tail over what is left of OLD's app; a song in the store;
     * update records (Felucca's step 1, the stock firmware's) */
    memset(nor, 0xFF, sizeof nor);
    memcpy(nor, old + ofo, 0x93000);
    for (i = 0x50000u; i < 0x70000u; i++) { x = x * 1103515245u + 12345u; nor[i] = (uint8_t)(x >> 16); }
    for (i = 0x97000u; i < 0x98000u; i++) nor[i] = (uint8_t)(i * 7u);
    {
        uint8_t r[112] = {0};
        r[2] = 0x0D; r[3] = 0x5A; r[4] = 0x01; r[5] = 0x5A; r[6] = 0x41; r[7] = 0x54;
        ota_wr16(r, ota_crc16(r + 2, 78, 0));
        memcpy(nor + 0xE4F00, r, sizeof r);
        memcpy(nor + 0xE8F00, r, sizeof r);
        /* a song from 0x90000 over 0x93000..0x95FFF (its slot header: firmware/store.c), with bytes at 0x94F00
         * that look like an update record; a real record at 0x96F00, in the gap's free sector */
        static const uint32_t slot[3] = {0x4C534D46u, 1u, 0x5F00u};
        for (i = 0x90000u; i < 0x96000u; i++) nor[i] = (uint8_t)(i * 13u);
        memcpy(nor + 0x90000, slot, sizeof slot);
        memcpy(nor + 0x94F00, r, sizeof r);
        memcpy(nor + 0x96F00, r, sizeof r);
    }
    memcpy(tail, nor + XMOD_SONGS_LO, sizeof tail);
    memcpy(store, nor + 0x97000, sizeof store);
    memcpy(gap, nor + 0x93000, 0x3000);           /* (the song's part of the gap) */
    memcpy(head, nor, sizeof head);
    bad += check("NEW's flash.bin: erased past the app (the tail)", ({
        int e = 1; for (i = XMOD_SONGS_LO; i < XMOD_APP_AREA_HI; i++) e &= logical[nfo + i] == 0xFF; e; }));
    /* a damaged package: refused before anything is erased */
    logical[nfo + 0x20123u] ^= 0x01;
    erases = 0;
    rc = ldr_session();
    bad += check("damaged package refused (-12), nothing erased", rc == -12 && erases == 0);
    logical[nfo + 0x20123u] ^= 0x01;
    /* the install */
    erases = tail_writes = bad_range = 0;
    rc = ldr_session();
    printf("  rc %d, %u requests, %u sector erases\n", rc, requests, erases);
    bad += check("install completes", rc == 0);
    bad += check("app [0x4000, XMOD_APP_HI) == NEW's flash.bin", !memcmp(nor + 0x4000, logical + nfo + 0x4000, XMOD_APP_HI - 0x4000));
    bad += check("the tail (songs, OLD's leftovers) is untouched, no erase or write in it",
                 !memcmp(nor + XMOD_SONGS_LO, tail, sizeof tail) && tail_writes == 0);
    bad += check("the store is untouched", !memcmp(nor + 0x97000, store, sizeof store));
    bad += check("a song over 0x93000..0x96000 is kept (its record look-alike too)", !memcmp(nor + 0x93000, gap, 0x3000));
    bad += check("a real record in the gap's free sector is erased (0x96F00)", nor[0x96F06] == 0xFF);
    bad += check("flash head [0, 0x4000) untouched", !memcmp(nor, head, sizeof head));
    bad += check("update records cleared (0xE4F00, 0xE8F00)", record_cleared && nor[0xE4F06] == 0xFF && nor[0xE8F06] == 0xFF);
    bad += check("no write outside the allowed windows", bad_range == 0);
    erases = 0;
    rc = ldr_session();
    bad += check("re-run with the same package erases nothing", rc == 0 && erases == 0);
    /* OLD (data in the tail) through this loader: refused before any erase */
    {
        uint8_t *keep = logical;
        size_t keep_len = logical_len;
        static uint8_t app[XMOD_APP_HI - 0x4000];
        memcpy(app, nor + 0x4000, sizeof app);
        logical = old;
        logical_len = olen;
        erases = 0;
        rc = ldr_session();
        bad += check("a package with data in the tail is refused (-15), nothing erased",
                     rc == -15 && erases == 0 && !memcmp(nor + 0x4000, app, sizeof app) &&
                     !memcmp(nor + XMOD_SONGS_LO, tail, sizeof tail));
        logical = keep;
        logical_len = keep_len;
    }
    printf(bad ? "ldrtest: %d FAILED\n" : "ldrtest: ok\n", bad);
    return bad != 0;
}
