/* SPDX-License-Identifier: GPL-3.0-only */
/* Song player: FT2 replay logic and an integer mixer.
 *
 * The replay logic (effects, envelopes, auto-vibrato, row/position handling, FT2 quirks) is a
 * port of ft2-clone's src/ft2_replayer.c and src/ft2_audio.c (Copyright (c) 2016-2026, Olav
 * Sorensen, BSD-3-Clause, see THIRD_PARTY.md), with its float volume and delta maths replaced by
 * fixed point so it runs on a CPU without an FPU. The mixer is our own: linear interpolation,
 * 32.32 positions, FT2-style volume ramps (a tick long, 5 ms for note/volume jumps, a 5 ms
 * fade-out voice when a note is cut by a new one). */
#include "player.h"
#include "ft2_tables.h"
#include "song.h"
#ifndef XMOD_FIRMWARE
#include <string.h>                  /* (the firmware has its own memset / memcpy: Felucca's libc.c) */
#endif

/* ------------------------------------------------------------------ data --- */

enum {
    CS_UPDATE_VOL = 1,
    CS_UPDATE_PERIOD = 2,
    CS_TRIGGER_VOICE = 4,
    CS_UPDATE_PAN = 8,
    CS_QUICK_RAMP = 16,
};

typedef struct {
    uint8_t note, instr, vol, efx, efx_data;
} cell_t;

typedef struct {
    uint8_t key_off, channel_off, mute, semitone_porta;
    uint8_t status;
    int8_t relative_note, finetune;
    uint8_t smp_num, instr_num, efx_data, efx, sample_offset, tremor_param, tremor_pos;
    uint8_t glob_vol_slide_speed, pan_slide_speed, vib_trem_ctrl, porta_dir;
    uint8_t vib_pos, trem_pos, vib_speed, vib_depth, trem_speed, trem_depth;
    uint8_t loop_start_row, loop_counter, vol_slide_speed, fvol_up, fvol_down;
    uint8_t fpitch_up, fpitch_down, efpitch_up, efpitch_down;
    uint8_t pitch_up, pitch_down, retrig_speed, retrig_counter, retrig_vol;
    uint8_t volcol, note_num, pan_env_pos, autovib_pos, vol_env_pos, real_vol, old_vol, out_vol;
    uint8_t old_pan, out_pan, final_pan;
    int16_t vol_env_delta, vol_env_value, pan_env_delta, pan_env_value;
    uint16_t out_period, real_period, final_period, instr_and_note, porta_target, porta_speed;
    uint16_t vol_env_tick, pan_env_tick, autovib_amp, autovib_sweep;
    uint16_t fadeout_vol, fadeout_speed;
    int32_t smp_start;
    uint32_t final_vol;              /* Q16, 0..65536 */
    const fmt_sample *smp;
    const fmt_instr *ins;
    cell_t last;                     /* for the display */
} channel_t;

typedef struct {
    const int8_t *b8;
    const int16_t *b16;
    uint32_t pos, frac, dint, dfrac;
    uint32_t end, loop_start, loop_len, mirror;
    uint8_t loop, is16, active, backwards, fade_voice, peak;
    uint8_t pan;
    int32_t vol_l, vol_r, tgt_l, tgt_r, d_l, d_r;   /* Q24 */
    uint32_t ramp;
    uint32_t vol_q16;
} voice_t;

static struct {
    const uint8_t *img;
    const fmt_header *h;
    const fmt_instr *ins;
    const fmt_sample *smp;
    uint8_t linear;
    uint8_t num_ch;

    /* song state (ft2-clone song_t) */
    uint8_t playing, pbreak_flag, posjump_flag, bxx_overflow;
    uint8_t patt_del, patt_del2, pbreak_pos;
    int16_t song_pos, patt_num, row, num_rows;
    uint16_t bpm, speed, global_vol, tick;
    uint8_t patt_loop;               /* the pattern repeats (player_loop_pattern) */
    /* ProTracker mode (a song from a .mod): Amiga panning, the A500 filters, PT's limits */
    uint8_t pt_enable, pt_mode, led;
    uint16_t stereo_sep;             /* 0..256 (pt2-clone's default 20 %) */
    int32_t lp[2], bq[2][4];         /* the filters' state: one-pole; LED biquad x1 x2 y1 y2 */
    uint8_t jam_note[FMT_MAX_CHANNELS], jam_ins[FMT_MAX_CHANNELS], jam_on[FMT_MAX_CHANNELS];
    uint32_t ticks, frames;

    /* mixer */
    uint32_t rate_num, rate_den, delta_mul, quick_ramp;
    uint32_t spt_int[256], spt_frac[256];          /* samples per tick by BPM, 32.32 */
    uint32_t freq_mul, freq_div;
    uint32_t tick_left, tick_frac;
    int amp;
    uint8_t user_mute[FMT_MAX_CHANNELS];
} P;

/* the song's length and restart position (song.c: they can change while it plays) */
#define SONG_LEN ((int16_t)song.hdr.song_length)
#define SONG_LOOP ((int16_t)(song.hdr.loop_start < song.hdr.song_length ? song.hdr.loop_start : 0))

static channel_t chan[FMT_MAX_CHANNELS];
static voice_t voice[2 * FMT_MAX_CHANNELS];     /* [32..63]: fade-out copies */
static player_state state;

/* FT2's placeholder instrument (instr[0]: one sample, volume 0) and an allocated instrument's
 * unused sample slots (volume 64, centre). */
static const fmt_instr empty_instr;
static const fmt_sample placeholder_smp = {.volume = 0, .panning = 128};
static const fmt_sample unused_smp = {.volume = 64, .panning = 128};

/* --------------------------------------------------------------- helpers --- */

static uint64_t udiv64(uint64_t n, uint64_t d)  /* no libgcc 64-bit division on the device */
{
    uint64_t q = 0, r = 0;
    int i;
    if (!d)
        return 0;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    return q;
}

static uint16_t note2period(uint32_t idx)       /* 0..1935 */
{
    if (idx > 1935u)
        idx = 1935u;
    return P.linear ? (uint16_t)((1936u - idx) * 4u) : ft2_amiga_periods[idx];
}

static const fmt_instr *instr_ptr(uint8_t n)
{
    if (n >= 1 && n <= P.h->num_instr && P.ins[n - 1].num_samples)
        return &P.ins[n - 1];
    return &empty_instr;
}

static const fmt_sample *sample_ptr(const fmt_instr *ins, uint8_t s)
{
    if (ins == &empty_instr)
        return &placeholder_smp;
    if (s >= ins->num_samples)
        return &unused_smp;
    return &P.smp[ins->first_sample + s];
}

/* -------------------------------------------------------- period, delta --- */

static uint32_t period2ft2delta(uint32_t period)    /* 16.16 at FT2's 44000 Hz */
{
    period &= 0xFFFFu;
    if (!period)
        return 0;
    if (P.linear) {
        uint32_t inv = ((12u * 192u * 4u) - period) & 0xFFFFu;
        uint32_t q = inv / 768u, r = inv % 768u;
        uint32_t shift = (14u - q) & 31u;
        uint32_t d = (uint32_t)(((uint64_t)ft2_log_tab[r] * P.freq_mul) >> 24);
        return d >> shift;
    }
    return P.freq_div / period;
}

static void set_voice_delta(voice_t *v, uint32_t period)
{
    uint64_t d = ((uint64_t)period2ft2delta(period) * P.delta_mul) >> 16;  /* 32.32 */
    v->dint = (uint32_t)(d >> 32);
    v->dfrac = (uint32_t)d;
}

/* ------------------------------------------------------------- replayer --- */

static void key_off(channel_t *ch);
static void vol_slide(channel_t *ch, uint8_t param);
static void do_vibrato(channel_t *ch);
static void portamento(channel_t *ch);

static void reset_volumes(channel_t *ch)
{
    ch->real_vol = ch->old_vol;
    ch->out_vol = ch->old_vol;
    ch->out_pan = ch->old_pan;
    ch->status |= CS_UPDATE_VOL | CS_UPDATE_PAN | CS_QUICK_RAMP;
}

static void trigger_instrument(channel_t *ch)
{
    const fmt_instr *ins = ch->ins;
    if (!(ch->vib_trem_ctrl & 0x04))
        ch->vib_pos = 0;
    if (!(ch->vib_trem_ctrl & 0x40))
        ch->trem_pos = 0;
    ch->retrig_counter = 0;
    ch->tremor_pos = 0;
    ch->key_off = 0;
    if (ins->vol_flags & FMT_ENV_ON) {
        ch->vol_env_tick = 65535;
        ch->vol_env_pos = 0;
    }
    if (ins->pan_flags & FMT_ENV_ON) {
        ch->pan_env_tick = 65535;
        ch->pan_env_pos = 0;
    }
    ch->fadeout_speed = ins->fadeout;
    ch->fadeout_vol = 32768;
    if (ins->vib_depth > 0) {
        ch->autovib_pos = 0;
        if (ins->vib_sweep > 0) {
            ch->autovib_amp = 0;
            ch->autovib_sweep = (uint16_t)((ins->vib_depth << 8) / ins->vib_sweep);
        } else {
            ch->autovib_amp = (uint16_t)(ins->vib_depth << 8);
            ch->autovib_sweep = 0;
        }
    }
}

