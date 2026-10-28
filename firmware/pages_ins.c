/* SPDX-License-Identifier: GPL-3.0-only */
/* The instrument page (ENV: the volume envelope, ENV again: the panning one) and the sample page
 * (LFO). Part of pages.c. PRESETS picks the instrument on both, as for note entry; the keys play it.
 * Everything changed here goes into song.c's RAM tables (song.ins, song.smp) and the undo log. */

#define GRAPH_Y 14u
#define GRAPH_STRIPS 11u           /* 12-pixel lines: 132 pixels */
#define GRAPH_H (GRAPH_STRIPS * 12u)

static fmt_instr *cur_ins(void)
{
    if (!ui.loaded_ok || ui.ins < 1u || ui.ins > song.hdr.num_instr)
        return 0;
    return &song.ins[ui.ins - 1u];
}

static fmt_sample *cur_smp(void)
{
    fmt_instr *in = cur_ins();
    if (!in || !in->num_samples)
        return 0;
    if (ui.smp_k >= in->num_samples)
        ui.smp_k = 0;
    if (in->first_sample + ui.smp_k >= song.hdr.num_samples)
        return 0;
    return &song.smp[in->first_sample + ui.smp_k];
}

static void strip_px(uint16_t *buf, uint32_t h, int32_t x, int32_t y, uint32_t col)
{
    if (x >= 0 && x < 240 && y >= 0 && y < (int32_t)h)
        buf[(uint32_t)y * 240u + (uint32_t)x] = pal_sw[col];
}

static void graph_enter(void)
{
    uint32_t i;
    lay(0, 0, 0);
    for (i = 0; i < GRAPH_STRIPS; i++)
        lay(1 + i, 0, (uint8_t)(GRAPH_Y + i * 12u));
    lay(12, 1, 150);
    lay(13, 1, 160);
    lay(14, 1, 170);
    lay(15, 1, 180);
    lay(16, 1, 190);
    lay(17, 1, 200);
    lay(27, 1, 224);
    lay(28, 1, 232);
    lcd_fill(0, 12, 240, 2, PALETTE[C_BG]);
    lcd_fill(0, 146, 240, 4, PALETTE[C_BG]);
    lcd_fill(0, 210, 240, 14, PALETTE[C_BG]);
}

/* ---------------------------------------------------------------- the envelope --- */
typedef struct {
    const int16_t (*pt)[2];
    uint8_t n, sel, on, sus, sus_on, ls, le, loop_on;
    uint16_t maxx;
    int32_t y0;                    /* this strip's top in the graph */
} env_ov;

static int32_t env_x(const env_ov *e, int32_t t) { return 6 + t * 228 / (e->maxx ? e->maxx : 1); }
static int32_t env_y(int32_t v) { return (int32_t)GRAPH_H - 5 - v * ((int32_t)GRAPH_H - 10) / 64; }

static void ov_env(uint16_t *buf, uint32_t h, const void *ctx)
{
    const env_ov *e = ctx;
    uint32_t k;
    int32_t x, y0 = e->y0;
    if (e->loop_on && e->ls < e->n && e->le < e->n)        /* the loop: shaded */
        for (x = env_x(e, e->pt[e->ls][0]); x <= env_x(e, e->pt[e->le][0]); x++)
            strip_rect(buf, h, x, 0, 1, (int32_t)h, C_BEAT);
    strip_rect(buf, h, 0, env_y(0) - y0, 240, 1, C_GRID);
    strip_rect(buf, h, 0, env_y(64) - y0, 240, 1, C_GRID);
    if (e->sus_on && e->sus < e->n)                         /* sustain: a dotted line */
        for (k = 0; k < h; k += 2u)
            strip_px(buf, h, env_x(e, e->pt[e->sus][0]), (int32_t)k, C_HI);
    for (k = 0; k + 1u < e->n; k++) {                       /* the segments */
        int32_t ax = env_x(e, e->pt[k][0]), ay = env_y(e->pt[k][1]);
        int32_t bx = env_x(e, e->pt[k + 1u][0]), by = env_y(e->pt[k + 1u][1]);
        int32_t steps = (bx - ax > (by > ay ? by - ay : ay - by) ? bx - ax : (by > ay ? by - ay : ay - by)), s;
        if (steps < 1)
            steps = 1;
        for (s = 0; s <= steps; s++)
            strip_px(buf, h, ax + (bx - ax) * s / steps, ay + (by - ay) * s / steps - y0, e->on ? C_VOL : C_DIM);
    }
    for (k = 0; k < e->n; k++) {                             /* the points */
        int32_t px = env_x(e, e->pt[k][0]), py = env_y(e->pt[k][1]) - y0, r = k == e->sel ? 2 : 1;
        strip_rect(buf, h, px - r, py - r, 2 * r + 1, 2 * r + 1, k == e->sel ? C_HI : C_TEXT);
    }
}

