/* SPDX-License-Identifier: GPL-3.0-only */
/* The GLO page, the mixer (part of pages.c):
 *
 *   title, song, position / pattern / row, speed / BPM / global volume, two lines from the platform
 *   (the FM-1: CPU load and USB), one bar per channel (its output level), muted channels red,
 *   the pattern around the playing row, 4 channels at a time, GLO again 8 (ALGORITHM moves them)
 *
 * On the GLO page the 16 white keys are channels 1..16 (OCT+ : 17..32, OCT- : back), their LEDs
 * light while the channel sounds; REC switches what a white key does: MUTE (toggle the channel
 * off) or SOLO (toggle it into the solo group: with any channel soloed only the solo group plays). */

enum { G_TITLE, G_SONG, G_POS, G_SPEED, G_CPU, G_USB, G_CHNUM, G_PHEAD, G_PAT0 };
#define G_PAT_ROWS 14
#define G_NLINES (G_PAT0 + G_PAT_ROWS)
#define BARS_Y 74u
#define BARS_H 32u

static gline glines[G_NLINES];

static void glo_enter(void)
{
    uint32_t i;
    for (i = 0; i < G_NLINES; i++)
        gl_init(&glines[i], i >= G_CHNUM, 0);
    glines[G_TITLE].y = 0;
    glines[G_SONG].y = 12;
    glines[G_POS].y = 24;
    glines[G_SPEED].y = 36;
    glines[G_CPU].y = 48;
    glines[G_USB].y = 60;
    glines[G_CHNUM].y = BARS_Y + BARS_H + 2;          /* 108 */
    glines[G_PHEAD].y = 120;
    for (i = 0; i < G_PAT_ROWS; i++)
        glines[G_PAT0 + i].y = (uint8_t)(128u + i * 8u);
    if (ui.view[PG_GLO]) {         /* GLO again: 8 channels, laid out as HOME's */
        uint32_t k;
        XMAP8[0] = 0;
        XMAP8[1] = 5;
        for (k = 0; k < 8u; k++)
            for (i = 0; i < 5u; i++)
                XMAP8[2 + k * 5 + i] = (uint8_t)(15 + k * 28 + i * 5);
        for (i = G_PHEAD; i < G_NLINES; i++) {
            glines[i].xmap = XMAP8;
            glines[i].ncols = sizeof XMAP8;
        }
        if (ui.win + 8u > song.hdr.num_channels)
            ui.win = (uint8_t)(song.hdr.num_channels > 8u ? song.hdr.num_channels - 8u : 0u);
    }
    lcd_fill(0, 72, 240, 2, PALETTE[C_BEAT]);
    glo_stats(0, 0, 1);
}

