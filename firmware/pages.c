/* SPDX-License-Identifier: GPL-3.0-only */
/* The tracker's pages. Portable: the same code runs on the FM-1 (firmware/ui.c) and in the PC
 * simulator (host/uisim.c). A button opens its page, pressing it again switches the page's view:
 *
 *   HOME  the pattern: 4 channels, again 8; follows playback, the playing row in the middle;
 *         edit mode (REC), note entry, the knobs per field (edit.c)
 *   SEQ   the song: the order list, again the pattern list
 *   SAVE  the songs: the flash slots (store.c) and the ones built into the firmware
 *   FX    effect help (FX again: back)
 *   GLO   the mixer (the platform draws it), again 8 channels; GLO held on any page: the white
 *         keys mute channels
 *   ENV, LFO  the instrument's envelopes and the sample (pages_ins.c)
 *
 * The platform gives: fm1_ms, the gfx.c drawing, SONGS / NSONGS, and the sys_*, stp_* and glo_*
 * functions declared below. */

/* panel ids: matrix buttons 0..13, encoders 0..6 (Felucca panel.c PANEL_DEFAULT; PLAY and REC
 * measured on hardware, the rest Felucca's map) */
#define BTN_OCTDN 0u
#define BTN_OCTUP 1u
#define BTN_FX 2u
#define BTN_SEL 3u
#define BTN_ENV 4u
#define BTN_LFO 5u
#define BTN_EDIT 6u
#define BTN_GLO 7u
#define BTN_HOME 8u
#define BTN_SAVE 9u
#define BTN_ARP 10u
#define BTN_SEQ 11u
#define BTN_PLAY 12u
#define BTN_REC 13u
#define ENC_SELECT 0u
#define ENC_ALGO 1u
#define ENC_K1 2u
#define ENC_K2 3u
#define ENC_K3 4u
#define ENC_K4 5u
#define ENC_PRESET 6u
#define NENC 7u
#define BIT(id) (1u << (id))

typedef struct {
    uint32_t pressed, released, held;   /* buttons: bit = id */
    uint32_t notes;                     /* note keys pressed: bit 0 = F3 .. bit 26 = G5 */
    uint32_t notes_held;                /* note keys down */
    int32_t enc[NENC];                  /* steps, + = clockwise */
} ui_in;

/* names (the console's and the simulator's scripts) */
static const struct { const char *n; uint8_t id; } BTN_NAMES[] = {
    {"OCT-", BTN_OCTDN}, {"OCT+", BTN_OCTUP}, {"FX", BTN_FX}, {"SEL", BTN_SEL}, {"ENV", BTN_ENV},
    {"LFO", BTN_LFO}, {"EDIT", BTN_EDIT}, {"GLO", BTN_GLO}, {"HOME", BTN_HOME}, {"SAVE", BTN_SAVE},
    {"ARP", BTN_ARP}, {"SEQ", BTN_SEQ}, {"PLAY", BTN_PLAY}, {"REC", BTN_REC}};
static const char *const ENC_NAMES[NENC] = {"SELECT", "ALGO", "K1", "K2", "K3", "K4", "PRESET"};

static int name_eq(const char *a, const char *b, uint32_t n)   /* a's first n chars == b */
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i] || !b[i])
            return 0;
    return b[n] == 0;
}

static int btn_id(const char *s, uint32_t n)       /* -1: unknown */
{
    uint32_t i;
    for (i = 0; i < sizeof BTN_NAMES / sizeof BTN_NAMES[0]; i++)
        if (name_eq(s, BTN_NAMES[i].n, n))
            return BTN_NAMES[i].id;
    return -1;
}

static int enc_id(const char *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < NENC; i++)
        if (name_eq(s, ENC_NAMES[i], n))
            return (int)i;
    return -1;
}

/* ------------------------------------------------------------ from the platform --- */
static void sys_audio_hold(void);       /* the audio stops calling the player (before load / play) */
static void sys_audio_go(void);
static uint32_t sys_lock(void);         /* the audio must not tick while the position changes */
static void sys_unlock(uint32_t f);
static void sys_led(uint32_t id, int on);
static void glo_stats(gline *cpu, gline *usb, int reset);   /* the mixer's two platform lines (ui.c, uisim.c, websim.c) */

/* ------------------------------------------------------------------- the state --- */
enum { PG_HOME, PG_SEQ, PG_ENV, PG_LFO, PG_GLO, PG_SAVE, PG_FX, NPAGES };
static const uint8_t PAGE_BTN[NPAGES] = {BTN_HOME, BTN_SEQ, BTN_ENV, BTN_LFO, BTN_GLO, BTN_SAVE, BTN_FX};

static struct {
    int song;                      /* index into SONGS, -1 = none loaded */
    int loaded_ok;
    uint8_t muted[FMT_MAX_CHANNELS];       /* MUTE toggles */
    uint8_t solo[FMT_MAX_CHANNELS];        /* SOLO toggles */
    uint8_t off[FMT_MAX_CHANNELS];         /* what the player has: muted, or not in the solo group */
    uint8_t solo_mode;                     /* the white keys: 0 MUTE, 1 SOLO */
    uint8_t bank;                          /* the white keys: 0 channels 1..16, 1 channels 17..32 */
    uint8_t level[FMT_MAX_CHANNELS];       /* meters, 0..255 */
    uint32_t frames;
    const char *msg;
    uint32_t msg_ms;

    uint8_t page, prev_page, view[NPAGES], redraw;
    uint16_t pos, row;             /* where the view is (follows playback) */
    uint8_t chan, win;             /* the cursor channel, the first channel shown */
    uint8_t ins, oct;              /* the instrument and octave notes go in with */
    uint16_t pat_cur;              /* SEQ pattern list */
    uint8_t fx_cur;                /* FX page */
    int song_cur;                  /* SAVE page */
    uint8_t glo_keys;              /* a white key went while GLO was held */
    uint16_t pat_notes[256], pat_notes_max;

    uint16_t vpat;                 /* the pattern shown on HOME */
    uint8_t edit;                  /* edit mode (REC) */
    uint8_t field;                 /* the cursor's field: 0 note, 1 instrument, 2 volume, 3 effect */
    uint8_t step;                  /* rows the cursor moves after a note */
    uint8_t changed;               /* the song differs from what was loaded / saved */
    uint8_t sel_on, sel_ch;        /* the block: from (sel_row, sel_ch) to the cursor, in sel_pat */
    uint16_t sel_row, sel_pat;
    uint8_t sel_moved, arp_done;   /* SEL held and the cursor moved; ARP held long enough (done) */
    uint32_t sel_t0, arp_t0;
    uint32_t notes_held;           /* the note keys down last frame */
    int8_t key_chan[27];           /* the channel a held key plays on, -1: none */
    int8_t chan_key[FMT_MAX_CHANNELS];   /* the key that played last on a channel */
    uint8_t looping;               /* playing one pattern over (EDIT+PLAY) */
    uint8_t env_pt, smp_k;         /* ENV: the envelope point; LFO: the sample in the instrument */
    uint8_t sep_i;                 /* GLO: the stereo separation step (ProTracker mode): 1 = 20 % */
    uint32_t img_off;              /* the flash slot the instruments and samples are read from, 0: none */
    uint32_t slot_off;             /* the flash slot SAVE replaces, 0: a new one */
    uint8_t act, naming, name_cur; /* SAVE page: the action; editing the name, its cursor */
    uint8_t confirm;               /* SAVE page: the action waiting for REC again (+1), 0 none */
    uint32_t confirm_ms;
} ui;

static void undo_clear(void);      /* (edit.c) */
static void page_open(uint32_t p);

static void ui_message(const char *m)
{
    ui.msg = m;
    ui.msg_ms = fm1_ms;
}

/* ---------------------------------------------------------------- song access --- */
static const fmt_header *song_h(void) { return ui.loaded_ok ? player_song() : 0; }

static const uint8_t *song_orders(const fmt_header *h) { (void)h; return song.orders; }

static uint32_t song_len(const fmt_header *h) { return h && h->song_length ? h->song_length : 1u; }

static uint32_t pat_rows(const fmt_header *h, uint32_t p)
{
    (void)h;
    return song_rows(p);           /* a missing pattern plays as 64 empty rows (FT2) */
}

static const fmt_instr *song_instr(const fmt_header *h, uint32_t i)        /* 1..num_instr */
{
    if (!h || i < 1u || i > h->num_instr)
        return 0;
    return (const fmt_instr *)(song.img + ((const fmt_header *)song.img)->instr_off) + (i - 1u);
}

static uint32_t pos_pattern(const fmt_header *h, uint32_t pos)
{
    return h && pos < song_len(h) ? song_orders(h)[pos] : 0;
}

static void get_row(const fmt_header *h, uint32_t p, uint32_t row, player_cell *cells)
{
    (void)h;
    song_read_row(p, row, (song_cell *)cells, FMT_MAX_CHANNELS);   /* (the same five bytes) */
}

static int is_mod(const fmt_header *h) { return h && (h->flags & FMT_FROM_MOD); }

/* ----------------------------------------------------------- load, play, mute --- */
static void ui_apply_mutes(void)   /* muted or outside the solo group -> the player */
{
    int k, any = 0;
    for (k = 0; k < FMT_MAX_CHANNELS; k++)
        any |= ui.solo[k];
    for (k = 0; k < FMT_MAX_CHANNELS; k++) {
        uint8_t off = (uint8_t)(ui.muted[k] || (any && !ui.solo[k]));
        if (off != ui.off[k]) {
            ui.off[k] = off;
            player_mute(k, off);
        }
    }
}