static void env_input(const ui_in *in)
{
    fmt_instr *ins = cur_ins();
    int pan = ui.view[PG_ENV] != 0, shift = (in->held & BIT(BTN_EDIT)) != 0;
    int32_t k1 = in->enc[ENC_K1], k2 = in->enc[ENC_K2], k3 = in->enc[ENC_K3], k4 = in->enc[ENC_K4];
    int16_t (*pt)[2];
    uint8_t *len, *sus, *ls, *le, *flags;
    if (in->enc[ENC_PRESET] && song.hdr.num_instr)
        ui.ins = (uint8_t)clampi((int32_t)ui.ins + in->enc[ENC_PRESET], 1, song.hdr.num_instr);
    if (!ins || !(k1 | k2 | k3 | k4 | (int32_t)(in->pressed & (BIT(BTN_SEL) | BIT(BTN_ARP)))))
        return;
    if (is_mod(&song.hdr)) {
        ui_message("A MOD HAS NO ENVELOPES");
        return;
    }
    pt = pan ? ins->pan_env : ins->vol_env;
    len = pan ? &ins->pan_env_len : &ins->vol_env_len;
    sus = pan ? &ins->pan_sus : &ins->vol_sus;
    ls = pan ? &ins->pan_loop_start : &ins->vol_loop_start;
    le = pan ? &ins->pan_loop_end : &ins->vol_loop_end;
    flags = pan ? &ins->pan_flags : &ins->vol_flags;
    if (*len < 2u) {               /* an empty envelope: two points to start from */
        EDIT(pt[0][0], 0);
        EDIT(pt[0][1], pan ? 32 : 64);
        EDIT(pt[1][0], 16);
        EDIT(pt[1][1], pan ? 32 : 64);
        EDIT(*len, 2);
    }
    if (ui.env_pt >= *len)
        ui.env_pt = (uint8_t)(*len - 1u);
    if (k1 && shift)
        EDIT(*ls, clampi(*ls + k1, 0, *len - 1));
    else if (k1)
        ui.env_pt = (uint8_t)clampi(ui.env_pt + k1, 0, *len - 1);
    if (k2 && shift) {
        EDIT(*le, clampi(*le + k2, 0, *len - 1));
    } else if (k2 && ui.env_pt > 0) {      /* the tick: between the neighbours (the first is at 0) */
        int32_t lo = pt[ui.env_pt - 1u][0] + 1, hi = ui.env_pt + 1u < *len ? pt[ui.env_pt + 1u][0] - 1 : 0xFFFF;
        EDIT(pt[ui.env_pt][0], clampi(pt[ui.env_pt][0] + k2, lo, hi));
    }
    if (k3 && shift)
        EDIT(*sus, clampi(*sus + k3, 0, *len - 1));
    else if (k3)
        EDIT(pt[ui.env_pt][1], clampi(pt[ui.env_pt][1] + k3, 0, 64));
    if (k4 && shift) {             /* points: more (after the last) or fewer */
        int32_t n = clampi(*len + k4, 2, 12), i;
        for (i = *len; i < n; i++) {
            EDIT(pt[i][0], pt[i - 1][0] + 16);
            EDIT(pt[i][1], pt[i - 1][1]);
        }
        EDIT(*len, n);
        if (ui.env_pt >= n)
            ui.env_pt = (uint8_t)(n - 1);
    } else if (k4 && !pan) {
        EDIT(ins->fadeout, clampi(ins->fadeout + k4 * 8, 0, 4095));
    }
    if (in->pressed & BIT(BTN_SEL))
        EDIT(*flags, *flags ^ (shift ? FMT_ENV_LOOP : FMT_ENV_ON));
    if (in->pressed & BIT(BTN_ARP))
        EDIT(*flags, *flags ^ FMT_ENV_SUSTAIN);
}