static void key_off(channel_t *ch)
{
    const fmt_instr *ins = ch->ins;
    ch->key_off = 1;
    if (ins->vol_flags & FMT_ENV_ON) {
        if (ch->vol_env_tick >= (uint16_t)ins->vol_env[ch->vol_env_pos][0])
            ch->vol_env_tick = (uint16_t)(ins->vol_env[ch->vol_env_pos][0] - 1);
    } else {
        ch->real_vol = 0;
        ch->out_vol = 0;
        ch->status |= CS_UPDATE_VOL | CS_QUICK_RAMP;
    }
    if (!(ins->pan_flags & FMT_ENV_ON)) {          /* FT2 logic bug, kept */
        if (ch->pan_env_tick >= (uint16_t)ins->pan_env[ch->pan_env_pos][0])
            ch->pan_env_tick = (uint16_t)(ins->pan_env[ch->pan_env_pos][0] - 1);
    }
}

static void trigger_note(uint8_t note, uint8_t efx, uint8_t efx_data, channel_t *ch)
{
    const fmt_instr *ins;
    const fmt_sample *s;
    if (note == FMT_NOTE_OFF) {
        key_off(ch);
        return;
    }
    if (note == 0) {
        note = ch->note_num;
        if (note == 0)
            return;
    }
    ch->note_num = note;
    ins = instr_ptr(ch->instr_num);
    ch->ins = ins;
    ch->mute = ins->mute;
    if (note > 96)
        note = 96;
    ch->smp_num = ins->note2sample[note - 1] & 0x0F;
    s = sample_ptr(ins, ch->smp_num);
    ch->smp = s;
    ch->relative_note = s->relative_note;
    note = (uint8_t)(note + ch->relative_note);
    if (note >= 10 * 12)
        return;
    ch->old_vol = s->volume;
    ch->old_pan = s->panning;
    if (efx == 0xE && (efx_data & 0xF0) == 0x50)
        ch->finetune = (int8_t)(((efx_data & 0x0F) * 16) - 128);
    else
        ch->finetune = s->finetune;
    if (note != 0) {
        uint32_t idx = ((uint32_t)(note - 1) * 16u) + (uint32_t)((ch->finetune >> 3) + 16);
        ch->out_period = ch->real_period = note2period(idx);
    }
    ch->status |= CS_UPDATE_PERIOD | CS_UPDATE_VOL | CS_UPDATE_PAN | CS_TRIGGER_VOICE | CS_QUICK_RAMP;
    if (efx == 9) {
        if (efx_data > 0)
            ch->sample_offset = ch->efx_data;
        ch->smp_start = ch->sample_offset << 8;
    } else {
        ch->smp_start = 0;
    }
}

/* ProTracker mode: porta slides stop at periods 113..856 (FT2's units are 4 times PT's) */
static void pt_clamp(channel_t *ch)
{
    if (!P.pt_mode)
        return;
    if (ch->real_period < 113 * 4)
        ch->real_period = 113 * 4;
    if (ch->real_period > 856 * 4)
        ch->real_period = 856 * 4;
}

/* -- tick-zero E effects -- */

static void fine_pitch_up(channel_t *ch, uint8_t p)
{
    if (p == 0)
        p = ch->fpitch_up;
    ch->fpitch_up = p;
    ch->real_period -= p * 4;
    if ((int16_t)ch->real_period < 1)
        ch->real_period = 1;
    pt_clamp(ch);
    ch->out_period = ch->real_period;
    ch->status |= CS_UPDATE_PERIOD;
}

static void fine_pitch_down(channel_t *ch, uint8_t p)
{
    if (p == 0)
        p = ch->fpitch_down;
    ch->fpitch_down = p;
    ch->real_period += p * 4;
    if ((int16_t)ch->real_period >= 32000)
        ch->real_period = 32000 - 1;
    pt_clamp(ch);
    ch->out_period = ch->real_period;
    ch->status |= CS_UPDATE_PERIOD;
}

static void pattern_loop(channel_t *ch, uint8_t p)
{
    if (p == 0) {
        ch->loop_start_row = (uint8_t)P.row;
    } else if (ch->loop_counter == 0) {
        ch->loop_counter = p;
        P.pbreak_pos = ch->loop_start_row;
        P.pbreak_flag = 1;
    } else if (--ch->loop_counter > 0) {
        P.pbreak_pos = ch->loop_start_row;
        P.pbreak_flag = 1;
    }
}

static void fine_vol_up(channel_t *ch, uint8_t p)
{
    if (p == 0)
        p = ch->fvol_up;
    ch->fvol_up = p;
    ch->real_vol += p;
    if (ch->real_vol > 64)
        ch->real_vol = 64;
    ch->out_vol = ch->real_vol;
    ch->status |= CS_UPDATE_VOL;
}

static void fine_vol_down(channel_t *ch, uint8_t p)
{
    if (p == 0)
        p = ch->fvol_down;
    ch->fvol_down = p;
    ch->real_vol -= p;
    if ((int8_t)ch->real_vol < 0)
        ch->real_vol = 0;
    ch->out_vol = ch->real_vol;
    ch->status |= CS_UPDATE_VOL;
}

static void e_effects_tick0(channel_t *ch, uint8_t param)
{
    uint8_t e = param >> 4;
    param &= 0x0F;
    if (ch->channel_off) {
        if (e == 0x6)
            pattern_loop(ch, param);
        else if (e == 0xE && P.patt_del2 == 0)
            P.patt_del = param + 1;
        return;
    }
    switch (e) {
    case 0x0:
        if (P.pt_mode)
            P.led = !(param & 1);
        break;
    case 0x1: fine_pitch_up(ch, param); break;
    case 0x2: fine_pitch_down(ch, param); break;
    case 0x3: ch->semitone_porta = (param != 0); break;
    case 0x4: ch->vib_trem_ctrl = (ch->vib_trem_ctrl & 0xF0) | param; break;
    case 0x6: pattern_loop(ch, param); break;
    case 0x7: ch->vib_trem_ctrl = (uint8_t)((param << 4) | (ch->vib_trem_ctrl & 0x0F)); break;
    case 0xA: fine_vol_up(ch, param); break;
    case 0xB: fine_vol_down(ch, param); break;
    case 0xC:
        if (param == 0) {
            ch->real_vol = 0;
            ch->out_vol = 0;
            ch->status |= CS_UPDATE_VOL | CS_QUICK_RAMP;
        }
        break;
    case 0xE:
        if (P.patt_del2 == 0)
            P.patt_del = param + 1;
        break;
    default: break;
    }
}

static void set_env_pos(channel_t *ch, uint8_t param)    /* Lxx */
{
    const fmt_instr *ins = ch->ins;
    int env;
    for (env = 0; env < 2; env++) {
        /* FT2 logic bug: the panning half tests the volume envelope's sustain flag */
        int on = env == 0 ? (ins->vol_flags & FMT_ENV_ON) : (ins->vol_flags & FMT_ENV_SUSTAIN);
        const int16_t (*pts)[2] = env == 0 ? ins->vol_env : ins->pan_env;
        int len = env == 0 ? ins->vol_env_len : ins->pan_env_len;
        int16_t delta = 0, value = 0;
        int point = 0, upd = 1;
        int32_t tick = param;
        if (!on)
            continue;
        if (len > 1) {
            int i;
            point++;
            for (i = 0; i < len - 1; i++) {
                if (tick < pts[point][0]) {
                    int16_t x0, x1, xd, y0, y1;
                    int8_t yd;
                    point--;
                    tick -= pts[point][0];
                    if (tick == 0) {
                        upd = 0;
                        break;
                    }
                    x0 = pts[point][0];
                    x1 = pts[point + 1][0];
                    xd = (int16_t)(x1 - x0);
                    if (xd <= 0) {
                        upd = 1;
                        break;
                    }
                    y0 = pts[point][1];
                    y1 = pts[point + 1][1];
                    yd = (int8_t)(y1 - y0);
                    delta = (int16_t)((yd << 8) / xd);
                    value = (int16_t)(((int8_t)y0 << 8) + (int16_t)(delta * (tick - 1)));
                    point++;
                    upd = 0;
                    break;
                }
                point++;
            }
            if (upd)
                point--;
        }
        if (env == 0) {
            ch->vol_env_tick = (uint16_t)(param - 1);
            if (upd) {
                ch->vol_env_delta = 0;
                ch->vol_env_value = (int16_t)((int8_t)pts[point][1] << 8);
            } else if (tick != 0) {
                ch->vol_env_delta = delta;
                ch->vol_env_value = value;
            }
        } else {
            ch->pan_env_tick = (uint16_t)(param - 1);
            if (upd) {
                ch->pan_env_delta = 0;
                ch->pan_env_value = (int16_t)((int8_t)pts[point][1] << 8);
            } else if (tick != 0) {
                ch->pan_env_delta = delta;
                ch->pan_env_value = value;
            }
        }
        if (point >= len) {
            point = len - 1;
            if (point < 0)
                point = 0;
        }
        if (env == 0)
            ch->vol_env_pos = (uint8_t)point;
        else
            ch->pan_env_pos = (uint8_t)point;
    }
}