static void glo_draw(const player_state *st)
{
    const fmt_header *h = song_h();
    gline *l;
    uint32_t i, x, nch = st->num_channels, wide = ui.view[PG_GLO] != 0;
    uint8_t lv[FMT_MAX_CHANNELS], col[FMT_MAX_CHANNELS];
    l = &glines[G_TITLE];
    gl_clear(l, ATTR(C_TEXT, C_HEAD));
    gl_puts(l, 0, "MIXER", ATTR(C_HI, C_HEAD));
    gl_puts(l, 12, ui.solo_mode ? "SOLO" : "MUTE", ui.solo_mode ? ATTR(C_HI, C_HEAD) : ATTR(C_TEXT, C_HEAD));
    gl_puts(l, 17, ui.bank ? "CH17-32" : "CH1-16", ATTR(C_TEXT, C_HEAD));
    if (player_pt_mode())
        gl_puts(l, 25, player_led() ? "PT LED" : "PT", ATTR(C_HI, C_HEAD));
    gl_puts(l, 33, ui.img_off ? "FLASH" : "ROM", ATTR(C_TEXT, C_HEAD));

    l = &glines[G_SONG];
    gl_clear(l, ATTR(C_TEXT, C_BG));
    if (ui.msg && fm1_ms - ui.msg_ms < 2000u)
        gl_puts(l, 1, ui.msg, ATTR(C_WARN, C_BG));
    else if (ui.song >= 0) {
        x = gl_puts(l, 1, SONGS[ui.song].name, ATTR(C_HI, C_BG));
        if (h && h->name[0]) {
            x = gl_puts(l, x, "  ", ATTR(C_DIM, C_BG));
            gl_puts(l, x, h->name, ATTR(C_DIM, C_BG));
        }
    } else if (h)
        gl_puts(l, 1, h->name[0] ? h->name : "(no name)", ATTR(C_HI, C_BG));
    else
        gl_puts(l, 1, "NO SONG LOADED", ATTR(C_WARN, C_BG));


    l = &glines[G_POS];
    gl_clear(l, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, 1, "POS ", ATTR(C_DIM, C_BG));
    x = gl_hex(l, x, st->song_pos, 2, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, "/", ATTR(C_DIM, C_BG));
    x = gl_hex(l, x, h && h->song_length ? h->song_length - 1u : 0, 2, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, "  PAT ", ATTR(C_DIM, C_BG));
    x = gl_hex(l, x, st->pattern, 2, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, "  ROW ", ATTR(C_DIM, C_BG));
    x = gl_hex(l, x, st->row, 2, ATTR(C_HI, C_BG));
    x = gl_puts(l, x, "/", ATTR(C_DIM, C_BG));
    x = gl_hex(l, x, st->num_rows ? st->num_rows - 1u : 0, 2, ATTR(C_TEXT, C_BG));
    gl_puts(l, 34, st->playing ? " PLAY" : " STOP", st->playing ? ATTR(C_OK, C_BG) : ATTR(C_DIM, C_BG));

    l = &glines[G_SPEED];
    gl_clear(l, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, 1, "SPD ", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, st->speed, 2, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, "  BPM ", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, st->bpm, 3, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, " GV ", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, st->global_vol, 2, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, " CH ", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, nch, 2, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, h && (h->flags & FMT_LINEAR) ? " LIN" : " AMI", ATTR(C_DIM, C_BG));
    x = gl_puts(l, x, " AMP ", ATTR(C_DIM, C_BG));
    gl_dec(l, x, (uint32_t)player_get_amp(), 1, ATTR(C_TEXT, C_BG));

    glo_stats(&glines[G_CPU], &glines[G_USB], 0);   /* (the platform: CPU, USB) */

    /* channel bars */
    for (i = 0; i < nch; i++) {
        lv[i] = ui.off[i] ? 255u : ui.level[i];
        col[i] = ui.off[i] ? C_MUTE : ui.solo[i] ? C_INS : (ui.level[i] > 220u ? C_BARHI : C_BAR);
    }
    if (ui.frames % 2u == 0u)
        gfx_bars(BARS_Y, BARS_H, lv, col, nch ? nch : 1u);

    l = &glines[G_CHNUM];
    gl_clear(l, ATTR(C_DIM, C_BG));
    if (nch) {
        uint32_t bw = 240u / nch > 24u ? 24u : 240u / nch, x0 = (240u - bw * nch) / 2u;
        for (i = 0; i < nch; i++) {
            uint32_t cx = (x0 + i * bw + bw / 2u) / 5u;
            char d[2] = {(char)('0' + (i + 1u) % 10u), 0};
            uint8_t a = ui.off[i] ? ATTR(C_MUTE, C_BG) : ui.solo[i] ? ATTR(C_INS, C_BG) :
                        i >= ui.win && i < ui.win + (wide ? 8u : 4u) ? ATTR(C_HI, C_BG) :
                        i / 16u == ui.bank ? ATTR(C_TEXT, C_BG) : ATTR(C_DIM, C_BG);
            if (cx < 48u)
                gl_puts(l, cx, d, a);
        }
    }

    /* pattern view: header, then the rows around the playing one */
    l = &glines[G_PHEAD];
    gl_clear(l, ATTR(C_DIM, C_BEAT));
    for (i = 0; i < (wide ? 8u : 4u) && ui.win + i < nch; i++) {
        uint32_t c = ui.win + i;
        if (wide) {
            gl_dec(l, 2 + i * 5, c + 1u, 2 | GL_ZERO, ui.off[c] ? ATTR(C_MUTE, C_BEAT) : ATTR(C_TEXT, C_BEAT));
            continue;
        }
        x = 3u + i * 11u;
        gl_puts(l, x, "|", ATTR(C_DIM, C_BEAT));
        gl_puts(l, x + 1u, "CH", ATTR(C_DIM, C_BEAT));
        gl_dec(l, x + 3u, c + 1u, 2, ui.off[c] ? ATTR(C_MUTE, C_BEAT) : ATTR(C_TEXT, C_BEAT));
    }
    for (i = 0; i < G_PAT_ROWS; i++) {
        int32_t row = (int32_t)st->row + (int32_t)i - G_PAT_ROWS / 2;
        uint8_t bg = row == (int32_t)st->row ? C_ROWBG : (row % 4 == 0 ? C_BEAT : C_BG);
        l = &glines[G_PAT0 + i];
        gl_clear(l, ATTR(C_TEXT, bg));
        if (row < 0 || row >= (int32_t)st->num_rows)
            continue;
        {
            player_cell cells[FMT_MAX_CHANNELS];
            uint32_t c;
            player_get_row(st->pattern, (uint16_t)row, cells, FMT_MAX_CHANNELS);
            gl_hex(l, 0, (uint32_t)row, 2, row == (int32_t)st->row ? ATTR(C_HI, bg) : ATTR(C_DIM, bg));
            for (c = 0; c < (wide ? 8u : 4u) && ui.win + c < nch; c++) {
                const player_cell *e = &cells[ui.win + c];
                if (wide) {                     /* note, the instrument's last digit, the effect letter */
                    char b[4];
                    uint32_t cx = 2 + c * 5;
                    note_str(b, e->note);
                    gl_puts(l, cx, b, ATTR(!e->note ? C_DIM : ui.off[ui.win + c] ? C_MUTE : C_NOTE, bg));
                    if (e->instr)
                        gl_hex(l, cx + 3, e->instr, 1, ATTR(C_INS, bg));
                    if (e->efx || e->efx_data) {
                        b[0] = fx_char(e->efx);
                        b[1] = 0;
                        gl_puts(l, cx + 4, b, ATTR(C_FX, bg));
                    }
                    continue;
                }
                x = 3u + c * 11u;
                gl_puts(l, x - 1u, "|", ATTR(C_DIM, bg));
                put_cell(l, x, e, ui.off[ui.win + c], is_mod(h), -1, bg);
            }
        }
    }
    for (i = 0; i < G_NLINES; i++)
        gl_draw_ov(&glines[i], 0, wide && i >= G_PHEAD ? ov_sep8 : 0, 0);
}