static void env_draw(void)
{
    fmt_instr *ins = cur_ins();
    int pan = ui.view[PG_ENV] != 0;
    env_ov e;
    uint32_t i, x;
    gline *l = &L[0];
    title_bar(l, "INS ");
    x = gl_hex(l, 4, ui.ins, 2, ATTR(C_HI, C_HEAD));
    if (ins)
        gl_putn(l, x + 1, ins->name, 22, ATTR(C_TEXT, C_HEAD));
    gl_puts(l, 33, pan ? "PAN ENV" : "VOL ENV", ATTR(C_HI, C_HEAD));
    memset(&e, 0, sizeof e);
    if (ins) {
        e.pt = (const int16_t (*)[2])(pan ? ins->pan_env : ins->vol_env);
        e.n = pan ? ins->pan_env_len : ins->vol_env_len;
        if (e.n > 12u)
            e.n = 12;
        e.sel = ui.env_pt;
        e.on = ((pan ? ins->pan_flags : ins->vol_flags) & FMT_ENV_ON) != 0;
        e.sus = pan ? ins->pan_sus : ins->vol_sus;
        e.sus_on = ((pan ? ins->pan_flags : ins->vol_flags) & FMT_ENV_SUSTAIN) != 0;
        e.ls = pan ? ins->pan_loop_start : ins->vol_loop_start;
        e.le = pan ? ins->pan_loop_end : ins->vol_loop_end;
        e.loop_on = ((pan ? ins->pan_flags : ins->vol_flags) & FMT_ENV_LOOP) != 0;
        e.maxx = e.n ? (uint16_t)(e.pt[e.n - 1][0] + 8) : 64u;
        if (e.maxx < 64u)
            e.maxx = 64;
    }
    for (i = 0; i < GRAPH_STRIPS; i++) {
        uint32_t key = 2166136261u, k;
        l = &L[1 + i];
        gl_clear(l, ATTR(C_TEXT, C_BG));
        if (i == 0 && (!ins || !e.n))
            gl_puts(l, 2, !ins ? "no instrument" : "no envelope: turn a knob to make one", ATTR(C_DIM, C_BG));
        e.y0 = (int32_t)(i * 12u);
        for (k = 0; k < e.n; k++)
            key = (key ^ (uint32_t)(e.pt[k][0] << 8 ^ e.pt[k][1])) * 16777619u;
        key ^= (uint32_t)e.n << 1 ^ (uint32_t)e.sel << 5 ^ (uint32_t)e.on << 9 ^ (uint32_t)e.sus << 10 ^
               (uint32_t)e.sus_on << 14 ^ (uint32_t)e.ls << 15 ^ (uint32_t)e.le << 19 ^ (uint32_t)e.loop_on << 23 ^
               (uint32_t)e.maxx << 24;
        gl_draw_ov(l, key, e.n ? ov_env : 0, &e);
    }
    for (i = 12; i <= 17u; i++)
        gl_clear(&L[i], ATTR(C_TEXT, C_BG));
    if (ins && e.n) {
        uint8_t lab = ATTR(C_DIM, C_BG), val = ATTR(C_TEXT, C_BG), hi = ATTR(C_HI, C_BG);
        l = &L[12];
        x = gl_puts(l, 0, "POINT ", lab);
        x = gl_dec(l, x, ui.env_pt + 1u, 1, hi);
        x = gl_puts(l, x, "/", lab);
        x = gl_dec(l, x, e.n, 1, val);
        x = gl_puts(l, x, "  TICK ", lab);
        x = gl_dec(l, x, (uint32_t)e.pt[ui.env_pt < e.n ? ui.env_pt : 0][0], 1, val);
        x = gl_puts(l, x, "  VALUE ", lab);
        gl_dec(l, x, (uint32_t)e.pt[ui.env_pt < e.n ? ui.env_pt : 0][1], 1, val);
        l = &L[13];
        x = gl_puts(l, 0, "ENVELOPE ", lab);
        x = gl_puts(l, x, e.on ? "ON " : "OFF", e.on ? ATTR(C_OK, C_BG) : lab);
        x = gl_puts(l, x, "  SUSTAIN ", lab);
        x = e.sus_on ? gl_dec(l, x, e.sus + 1u, 1, hi) : gl_puts(l, x, "off", lab);
        x = gl_puts(l, x, "  LOOP ", lab);
        if (e.loop_on) {
            x = gl_dec(l, x, e.ls + 1u, 1, hi);
            x = gl_puts(l, x, "-", lab);
            gl_dec(l, x, e.le + 1u, 1, hi);
        } else
            gl_puts(l, x, "off", lab);
        l = &L[14];
        if (!pan) {
            x = gl_puts(l, 0, "FADEOUT ", lab);
            gl_dec(l, x, ins->fadeout, 1, val);
        }
        l = &L[15];
        x = gl_puts(l, 0, "AUTO-VIBRATO type ", lab);
        x = gl_dec(l, x, ins->vib_type, 1, val);
        x = gl_puts(l, x, " sweep ", lab);
        x = gl_dec(l, x, ins->vib_sweep, 1, val);
        x = gl_puts(l, x, " depth ", lab);
        x = gl_dec(l, x, ins->vib_depth, 1, val);
        x = gl_puts(l, x, " rate ", lab);
        gl_dec(l, x, ins->vib_rate, 1, val);
        l = &L[16];
        x = gl_puts(l, 0, "SAMPLES ", lab);
        gl_dec(l, x, ins->num_samples, 1, val);
    }
    if (ui.msg && fm1_ms - ui.msg_ms < 2000u)
        gl_puts(&L[17], 0, ui.msg, ATTR(C_WARN, C_BG));
    footer(27, pan ? "K1 POINT K2 TICK K3 VALUE  EDIT+K4 points" : "K1 POINT K2 TICK K3 VALUE K4 FADEOUT",
           pan ? "SEL on  ARP sustain  EDIT+SEL loop  ENV: vol" : "SEL on  ARP sustain  EDIT+SEL loop  ENV: pan");
    for (i = 0; i < nl; i++)
        if (i < 1u || i > GRAPH_STRIPS)
            gl_draw(&L[i]);
}