static void more_effects_tick0(channel_t *ch)    /* called even when the channel is muted */
{
    uint8_t p = ch->efx_data;
    switch (ch->efx) {
    case 0xB: {                                    /* position jump */
        int16_t pos = (int16_t)p - 1;
        if (pos < 0 || pos >= SONG_LEN)
            P.bxx_overflow = 1;
        else
            P.song_pos = pos;
        P.pbreak_pos = 0;
        P.posjump_flag = 1;
        break;
    }
    case 0xD:                                      /* pattern break */
        p = (uint8_t)(((p >> 4) * 10) + (p & 0x0F));
        P.pbreak_pos = p <= 63 ? p : 0;
        P.posjump_flag = 1;
        break;
    case 0xE:
        e_effects_tick0(ch, p);
        break;
    case 0xF:
        if (p == 0 && P.pt_mode)
            P.playing = 0;                         /* (ProTracker: F00 stops the song) */
        else if (p >= 32)
            P.bpm = p;
        else
            P.tick = P.speed = p;
        break;
    case 16: {                                     /* Gxx: global volume */
        int i;
        P.global_vol = p > 64 ? 64 : p;
        for (i = 0; i < P.num_ch; i++)
            chan[i].status |= CS_UPDATE_VOL;
        break;
    }
    case 21:                                       /* Lxx */
        set_env_pos(ch, p);
        break;
    default:
        break;
    }
}

/* -- volume column, tick zero -- */
static void volcol_tick0(channel_t *ch, uint8_t *vc)
{
    switch (ch->volcol >> 4) {
    case 1: case 2: case 3: case 4: case 5:        /* set volume */
        *vc -= 16;
        if (*vc > 64)
            *vc = 64;
        ch->out_vol = ch->real_vol = *vc;
        ch->status |= CS_UPDATE_VOL | CS_QUICK_RAMP;
        break;
    case 8:                                        /* fine volume down */
        *vc = (uint8_t)(0 - (ch->volcol & 0x0F)) + ch->real_vol;
        if ((int8_t)*vc < 0)
            *vc = 0;
        ch->out_vol = ch->real_vol = *vc;
        ch->status |= CS_UPDATE_VOL;
        break;
    case 9:                                        /* fine volume up */
        *vc = (uint8_t)((ch->volcol & 0x0F) + ch->real_vol);
        if (*vc > 64)
            *vc = 64;
        ch->out_vol = ch->real_vol = *vc;
        ch->status |= CS_UPDATE_VOL;
        break;
    case 0xA:                                      /* vibrato speed */
        *vc = (uint8_t)((ch->volcol & 0x0F) * 4);
        if (*vc != 0)
            ch->vib_speed = *vc;
        break;
    case 0xC:                                      /* set panning */
        *vc = (uint8_t)(*vc << 4);
        ch->out_pan = *vc;
        ch->status |= CS_UPDATE_PAN;
        break;
    default:
        break;
    }
}

static void do_multi_retrig(channel_t *ch)
{
    uint8_t cnt = (uint8_t)(ch->retrig_counter + 1);
    int16_t vol;
    if (cnt < ch->retrig_speed) {
        ch->retrig_counter = cnt;
        return;
    }
    ch->retrig_counter = 0;
    vol = ch->real_vol;
    switch (ch->retrig_vol) {
    case 0x1: vol -= 1; break;
    case 0x2: vol -= 2; break;
    case 0x3: vol -= 4; break;
    case 0x4: vol -= 8; break;
    case 0x5: vol -= 16; break;
    case 0x6: vol = (int16_t)((vol >> 1) + (vol >> 3) + (vol >> 4)); break;
    case 0x7: vol >>= 1; break;
    case 0x9: vol += 1; break;
    case 0xA: vol += 2; break;
    case 0xB: vol += 4; break;
    case 0xC: vol += 8; break;
    case 0xD: vol += 16; break;
    case 0xE: vol = (int16_t)((vol >> 1) + vol); break;
    case 0xF: vol = (int16_t)(vol + vol); break;
    default: break;
    }
    if (vol < 0)
        vol = 0;
    if (vol > 64)
        vol = 64;
    ch->real_vol = (uint8_t)vol;
    ch->out_vol = ch->real_vol;
    if (ch->volcol >= 0x10 && ch->volcol <= 0x50) {
        ch->out_vol = (uint8_t)(ch->volcol - 0x10);
        ch->real_vol = ch->out_vol;
    } else if (ch->volcol >= 0xC0 && ch->volcol <= 0xCF) {
        ch->out_pan = (uint8_t)((ch->volcol & 0x0F) << 4);
    }
    trigger_note(0, 0, 0, ch);
}

static void effects_tick0(channel_t *ch)
{
    uint8_t vc = ch->volcol;
    uint8_t p = ch->efx_data;
    volcol_tick0(ch, &vc);
    if (ch->efx == 0 && p == 0)
        return;
    if (ch->efx == 8) {
        ch->out_pan = p;
        ch->status |= CS_UPDATE_PAN;
    } else if (ch->efx == 12) {
        if (p > 64)
            p = 64;
        ch->out_vol = ch->real_vol = p;
        ch->status |= CS_UPDATE_VOL | CS_QUICK_RAMP;
    } else if (ch->efx == 27) {                    /* Rxy */
        uint8_t t = p & 0x0F;
        if (t == 0)
            t = ch->retrig_speed;
        ch->retrig_speed = t;
        t = p >> 4;
        if (t == 0)
            t = ch->retrig_vol;
        ch->retrig_vol = t;
        if (vc == 0)
            do_multi_retrig(ch);
    } else if (ch->efx == 33) {                    /* X1x / X2x */
        uint8_t type = p >> 4;
        p &= 0x0F;
        if (type == 1) {
            uint16_t np;
            if (p == 0)
                p = ch->efpitch_up;
            ch->efpitch_up = p;
            np = (uint16_t)(ch->real_period - p);
            if ((int16_t)np < 1)
                np = 1;
            ch->out_period = ch->real_period = np;
            ch->status |= CS_UPDATE_PERIOD;
        } else if (type == 2) {
            uint16_t np;
            if (p == 0)
                p = ch->efpitch_down;
            ch->efpitch_down = p;
            np = (uint16_t)(ch->real_period + p);
            if ((int16_t)np >= 32000)
                np = 32000 - 1;
            ch->out_period = ch->real_period = np;
            ch->status |= CS_UPDATE_PERIOD;
        }
    }
    more_effects_tick0(ch);
}

static void prepare_porta(channel_t *ch, const cell_t *c, uint8_t inst)
{
    if (c->note > 0) {
        if (c->note == FMT_NOTE_OFF) {
            key_off(ch);
        } else {
            uint32_t note = ((uint32_t)((c->note - 1) + ch->relative_note) * 16u) +
                            (uint32_t)((ch->finetune >> 3) + 16);
            if (note < (10 * 12 * 16) + 16) {
                ch->porta_target = note2period(note);
                if (ch->porta_target == ch->real_period)
                    ch->porta_dir = 0;
                else if (ch->porta_target > ch->real_period)
                    ch->porta_dir = 1;
                else
                    ch->porta_dir = 2;
            }
        }
    }
    if (inst > 0) {
        reset_volumes(ch);
        if (c->note != FMT_NOTE_OFF)
            trigger_instrument(ch);
    }
}

static void get_new_note(channel_t *ch, const cell_t *c)
{
    uint8_t inst;
    ch->volcol = c->vol;
    ch->last = *c;
    if (ch->efx == 0) {
        if (ch->efx_data > 0) {                    /* arpeggio was running: period back */
            ch->out_period = ch->real_period;
            ch->status |= CS_UPDATE_PERIOD;
        }
    } else if ((ch->efx == 4 || ch->efx == 6) && (c->efx != 4 && c->efx != 6)) {
        ch->out_period = ch->real_period;          /* vibrato ended */
        ch->status |= CS_UPDATE_PERIOD;
    }
    ch->efx = c->efx;
    ch->efx_data = c->efx_data;
    ch->instr_and_note = (uint16_t)((c->instr << 8) | c->note);
    if (ch->channel_off) {
        more_effects_tick0(ch);
        return;
    }
    inst = c->instr;
    if (inst > 0) {
        if (inst <= 128)
            ch->instr_num = inst;
        else
            inst = 0;
    }
    if (c->efx == 0x0E && c->efx_data >= 0xD1 && c->efx_data <= 0xDF)
        return;                                    /* note delay */
    if (c->efx != 0x0E || c->efx_data != 0x90) {
        if ((ch->volcol & 0xF0) == 0xF0) {         /* volume column portamento */
            uint8_t p = ch->volcol & 0x0F;
            if (p > 0)
                ch->porta_speed = (uint16_t)((p << 4) * 4);
            prepare_porta(ch, c, inst);
            effects_tick0(ch);
            return;
        }
        if (c->efx == 3 || c->efx == 5) {
            if (c->efx != 5 && c->efx_data != 0)
                ch->porta_speed = (uint16_t)(c->efx_data * 4);
            prepare_porta(ch, c, inst);
            effects_tick0(ch);
            return;
        }
        if (c->efx == 0x14 && c->efx_data == 0) {  /* K00 */
            key_off(ch);
            if (inst)
                reset_volumes(ch);
            effects_tick0(ch);
            return;
        }
        if (c->note == 0) {
            if (inst > 0) {
                reset_volumes(ch);
                trigger_instrument(ch);
            }
            effects_tick0(ch);
            return;
        }
    }
    if (c->note == FMT_NOTE_OFF)
        key_off(ch);
    else
        trigger_note(c->note, c->efx, c->efx_data, ch);
    if (inst > 0) {
        reset_volumes(ch);
        if (c->note != FMT_NOTE_OFF)
            trigger_instrument(ch);
    }
    effects_tick0(ch);
}

