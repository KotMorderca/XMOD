/* SPDX-License-Identifier: GPL-3.0-only */
/* The song player: FT2 replay logic (ported from ft2-clone, integer only) and an interpolating
 * mixer. One global instance. Thread model: player_render runs in the audio context; the
 * player_* control calls and the state reads are for the main loop (a torn read of the state
 * only shows a stale number on screen). */
#ifndef PLAYER_H
#define PLAYER_H
#include <stdint.h>
#include "fmt.h"

#define PLAYER_MAX_CHANNELS FMT_MAX_CHANNELS

/* What the mixer page shows of each channel (updated every tick). */
typedef struct {
    uint8_t note, instr, vol, efx, efx_data, volcol;   /* the last row read */
    uint8_t active;                                     /* a voice is sounding */
    uint8_t final_vol;                                  /* 0..64 */
    uint8_t final_pan;                                  /* 0..255 */
    uint8_t peak;                                       /* 0..255, mixer peak since player_take_peaks */
    uint16_t period;
} player_chan_info;

typedef struct {
    uint8_t playing;
    uint8_t num_channels;
    uint16_t song_pos, pattern, row, num_rows;
    uint16_t speed, bpm, tick, global_vol;
    uint32_t ticks;                  /* replayer ticks since play */
    uint32_t frames;                 /* output frames since play */
    player_chan_info ch[PLAYER_MAX_CHANNELS];
} player_state;

/* Output rate as a fraction (44100/1 on the PC, 24000000/544 on the FM-1). */
void player_init(uint32_t rate_num, uint32_t rate_den);
/* Bind a song image (kept, read in place). 0 ok, negative: not a valid image. */
int player_load(const uint8_t *image, uint32_t size);
void player_play(uint16_t song_pos);
void player_stop(void);
/* jump to an order position, row 0 (playing or not; the sounding notes go on) */
void player_set_pos(uint16_t song_pos);
/* Interleaved stereo, 16-bit range in int32 (not clipped). */
void player_render(int32_t *out, uint32_t frames);
/* 1..32, FT2's "amplification" (default 4). */
void player_set_amp(int amp);
int player_get_amp(void);
/* now (the song's settings for the next play are song.c's) */
void player_set_speed(int speed);
void player_set_bpm(int bpm);
void player_set_global_vol(int v);   /* 0..64 */
void player_mute(int channel, int muted);
/* the instruments and samples are now read from image (a saved or moved copy of the same song);
 * stops playing; the patterns (song.c) stay */
void player_rebind(const uint8_t *image);
/* ProTracker mode for songs from a .mod (on by default): Amiga panning with stereo separation, the
 * A500's low-pass filter, the LED filter (E00 on / E01 off), F00 stops, porta limits 113..856.
 * Off: the MOD plays as FT2 plays it. */
void player_set_pt_mode(int on);
int player_pt_mode(void);            /* the song loaded plays in it */
/* cheap reads for the audio context: replayer ticks since play (one tick = one MIDI clock: FT2's
 * tick rate is BPM * 2 / 5 Hz, 24 per quarter note), playing or not */
uint32_t player_ticks(void);
int player_playing(void);
int player_led(void);
void player_set_stereo_sep(int percent);   /* 0..100, default 20 */
/* the playing pattern repeats instead of going on to the next position */
void player_loop_pattern(int on);
/* play a note on a channel now, playing or not (note FMT_NOTE_OFF: key off; instr 0: the last) */
void player_jam(int channel, uint8_t note, uint8_t instr);

const player_state *player_get_state(void);
/* the channels' mixer peaks (0..255) since the last call, then cleared */
void player_take_peaks(uint8_t *peaks, int n);
const fmt_header *player_song(void);
/* Pattern access for display: the cell of (pattern, row, channel) unpacked. */
typedef struct { uint8_t note, instr, vol, efx, efx_data; } player_cell;
int player_get_row(uint16_t pattern, uint16_t row, player_cell *cells, int max_cells);

#endif
