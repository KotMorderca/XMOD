/* SPDX-License-Identifier: GPL-3.0-only */
/* The editable song: what the player reads, in RAM. The order list, the settings and the patterns
 * live here; the instruments and the samples stay in the image the song came from (flash on the
 * FM-1). Patterns are kept packed (XM packing, as in the image) in an arena; the one being edited
 * is unpacked in an edit buffer and packed again when another one is edited (or song_flush).
 *
 * Thread model: the audio context reads (song_read_row, song_rows, the orders); the main loop
 * writes. Writes that move memory (packing, compacting the arena) run under the lock given with
 * song_set_lock; a cell written in the edit buffer is a few byte stores, harmless if read half way. */
#ifndef SONG_H
#define SONG_H
#include <stdint.h>
#include "fmt.h"

#ifndef SONG_ARENA
#define SONG_ARENA (176u * 1024u)    /* packed patterns */
#endif
#define SONG_MAX_PATTERNS 256
#define SONG_MAX_ROWS 256

typedef struct { uint8_t note, instr, vol, efx, efx_data; } song_cell;

typedef struct {
    uint16_t rows;                   /* 1..256 */
    uint16_t size;                   /* bytes in the arena (rows_off + data), 0 = every row empty */
    uint8_t *data;                   /* uint16_t rows_off[rows], then the packed rows */
} song_pat;

typedef struct {
    fmt_header hdr;                  /* a copy: name, song_length, loop_start, speed, bpm, num_patterns change */
    const uint8_t *img;              /* the image: instruments and samples */
    uint32_t img_size;
    uint8_t orders[256];
    song_pat pat[SONG_MAX_PATTERNS];
    int16_t epat;                    /* the pattern in the edit buffer, -1: none */
    uint32_t top, live;              /* arena: next free byte, bytes in use */
    /* the instruments and the sample headers, editable: at the bottom of the arena (data_off still
     * points into img, where the sample data stays) */
    fmt_instr *ins;
    fmt_sample *smp;
    uint32_t tables;                 /* their bytes (the arena's patterns start after them) */
} song_t;

extern song_t song;

void song_set_lock(uint32_t (*lock)(void), void (*unlock)(uint32_t));
/* 0 ok; -1 not a valid image; -2 the patterns do not fit */
int song_load(const uint8_t *img, uint32_t size);
uint32_t song_rows(uint32_t p);      /* 64 for a pattern that does not exist (FT2) */
/* one row of pattern p, nch cells (empty past the end) */
void song_read_row(uint32_t p, uint32_t row, song_cell *out, uint32_t nch);

/* editing; 0 ok, -1 out of memory, -2 bad argument */
int song_edit(uint32_t p);           /* p into the edit buffer (creates it if needed) */
song_cell *song_cell_at(uint32_t p, uint32_t row, uint32_t ch);   /* after song_edit(p); NULL if out of range */
int song_set_rows(uint32_t p, uint32_t rows);                     /* cells past the end are dropped */
int song_new_pattern(void);          /* the first unused pattern number, 64 empty rows; -1: none left */
int song_copy_pattern(uint32_t from, uint32_t to);
void song_set_num_patterns(uint32_t n);   /* fewer: the last ones are dropped; more: empty ones */
int song_flush(void);                /* pack the edit buffer (it is then no longer the edited one) */
uint32_t song_mem_used(void);        /* arena bytes live (+ the edit buffer's pattern packed: an estimate) */

/* Saving: the song as an FMTK image (the RAM patterns, the image's instruments and samples), in
 * order, through sink (data NULL: n zero bytes). The edit buffer is packed first.
 * 0 ok, -1 out of memory, else the sink's error. size: the image's size (may be NULL). */
typedef int (*song_sink)(void *ctx, const void *data, uint32_t n);
int song_save(song_sink sink, void *ctx, uint32_t *size);
/* the instruments and samples now come from img (a saved copy of this song: same instruments) */
void song_rebind(const uint8_t *img, uint32_t size);

#endif