/* one envelope step (volume: env == 0, panning: env == 1), returns the envelope value */
static int16_t envelope_step(channel_t *ch, int env)
{
    const fmt_instr *ins = ch->ins;
    const int16_t (*pts)[2] = env ? ins->pan_env : ins->vol_env;
    uint8_t flags = env ? ins->pan_flags : ins->vol_flags;
    uint8_t len = env ? ins->pan_env_len : ins->vol_env_len;
    uint8_t sus = env ? ins->pan_sus : ins->vol_sus;
    uint8_t lstart = env ? ins->pan_loop_start : ins->vol_loop_start;
    uint8_t lend = env ? ins->pan_loop_end : ins->vol_loop_end;
    uint8_t *ppos = env ? &ch->pan_env_pos : &ch->vol_env_pos;
    uint16_t *ptick = env ? &ch->pan_env_tick : &ch->vol_env_tick;
    int16_t *pval = env ? &ch->pan_env_value : &ch->vol_env_value;
    int16_t *pdelta = env ? &ch->pan_env_delta : &ch->vol_env_delta;
    int interpolated = 0;
    uint8_t pos = *ppos;
    int16_t val = 0;

    (*ptick)++;
    if (*ptick == (uint16_t)pts[pos][0]) {
        *pval = (int16_t)((int8_t)pts[pos][1] << 8);
        pos++;
        if (flags & FMT_ENV_LOOP) {
            pos--;
            if (pos == lend) {
                if (!(flags & FMT_ENV_SUSTAIN) || pos != sus || !ch->key_off) {
                    pos = lstart;
                    *ptick = (uint16_t)pts[pos][0];
                    *pval = (int16_t)((int8_t)pts[pos][1] << 8);
                }
            }
            pos++;
        }
        if (pos < len) {
            int interp = 1;
            if ((flags & FMT_ENV_SUSTAIN) && !ch->key_off) {
                if (pos - 1 == sus) {
                    pos--;
                    *pdelta = 0;
                    interp = 0;
                }
            }
            if (interp) {
                int16_t x0, x1, xd;
                *ppos = pos;
                x0 = pts[pos - 1][0];
                x1 = pts[pos][0];
                xd = (int16_t)(x1 - x0);
                if (xd > 0) {
                    int16_t y0 = pts[pos - 1][1], y1 = pts[pos][1];
                    int8_t yd = (int8_t)(y1 - y0);
                    *pdelta = (int16_t)((yd << 8) / xd);
                    val = *pval;
                    interpolated = 1;
                } else {
                    *pdelta = 0;
                }
            }
        } else {
            *pdelta = 0;
        }
    }
    if (!interpolated) {
        uint8_t hi;
        *pval = (int16_t)(*pval + *pdelta);
        val = *pval;
        hi = (uint8_t)(val >> 8);
        if (hi > 64) {
            val = hi <= 160 ? 64 * 256 : 0;
            *pdelta = 0;
        }
    }
    return val;
}

static void update_vol_pan_autovib(channel_t *ch)
{
    const fmt_instr *ins = ch->ins;
    if (ch->key_off) {
        if (ch->fadeout_speed > ch->fadeout_vol) {
            ch->fadeout_vol = 0;
            ch->fadeout_speed = 0;
        } else {
            ch->fadeout_vol -= ch->fadeout_speed;
        }
        ch->status |= CS_UPDATE_VOL;
    }
    if (!ch->mute) {
        uint32_t vol = (uint32_t)P.global_vol * ch->out_vol * ch->fadeout_vol;   /* <= 2^27 */
        uint32_t q = vol >> 11;                                                    /* Q16 */
        if (ins->vol_flags & FMT_ENV_ON) {
            uint32_t e = (uint16_t)envelope_step(ch, 0);
            if (e > 64u * 256u)
                e = 64u * 256u;
            q = (q * e) >> 14;
            ch->status |= CS_UPDATE_VOL;
        }
        if (q > 65536u)
            q = 65536u;
        ch->final_vol = q;
    } else {
        ch->final_vol = 0;
    }
    if (ins->pan_flags & FMT_ENV_ON) {
        int32_t e = envelope_step(ch, 1);
        int16_t pm = (int16_t)(ch->out_pan - 128);
        int8_t add;
        if (pm >= 0)
            pm = (int16_t)(0 - pm);
        pm = (int16_t)(pm + 128);
        pm = (int16_t)(pm << 3);
        e -= 32 * 256;
        add = (int8_t)((e * pm) >> 16);
        ch->final_pan = (uint8_t)(ch->out_pan + add);
        ch->status |= CS_UPDATE_PAN;
    } else {
        ch->final_pan = ch->out_pan;
    }
    if (ins->vib_depth > 0) {
        uint16_t amp;
        int16_t v;
        uint16_t tp;
        if (ch->autovib_sweep > 0) {
            amp = ch->autovib_sweep;
            if (!ch->key_off) {
                amp = (uint16_t)(amp + ch->autovib_amp);
                if ((amp >> 8) > ins->vib_depth) {
                    amp = (uint16_t)(ins->vib_depth << 8);
                    ch->autovib_sweep = 0;
                }
                ch->autovib_amp = amp;
            }
        } else {
            amp = ch->autovib_amp;
        }
        ch->autovib_pos = (uint8_t)(ch->autovib_pos + ins->vib_rate);
        if (ins->vib_type == 1)
            v = ch->autovib_pos > 127 ? 64 : -64;
        else if (ins->vib_type == 2)
            v = (int16_t)((((ch->autovib_pos >> 1) + 64) & 127) - 64);
        else if (ins->vib_type == 3)
            v = (int16_t)(((-(ch->autovib_pos >> 1) + 64) & 127) - 64);
        else
            v = ft2_autovib_sine[ch->autovib_pos];
        v = (int16_t)((v * (int16_t)amp) >> (6 + 8));
        tp = (uint16_t)(ch->out_period + v);
        if (tp >= 32000)
            tp = 0;
        ch->final_period = tp;
        ch->status |= CS_UPDATE_PERIOD;
    } else {
        ch->final_period = ch->out_period;
    }
}

static uint16_t period2note_period(uint16_t period, uint8_t offset, channel_t *ch)
{
    int32_t ft = ((int8_t)ch->finetune >> 3) + 16;
    int32_t hi = 8 * 12 * 16, lo = 0, tp, i;
    for (i = 0; i < 8; i++) {
        int32_t look;
        tp = (((lo + hi) >> 1) & ~15) + ft;
        look = tp - 8;
        if (look < 0)
            look = 0;
        if (period >= note2period((uint32_t)look))
            hi = (tp - ft) & ~15;
        else
            lo = (tp - ft) & ~15;
    }
    tp = lo + ft + (offset << 4);
    if (tp >= (8 * 12 * 16 + 15) - 1)
        tp = (8 * 12 * 16 + 16) - 1;
    return note2period((uint32_t)tp);
}

static void do_vibrato(channel_t *ch)
{
    uint8_t t = (ch->vib_pos >> 2) & 0x1F;
    switch (ch->vib_trem_ctrl & 3) {
    case 0: t = ft2_vibrato[t]; break;
    case 1:
        t = (uint8_t)(t << 3);
        if ((int8_t)ch->vib_pos < 0)
            t = (uint8_t)~t;
        break;
    default: t = 255; break;
    }
    t = (uint8_t)((t * ch->vib_depth) >> 5);
    if ((int8_t)ch->vib_pos < 0)
        ch->out_period = (uint16_t)(ch->real_period - t);
    else
        ch->out_period = (uint16_t)(ch->real_period + t);
    ch->status |= CS_UPDATE_PERIOD;
    ch->vib_pos = (uint8_t)(ch->vib_pos + ch->vib_speed);
}

static void portamento(channel_t *ch)
{
    if (ch->porta_dir == 0)
        return;
    if (ch->porta_dir > 1) {
        ch->real_period -= ch->porta_speed;
        if ((int16_t)ch->real_period <= (int16_t)ch->porta_target) {
            ch->porta_dir = 1;
            ch->real_period = ch->porta_target;
        }
    } else {
        ch->real_period += ch->porta_speed;
        if (ch->real_period >= ch->porta_target) {
            ch->porta_dir = 1;
            ch->real_period = ch->porta_target;
        }
    }
    ch->out_period = ch->semitone_porta ? period2note_period(ch->real_period, 0, ch) : ch->real_period;
    ch->status |= CS_UPDATE_PERIOD;
}