/* ------------------------------------------------------------------ the sample --- */
static int8_t wmin[232], wmax[232];
static uint32_t wave_key;

typedef struct {
    int32_t y0;
    uint32_t len, ls, le;
    uint8_t loop;
} wave_ov;

static int32_t wave_x(const wave_ov *w, uint32_t pos) { return 4 + (int32_t)(pos * 232u / (w->len ? w->len : 1u));   /* (lengths < 18 M: no 64 bits) */ }

static void ov_wave(uint16_t *buf, uint32_t h, const void *ctx)
{
    const wave_ov *w = ctx;
    int32_t x, mid = (int32_t)GRAPH_H / 2 - w->y0, sc = (int32_t)GRAPH_H / 2 - 4;
    if (w->loop) {
        int32_t a = wave_x(w, w->ls), b = wave_x(w, w->le);
        strip_rect(buf, h, a, 0, b - a + 1, (int32_t)h, C_BEAT);
    }
    strip_rect(buf, h, 4, mid, 232, 1, C_GRID);
    for (x = 0; x < 232; x++) {
        int32_t a = mid - wmax[x] * sc / 127, b = mid - wmin[x] * sc / 127;
        strip_rect(buf, h, 4 + x, a, 1, b - a + 1, C_INS);
    }
    if (w->loop) {
        strip_rect(buf, h, wave_x(w, w->ls), 0, 1, (int32_t)h, C_HI);
        strip_rect(buf, h, wave_x(w, w->le), 0, 1, (int32_t)h, C_HI);
    }
}