static void count_notes(void)
{
    const fmt_header *h = song_h();
    uint32_t p, r, c;
    player_cell cells[FMT_MAX_CHANNELS];
    ui.pat_notes_max = 1;
    for (p = 0; p < 256u; p++) {
        uint32_t n = 0;
        if (h && p < h->num_patterns)
            for (r = 0; r < pat_rows(h, p); r++) {
                get_row(h, p, r, cells);
                for (c = 0; c < h->num_channels; c++)
                    n += cells[c].note != 0;
            }
        ui.pat_notes[p] = (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
        if (n > ui.pat_notes_max)
            ui.pat_notes_max = (uint16_t)ui.pat_notes[p];
    }
}

/* a song image: built-in song `builtin` (-1: none), or the flash slot at slot_off (0: none) */
static void ui_load_image(const uint8_t *img, uint32_t size, int builtin, uint32_t slot_off)
{
    int k;
    const fmt_header *h;
    sys_audio_hold();
    player_stop();
    ui.song = builtin;
    ui.img_off = ui.slot_off = slot_off;
    ui.loaded_ok = player_load(img, size) == 0;
    for (k = 0; k < FMT_MAX_CHANNELS; k++) {
        ui.muted[k] = ui.solo[k] = ui.off[k] = ui.level[k] = 0;
        player_mute(k, 0);
    }
    h = song_h();
    ui.bank = 0;
    ui.pos = ui.row = 0;
    ui.chan = ui.win = 0;
    ui.ins = h && h->num_instr ? 1u : 0u;
    ui.pat_cur = 0;
    ui.redraw = 1;
    ui.sel_on = 0;
    ui.changed = 0;
    for (k = 0; k < 27; k++)
        ui.key_chan[k] = -1;
    for (k = 0; k < FMT_MAX_CHANNELS; k++)
        ui.chan_key[k] = -1;
    ui.field = 0;
    undo_clear();
    count_notes();
    sys_audio_go();                /* (stopped: the keys and the envelopes still sound) */
    if (!ui.loaded_ok)
        ui_message("BAD SONG IMAGE");
}

static void ui_load(int i) { ui_load_image(SONGS[i].data, SONGS[i].size, i, 0); }

static void ui_play_loop(int on, int loop)   /* from the view's position; loop: this pattern only */
{
    if (!ui.loaded_ok)
        return;
    ui.looping = (uint8_t)(on && loop);
    sys_audio_hold();
    player_loop_pattern(on && loop);
    if (on)
        player_play(ui.pos);
    else
        player_stop();
    sys_audio_go();
}

static void ui_play(int on) { ui_play_loop(on, 0); }

static void ui_seek(int32_t pos)   /* playing: jump there, row 0 */
{
    const fmt_header *h = song_h();
    uint32_t f;
    if (!h)
        return;
    pos = pos < 0 ? 0 : pos >= (int32_t)song_len(h) ? (int32_t)song_len(h) - 1 : pos;
    f = sys_lock();
    player_set_pos((uint16_t)pos);
    sys_unlock(f);
    ui.pos = (uint16_t)pos;
    ui.row = 0;
}

/* stopped: move the view by d rows, across patterns */
static void view_rows(int32_t d)
{
    const fmt_header *h = song_h();
    int32_t row = (int32_t)ui.row + d, pos = ui.pos, last = (int32_t)song_len(h) - 1;
    if (!h)
        return;
    while (row < 0) {
        if (pos == 0) { row = 0; break; }
        pos--;
        row += (int32_t)pat_rows(h, pos_pattern(h, (uint32_t)pos));
    }
    while (row >= (int32_t)pat_rows(h, pos_pattern(h, (uint32_t)pos))) {
        if (pos == last) { row = (int32_t)pat_rows(h, pos_pattern(h, (uint32_t)pos)) - 1; break; }
        row -= (int32_t)pat_rows(h, pos_pattern(h, (uint32_t)pos));
        pos++;
    }
    ui.pos = (uint16_t)pos;
    ui.row = (uint16_t)(row < 0 ? 0 : row);
}

/* the white keys, F3 .. G5: key number (0 = F3) of white key w */
static const uint8_t WHITE_KEY[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};

static void keys_mute(uint32_t notes, uint32_t nch)
{
    uint32_t k;
    for (k = 0; k < 16u; k++) {
        uint32_t c = ui.bank * 16u + k;
        if (!((notes >> WHITE_KEY[k]) & 1u) || c >= nch)
            continue;
        if (ui.solo_mode)
            ui.solo[c] ^= 1u;
        else
            ui.muted[c] ^= 1u;
        ui.glo_keys = 1;
    }
    ui_apply_mutes();
}

static uint32_t str_n(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* the cursor channel and the window of n channels that shows it */
static void move_chan(int32_t d, uint32_t n, uint32_t nch)
{
    int32_t c = clampi((int32_t)ui.chan + d, 0, nch ? (int32_t)nch - 1 : 0);
    ui.chan = (uint8_t)c;
    if (c < ui.win)
        ui.win = (uint8_t)c;
    if (c >= (int32_t)ui.win + (int32_t)n)
        ui.win = (uint8_t)(c - (int32_t)n + 1);
    if (nch >= n && ui.win + n > nch)
        ui.win = (uint8_t)(nch - n);
    if (nch < n)
        ui.win = 0;
}

/* -------------------------------------------------------------- effect help --- */
typedef struct { char code; const char *par, *name, *desc; } fx_info;
static const fx_info FXS[] = {
    {'0', "xy", "Arpeggio", "note, +x, +y semitones"}, {'1', "xx", "Porta up", "pitch up by xx"},
    {'2', "xx", "Porta down", "pitch down by xx"}, {'3', "xx", "Tone porta", "slide to note, speed xx"},
    {'4', "xy", "Vibrato", "x speed, y depth"}, {'5', "xy", "Porta+vol slide", "3 + A"},
    {'6', "xy", "Vibrato+vol slide", "4 + A"}, {'7', "xy", "Tremolo", "x speed, y depth"},
    {'8', "xx", "Panning", "00 left .. FF right"}, {'9', "xx", "Sample offset", "start at xx00"},
    {'A', "xy", "Volume slide", "x up / y down"}, {'B', "xx", "Position jump", "to position xx"},
    {'C', "xx", "Set volume", "00..40"}, {'D', "xx", "Pattern break", "next pattern, row xx"},
    {'E', "xy", "Extended", "E1..EE: fine, loop, delay"}, {'F', "xx", "Speed / BPM", "<20 speed, >=20 BPM"},
    {'G', "xx", "Global volume", "00..40"}, {'H', "xy", "Global vol slide", "x up / y down"},
    {'K', "xx", "Key off", "at tick xx"}, {'L', "xx", "Envelope pos", "envelope to xx"},
    {'P', "xy", "Panning slide", "x right / y left"}, {'R', "xy", "Multi retrig", "x volume, y ticks"},
    {'T', "xy", "Tremor", "x on / y off"}, {'X', "xy", "Extra fine porta", "X1y up, X2y down"},
};
#define NFX (sizeof FXS / sizeof FXS[0])
static const char *const FX_E[16] = {
    "Amiga filter (MOD)", "Fine porta up by y", "Fine porta down by y", "Glissando y=1 on",
    "Vibrato waveform y", "Set finetune y", "Pattern loop: y=0 start, y times", "Tremolo waveform y",
    "(unused)", "Retrigger every y ticks", "Fine volume up by y", "Fine volume down by y",
    "Note cut at tick y", "Note delay by y ticks", "Pattern delay y rows", "(unused)",
};

static char fx_char(uint32_t e) { return e < 10u ? (char)('0' + e) : e < 36u ? (char)('A' + e - 10u) : '?'; }

static int fx_find(uint32_t e)
{
    uint32_t i;
    char c = fx_char(e);
    for (i = 0; i < NFX; i++)
        if (FXS[i].code == c)
            return (int)i;
    return -1;
}

/* "A0F Volume slide: x up / y down" */
static void put_fx_help(gline *l, uint32_t x, uint32_t e, uint32_t p, uint8_t attr)
{
    int i = fx_find(e);
    char b[2] = {fx_char(e), 0};
    if (i < 0)
        return;
    x = gl_puts(l, x, b, ATTR(C_FX, attr >> 4));
    x = gl_hex(l, x, p, 2, ATTR(C_FX, attr >> 4));
    x = gl_puts(l, x, " ", attr);
    if (FXS[i].code == 'E') {
        gl_puts(l, x, FX_E[p >> 4], attr);
        return;
    }
    if (FXS[i].code == 'X' && (p >> 4) != 1u && (p >> 4) != 2u) {
        gl_puts(l, x, "(unused)", attr);
        return;
    }
    x = gl_puts(l, x, FXS[i].name, attr);
    x = gl_puts(l, x, ": ", attr);
    gl_puts(l, x, FXS[i].desc, attr);
}

#include "edit.c"
#include "store.c"

/* ---------------------------------------------------------------- the screen --- */
#define NL 30
static gline L[NL];
static uint8_t nl;                 /* lines the page uses */

static void lay(uint32_t i, uint8_t font, uint8_t y)
{
    gl_init(&L[i], font, y);
    if (i + 1u > nl)
        nl = (uint8_t)(i + 1u);
}

static void title_bar(gline *l, const char *left)
{
    gl_clear(l, ATTR(C_TEXT, C_HEAD));
    gl_puts(l, 0, left, ATTR(C_HI, C_HEAD));
}

/* the two footer lines, f0 and f0+1 */
static void footer(uint32_t f0, const char *a, const char *b)
{
    gl_clear(&L[f0], ATTR(C_TEXT, C_PANEL));
    gl_clear(&L[f0 + 1u], ATTR(C_DIM, C_PANEL));
    if (a)
        gl_puts(&L[f0], 0, a, ATTR(C_TEXT, C_PANEL));
    if (b)
        gl_puts(&L[f0 + 1u], 0, b, ATTR(C_DIM, C_PANEL));
}

static const char NOTE_NAMES[] = "C-C#D-D#E-F-F#G-G#A-A#B-";

static void note_str(char *b, uint32_t note)
{
    if (note == 0) {
        b[0] = b[1] = b[2] = '.';
    } else if (note == FMT_NOTE_OFF) {
        b[0] = b[1] = b[2] = '=';
    } else {
        uint32_t n = note - 1u;
        b[0] = NOTE_NAMES[(n % 12u) * 2u];
        b[1] = NOTE_NAMES[(n % 12u) * 2u + 1u];
        b[2] = (char)('0' + n / 12u);
    }
    b[3] = 0;
}

/* "C-4 01 40 F03": note, instrument, volume column, effect; 10 columns. field: the cursor's, -1 none */
static void put_cell(gline *l, uint32_t x, const player_cell *c, int muted, int mod, int field, uint8_t bg)
{
    char b[4];
    uint8_t dim = ATTR(C_DIM, bg), cur = ATTR(C_BLACK, C_HI);
    note_str(b, c->note);
    gl_puts(l, x, b, field == 0 ? cur : !c->note ? dim : ATTR(muted ? C_MUTE : C_NOTE, bg));
    if (c->instr)
        gl_hex(l, x + 3u, c->instr, 2, field == 1 ? cur : ATTR(C_INS, bg));
    else
        gl_puts(l, x + 3u, "..", field == 1 ? cur : dim);
    if (mod)
        gl_puts(l, x + 5u, "  ", dim);
    else if (c->vol)
        gl_hex(l, x + 5u, c->vol, 2, field == 2 ? cur : ATTR(C_VOL, bg));
    else
        gl_puts(l, x + 5u, "..", field == 2 ? cur : dim);
    if (c->efx || c->efx_data) {
        b[0] = fx_char(c->efx);
        b[1] = 0;
        gl_puts(l, x + 7u, b, field == 3 ? cur : ATTR(C_FX, bg));
        gl_hex(l, x + 8u, c->efx_data, 2, field == 3 ? cur : ATTR(C_FX, bg));
    } else {
        gl_puts(l, x + 7u, "...", field == 3 ? cur : dim);
    }
}

/* meters drawn into the channel header line */
typedef struct {
    uint8_t n, w;
    uint8_t x[8], fill[8], col[8];
} meters;

static void ov_meters(uint16_t *buf, uint32_t h, const void *ctx)
{
    const meters *m = ctx;
    uint32_t k;
    for (k = 0; k < m->n; k++) {
        strip_rect(buf, h, m->x[k], 2, m->w, 4, C_GRID);
        strip_rect(buf, h, m->x[k], 2, m->fill[k], 4, m->col[k]);
    }
}

static uint32_t meters_key(const meters *m)
{
    uint32_t k, s = m->n;
    for (k = 0; k < m->n; k++)
        s = s * 131u + m->fill[k] * 7u + m->col[k];
    return s;
}

static void meter_set(meters *m, uint32_t k, uint32_t x, uint32_t c)
{
    uint32_t lv = ui.off[c] ? 0u : ui.level[c];
    m->x[k] = (uint8_t)x;
    m->fill[k] = (uint8_t)(lv * m->w / 255u);
    m->col[k] = lv > 220u ? C_BARHI : C_BAR;
}

static uint8_t chan_attr(uint32_t c, uint8_t bg)
{
    return ATTR(ui.off[c] ? C_MUTE : c == ui.chan ? C_HI : ui.solo[c] ? C_INS : C_TEXT, bg);
}

/* ------------------------------------------------------------------ HOME --- */
#define HOME_ROWS 24
#define HOME_MID 12
/* 8 channels: the row number, then note (3), instrument digit, effect letter at 15 + 28 k */
static uint8_t XMAP8[2 + 8 * 5];

static void home_enter(void)
{
    uint32_t i, k;
    lay(0, 0, 0);
    lay(1, 0, 12);
    lay(2, 1, 24);
    for (i = 0; i < HOME_ROWS; i++)
        lay(3 + i, 1, (uint8_t)(32 + i * 8));
    lay(27, 1, 224);
    lay(28, 1, 232);
    XMAP8[0] = 0;
    XMAP8[1] = 5;
    for (k = 0; k < 8u; k++)
        for (i = 0; i < 5u; i++)
            XMAP8[2 + k * 5 + i] = (uint8_t)(15 + k * 28 + i * 5);
    if (ui.view[PG_HOME]) {
        for (i = 2; i < 3 + HOME_ROWS; i++) {
            L[i].xmap = XMAP8;
            L[i].ncols = sizeof XMAP8;
        }
    }
}

static void ov_sep8(uint16_t *buf, uint32_t h, const void *ctx)
{
    uint32_t k;
    (void)ctx;
    for (k = 0; k < 8u; k++)
        strip_rect(buf, h, (int32_t)(15 + k * 28) - 3, 0, 1, (int32_t)h, C_GRID);
}

/* after a note: the cursor moves down the edit step (stopped; within the pattern) */
static void advance(const player_state *st, uint32_t rows)
{
    if (!st->playing && rows)
        ui.row = (uint16_t)((ui.row + ui.step) % rows);
}

/* the note of key k (0 = F3): the C in the middle of the keys is C of the octave set */
static uint8_t key_note(uint32_t k)
{
    return (uint8_t)clampi(12 * ((int32_t)ui.oct - 1) + 5 + (int32_t)k + 1, 1, 96);
}

/* the note keys: play on the cursor's channel; in edit mode on HOME also into the cell */
static void keys_play(const ui_in *in, const player_state *st, int record)
{
    uint32_t k, up = ui.notes_held & ~in->notes_held, nch = song.hdr.num_channels;
    ui.notes_held = in->notes_held;
    for (k = 0; k < 27u; k++) {
        if ((up >> k) & 1u) {
            int c = ui.key_chan[k];
            ui.key_chan[k] = -1;
            if (c >= 0 && ui.chan_key[c] == (int8_t)k) {
                player_jam(c, FMT_NOTE_OFF, 0);
                ui.chan_key[c] = -1;
            }
        }
        if (((in->notes >> k) & 1u) && ui.chan < nch) {
            uint8_t n = key_note(k);
            player_jam(ui.chan, n, ui.ins);
            ui.key_chan[k] = (int8_t)ui.chan;
            ui.chan_key[ui.chan] = (int8_t)k;
            if (record && ui.edit) {
                song_cell *c = cell_edit(ui.vpat, ui.row, ui.chan, -1);
                if (c) {
                    c->note = n;
                    if (ui.ins)
                        c->instr = ui.ins;
                    advance(st, song_rows(ui.vpat));
                }
            }
        }
    }
}

static void home_input(const ui_in *in, const player_state *st, const fmt_header *h)
{
    uint32_t nch = h ? h->num_channels : 0, wide = ui.view[PG_HOME] ? 8u : 4u, rows = song_rows(ui.vpat);
    int32_t s = in->enc[ENC_SELECT], a = in->enc[ENC_ALGO], p = in->enc[ENC_PRESET];
    int shift = (in->held & BIT(BTN_EDIT)) != 0, mod = is_mod(h), k;
    if (!h || !nch)
        return;
    if (in->pressed & BIT(BTN_SEL)) {          /* a tap: note off; held: the block from here */
        ui.sel_t0 = fm1_ms;
        ui.sel_moved = 0;
        ui.sel_row = ui.row;
        ui.sel_ch = ui.chan;
    }
    if (s || a) {
        if (s) {
            if (st->playing)
                ui_seek((int32_t)ui.pos + s);
            else
                view_rows(s * (shift ? 16 : 1));
        }
        if (a && wide == 8u) {
            move_chan(a * (shift ? 8 : 1), 8, nch);
        } else if (a) {                        /* field by field, EDIT: channel by channel */
            int32_t f = (int32_t)ui.chan * 4 + ui.field, d = a > 0 ? 1 : -1, n = a < 0 ? -a : a;
            if (shift)
                f = clampi(f + a * 4, 0, (int32_t)nch * 4 - 1);
            else
                while (n--) {
                    f = clampi(f + d, 0, (int32_t)nch * 4 - 1);
                    if (mod && f % 4 == 2)     /* (a MOD has no volume column) */
                        f = clampi(f + d, 0, (int32_t)nch * 4 - 1);
                }
            move_chan(f / 4 - (int32_t)ui.chan, 4, nch);
            ui.field = (uint8_t)(f % 4);
        }
        if (in->held & BIT(BTN_SEL)) {
            if (!ui.sel_on) {
                ui.sel_on = 1;
                ui.sel_pat = ui.vpat;
            }
            ui.sel_moved = 1;
        } else {
            ui.sel_on = 0;
        }
    }
    if ((in->released & BIT(BTN_SEL)) && !ui.sel_moved && fm1_ms - ui.sel_t0 < 400u) {
        if (ui.sel_on) {
            ui.sel_on = 0;
        } else if (ui.edit) {
            song_cell *c = cell_edit(ui.vpat, ui.row, ui.chan, -1);
            if (c) {
                c->note = FMT_NOTE_OFF;
                c->instr = 0;
                advance(st, rows);
            }
        }
    }
    if (in->pressed & BIT(BTN_ARP)) {
        ui.arp_t0 = fm1_ms;
        ui.arp_done = 0;
    }
    if ((in->held & BIT(BTN_ARP)) && !ui.arp_done && fm1_ms - ui.arp_t0 >= 400u) {
        ui.arp_done = 1;                       /* held: copy the block, or paste */
        if (in_sel(ui.row, ui.chan))
            block_copy();
        else if (ui.edit)
            block_paste();
        else
            ui_message("REC: EDIT MODE FIRST");
    }
    if ((in->released & BIT(BTN_ARP)) && !ui.arp_done) {
        if (!ui.edit) {
            ui_message("REC: EDIT MODE FIRST");
        } else if (in_sel(ui.row, ui.chan)) {
            block_clear();
        } else {
            song_cell *c = cell_edit(ui.vpat, ui.row, ui.chan, -1);
            if (c) {
                switch (ui.field) {
                case 0: c->note = c->instr = 0; break;
                case 1: c->instr = 0; break;
                case 2: c->vol = 0; break;
                default: c->efx = c->efx_data = 0; break;
                }
                advance(st, rows);
            }
        }
    }
    /* KNOB 1..4: the cell's instrument, volume, effect, parameter */
    for (k = 0; k < 4; k++) {
        int32_t d = in->enc[ENC_K1 + (uint32_t)k];
        song_cell *c;
        if (!d)
            continue;
        if (!ui.edit) {
            ui_message("REC: EDIT MODE FIRST");
            break;
        }
        if (k == 1 && mod) {
            ui_message("A MOD HAS NO VOLUME COLUMN");
            continue;
        }
        c = cell_edit(ui.vpat, ui.row, ui.chan, k);
        if (!c)
            break;
        switch (k) {
        case 0: c->instr = (uint8_t)clampi(c->instr + d * (shift ? 16 : 1), 0, h->num_instr ? h->num_instr : 128); break;
        case 1: c->vol = vol_step(c->vol, d * (shift ? 16 : 1)); break;
        case 2: c->efx = fx_step(c->efx, d, mod); break;
        default: c->efx_data = (uint8_t)clampi(c->efx_data + d * (shift ? 16 : 1), 0, 255); break;
        }
        ui.field = (uint8_t)(k == 3 ? 3 : k + 1);
    }
    if (p && shift)
        ui.step = (uint8_t)clampi((int32_t)ui.step + p, 0, 16);
    else if (p && h->num_instr)
        ui.ins = (uint8_t)clampi((int32_t)ui.ins + p, 1, h->num_instr);
}

static void home_draw(const player_state *st, const fmt_header *h)
{
    uint32_t nch = h ? h->num_channels : 0, pat = ui.vpat, rows, i, k, x, wide = ui.view[PG_HOME];
    const fmt_instr *ins = song_instr(h, ui.ins);
    player_cell cells[FMT_MAX_CHANNELS];
    meters m;
    gline *l;
    int mod = is_mod(h), msg = ui.msg && fm1_ms - ui.msg_ms < 2000u;
    rows = pat_rows(h, pat);

    l = &L[0];
    title_bar(l, "");
    x = gl_puts(l, 0, "POS ", ATTR(C_TEXT, C_HEAD));
    x = gl_hex(l, x, ui.pos, 2, ATTR(C_HI, C_HEAD));
    x = gl_puts(l, x, "/", ATTR(C_TEXT, C_HEAD));
    x = gl_hex(l, x, song_len(h) - 1u, 2, ATTR(C_TEXT, C_HEAD));
    x = gl_puts(l, x, " PAT ", ATTR(C_TEXT, C_HEAD));
    x = gl_hex(l, x, pat, 2, ATTR(C_HI, C_HEAD));
    x = gl_puts(l, x, " ROW ", ATTR(C_TEXT, C_HEAD));
    x = gl_hex(l, x, ui.row, 2, ATTR(C_HI, C_HEAD));
    x = gl_puts(l, x, "/", ATTR(C_TEXT, C_HEAD));
    gl_hex(l, x, rows - 1u, 2, ATTR(C_TEXT, C_HEAD));
    if (ui.changed)
        gl_puts(l, 35, "*", ATTR(C_HI, C_HEAD));
    gl_dec(l, 37, st->playing ? st->bpm : h ? h->bpm : 0, 3, ATTR(C_TEXT, C_HEAD));

    l = &L[1];
    gl_clear(l, ATTR(C_TEXT, C_PANEL));
    if (msg) {
        gl_puts(l, 0, ui.msg, ATTR(C_WARN, C_PANEL));
    } else if (!h) {
        gl_puts(l, 0, NSONGS ? "NO SONG LOADED (SAVE)" : "NO SONGS BUILT IN", ATTR(C_WARN, C_PANEL));
    } else if (wide) {
        x = gl_puts(l, 0, "8 CHANNELS  ", ATTR(C_TEXT, C_PANEL));
        x = gl_dec(l, x, ui.win + 1u, 2 | GL_ZERO, ATTR(C_TEXT, C_PANEL));
        x = gl_puts(l, x, "-", ATTR(C_TEXT, C_PANEL));
        gl_dec(l, x, ui.win + 8u > nch ? nch : ui.win + 8u, 2 | GL_ZERO, ATTR(C_TEXT, C_PANEL));
    } else {
        x = gl_puts(l, 0, "INS ", ATTR(C_DIM, C_PANEL));
        if (ins) {
            x = gl_hex(l, x, ui.ins, 2, ATTR(C_INS, C_PANEL));
            x = gl_puts(l, x, " ", ATTR(C_TEXT, C_PANEL));
            gl_putn(l, x, ins->name, 12, ATTR(C_TEXT, C_PANEL));
        } else {
            gl_puts(l, x, "--", ATTR(C_DIM, C_PANEL));
        }
    }
    if (h && !msg) {
        if (!wide) {
            x = gl_puts(l, 20, "STEP", ATTR(C_DIM, C_PANEL));
            gl_dec(l, x, ui.step, 1, ATTR(C_TEXT, C_PANEL));
        }
        x = gl_puts(l, 27, "OCT", ATTR(C_DIM, C_PANEL));
        gl_dec(l, x, ui.oct, 1, ATTR(C_TEXT, C_PANEL));
        if (ui.edit)
            gl_puts(l, 32, st->playing ? " REC  " : " EDIT ", ATTR(C_TEXT, C_MUTE));
        else
            gl_puts(l, 33, st->playing ? (ui.looping ? "LOOP" : "PLAY") : "STOP", ATTR(st->playing ? C_OK : C_DIM, C_PANEL));
    }

    /* channel header with a meter each */
    l = &L[2];
    gl_clear(l, ATTR(C_TEXT, C_BEAT));
    m.n = 0;
    m.w = wide ? 14u : 36u;
    for (k = 0; k < (wide ? 8u : 4u) && ui.win + k < nch; k++) {
        uint32_t c = ui.win + k;
        if (wide) {
            gl_dec(l, 2 + k * 5, c + 1u, 2 | GL_ZERO, chan_attr(c, C_BEAT));
            meter_set(&m, m.n++, 15 + k * 28 + 11, c);
        } else {
            gl_puts(l, 2 + k * 11, "|", ATTR(C_DIM, C_BEAT));
            gl_dec(l, 3 + k * 11, c + 1u, 2 | GL_ZERO, chan_attr(c, C_BEAT));
            meter_set(&m, m.n++, 15 + k * 55 + 12, c);
        }
    }
    gl_draw_ov(l, meters_key(&m), ov_meters, &m);

    for (i = 0; i < HOME_ROWS; i++) {
        int32_t row = (int32_t)ui.row + (int32_t)i - HOME_MID;
        int cur = row == (int32_t)ui.row;
        uint8_t bg = cur ? C_ROWBG : (row % 4 == 0 ? C_BEAT : C_BG);
        l = &L[3 + i];
        gl_clear(l, ATTR(C_TEXT, bg));
        if (row < 0 || row >= (int32_t)rows || !h)
            continue;
        get_row(h, pat, (uint32_t)row, cells);
        gl_hex(l, 0, (uint32_t)row, 2, ATTR(cur ? C_HI : C_DIM, bg));
        for (k = 0; k < (wide ? 8u : 4u) && ui.win + k < nch; k++) {
            uint32_t c = ui.win + k;
            const player_cell *e = &cells[c];
            uint8_t cbg = in_sel((uint32_t)row, c) ? C_HEAD : bg;
            if (wide) {
                char b[4];
                uint32_t cx = 2 + k * 5;
                note_str(b, e->note);
                gl_attr(l, cx, cx + 5, ATTR(C_DIM, cbg));
                gl_puts(l, cx, b, cur && c == ui.chan ? ATTR(C_BLACK, C_HI) :
                        ATTR(!e->note ? C_DIM : ui.off[c] ? C_MUTE : C_NOTE, cbg));
                if (e->instr)
                    gl_hex(l, cx + 3, e->instr, 1, ATTR(C_INS, cbg));
                if (e->efx || e->efx_data) {
                    b[0] = fx_char(e->efx);
                    b[1] = 0;
                    gl_puts(l, cx + 4, b, ATTR(C_FX, cbg));
                }
            } else {
                gl_puts(l, 2 + k * 11, "|", ATTR(C_DIM, bg));
                put_cell(l, 3 + k * 11, e, ui.off[c], mod, cur && c == ui.chan ? ui.field : -1, cbg);
            }
        }
    }

    /* footer: the cell under the cursor (what the knobs change) and its effect */
    if (wide) {
        footer(27, "ALGORITHM channel  HOME 4-channel view",
               "note, last digit of instrument, effect letter");
    } else if (st->playing) {          /* (the cell under the cursor changes every row: it would flicker) */
        footer(27, ui.edit ? "keys: record notes  PLAY: stop" : "SELECT position  PLAY: stop",
               ui.looping ? "looping this pattern  REC: edit" : "EDIT+PLAY: loop the pattern  REC: edit");
    } else {
        const player_cell *e = &cells[ui.chan < FMT_MAX_CHANNELS ? ui.chan : 0];
        char b[4];
        uint32_t r0, r1, c0, c1;
        get_row(h, pat, ui.row, cells);
        footer(27, 0, 0);
        l = &L[27];
        if (h && nch) {
            x = gl_puts(l, 0, ui.edit ? "K1 INS " : "INS ", ATTR(C_DIM, C_PANEL));
            x = e->instr ? gl_hex(l, x, e->instr, 2, ATTR(C_INS, C_PANEL)) : gl_puts(l, x, "..", ATTR(C_DIM, C_PANEL));
            x = gl_puts(l, x, ui.edit ? "  K2 VOL " : "  VOL ", ATTR(C_DIM, C_PANEL));
            x = mod ? gl_puts(l, x, "--", ATTR(C_DIM, C_PANEL)) :
                e->vol ? gl_hex(l, x, e->vol, 2, ATTR(C_VOL, C_PANEL)) : gl_puts(l, x, "..", ATTR(C_DIM, C_PANEL));
            x = gl_puts(l, x, ui.edit ? "  K3 FX " : "  FX ", ATTR(C_DIM, C_PANEL));
            b[0] = (e->efx || e->efx_data) ? fx_char(e->efx) : '.';
            b[1] = 0;
            x = gl_puts(l, x, b, ATTR(b[0] == '.' ? C_DIM : C_FX, C_PANEL));
            x = gl_puts(l, x, ui.edit ? "  K4 " : " ", ATTR(C_DIM, C_PANEL));
            if (e->efx || e->efx_data)
                gl_hex(l, x, e->efx_data, 2, ATTR(C_FX, C_PANEL));
            else
                gl_puts(l, x, "..", ATTR(C_DIM, C_PANEL));
        }
        l = &L[28];
        if (sel_rect(&r0, &r1, &c0, &c1)) {
            x = gl_puts(l, 0, "BLOCK ", ATTR(C_TEXT, C_PANEL));
            x = gl_dec(l, x, c1 - c0 + 1u, 1, ATTR(C_HI, C_PANEL));
            x = gl_puts(l, x, "x", ATTR(C_TEXT, C_PANEL));
            x = gl_dec(l, x, r1 - r0 + 1u, 1, ATTR(C_HI, C_PANEL));
            gl_puts(l, x, "  ARP hold: copy  ARP: clear", ATTR(C_DIM, C_PANEL));
        } else if (e->efx || e->efx_data) {
            put_fx_help(l, 0, e->efx, e->efx_data, ATTR(C_DIM, C_PANEL));
        } else if (ui.edit) {
            gl_puts(l, 0, "keys: note  SEL: note off  ARP: clear", ATTR(C_DIM, C_PANEL));
        } else {
            gl_puts(l, 0,
                    "SELECT row  ALGORITHM field  REC: edit", ATTR(C_DIM, C_PANEL));
        }
    }
    for (i = 0; i < nl; i++)
        if (i != 2)
            gl_draw_ov(&L[i], 0, wide && i >= 3 && i < 3 + HOME_ROWS ? ov_sep8 : 0, 0);
}

/* ------------------------------------------------------------------- SEQ --- */
#define SEQ_ROWS 25
#define SEQ_MID 6

static void seq_enter(void)
{
    uint32_t i;
    lay(0, 0, 0);
    lay(1, 1, 12);
    for (i = 0; i < SEQ_ROWS; i++)
        lay(2 + i, 1, (uint8_t)(20 + i * 8));
    lay(27, 1, 224);
    lay(28, 1, 232);
    lcd_fill(0, 220, 240, 4, PALETTE[C_BG]);
}

typedef struct { uint8_t w, col; } bar1;

static void ov_bar(uint16_t *buf, uint32_t h, const void *ctx)
{
    const bar1 *b = ctx;
    if (b->w)
        strip_rect(buf, h, 60, 2, b->w, 4, b->col);
}

static void seq_input(const ui_in *in, const player_state *st, const fmt_header *h)
{
    int32_t s = in->enc[ENC_SELECT], k1 = in->enc[ENC_K1], k2 = in->enc[ENC_K2], k3 = in->enc[ENC_K3],
            k4 = in->enc[ENC_K4];
    int shift = (in->held & BIT(BTN_EDIT)) != 0, m = shift ? 16 : 1;
    if (!h)
        return;
    if (ui.view[PG_SEQ]) {                     /* the pattern list */
        if (s)
            ui.pat_cur = (uint16_t)clampi((int32_t)ui.pat_cur + s * m, 0, h->num_patterns ? h->num_patterns - 1 : 0);
        if (k2)
            set_rows(ui.pat_cur, clampi((int32_t)song_rows(ui.pat_cur) + k2 * m, 1, SONG_MAX_ROWS));
        if (shift && (in->pressed & (BIT(BTN_SEL) | BIT(BTN_ARP)))) {
            int p = new_pattern((in->pressed & BIT(BTN_ARP)) ? (int)ui.pat_cur : -1);
            if (p >= 0) {
                ui.pat_cur = (uint16_t)p;
                ui_message((in->pressed & BIT(BTN_ARP)) ? "PATTERN COPIED" : "NEW PATTERN");
                count_notes();
            }
        }
        return;
    }
    if (s) {                                   /* the order list */
        if (st->playing)
            ui_seek((int32_t)ui.pos + s * m);
        else {
            ui.pos = (uint16_t)clampi((int32_t)ui.pos + s * m, 0, (int32_t)song_len(h) - 1);
            ui.row = 0;
        }
    }
    if (k1)
        set_order(ui.pos, clampi((int32_t)song.orders[ui.pos] + k1 * m, 0, SONG_MAX_PATTERNS - 1));
    if (k2)
        set_hdr(H_LEN, clampi((int32_t)song.hdr.song_length + k2 * m, 1, 256));
    if (k3)
        set_hdr(H_RESTART, clampi((int32_t)song.hdr.loop_start + k3 * m, 0, (int32_t)song.hdr.song_length - 1));
    if (k4 && shift)
        set_hdr(H_SPEED, clampi((int32_t)song.hdr.speed + k4, 1, 31));
    else if (k4)
        set_hdr(H_BPM, clampi((int32_t)song.hdr.bpm + k4, 32, 255));
    if (shift && (in->pressed & BIT(BTN_SEL)))
        insert_pos(ui.pos);
    if (shift && (in->pressed & BIT(BTN_ARP))) {
        delete_pos(ui.pos);
        if (ui.pos >= song.hdr.song_length)
            ui.pos = (uint16_t)(song.hdr.song_length - 1u);
    }
}

/* the song's settings on the right of row r (every other row) */
static void seq_info(gline *l, uint32_t r, const fmt_header *h)
{
    uint8_t lab = ATTR(C_DIM, C_BG), val = ATTR(C_TEXT, C_BG);
    uint32_t x = 31;
    if (!h || !(r & 1u))
        return;
    if (ui.view[PG_SEQ]) {
        uint32_t p = ui.pat_cur, n = 0, first = 0xFFFF, i;
        for (i = 0; i < song_len(h); i++)
            if (song_orders(h)[i] == p) {
                if (first == 0xFFFF)
                    first = i;
                n++;
            }
        switch (r) {
        case 1: gl_puts(l, 22, "PATTERN", lab); gl_hex(l, x, p, 2, ATTR(C_HI, C_BG)); break;
        case 3: gl_puts(l, 22, "ROWS", lab); gl_dec(l, x, pat_rows(h, p), 1, val); break;
        case 5: gl_puts(l, 22, "NOTES", lab); gl_dec(l, x, ui.pat_notes[p & 255u], 1, val); break;
        case 7: gl_puts(l, 22, "USED", lab); x = gl_dec(l, x, n, 1, val); gl_puts(l, x, n == 1 ? " time" : " times", lab); break;
        case 9:
            gl_puts(l, 22, "FIRST AT", lab);
            if (n)
                gl_hex(l, x, first, 2, val);
            else
                gl_puts(l, x, "--", lab);
            break;
        }
        return;
    }
    switch (r) {
    case 1: gl_puts(l, 22, "FORMAT", lab);
        gl_puts(l, x, is_mod(h) ? (player_pt_mode() ? "MOD PROTRACKER" : "MOD (FT2)") : (h->flags & FMT_LINEAR) ? "XM LINEAR" : "XM AMIGA", val); break;
    case 3: gl_puts(l, 22, "LENGTH", lab); gl_hex(l, x, song_len(h), 2, val); break;
    case 5: gl_puts(l, 22, "RESTART", lab); gl_hex(l, x, h->loop_start, 2, val); break;
    case 7: gl_puts(l, 22, "CHANNELS", lab); gl_dec(l, x, h->num_channels, 1, val); break;
    case 9: gl_puts(l, 22, "SPEED", lab); gl_dec(l, x, h->speed, 1, val); break;
    case 11: gl_puts(l, 22, "BPM", lab); gl_dec(l, x, h->bpm, 1, val); break;
    case 13: gl_puts(l, 22, "PATTERNS", lab); gl_dec(l, x, h->num_patterns, 1, val); break;
    case 15: gl_puts(l, 22, "INSTR.", lab); gl_dec(l, x, h->num_instr, 1, val); break;
    case 17: gl_puts(l, 22, "SAMPLES", lab); gl_dec(l, x, h->num_samples, 1, val); break;
    case 19: gl_puts(l, 22, "TRACKER", lab); gl_putn(l, x, h->tracker, 17, val); break;
    }
}

static void seq_draw(const player_state *st, const fmt_header *h)
{
    uint32_t i, pats = ui.view[PG_SEQ];
    gline *l = &L[0];
    title_bar(l, pats ? "PATTERNS" : "SONG");
    if (h) {
        char nm[25];
        uint32_t n = 0;
        while (n < 20u && h->name[n]) {
            nm[n] = h->name[n];
            n++;
        }
        nm[n] = 0;
        gl_puts(l, 40u - n, nm, ATTR(C_TEXT, C_HEAD));
    }
    l = &L[1];
    gl_clear(l, ATTR(C_DIM, C_BEAT));
    gl_puts(l, 0, pats ? "PAT ROWS NOTES" : "POS PAT ROWS NOTES", ATTR(C_DIM, C_BEAT));
    gl_puts(l, 20, "|", ATTR(C_DIM, C_BEAT));
    for (i = 0; i < SEQ_ROWS; i++) {
        int32_t k = (int32_t)(pats ? ui.pat_cur : ui.pos) + (int32_t)i - SEQ_MID;
        int32_t n = pats ? (h ? h->num_patterns : 0) : (int32_t)song_len(h);
        int cur = k == (int32_t)(pats ? ui.pat_cur : ui.pos);
        uint8_t bg = cur ? C_ROWBG : C_BG;
        bar1 b = {0, C_SHADE};
        l = &L[2 + i];
        gl_clear(l, ATTR(C_TEXT, C_BG));
        gl_attr(l, 0, 20, ATTR(C_TEXT, bg));
        if (h && k >= 0 && k < n) {
            uint32_t p = pats ? (uint32_t)k : pos_pattern(h, (uint32_t)k);
            if (pats) {
                gl_hex(l, 0, p, 2, ATTR(cur ? C_HI : C_TEXT, bg));
                gl_dec(l, 4, pat_rows(h, p), 3, ATTR(C_DIM, bg));
                if (p == pos_pattern(h, ui.pos))
                    gl_puts(l, 19, "*", ATTR(C_OK, bg));
            } else {
                gl_hex(l, 0, (uint32_t)k, 2, ATTR(cur ? C_HI : C_DIM, bg));
                gl_hex(l, 4, p, 2, ATTR(cur ? C_HI : C_TEXT, bg));
                gl_dec(l, 8, pat_rows(h, p), 3, ATTR(C_DIM, bg));
                if (k == h->loop_start)
                    gl_puts(l, 19, "<", ATTR(C_OK, bg));
            }
            b.w = (uint8_t)(ui.pat_notes[p & 255u] * 36u / ui.pat_notes_max);
            if (ui.pat_notes[p & 255u] && !b.w)
                b.w = 1;
            b.col = cur ? C_BARHI : C_SHADE;
        } else if (h && !pats && k == n) {
            gl_puts(l, 0, "--- end", ATTR(C_DIM, C_BG));
        }
        seq_info(l, (uint32_t)i, h);
        gl_draw_ov(l, b.w * 32u + b.col, ov_bar, &b);
    }
    (void)st;
    if (ui.msg && fm1_ms - ui.msg_ms < 2000u)
        footer(27, ui.msg, 0);
    else if (pats)
        footer(27, "SELECT pattern  K2 ROWS  SEQ: order list", "EDIT+SEL: new pattern  EDIT+ARP: copy it");
    else
        footer(27, "K1 PATTERN K2 LENGTH K3 RESTART K4 BPM", "EDIT+SEL/ARP: insert/delete  EDIT+K4 speed");
    gl_draw(&L[0]);
    gl_draw(&L[1]);
    gl_draw(&L[27]);
    gl_draw(&L[28]);
}

/* ------------------------------------------------------------------ SAVE --- */
/* The songs: the flash slots (store.c), then the ones built into the firmware. SELECT a song,
 * ALGORITHM an action, REC does it; PLAY loads the song and plays it. */
#define SAVE_ROWS 13
enum { A_LOAD, A_SAVE, A_NEW, A_NAME, A_DEL, NACT };
static const char *const ACT_NAME[NACT] = {"LOAD", "SAVE", "NEW", "NAME", "DEL"};
static const char NAME_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!#&()+";

static int save_count(void) { return store_n + NSONGS; }

static void save_enter(void)
{
    uint32_t i;
    lay(0, 0, 0);
    for (i = 0; i < SAVE_ROWS; i++)
        lay(1 + i, 0, (uint8_t)(14 + i * 12));
    lay(14, 1, 174);               /* the flash in use */
    lay(15, 0, 186);               /* the actions */
    lay(16, 1, 204);               /* the song loaded */
    lay(27, 1, 224);
    lay(28, 1, 232);
    if (ui.song_cur < 0 || ui.song_cur >= save_count())
        ui.song_cur = 0;
    ui.naming = 0;
    ui.confirm = 0;
}

/* a slot moved (compacting): the loaded song follows */
static void slot_moved(uint32_t from, uint32_t to)
{
    if (ui.img_off == from) {
        ui.img_off = to;
        player_rebind(stp_ptr(to + SLOT_HDR));
    }
    if (ui.slot_off == from)
        ui.slot_off = to;
}

static void ui_load_slot(int k)    /* flash slot k (checked) */
{
    uint32_t i;
    ui_load_image(store_image(k), store_slots[k].size, -1, store_slots[k].off);
    for (i = 0; i < sizeof song.hdr.name; i++)        /* the slot's name is the song's */
        song.hdr.name[i] = i < 23u ? store_slots[k].name[i] : 0;
}

static void save_load(int k)       /* list entry k */
{
    if (k < store_n) {
        if (!store_check(k)) {
            ui_message("BAD CHECKSUM: NOT LOADED");
            return;
        }
        ui_load_slot(k);
    } else {
        ui_load(k - store_n);
    }
}

static void save_song(void)
{
    uint32_t off;
    int err;
    if (!song.hdr.name[0]) {
        const char *u = "untitled";
        uint32_t i;
        for (i = 0; u[i]; i++)
            song.hdr.name[i] = u[i];
    }
    ui_play(0);
    sys_audio_hold();              /* (the flash writes stop the audio anyway) */
    off = store_save(song.hdr.name, ui.slot_off, slot_moved, &err);
    if (off) {
        player_rebind(stp_ptr(off + SLOT_HDR));   /* the samples from the new copy (the old one is gone) */
        ui.img_off = ui.slot_off = off;
        ui.song = -1;
        ui.changed = 0;
        ui.song_cur = store_find(off);
        ui_message("SAVED");
    } else {
        ui_message(err == -3 ? "NO ROOM IN FLASH" : err == -1 ? "OUT OF MEMORY" : err == -5 ? "NO FLASH" :
                   "FLASH WRITE FAILED");
    }
    sys_audio_go();
}

static void song_zap(void)         /* NEW: the instruments stay, the patterns and the order list go */
{
    uint32_t i;
    ui_play(0);
    song_set_num_patterns(0);
    song_new_pattern();
    for (i = 0; i < 256u; i++)
        song.orders[i] = 0;
    song.hdr.song_length = 1;
    song.hdr.loop_start = 0;
    for (i = 0; i < sizeof song.hdr.name; i++)
        song.hdr.name[i] = 0;
    undo_clear();
    ui.slot_off = 0;               /* (saved as a new song) */
    ui.song = -1;
    ui.pos = ui.row = 0;
    ui.changed = 1;
    count_notes();
}

static int confirmed(uint32_t act, const char *ask)   /* REC again within 3 s */
{
    if (ui.confirm == act + 1u && fm1_ms - ui.confirm_ms < 3000u) {
        ui.confirm = 0;
        return 1;
    }
    ui.confirm = (uint8_t)(act + 1u);
    ui.confirm_ms = fm1_ms;
    ui_message(ask);
    return 0;
}

static void save_input(const ui_in *in)
{
    int32_t s = in->enc[ENC_SELECT], a = in->enc[ENC_ALGO];
    int n = save_count();
    if (ui.naming) {               /* ALGORITHM the letter, SELECT what it is, REC done */
        char *nm = song.hdr.name;
        if (a)
            ui.name_cur = (uint8_t)clampi((int32_t)ui.name_cur + a, 0, 22);
        if (s) {
            uint32_t i, c = 0;
            for (i = 0; NAME_CHARS[i]; i++)
                if (NAME_CHARS[i] == (nm[ui.name_cur] ? nm[ui.name_cur] : ' '))
                    c = i;
            c = (uint32_t)clampi((int32_t)c + s, 0, (int32_t)sizeof NAME_CHARS - 2);
            for (i = 0; i < ui.name_cur; i++)  /* (no holes before the cursor) */
                if (!nm[i])
                    nm[i] = ' ';
            nm[ui.name_cur] = NAME_CHARS[c];
            ui.changed = 1;
        }
        if (in->pressed & (BIT(BTN_REC) | BIT(BTN_SAVE))) {
            int i;
            for (i = 22; i >= 0 && (nm[i] == ' ' || !nm[i]); i--)
                nm[i] = 0;
            ui.naming = 0;
        }
        return;
    }
    if (s && n)
        ui.song_cur = clampi(ui.song_cur + s, 0, n - 1);
    if (a)
        ui.act = (uint8_t)clampi((int32_t)ui.act + a, 0, NACT - 1);
    if ((in->pressed & BIT(BTN_PLAY)) && n) {   /* (the global PLAY is skipped on this page) */
        if (ui.changed && !confirmed(NACT, "UNSAVED CHANGES: PLAY AGAIN"))
            return;
        save_load(ui.song_cur);
        if (ui.loaded_ok) {
            ui_play(1);
            ui.page = PG_HOME;
            ui.redraw = 1;
        }
        return;
    }
    if (!(in->pressed & BIT(BTN_REC)))
        return;
    switch (ui.act) {
    case A_LOAD:
        if (!n || (ui.changed && !confirmed(A_LOAD, "UNSAVED CHANGES: REC AGAIN")))
            break;
        save_load(ui.song_cur);
        if (ui.loaded_ok)
            ui_message("LOADED");
        break;
    case A_SAVE:
        if (ui.loaded_ok)
            save_song();
        break;
    case A_NEW:
        if (ui.loaded_ok && (!ui.changed || confirmed(A_NEW, "UNSAVED CHANGES: REC AGAIN"))) {
            song_zap();
            ui_message("NEW SONG: THE INSTRUMENTS STAY");
        }
        break;
    case A_NAME:
        if (ui.loaded_ok) {
            uint32_t i = 0;
            while (i < 22u && song.hdr.name[i])
                i++;
            ui.name_cur = (uint8_t)i;
            ui.naming = 1;
        }
        break;
    case A_DEL:
        if (ui.song_cur >= store_n) {
            ui_message("BUILT-IN SONGS STAY");
        } else if (store_slots[ui.song_cur].off == ui.img_off) {
            ui_message("IN USE: LOAD ANOTHER FIRST");
        } else if (confirmed(A_DEL, "DELETE? REC AGAIN")) {
            if (store_slots[ui.song_cur].off == ui.slot_off)
                ui.slot_off = 0;
            store_delete(ui.song_cur);
            if (ui.song_cur >= save_count())
                ui.song_cur = save_count() - 1;
            ui_message("DELETED");
        }
        break;
    }
}

typedef struct { uint8_t used, all; } flash_bar;

static void ov_flash(uint16_t *buf, uint32_t h, const void *ctx)
{
    const flash_bar *f = ctx;
    strip_rect(buf, h, 150, 2, 88, 4, C_GRID);
    strip_rect(buf, h, 150, 2, f->all ? f->used * 88 / f->all : 0, 4, C_BAR);
}

static void save_draw(void)
{
    int n = save_count();
    uint32_t i, x, top = ui.song_cur >= SAVE_ROWS ? (uint32_t)ui.song_cur - SAVE_ROWS + 1u : 0u;
    uint32_t free_k = store_free_sectors() * 4u, all_k = STORE_NSECT * 4u;
    flash_bar fb;
    gline *l = &L[0];
    title_bar(l, "SONGS");
    x = gl_dec(l, 33, (uint32_t)ui.song_cur + 1u, 3, ATTR(C_TEXT, C_HEAD));
    x = gl_puts(l, x, "/", ATTR(C_TEXT, C_HEAD));
    gl_dec(l, x, (uint32_t)n, 1, ATTR(C_TEXT, C_HEAD));
    for (i = 0; i < SAVE_ROWS; i++) {
        int k = (int)(top + i);
        uint8_t bg = k == ui.song_cur ? C_ROWBG : C_BG;
        int flash = k < store_n, cur;
        const char *name;
        uint32_t size;
        l = &L[1 + i];
        gl_clear(l, ATTR(C_TEXT, bg));
        if (k >= n)
            continue;
        name = flash ? store_slots[k].name : SONGS[k - store_n].name;
        size = flash ? store_slots[k].size : SONGS[k - store_n].size;
        cur = flash ? store_slots[k].off == ui.slot_off && ui.slot_off : k - store_n == ui.song;
        gl_puts(l, 0, cur ? "*" : " ", ATTR(C_OK, bg));
        gl_putn(l, 2, name[0] ? name : "(no name)", 23, ATTR(k == ui.song_cur ? C_HI : C_TEXT, bg));
        x = gl_dec(l, 26, (size + 1023u) / 1024u, 4, ATTR(C_DIM, bg));
        x = gl_puts(l, x, "K", ATTR(C_DIM, bg));
        gl_puts(l, 33, flash ? "FLASH" : "ROM", ATTR(flash ? C_INS : C_DIM, bg));
    }
    l = &L[14];
    gl_clear(l, ATTR(C_DIM, C_BG));
    x = gl_puts(l, 0, "FLASH ", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, all_k - free_k, 1, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, "K USED, ", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, free_k, 1, ATTR(C_TEXT, C_BG));
    gl_puts(l, x, "K FREE", ATTR(C_DIM, C_BG));
    fb.used = (uint8_t)((all_k - free_k) * 255u / all_k);
    fb.all = 255;
    gl_draw_ov(l, fb.used, ov_flash, &fb);
    l = &L[15];
    gl_clear(l, ATTR(C_TEXT, C_BG));
    for (i = 0; i < NACT; i++) {
        uint8_t a = i == ui.act ? ATTR(C_BLACK, C_HI) : ATTR(C_TEXT, C_PANEL);
        gl_puts(l, i * 8u, "       ", a);
        gl_puts(l, i * 8u + (7u - (uint32_t)str_n(ACT_NAME[i])) / 2u, ACT_NAME[i], a);
    }
    l = &L[16];
    gl_clear(l, ATTR(C_DIM, C_BG));
    x = gl_puts(l, 0, ui.naming ? "NAME: " : "NOW: ", ATTR(C_DIM, C_BG));
    if (ui.loaded_ok) {
        uint32_t k;
        for (k = 0; k < 23u; k++) {
            char c[2] = {song.hdr.name[k] ? song.hdr.name[k] : ' ', 0};
            if (!song.hdr.name[k] && !ui.naming && k > 0)
                break;
            gl_puts(l, x + k, c, ui.naming && k == ui.name_cur ? ATTR(C_BLACK, C_HI) : ATTR(C_HI, C_BG));
        }
        if (ui.changed && !ui.naming)
            gl_puts(l, 44, "*", ATTR(C_HI, C_BG));
    }
    if (ui.naming)
        footer(27, "ALGORITHM letter  SELECT change it", "REC: done");
    else
        footer(27, "SELECT song  ALGORITHM action  REC: do it", "PLAY: load and play  EDIT+SAVE: undo");
    if (ui.msg && fm1_ms - ui.msg_ms < 2500u) {
        gl_clear(&L[28], ATTR(C_WARN, C_PANEL));
        gl_puts(&L[28], 0, ui.msg, ATTR(C_WARN, C_PANEL));
    }
    for (i = 0; i < nl; i++)
        if (i != 14)
            gl_draw(&L[i]);
}

/* -------------------------------------------------------------------- FX --- */
static void fx_enter(void)
{
    uint32_t i;
    lay(0, 0, 0);
    for (i = 0; i < NFX; i++)
        lay(1 + i, 1, (uint8_t)(14 + i * 8));
    lay(27, 1, 224);
    lay(28, 1, 232);
}

static void fx_input(const ui_in *in)
{
    int32_t s = in->enc[ENC_SELECT];
    if (s)
        ui.fx_cur = (uint8_t)clampi((int32_t)ui.fx_cur + s, 0, NFX - 1);
    if ((in->pressed & BIT(BTN_REC)) && ui.loaded_ok) {   /* the effect into the cell under the cursor */
        char c = FXS[ui.fx_cur].code;
        song_cell *e;
        if (is_mod(&song.hdr) && ui.fx_cur >= 16u) {
            ui_message("NOT IN A MOD");
            return;
        }
        e = cell_edit(ui.vpat, ui.row, ui.chan, -1);
        if (e) {
            e->efx = (uint8_t)(c <= '9' ? c - '0' : c - 'A' + 10);
            ui.field = 3;
            page_open(PG_FX);                  /* (FX again: back to the pattern) */
        }
    }
}

static void fx_draw(void)
{
    uint32_t i;
    gline *l = &L[0];
    title_bar(l, "EFFECTS");
    gl_puts(l, 31, "XM / MOD", ATTR(C_TEXT, C_HEAD));
    for (i = 0; i < NFX; i++) {
        uint8_t bg = i == ui.fx_cur ? C_ROWBG : C_BG;
        char b[2] = {FXS[i].code, 0};
        l = &L[1 + i];
        gl_clear(l, ATTR(C_TEXT, bg));
        gl_puts(l, 0, b, ATTR(C_FX, bg));
        gl_puts(l, 1, FXS[i].par, ATTR(C_FX, bg));
        gl_puts(l, 4, FXS[i].name, ATTR(i == ui.fx_cur ? C_HI : C_TEXT, bg));
        gl_puts(l, 22, FXS[i].desc, ATTR(C_DIM, bg));
    }
    if (FXS[ui.fx_cur].code == 'E')
        footer(27, "E1 E2 fine porta  E6 loop  E9 retrig", "EA EB fine vol  EC cut  ED delay  EE patt.delay");
    else
        footer(27, "SELECT effect", "FX: back");
    for (i = 0; i < nl; i++)
        gl_draw(&L[i]);
}

/* -------------------------------------------------------------- ENV, LFO --- */
#include "pages_ins.c"
#include "pages_glo.c"

/* ------------------------------------------------------------------- GLO --- */
/* the mixer's knobs: KNOB 1 global volume, 2 speed, 3 BPM (now, and the song's for the next play),
 * 4 amplification */
static void glo_knobs(const ui_in *in, const player_state *st)
{
    int32_t k1 = in->enc[ENC_K1], k2 = in->enc[ENC_K2], k3 = in->enc[ENC_K3], k4 = in->enc[ENC_K4];
    if (k1)
        player_set_global_vol(clampi((int32_t)st->global_vol + k1, 0, 64));
    if (k2 && ui.loaded_ok) {
        int32_t v = clampi((int32_t)st->speed + k2, 1, 31);
        player_set_speed(v);
        set_hdr(H_SPEED, v);
    }
    if (k3 && ui.loaded_ok) {
        int32_t v = clampi((int32_t)st->bpm + k3, 32, 255);
        player_set_bpm(v);
        set_hdr(H_BPM, v);
    }
    if (k4)
        player_set_amp(clampi(player_get_amp() + k4, 1, 32));
}

/* ---------------------------------------------------------------- the frame --- */
static void page_open(uint32_t p)
{
    if (p == ui.page) {
        if (p == PG_HOME || p == PG_SEQ || p == PG_ENV || p == PG_GLO)
            ui.view[p] ^= 1u;
        else if (p == PG_FX)
            p = ui.prev_page;
    } else if (p == PG_FX) {           /* opened on the effect under the cursor */
        const fmt_header *h = song_h();
        player_cell cells[FMT_MAX_CHANNELS];
        get_row(h, pos_pattern(h, ui.pos), ui.row, cells);
        if (ui.page == PG_HOME && (cells[ui.chan].efx || cells[ui.chan].efx_data) &&
            fx_find(cells[ui.chan].efx) >= 0)
            ui.fx_cur = (uint8_t)fx_find(cells[ui.chan].efx);
    }
    if (p != ui.page)
        ui.prev_page = ui.page;
    ui.page = (uint8_t)p;
    ui.redraw = 1;
}

static void pages_enter(void)
{
    uint32_t i;
    nl = 0;
    for (i = 0; i < NL; i++)
        L[i].y = GL_UNUSED;
    lcd_fill(0, 0, 240, 240, PALETTE[C_BG]);
    switch (ui.page) {
    case PG_HOME: home_enter(); break;
    case PG_SEQ: count_notes(); seq_enter(); break;
    case PG_SAVE: save_enter(); break;
    case PG_FX: fx_enter(); break;
    case PG_GLO: glo_enter(); break;
    default: graph_enter(); break;
    }
    for (i = 0; i < nl; i++)
        L[i].sum = 0;
}

static void pages_leds(const player_state *st, const ui_in *in)
{
    uint32_t k, keys = ui.page == PG_GLO || (in->held & BIT(BTN_GLO));
    for (k = 0; k < 27u; k++)
        sys_led(14u + k, 0);
    if (keys)
        for (k = 0; k < 16u; k++) {
            uint32_t c = ui.bank * 16u + k;
            sys_led(14u + WHITE_KEY[k], c < st->num_channels && !ui.off[c] && st->ch[c].active && ui.level[c] > 16);
        }
    for (k = 0; k < NPAGES; k++)
        sys_led(PAGE_BTN[k], ui.page == k);
    sys_led(BTN_PLAY, st->playing);
    sys_led(BTN_REC, ui.page == PG_GLO ? ui.solo_mode : ui.edit);
    if (ui.page == PG_GLO) {
        sys_led(BTN_OCTUP, ui.bank == 1);
        sys_led(BTN_OCTDN, 0);
    } else {
        sys_led(BTN_OCTUP, ui.oct > 4);
        sys_led(BTN_OCTDN, ui.oct < 4);
    }
}

/* a USB-MIDI event packet from the computer: Start / Continue / Stop drive the playback, notes play
 * the PRESETS instrument on the cursor's channel (as the keys do) */
static void ui_midi(uint32_t pkt)
{
    uint32_t cin = pkt & 15u, st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
    if (!ui.loaded_ok)
        return;
    if (cin == 0xFu && st == 0xFAu) {          /* Start: the song from the top */
        ui.pos = ui.row = 0;
        ui_play(1);
    } else if (cin == 0xFu && st == 0xFBu) {   /* Continue: from the position shown */
        if (!player_playing())
            ui_play(1);
    } else if (cin == 0xFu && st == 0xFCu) {
        ui_play(0);
    } else if ((cin == 0x9u || cin == 0x8u) && ui.chan < song.hdr.num_channels) {
        int32_t n = (int32_t)d1 - 11;          /* MIDI 12 (C0) = XM C-0 (1); MIDI 60 = C-4 */
        if (n < 1 || n > 96)
            return;
        if (cin == 0x9u && d2)
            player_jam(ui.chan, (uint8_t)n, ui.ins);
        else
            player_jam(ui.chan, FMT_NOTE_OFF, 0);
    }
}

static void pages_init(void)
{
    ui.song = -1;
    song_set_lock(sys_lock, sys_unlock);
    ui.oct = 4;
    ui.sep_i = 1;                  /* (20 %, the player's default) */
    ui.step = 1;
    ui.page = PG_HOME;
    ui.redraw = 1;
    store_scan();
    {
        int i, best = -1;
        for (i = 0; i < store_n; i++)
            if ((best < 0 || store_slots[i].seq > store_slots[best].seq) && store_check(i))
                best = i;
        if (best >= 0)
            ui_load_slot(best);
        else if (NSONGS > 0)
            ui_load(0);
    }
}

static void pages_frame(const ui_in *in)
{
    const player_state *st = player_get_state();
    const fmt_header *h = song_h();
    uint32_t nch = h ? h->num_channels : 0, i, k;
    uint8_t peaks[FMT_MAX_CHANNELS];

    /* meters: the mixer peak, falling 1/8 per frame */
    player_take_peaks(peaks, (int)nch);
    for (i = 0; i < nch; i++) {
        if (peaks[i] > ui.level[i])
            ui.level[i] = peaks[i];
        else
            ui.level[i] = (uint8_t)(ui.level[i] - (ui.level[i] >> 3) - (ui.level[i] ? 1u : 0u));
    }
    if (st->playing) {
        ui.pos = st->song_pos;
        ui.row = st->row;
    }
    if (h && ui.chan >= nch)
        ui.chan = 0;
    ui.vpat = (uint16_t)(st->playing ? st->pattern : pos_pattern(h, ui.pos));

    /* EDIT+SAVE: undo */
    if ((in->pressed & BIT(BTN_SAVE)) && (in->held & BIT(BTN_EDIT))) {
        ui_message(undo() ? "UNDO" : "NOTHING TO UNDO");
        if (ui.page == PG_SEQ)
            count_notes();
    }
    /* page buttons; GLO opens on release, unless the white keys were used while it was held */
    for (k = 0; k < NPAGES; k++) {
        if (PAGE_BTN[k] == BTN_GLO || (PAGE_BTN[k] == BTN_SAVE && (in->held & BIT(BTN_EDIT))))
            continue;
        if (in->pressed & BIT(PAGE_BTN[k]))
            page_open(k);
    }
    if (in->pressed & BIT(BTN_GLO))
        ui.glo_keys = 0;
    if ((in->released & BIT(BTN_GLO)) && !ui.glo_keys)
        page_open(PG_GLO);
    if (ui.page == PG_GLO || (in->held & BIT(BTN_GLO)))
        keys_mute(in->notes, nch);
    if ((in->pressed & BIT(BTN_PLAY)) && ui.page != PG_SAVE)
        ui_play_loop(!st->playing, (in->held & BIT(BTN_EDIT)) != 0);
    if ((in->pressed & BIT(BTN_REC)) && ui.page != PG_GLO && ui.page != PG_SAVE && ui.page != PG_FX && h)
        ui.edit ^= 1u;
    st = player_get_state();
    if (st->playing) {
        ui.pos = st->song_pos;
        ui.row = st->row;
        ui.vpat = st->pattern;
    }
    if (h && !(in->held & BIT(BTN_GLO)) && ui.page != PG_GLO && ui.page != PG_SAVE && ui.page != PG_FX) {
        if ((in->pressed & BIT(BTN_OCTDN)) && ui.oct > 0)    /* the octave: wherever the keys play */
            ui.oct--;
        if ((in->pressed & BIT(BTN_OCTUP)) && ui.oct < 8)
            ui.oct++;
        keys_play(in, st, ui.page == PG_HOME);
    } else
        ui.notes_held = in->notes_held;

    switch (ui.page) {
    case PG_HOME: home_input(in, st, h); break;
    case PG_SEQ: seq_input(in, st, h); break;
    case PG_SAVE: save_input(in); break;
    case PG_FX: fx_input(in); break;
    case PG_GLO:
        if (in->pressed & BIT(BTN_REC))
            ui.solo_mode ^= 1u;
        if ((in->pressed & BIT(BTN_OCTUP)) && nch > 16u)
            ui.bank = 1;
        if (in->pressed & BIT(BTN_OCTDN))
            ui.bank = 0;
        if (in->enc[ENC_ALGO])
            move_chan(in->enc[ENC_ALGO] * ((in->held & BIT(BTN_EDIT)) ? (ui.view[PG_GLO] ? 8 : 4) : 1), ui.view[PG_GLO] ? 8u : 4u, nch);
        glo_knobs(in, st);
        if ((in->pressed & BIT(BTN_SEL)) && is_mod(h)) {    /* ProTracker mode on / off */
            player_set_pt_mode(!player_pt_mode());
            ui_message(player_pt_mode() ? "PROTRACKER MODE" : "FT2 MODE");
        }
        if (in->pressed & BIT(BTN_ARP)) {                 /* stereo separation: 0, 20, 50, 100 % */
            static const uint8_t SEP[4] = {0, 20, 50, 100};
            ui.sep_i = (uint8_t)((ui.sep_i + 1u) % 4u);
            player_set_stereo_sep(SEP[ui.sep_i]);
            ui_message(ui.sep_i == 0 ? "SEPARATION 0%" : ui.sep_i == 1 ? "SEPARATION 20%" : ui.sep_i == 2 ? "SEPARATION 50%" : "SEPARATION 100%");
        }
        break;
    case PG_ENV: env_input(in); break;
    case PG_LFO: smp_input(in); break;
    }
    h = song_h();
    st = player_get_state();
    if (st->playing) {
        ui.pos = st->song_pos;
        ui.row = st->row;
    }
    ui.vpat = (uint16_t)(st->playing ? st->pattern : pos_pattern(h, ui.pos));

    if (ui.redraw) {
        ui.redraw = 0;
        pages_enter();
    }
    switch (ui.page) {
    case PG_HOME: home_draw(st, h); break;
    case PG_SEQ: seq_draw(st, h); break;
    case PG_SAVE: save_draw(); break;
    case PG_FX: fx_draw(); break;
    case PG_GLO: glo_draw(st); break;
    case PG_ENV: env_draw(); break;
    default: smp_draw(); break;
    }
    pages_leds(st, in);
    ui.frames++;
}