static void vol_slide(channel_t *ch, uint8_t p)
{
    uint8_t nv;
    if (p == 0)
        p = ch->vol_slide_speed;
    ch->vol_slide_speed = p;
    nv = ch->real_vol;
    if ((p & 0xF0) == 0) {
        nv -= p;
        if ((int8_t)nv < 0)
            nv = 0;
    } else {
        nv += p >> 4;
        if (nv > 64)
            nv = 64;
    }
    ch->out_vol = ch->real_vol = nv;
    ch->status |= CS_UPDATE_VOL;
}

static void tremolo(channel_t *ch, uint8_t p)
{
    uint8_t t;
    int16_t tv;
    if (p > 0) {
        if (p & 0x0F)
            ch->trem_depth = p & 0x0F;
        if ((p & 0xF0) >> 2)
            ch->trem_speed = (p & 0xF0) >> 2;
    }
    t = (ch->trem_pos >> 2) & 0x1F;
    switch ((ch->vib_trem_ctrl >> 4) & 3) {
    case 0: t = ft2_vibrato[t]; break;
    case 1:
        t = (uint8_t)(t << 3);
        if ((int8_t)ch->vib_pos < 0)               /* FT2 bug: vibrato position, kept */
            t = (uint8_t)~t;
        break;
    default: t = 255; break;
    }
    t = (uint8_t)((t * ch->trem_depth) >> 6);
    if ((int8_t)ch->trem_pos < 0) {
        tv = (int16_t)(ch->real_vol - t);
        if (tv < 0)
            tv = 0;
    } else {
        tv = (int16_t)(ch->real_vol + t);
        if (tv > 64)
            tv = 64;
    }
    ch->out_vol = (uint8_t)tv;
    ch->status |= CS_UPDATE_VOL;
    ch->trem_pos = (uint8_t)(ch->trem_pos + ch->trem_speed);
}

static void effects_tick_nonzero(channel_t *ch)
{
    uint8_t p = ch->efx_data;
    if (ch->channel_off)
        return;
    /* volume column */
    switch (ch->volcol >> 4) {
    case 6: {                                      /* slide down */
        uint8_t nv = (uint8_t)(0 - (ch->volcol & 0x0F)) + ch->real_vol;
        if ((int8_t)nv < 0)
            nv = 0;
        ch->out_vol = ch->real_vol = nv;
        ch->status |= CS_UPDATE_VOL;
        break;
    }
    case 7: {                                      /* slide up */
        uint8_t nv = (uint8_t)((ch->volcol & 0x0F) + ch->real_vol);
        if (nv > 64)
            nv = 64;
        ch->out_vol = ch->real_vol = nv;
        ch->status |= CS_UPDATE_VOL;
        break;
    }
    case 0xB:                                      /* vibrato */
        if (ch->volcol & 0x0F)
            ch->vib_depth = ch->volcol & 0x0F;
        do_vibrato(ch);
        break;
    case 0xD: {                                    /* pan slide left (FT2 bug: 0 sets pan 0) */
        uint16_t t = (uint16_t)(ch->out_pan + (uint8_t)(0 - (ch->volcol & 0x0F)));
        if (t < 256)
            t = 0;
        ch->out_pan = (uint8_t)t;
        ch->status |= CS_UPDATE_PAN;
        break;
    }
    case 0xE: {                                    /* pan slide right */
        uint16_t t = (uint16_t)(ch->out_pan + (ch->volcol & 0x0F));
        if (t > 255)
            t = 255;
        ch->out_pan = (uint8_t)t;
        ch->status |= CS_UPDATE_PAN;
        break;
    }
    case 0xF:
        portamento(ch);
        break;
    default:
        break;
    }
    if ((ch->efx == 0 && p == 0) || ch->efx > 35)
        return;
    switch (ch->efx) {
    case 0: {                                      /* arpeggio */
        uint8_t t = ft2_arpeggio[P.tick & 31];
        if (t == 0)
            ch->out_period = ch->real_period;
        else
            ch->out_period = period2note_period(ch->real_period, t == 1 ? (p >> 4) : (p & 0x0F), ch);
        ch->status |= CS_UPDATE_PERIOD;
        break;
    }
    case 1:
        if (p == 0)
            p = ch->pitch_up;
        ch->pitch_up = p;
        ch->real_period -= p * 4;
        if ((int16_t)ch->real_period < 1)
            ch->real_period = 1;
        pt_clamp(ch);
        ch->out_period = ch->real_period;
        ch->status |= CS_UPDATE_PERIOD;
        break;
    case 2:
        if (p == 0)
            p = ch->pitch_down;
        ch->pitch_down = p;
        ch->real_period += p * 4;
        if ((int16_t)ch->real_period >= 32000)
            ch->real_period = 32000 - 1;
        pt_clamp(ch);
        ch->out_period = ch->real_period;
        ch->status |= CS_UPDATE_PERIOD;
        break;
    case 3:
        portamento(ch);
        break;
    case 4:
        if (p > 0) {
            if (p & 0x0F)
                ch->vib_depth = p & 0x0F;
            if ((p & 0xF0) >> 2)
                ch->vib_speed = (p & 0xF0) >> 2;
        }
        do_vibrato(ch);
        break;
    case 5:
        portamento(ch);
        vol_slide(ch, p);
        break;
    case 6:
        do_vibrato(ch);
        vol_slide(ch, p);
        break;
    case 7:
        tremolo(ch, p);
        break;
    case 0xA:
        vol_slide(ch, p);
        break;
    case 0xE: {
        uint8_t e = p >> 4, q = p & 0x0F;
        if (e == 0x9) {                            /* retrig */
            if (q != 0 && (P.speed - P.tick) % q == 0) {
                trigger_note(0, 0, 0, ch);
                trigger_instrument(ch);
            }
        } else if (e == 0xC) {                     /* note cut */
            if ((uint8_t)(P.speed - P.tick) == q) {
                ch->out_vol = ch->real_vol = 0;
                ch->status |= CS_UPDATE_VOL | CS_QUICK_RAMP;
            }
        } else if (e == 0xD) {                     /* note delay */
            if ((uint8_t)(P.speed - P.tick) == q) {
                trigger_note((uint8_t)(ch->instr_and_note & 0xFF), 0, 0, ch);
                if (ch->instr_and_note >> 8)
                    reset_volumes(ch);
                trigger_instrument(ch);
                if (ch->volcol >= 0x10 && ch->volcol <= 0x50) {
                    ch->out_vol = (uint8_t)(ch->volcol - 16);
                    ch->real_vol = ch->out_vol;
                } else if (ch->volcol >= 0xC0 && ch->volcol <= 0xCF) {
                    ch->out_pan = (uint8_t)((ch->volcol & 0x0F) << 4);
                }
            }
        }
        break;
    }
    case 17: {                                     /* Hxy: global volume slide */
        uint8_t nv;
        int i;
        if (p == 0)
            p = ch->glob_vol_slide_speed;
        ch->glob_vol_slide_speed = p;
        nv = (uint8_t)P.global_vol;
        if ((p & 0xF0) == 0) {
            nv -= p;
            if ((int8_t)nv < 0)
                nv = 0;
        } else {
            nv += p >> 4;
            if (nv > 64)
                nv = 64;
        }
        P.global_vol = nv;
        for (i = 0; i < P.num_ch; i++)
            chan[i].status |= CS_UPDATE_VOL;
        break;
    }
    case 20:                                       /* Kxx */
        if ((uint8_t)(P.speed - P.tick) == (p & 31))
            key_off(ch);
        break;
    case 25: {                                     /* Pxy: panning slide */
        int16_t np;
        if (p == 0)
            p = ch->pan_slide_speed;
        ch->pan_slide_speed = p;
        np = ch->out_pan;
        if ((p & 0xF0) == 0) {
            np -= p;
            if (np < 0)
                np = 0;
        } else {
            np += p >> 4;
            if (np > 255)
                np = 255;
        }
        ch->out_pan = (uint8_t)np;
        ch->status |= CS_UPDATE_PAN;
        break;
    }
    case 27:
        do_multi_retrig(ch);
        break;
    case 29: {                                     /* Txy: tremor */
        uint8_t sign, data;
        if (p == 0)
            p = ch->tremor_param;
        ch->tremor_param = p;
        sign = ch->tremor_pos & 0x80;
        data = ch->tremor_pos & 0x7F;
        data--;
        if ((int8_t)data < 0) {
            if (sign == 0x80) {
                sign = 0x00;
                data = p & 0x0F;
            } else {
                sign = 0x80;
                data = p >> 4;
            }
        }
        ch->tremor_pos = sign | data;
        ch->out_vol = sign == 0x80 ? ch->real_vol : 0;
        ch->status |= CS_UPDATE_VOL | CS_QUICK_RAMP;
        break;
    }
    default:
        break;
    }
}