static void wave_scan(const fmt_sample *s)     /* the columns' min and max (when the sample changes) */
{
    uint32_t key = s ? s->data_off ^ s->length * 2654435761u : 0, x;
    if (key == wave_key)
        return;
    wave_key = key;
    for (x = 0; x < 232u; x++) {
        uint32_t a = s ? x * s->length / 232u : 0, b = s ? (x + 1u) * s->length / 232u : 0, i;
        int32_t lo = 127, hi = -128, step;
        if (b <= a)
            b = a + 1u;
        step = (int32_t)((b - a) / 64u) + 1;   /* (at most 64 reads a column) */
        for (i = a; s && i < b && i < s->length; i += (uint32_t)step) {
            int32_t v = (s->flags & FMT_16BIT) ? ((const int16_t *)(song.img + s->data_off))[i] >> 8 :
                        ((const int8_t *)(song.img + s->data_off))[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (hi < lo)
            lo = hi = 0;
        wmin[x] = (int8_t)lo;
        wmax[x] = (int8_t)hi;
    }
}

static const char *const LOOP_NAME[3] = {"off", "forward", "ping-pong"};

static void smp_input(const ui_in *in)
{
    fmt_instr *ins = cur_ins();
    fmt_sample *s;
    int shift = (in->held & BIT(BTN_EDIT)) != 0;
    int32_t k1 = in->enc[ENC_K1], k2 = in->enc[ENC_K2], k3 = in->enc[ENC_K3], k4 = in->enc[ENC_K4], step;
    if (in->enc[ENC_PRESET] && song.hdr.num_instr) {
        ui.ins = (uint8_t)clampi((int32_t)ui.ins + in->enc[ENC_PRESET], 1, song.hdr.num_instr);
        ui.smp_k = 0;
    }
    if (ins && in->enc[ENC_ALGO] && ins->num_samples)
        ui.smp_k = (uint8_t)clampi(ui.smp_k + in->enc[ENC_ALGO], 0, ins->num_samples - 1);
    s = cur_smp();
    if (!s)
        return;
    step = shift ? 1 : (int32_t)(s->length / 200u) + 1;
    if ((k1 || k2) && !(s->flags & 3)) {
        ui_message("SEL: LOOP ON FIRST");
    } else {
        if (k1)
            EDIT(s->loop_start, clampi((int32_t)s->loop_start + k1 * step, 0, (int32_t)(s->length - s->loop_length)));
        if (k2)
            EDIT(s->loop_length, clampi((int32_t)s->loop_length + k2 * step, 2, (int32_t)(s->length - s->loop_start)));
    }
    if (k3 && shift)
        EDIT(s->panning, clampi(s->panning + k3 * 4, 0, 255));
    else if (k3)
        EDIT(s->volume, clampi(s->volume + k3, 0, 64));
    if (k4 && shift)
        EDIT(s->relative_note, clampi(s->relative_note + k4, -96, 95));
    else if (k4)
        EDIT(s->finetune, clampi(s->finetune + k4 * (is_mod(&song.hdr) ? 16 : 1), -128, 127));
    if ((in->pressed & BIT(BTN_SEL)) && s->length >= 2u) {   /* the loop: off, forward, ping-pong */
        uint32_t t = ((s->flags & 3u) + 1u) % (is_mod(&song.hdr) ? 2u : 3u);
        if (t && s->loop_length < 2u) {
            EDIT(s->loop_start, 0);
            EDIT(s->loop_length, s->length);
        }
        EDIT(s->flags, (s->flags & ~3u) | t);
    }
}

static void smp_draw(void)
{
    fmt_instr *ins = cur_ins();
    fmt_sample *s = cur_smp();
    wave_ov w;
    uint32_t i, x, bps = s && (s->flags & FMT_16BIT) ? 2u : 1u;
    uint8_t lab = ATTR(C_DIM, C_BG), val = ATTR(C_TEXT, C_BG), hi = ATTR(C_HI, C_BG);
    gline *l = &L[0];
    title_bar(l, "SMP ");
    x = gl_hex(l, 4, ui.ins, 2, ATTR(C_HI, C_HEAD));
    x = gl_puts(l, x, ".", ATTR(C_TEXT, C_HEAD));
    x = gl_dec(l, x, ui.smp_k + 1u, 1, ATTR(C_HI, C_HEAD));
    if (s)
        gl_putn(l, x + 1, s->name, 22, ATTR(C_TEXT, C_HEAD));
    wave_scan(s);
    memset(&w, 0, sizeof w);
    if (s) {
        w.len = s->length;
        w.loop = (uint8_t)(s->flags & 3u);
        w.ls = s->loop_start;
        w.le = s->loop_start + s->loop_length;
    }
    for (i = 0; i < GRAPH_STRIPS; i++) {
        l = &L[1 + i];
        gl_clear(l, ATTR(C_TEXT, C_BG));
        if (i == 0 && !s)
            gl_puts(l, 2, ins ? "this instrument has no sample" : "no instrument", ATTR(C_DIM, C_BG));
        w.y0 = (int32_t)(i * 12u);
        gl_draw_ov(l, wave_key ^ w.ls * 31u ^ w.le * 977u ^ w.loop * 7u ^ i, s ? ov_wave : 0, &w);
    }
    for (i = 12; i <= 17u; i++)
        gl_clear(&L[i], ATTR(C_TEXT, C_BG));
    if (s) {
        l = &L[12];
        x = gl_puts(l, 0, "LENGTH ", lab);
        x = gl_dec(l, x, s->length, 1, val);
        x = gl_puts(l, x, bps == 2u ? "  16-BIT" : "  8-BIT", lab);
        x = gl_puts(l, x, "  ", lab);
        x = gl_dec(l, x, ui.smp_k + 1u, 1, val);
        x = gl_puts(l, x, " of ", lab);
        gl_dec(l, x, ins->num_samples, 1, val);
        l = &L[13];
        x = gl_puts(l, 0, "LOOP ", lab);
        x = gl_puts(l, x, LOOP_NAME[(s->flags & 3u) < 3u ? (s->flags & 3u) : 0u], (s->flags & 3u) ? hi : lab);
        if (s->flags & 3u) {
            x = gl_puts(l, x, "  START ", lab);
            x = gl_dec(l, x, s->loop_start, 1, val);
            x = gl_puts(l, x, "  LENGTH ", lab);
            gl_dec(l, x, s->loop_length, 1, val);
        }
        l = &L[14];
        x = gl_puts(l, 0, "VOLUME ", lab);
        x = gl_dec(l, x, s->volume, 1, val);
        x = gl_puts(l, x, "  FINETUNE ", lab);
        if (s->finetune < 0)
            x = gl_puts(l, x, "-", val);
        x = gl_dec(l, x, (uint32_t)(s->finetune < 0 ? -s->finetune : s->finetune), 1, val);
        x = gl_puts(l, x, "  PAN ", lab);
        gl_dec(l, x, s->panning, 1, val);
        l = &L[15];
        x = gl_puts(l, 0, "RELATIVE NOTE ", lab);
        if (s->relative_note < 0)
            x = gl_puts(l, x, "-", val);
        else
            x = gl_puts(l, x, "+", val);
        gl_dec(l, x, (uint32_t)(s->relative_note < 0 ? -s->relative_note : s->relative_note), 1, val);
    }
    if (ui.msg && fm1_ms - ui.msg_ms < 2000u)
        gl_puts(&L[17], 0, ui.msg, ATTR(C_WARN, C_BG));
    footer(27, "K1 LOOP START K2 LOOP LEN K3 VOL K4 FINE", "SEL loop  EDIT+K3 pan  EDIT+K4 rel.note");
    for (i = 0; i < nl; i++)
        if (i < 1u || i > GRAPH_STRIPS)
            gl_draw(&L[i]);
}
