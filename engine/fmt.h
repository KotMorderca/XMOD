/* SPDX-License-Identifier: GPL-3.0-only */
/* FMTK: the song image the player reads in place (from flash on the FM-1, from a file on the PC).
 * Made from a .xm or .mod by host/modconv.c. Everything is little-endian, offsets are from the
 * start of the image, every struct is 4-byte aligned. Samples are plain PCM (no delta coding),
 * with one guard sample before and two after for the interpolating mixer. */
#ifndef FMT_H
#define FMT_H
#include <stdint.h>

#define FMT_MAGIC   0x4B544D46u      /* "FMTK" */
#define FMT_VERSION 1u

enum {                               /* fmt_header.flags */
    FMT_LINEAR = 1,                  /* linear frequency table (XM); else Amiga periods */
    FMT_FROM_MOD = 2,                /* converted from a .mod */
};

enum {                               /* fmt_sample.flags */
    FMT_LOOP_FWD = 1,
    FMT_LOOP_PINGPONG = 2,
    FMT_16BIT = 16,
};

enum {                               /* envelope flags */
    FMT_ENV_ON = 1,
    FMT_ENV_SUSTAIN = 2,
    FMT_ENV_LOOP = 4,
};

#define FMT_MAX_CHANNELS 32
#define FMT_MAX_ROWS 256
#define FMT_NOTE_OFF 97

typedef struct {
    uint32_t magic, version, size;   /* size: the whole image in bytes */
    char name[24];                   /* NUL-terminated */
    char tracker[24];                /* NUL-terminated ("FastTracker v2.00", "ProTracker", ...) */
    uint16_t flags;
    uint8_t num_channels, num_instr; /* instruments 1..num_instr */
    uint16_t song_length, loop_start;
    uint16_t speed, bpm;
    uint16_t num_patterns, num_samples;
    uint32_t orders_off;             /* uint8_t[256] */
    uint32_t patterns_off;           /* fmt_pattern[num_patterns] */
    uint32_t instr_off;              /* fmt_instr[num_instr] */
    uint32_t samples_off;            /* fmt_sample[num_samples] */
} fmt_header;

/* Pattern data: XM packing. Per cell one byte b: b == 0x80 empty; b & 0x80 a mask (bit 0 note,
 * 1 instrument, 2 volume column, 3 effect, 4 parameter) followed by those bytes; otherwise b is
 * the note and the other four bytes follow. rows_off: uint16_t[num_rows], each row's offset
 * from data_off. data_off 0: every row is empty. */
typedef struct {
    uint16_t num_rows, data_bytes;
    uint32_t data_off, rows_off;
} fmt_pattern;

typedef struct {
    char name[24];
    uint8_t num_samples, vib_type, vib_sweep, vib_depth;
    uint8_t vib_rate, mute, vol_env_len, pan_env_len;
    uint8_t vol_sus, vol_loop_start, vol_loop_end, vol_flags;
    uint8_t pan_sus, pan_loop_start, pan_loop_end, pan_flags;
    uint16_t fadeout, first_sample;  /* first_sample: index into the sample table */
    uint8_t note2sample[96];
    int16_t vol_env[12][2], pan_env[12][2];
} fmt_instr;

typedef struct {
    char name[24];
    uint8_t flags, volume;
    int8_t finetune, relative_note;
    uint8_t panning, pad[3];
    uint32_t length, loop_start, loop_length;   /* in frames; length ends at the loop end when looped */
    uint32_t data_off;               /* frame 0 (data_off - 1 or -2 bytes: the guard sample) */
} fmt_sample;

#endif