static void set_pos(int16_t pos, int16_t row)
{
    P.song_pos = pos;
    if (SONG_LEN > 0 && P.song_pos >= SONG_LEN)
        P.song_pos = (int16_t)(SONG_LEN - 1);
    P.patt_num = song.orders[P.song_pos];
    P.num_rows = (int16_t)song_rows((uint32_t)P.patt_num);
    P.row = row;
    if (P.row >= P.num_rows)
        P.row = (int16_t)(P.num_rows - 1);
}

static void get_next_pos(void)
{
    if (P.tick != 1)
        return;
    P.row++;
    if (P.patt_del > 0) {
        P.patt_del2 = P.patt_del;
        P.patt_del = 0;
    }
    if (P.patt_del2 > 0) {
        P.patt_del2--;
        if (P.patt_del2 > 0)
            P.row--;
    }
    if (P.pbreak_flag) {
        P.pbreak_flag = 0;
        P.row = P.pbreak_pos;
    }
    P.num_rows = (int16_t)song_rows((uint32_t)P.patt_num);   /* (the pattern may have been resized) */
    if (P.row >= P.num_rows || P.posjump_flag) {
        P.row = P.pbreak_pos;
        P.pbreak_pos = 0;
        P.posjump_flag = 0;
        if (P.patt_loop) {                         /* the same pattern again */
            P.bxx_overflow = 0;
            if (P.row >= P.num_rows)
                P.row = 0;
            return;
        }
        if (P.bxx_overflow) {
            P.song_pos = 0;
            P.bxx_overflow = 0;
        } else if (++P.song_pos >= SONG_LEN) {
            P.song_pos = SONG_LOOP;
        }
        P.patt_num = song.orders[P.song_pos & 0xFF];
        P.num_rows = (int16_t)song_rows((uint32_t)P.patt_num);
        if (P.row >= P.num_rows)
            P.row = 0;
    }
}

/* unpack one row of a pattern (all channels) */
static void read_row(uint16_t pattern, uint16_t row, cell_t *out, int nch)
{
    song_read_row(pattern, row, (song_cell *)out, (uint32_t)nch);   /* (the same five bytes) */
}

/* notes played on the keys (player_jam), on the next tick */
static void jam_tick(void)
{
    int i;
    for (i = 0; i < P.num_ch; i++)
        if (P.jam_on[i]) {
            cell_t c = {P.jam_note[i], P.jam_ins[i], 0, 0, 0};
            P.jam_on[i] = 0;
            get_new_note(&chan[i], &c);
        }
}

static void tick_replayer(void)
{
    int i;
    jam_tick();
    if (!P.playing) {
        for (i = 0; i < P.num_ch; i++)
            update_vol_pan_autovib(&chan[i]);
        return;
    }
    P.ticks++;
    if (--P.tick == 0) {
        P.tick = P.speed;
        if (P.patt_del2 == 0) {
            cell_t row[FMT_MAX_CHANNELS];
            read_row((uint16_t)P.patt_num, (uint16_t)P.row, row, P.num_ch);
            state.song_pos = (uint16_t)P.song_pos;
            state.pattern = (uint16_t)P.patt_num;
            state.row = (uint16_t)P.row;
            state.num_rows = (uint16_t)P.num_rows;
            for (i = 0; i < P.num_ch; i++) {
                get_new_note(&chan[i], &row[i]);
                update_vol_pan_autovib(&chan[i]);
            }
            get_next_pos();
            return;
        }
    }
    for (i = 0; i < P.num_ch; i++) {
        effects_tick_nonzero(&chan[i]);
        update_vol_pan_autovib(&chan[i]);
    }
    get_next_pos();
}

/* ----------------------------------------------------------------- voices --- */

static void voice_update_volumes(int i, uint8_t status)
{
    voice_t *v = &voice[i];
    /* 64-bit product: full volume (65536) at a hard pan (65536) is 2^32 */
    v->tgt_l = (int32_t)((((uint64_t)v->vol_q16 * ft2_sqrt_pan[256 - v->pan]) >> 16) << 8);
    v->tgt_r = (int32_t)((((uint64_t)v->vol_q16 * ft2_sqrt_pan[v->pan]) >> 16) << 8);
    if (status & CS_TRIGGER_VOICE) {
        if (v->vol_l > 0 || v->vol_r > 0) {        /* fade the old note out over 5 ms */
            voice_t *f = &voice[FMT_MAX_CHANNELS + i];
            *f = *v;
            f->ramp = P.quick_ramp;
            f->tgt_l = f->tgt_r = 0;
            f->d_l = -f->vol_l / (int32_t)P.quick_ramp;
            f->d_r = -f->vol_r / (int32_t)P.quick_ramp;
            f->fade_voice = 1;
        }
        v->vol_l = v->vol_r = 0;
    }
    if (!(status & CS_TRIGGER_VOICE) && v->tgt_l == v->vol_l && v->tgt_r == v->vol_r) {
        v->ramp = 0;
    } else {
        uint32_t len = (status & CS_QUICK_RAMP) ? P.quick_ramp : P.spt_int[P.bpm];
        if (len == 0)
            len = 1;
        v->ramp = len;
        v->d_l = (v->tgt_l - v->vol_l) / (int32_t)len;
        v->d_r = (v->tgt_r - v->vol_r) / (int32_t)len;
    }
}

static void voice_trigger(int i, const fmt_sample *s, int32_t start)
{
    voice_t *v = &voice[i];
    uint32_t loop = s->flags & (FMT_LOOP_FWD | FMT_LOOP_PINGPONG);
    if (!s->data_off || s->length < 1) {
        v->active = 0;
        return;
    }
    if (s->loop_length < 1)
        loop = 0;
    v->is16 = (s->flags & FMT_16BIT) != 0;
    v->b8 = (const int8_t *)(P.img + s->data_off);
    v->b16 = (const int16_t *)(P.img + s->data_off);
    v->loop = (uint8_t)loop;
    v->loop_start = s->loop_start;
    v->loop_len = s->loop_length;
    v->end = loop ? s->loop_start + s->loop_length : s->length;
    v->mirror = s->loop_start + v->end - 1;
    v->backwards = 0;
    v->pos = (uint32_t)start;
    v->frac = 0;
    v->fade_voice = 0;
    if (v->pos >= v->end) {
        v->active = 0;
        return;
    }
    v->active = 1;
}

static void update_voices(void)
{
    int i;
    for (i = 0; i < P.num_ch; i++) {
        channel_t *ch = &chan[i];
        voice_t *v = &voice[i];
        uint8_t st = ch->status;
        if (!st)
            continue;
        ch->status = 0;
        if (st & CS_UPDATE_VOL)
            v->vol_q16 = ch->final_vol;
        if (P.pt_mode)                             /* Amiga: left, right, right, left */
            v->pan = (i & 3) == 0 || (i & 3) == 3 ? 0 : 255;
        else if (st & CS_UPDATE_PAN)
            v->pan = ch->final_pan;
        if (st & (CS_UPDATE_VOL | CS_UPDATE_PAN))
            voice_update_volumes(i, st);
        if (st & CS_UPDATE_PERIOD)
            set_voice_delta(v, ch->final_period);
        if (st & CS_TRIGGER_VOICE)
            voice_trigger(i, ch->smp, ch->smp_start);
    }
}

/* ------------------------------------------------------------------ mixer --- */

/* after pos passed v->end: wrap (loop) or stop; 0 = stopped */
static int voice_wrap(voice_t *v)
{
    if (!v->loop) {
        v->active = 0;
        return 0;
    }
    if (v->loop == FMT_LOOP_FWD) {
        v->pos = v->loop_len >= 2 ? v->loop_start + (v->pos - v->end) % v->loop_len : v->loop_start;
    } else if (v->loop_len >= 2) {
        uint32_t over = v->pos - v->end;
        uint32_t cycles = over / v->loop_len, phase = over % v->loop_len;
        v->pos = v->loop_start + phase;
        v->backwards ^= !(cycles & 1u);
    } else {
        v->pos = v->loop_start;
    }
    return 1;
}

static void voice_skip(voice_t *v, uint32_t n)  /* silent voice: move the position only */
{
    uint64_t adv = (uint64_t)v->dfrac * n + v->frac;
    v->pos += v->dint * n + (uint32_t)(adv >> 32);
    v->frac = (uint32_t)adv;
    if (v->pos >= v->end)
        voice_wrap(v);
}

#define MIX_LOOP(READ)                                                                 \
    for (; i < lim; i++) {                                                             \
        int32_t s0, s1, s;                                                             \
        uint32_t f;                                                                    \
        if (!v->backwards) {                                                           \
            s0 = READ(pos);                                                            \
            /* the loop's end interpolates to its start (loops edited on the FM-1 have \
             * no guard copy of it there) */                                           \
            s1 = pos + 1u >= v->end && v->loop == FMT_LOOP_FWD ? READ(v->loop_start)   \
                                                                : READ(pos + 1);       \
        } else {                                                                       \
            uint32_t a = v->mirror - pos;                                              \
            s0 = READ(a);                                                              \
            s1 = READ(a - 1);                                                          \
        }                                                                              \
        s = s0 + (((s1 - s0) * (int32_t)(frac >> 18)) >> 14);                          \
        if (ramp) {                                                                    \
            vl += dl;                                                                  \
            vr += dr;                                                                  \
            if (--ramp == 0) {                                                         \
                vl = v->tgt_l;                                                         \
                vr = v->tgt_r;                                                         \
                if (v->fade_voice) {                                                   \
                    v->active = 0;                                                     \
                    break;                                                             \
                }                                                                      \
            }                                                                          \
        }                                                                              \
        if (s > peak)                                                                  \
            peak = s;                                                                  \
        else if (-s > peak)                                                            \
            peak = -s;                                                                 \
        out[2 * i] += (s * (vl >> 9)) >> 15;                                           \
        out[2 * i + 1] += (s * (vr >> 9)) >> 15;                                       \
        f = frac + v->dfrac;                                                           \
        pos += v->dint + (f < frac);                                                   \
        frac = f;                                                                      \
        if (pos >= v->end) {                                                           \
            v->pos = pos;                                                              \
            if (!voice_wrap(v))                                                        \
                break;                                                                 \
            pos = v->pos;                                                              \
        }                                                                              \
    }

#define RD8(x) ((int32_t)b8[(int32_t)(x)] * 256)
#define RD16(x) ((int32_t)b16[(int32_t)(x)])

/* m samples that cannot reach the sample's end (pos + 1 < end throughout) nor the ramp's last step:
 * MIX_LOOP's arithmetic without its checks */
#define MIX_FAST(READ)                                                                 \
    if (!v->backwards) {                                                               \
        for (; m; m--, i++) {                                                          \
            int32_t s0 = READ(pos), s1 = READ(pos + 1), s;                             \
            uint32_t f;                                                                \
            s = s0 + (((s1 - s0) * (int32_t)(frac >> 18)) >> 14);                      \
            if (ramp) { vl += dl; vr += dr; ramp--; }                                  \
            if (s > peak) peak = s; else if (-s > peak) peak = -s;                     \
            out[2 * i] += (s * (vl >> 9)) >> 15;                                       \
            out[2 * i + 1] += (s * (vr >> 9)) >> 15;                                   \
            f = frac + v->dfrac;                                                       \
            pos += v->dint + (f < frac);                                               \
            frac = f;                                                                  \
        }                                                                              \
    } else {                                                                           \
        for (; m; m--, i++) {                                                          \
            uint32_t a = v->mirror - pos, f;                                           \
            int32_t s0 = READ(a), s1 = READ(a - 1), s;                                 \
            s = s0 + (((s1 - s0) * (int32_t)(frac >> 18)) >> 14);                      \
            if (ramp) { vl += dl; vr += dr; ramp--; }                                  \
            if (s > peak) peak = s; else if (-s > peak) peak = -s;                     \
            out[2 * i] += (s * (vl >> 9)) >> 15;                                       \
            out[2 * i + 1] += (s * (vr >> 9)) >> 15;                                   \
            f = frac + v->dfrac;                                                       \
            pos += v->dint + (f < frac);                                               \
            frac = f;                                                                  \
        }                                                                              \
    }

static void mix_voice(voice_t *v, int32_t *out, uint32_t n)
{
    uint32_t i = 0, lim, pos = v->pos, frac = v->frac, ramp = v->ramp;
    int32_t vl = v->vol_l, vr = v->vol_r, dl = v->d_l, dr = v->d_r, peak = 0;
    const int8_t *b8 = v->b8;
    const int16_t *b16 = v->b16;
    while (i < n && v->active) {
        /* how many samples surely stay before end - 1 (a sample moves pos at most dint + 1) */
        uint32_t m = pos + 2u <= v->end ? (v->end - 2u - pos) / (v->dint + 1u) + 1u : 0u;
        if (m > n - i)
            m = n - i;
        if (ramp && m > ramp - 1u)
            m = ramp - 1u;
        if (m) {
            if (v->is16) {
                MIX_FAST(RD16)
            } else {
                MIX_FAST(RD8)
            }
            if (pos >= v->end) {               /* (as MIX_LOOP after its last step) */
                v->pos = pos;
                if (!voice_wrap(v))
                    break;
                pos = v->pos;
            }
        } else {                               /* near the end or the ramp's end: one exact step */
            lim = i + 1u;
            if (v->is16) {
                MIX_LOOP(RD16)
            } else {
                MIX_LOOP(RD8)
            }
        }
    }
    if (v->active) {
        v->pos = pos;
        v->frac = frac;
    }
    v->ramp = ramp;
    v->vol_l = vl;
    v->vol_r = vr;
    {
        uint32_t pk = (uint32_t)(((uint64_t)(uint32_t)peak * (uint32_t)((vl > vr ? vl : vr) >> 8)) >> 23);
        if (pk > 255)
            pk = 255;
        if (pk > v->peak)
            v->peak = (uint8_t)pk;
    }
}

static void mix(int32_t *out, uint32_t n)
{
    int i;
    for (i = 0; i < P.num_ch; i++) {
        voice_t *v = &voice[i], *f = &voice[FMT_MAX_CHANNELS + i];
        if (v->active) {
            if (!v->ramp && v->vol_l == 0 && v->vol_r == 0)
                voice_skip(v, n);
            else
                mix_voice(v, out, n);
        }
        if (f->active)
            mix_voice(f, out, n);
    }
}

/* -------------------------------------------------------------------- API --- */

void player_init(uint32_t rate_num, uint32_t rate_den)
{
    uint32_t bpm;
    memset(&P, 0, sizeof P);
    P.rate_num = rate_num;
    P.rate_den = rate_den;
    P.amp = 4;
    P.pt_enable = 1;                               /* MODs play as on an Amiga 500 */
    P.stereo_sep = 20 * 256 / 100;                 /* (pt2-clone's default) */
    /* FT2 at 44000 Hz: "Boy, what a mess." (ft2-clone calcReplayerVars) */
    P.freq_mul = (uint32_t)udiv64(256ull * 65536ull * 8363ull + 22000ull, 44000ull);
    P.freq_div = (uint32_t)udiv64(65536ull * 1712ull * 8363ull + 22000ull, 44000ull);
    /* 16.16 at 44000 Hz -> 32.32 at our rate: * 65536 * 44000 / rate */
    P.delta_mul = (uint32_t)udiv64(((uint64_t)44000u * rate_den << 32) + rate_num / 2u, rate_num);
    P.quick_ramp = (uint32_t)udiv64((uint64_t)rate_num + 100ull * rate_den, 200ull * rate_den);
    for (bpm = 32; bpm <= 255; bpm++) {
        uint32_t ft2spt = 110000u / bpm;           /* (44000*2 + 44000/2) / bpm */
        uint64_t nn = (uint64_t)rate_num * ft2spt, dd = (uint64_t)44000u * rate_den;
        uint64_t q = udiv64(nn, dd);
        uint64_t r = nn - q * dd;
        P.spt_int[bpm] = (uint32_t)q;
        P.spt_frac[bpm] = (uint32_t)udiv64(r << 32, dd);
    }
    memset(chan, 0, sizeof chan);
    memset(voice, 0, sizeof voice);
    memset(&state, 0, sizeof state);
}

static void reset_channels(void)
{
    int i;
    memset(chan, 0, sizeof chan);
    memset(voice, 0, sizeof voice);
    for (i = 0; i < FMT_MAX_CHANNELS; i++) {
        chan[i].ins = &empty_instr;
        chan[i].smp = &placeholder_smp;
        chan[i].status = CS_UPDATE_VOL;
        chan[i].old_pan = chan[i].out_pan = chan[i].final_pan = 128;
        chan[i].channel_off = P.user_mute[i];
    }
}

int player_load(const uint8_t *image, uint32_t size)
{
    const fmt_header *h = (const fmt_header *)image;
    int r;
    P.playing = 0;
    P.h = 0;
    P.patt_loop = 0;
    r = song_load(image, size);
    P.pt_mode = !r && P.pt_enable && (((const fmt_header *)image)->flags & FMT_FROM_MOD);
    P.led = 0;
    memset(P.lp, 0, sizeof P.lp);
    memset(P.bq, 0, sizeof P.bq);    /* the order list and the patterns go to RAM */
    if (r)
        return r;
    P.img = image;
    P.h = h;
    P.ins = song.ins;                /* (the RAM copies, song.c) */
    P.smp = song.smp;
    P.linear = (h->flags & FMT_LINEAR) != 0;
    P.num_ch = h->num_channels;
    P.bpm = h->bpm < 32 ? 32 : h->bpm > 255 ? 255 : h->bpm;
    P.speed = h->speed < 1 ? 1 : h->speed > 31 ? 31 : h->speed;
    P.global_vol = 64;
    reset_channels();
    set_pos(0, 0);
    P.tick = 1;
    state.num_channels = P.num_ch;
    return 0;
}

void player_rebind(const uint8_t *image)
{
    const fmt_header *h = (const fmt_header *)image;
    int i;
    if (!P.h)
        return;
    P.playing = 0;
    for (i = 0; i < 2 * FMT_MAX_CHANNELS; i++)
        voice[i].active = 0;
    reset_channels();
    P.img = image;
    P.h = h;
    P.ins = song.ins;                /* (the RAM copies, song.c) */
    P.smp = song.smp;
    song_rebind(image, h->size);
}

void player_set_pt_mode(int on)
{
    P.pt_enable = (uint8_t)(on != 0);
    P.pt_mode = P.pt_enable && P.h && (P.h->flags & FMT_FROM_MOD);
}

int player_pt_mode(void) { return P.pt_mode; }
uint32_t player_ticks(void) { return P.ticks; }
int player_playing(void) { return P.playing; }
int player_led(void) { return P.led; }

void player_set_stereo_sep(int percent)
{
    P.stereo_sep = (uint16_t)((percent < 0 ? 0 : percent > 100 ? 100 : percent) * 256 / 100);
}

void player_loop_pattern(int on)
{
    P.patt_loop = (uint8_t)(on != 0);
}

void player_jam(int c, uint8_t note, uint8_t instr)
{
    if (c < 0 || c >= P.num_ch)
        return;
    P.jam_note[c] = note;
    P.jam_ins[c] = instr;
    P.jam_on[c] = 1;                 /* (taken by the next tick, in the audio context) */
}

void player_play(uint16_t song_pos)
{
    if (!P.h)
        return;
    P.playing = 0;
    reset_channels();
    P.bpm = song.hdr.bpm < 32 ? 32 : song.hdr.bpm > 255 ? 255 : song.hdr.bpm;
    P.speed = song.hdr.speed < 1 ? 1 : song.hdr.speed > 31 ? 31 : song.hdr.speed;
    P.global_vol = 64;
    P.patt_del = P.patt_del2 = 0;
    P.posjump_flag = P.pbreak_flag = P.bxx_overflow = 0;
    P.pbreak_pos = 0;
    set_pos((int16_t)song_pos, 0);
    P.tick = 1;
    P.ticks = 0;
    P.frames = 0;
    P.tick_left = 0;
    P.tick_frac = 0;
    P.playing = 1;
}

void player_set_pos(uint16_t song_pos)
{
    if (!P.h)
        return;
    if (song_pos >= song.hdr.song_length)
        song_pos = (uint16_t)(song.hdr.song_length - 1);
    P.patt_del = P.patt_del2 = 0;
    P.posjump_flag = P.pbreak_flag = P.bxx_overflow = 0;
    P.pbreak_pos = 0;
    set_pos((int16_t)song_pos, 0);
    P.tick = 1;                                    /* the next tick reads row 0 */
    P.tick_left = 0;
    state.song_pos = song_pos;
    state.pattern = (uint16_t)P.patt_num;
    state.row = 0;
    state.num_rows = (uint16_t)P.num_rows;
}

void player_stop(void)
{
    int i;
    P.playing = 0;
    for (i = 0; i < 2 * FMT_MAX_CHANNELS; i++)
        voice[i].active = 0;
}

void player_set_speed(int speed) { P.speed = (uint16_t)(speed < 1 ? 1 : speed > 31 ? 31 : speed); }
void player_set_bpm(int bpm) { P.bpm = (uint16_t)(bpm < 32 ? 32 : bpm > 255 ? 255 : bpm); }
void player_set_global_vol(int v) { P.global_vol = (uint16_t)(v < 0 ? 0 : v > 64 ? 64 : v); }
int player_get_amp(void) { return P.amp; }

void player_set_amp(int amp)
{
    P.amp = amp < 1 ? 1 : amp > 32 ? 32 : amp;
}

void player_mute(int c, int muted)
{
    if (c < 0 || c >= FMT_MAX_CHANNELS)
        return;
    P.user_mute[c] = (uint8_t)(muted != 0);
    chan[c].channel_off = P.user_mute[c];
    if (muted) {
        voice[c].active = 0;
        voice[FMT_MAX_CHANNELS + c].active = 0;
    }
}

static void tick(void)
{
    int i;
    for (i = 0; i < P.num_ch; i++) {             /* ft2-clone resetRampVolumes */
        voice[i].vol_l = voice[i].tgt_l;
        voice[i].vol_r = voice[i].tgt_r;
        voice[i].ramp = 0;
    }
    tick_replayer();
    update_voices();
    P.tick_left = P.spt_int[P.bpm];
    {
        uint32_t f = P.tick_frac + P.spt_frac[P.bpm];
        if (f < P.tick_frac)
            P.tick_left++;
        P.tick_frac = f;
    }
}

/* ProTracker mode, the mixed output: stereo separation (mid / side, as pt2-clone), the A500's
 * 4421 Hz one-pole low-pass, and the LED filter (2-pole Sallen-Key, 3091 Hz, Q 0.66) when on.
 * Coefficients for 44118 Hz (the FM-1; the PC's 44100 is close enough), Q24. */
#define PT_LP_A0 7838498
#define PT_LP_B1 8938718
#define PT_LED_A1 604486
#define PT_LED_A2 1208971
#define PT_LED_B1 (-22950687)
#define PT_LED_B2 8591414

static void pt_output(int32_t *out, uint32_t frames)
{
    uint32_t i, c;
    for (i = 0; i < frames; i++) {
        int32_t l = out[2 * i], r = out[2 * i + 1];
        int32_t mid = (l + r) >> 1, side = (int32_t)(((int64_t)((l - r) >> 1) * P.stereo_sep) >> 8);
        int32_t v[2] = {mid + side, mid - side};
        for (c = 0; c < 2; c++) {
            int32_t x = v[c], *b = P.bq[c];
            P.lp[c] = (int32_t)(((int64_t)x * PT_LP_A0 + (int64_t)P.lp[c] * PT_LP_B1) >> 24);
            x = P.lp[c];
            if (P.led) {
                int32_t y = (int32_t)(((int64_t)x * PT_LED_A1 + (int64_t)b[0] * PT_LED_A2 + (int64_t)b[1] * PT_LED_A1 -
                                       (int64_t)b[2] * PT_LED_B1 - (int64_t)b[3] * PT_LED_B2) >> 24);
                b[1] = b[0];
                b[0] = x;
                b[3] = b[2];
                b[2] = y;
                x = y;
            }
            out[2 * i + c] = x;
        }
    }
}

void player_render(int32_t *out, uint32_t frames)
{
    uint32_t i, done = 0;
    for (i = 0; i < 2u * frames; i++)
        out[i] = 0;
    if (!P.h)
        return;
    while (done < frames) {
        uint32_t n;
        if (P.tick_left == 0) {
            tick();                               /* (stopped: envelopes and the notes played on the keys) */
        }
        n = frames - done;
        if (P.tick_left > 0 && n > P.tick_left)
            n = P.tick_left;
        mix(out + 2u * done, n);
        done += n;
        P.tick_left -= n;
    }
    if (P.pt_mode)
        pt_output(out, frames);
    if (P.amp != 32)
        for (i = 0; i < 2u * frames; i++)
            out[i] = (out[i] * P.amp) >> 5;
    P.frames += frames;
}

const player_state *player_get_state(void)
{
    int i;
    state.playing = P.playing;
    state.speed = P.speed;
    state.bpm = P.bpm;
    state.tick = P.tick;
    state.global_vol = P.global_vol;
    state.ticks = P.ticks;
    state.frames = P.frames;
    state.num_channels = P.num_ch;
    for (i = 0; i < P.num_ch; i++) {
        const channel_t *ch = &chan[i];
        voice_t *v = &voice[i];
        player_chan_info *c = &state.ch[i];
        c->note = ch->last.note;
        c->instr = ch->last.instr;
        c->vol = ch->last.vol;
        c->efx = ch->last.efx;
        c->efx_data = ch->last.efx_data;
        c->active = v->active;
        c->final_vol = (uint8_t)(ch->final_vol >> 10);
        c->final_pan = ch->final_pan;
        c->period = ch->final_period;
        c->peak = v->peak > voice[FMT_MAX_CHANNELS + i].peak ? v->peak : voice[FMT_MAX_CHANNELS + i].peak;
    }
    return &state;
}

void player_take_peaks(uint8_t *peaks, int n)
{
    int i;
    for (i = 0; i < n && i < FMT_MAX_CHANNELS; i++) {
        voice_t *v = &voice[i], *f = &voice[FMT_MAX_CHANNELS + i];
        peaks[i] = v->peak > f->peak ? v->peak : f->peak;
        v->peak = 0;
        f->peak = 0;
    }
}

const fmt_header *player_song(void)
{
    return P.h ? &song.hdr : 0;
}

int player_get_row(uint16_t pattern, uint16_t row, player_cell *cells, int max_cells)
{
    cell_t tmp[FMT_MAX_CHANNELS];
    int i, n;
    if (!P.h)
        return 0;
    n = P.num_ch < max_cells ? P.num_ch : max_cells;
    read_row(pattern, row, tmp, P.num_ch);
    for (i = 0; i < n; i++) {
        cells[i].note = tmp[i].note;
        cells[i].instr = tmp[i].instr;
        cells[i].vol = tmp[i].vol;
        cells[i].efx = tmp[i].efx;
        cells[i].efx_data = tmp[i].efx_data;
    }
    return n;
}
