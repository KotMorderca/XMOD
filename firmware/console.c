/* SPDX-License-Identifier: GPL-3.0-only */
/* USB serial console (CDC-ACM; any terminal, any baud rate). Runs in the main loop; Felucca's
 * usb_poll moves the bytes. `help` lists the commands.
 *
 * The song manager (web/, tools/songs.py) talks to it too: `raw` (no echo, no prompt), then
 *   info                      fw, slots, flash use, the song loaded
 *   ls                        slot I SIZE SEQ NAME ... ok
 *   get I                     data SIZE CRC, the image in base64 lines, ok
 *   put SIZE CRC NAME         ready; then lines "d BASE64" (any lengths); ack N every 4 KiB;
 *                             at the end: saved I, or err CODE
 *   del I / ren I NAME / load I [force]     ok, or err ...
 * CRC: CRC-32 (zlib's) of the image. */
#define CON_LINE 200u

static struct {
    char line[CON_LINE];
    uint32_t len;
    uint8_t dtr_seen, stalled, raw;
    uint8_t put;                   /* a `put` is receiving */
    uint32_t put_n, put_size, put_crc, put_ack;
} con;

static void con_putc(char c)
{
    uint32_t t0 = fm1_ms;
    while (co_w - co_r >= CO_N) {
        if (!cdc.dtr || con.stalled || fm1_ms - t0 > (con.raw ? 2000u : 20u)) {
            con.stalled = 1;
            return;
        }
        fm1_wdt_feed();
    }
    cdc_out[co_w % CO_N] = (uint8_t)c;
    RING_PUBLISH();
    co_w++;
}

static void con_puts(const char *s)
{
    while (*s)
        con_putc(*s++);
}

static void con_dec(int32_t v)
{
    char b[12];
    uint32_t n = 0, u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    if (v < 0)
        con_putc('-');
    do
        b[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) != 0 && n < sizeof b);
    while (n)
        con_putc(b[--n]);
}

static void con_kv(const char *k, int32_t v)
{
    con_puts(k);
    con_putc(' ');
    con_dec(v);
    con_puts("\r\n");
}

static int con_arg(const char *p, int32_t *v)
{
    int32_t n = 0;
    int any = 0;
    while (*p == ' ')
        p++;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p++ - '0');
        any = 1;
    }
    *v = n;
    return any;
}

static int con_cmd(const char *line, const char *cmd, const char **rest)
{
    uint32_t n = str_len(cmd);
    if (memcmp(line, cmd, n) || (line[n] && line[n] != ' '))
        return 0;
    *rest = line + n;
    return 1;
}

/* ------------------------------------------------------------ the song manager --- */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void con_hex(uint32_t v)
{
    int i;
    for (i = 28; i >= 0; i -= 4)
        con_putc("0123456789abcdef"[(v >> i) & 15u]);
}

static int b64_val(char c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
           c == '+' ? 62 : c == '/' ? 63 : -1;
}

static int b64_decode(const char *s, uint8_t *out)   /* bytes, -1: bad */
{
    uint32_t acc = 0, bits = 0;
    int n = 0, v;
    for (; *s && *s != '='; s++) {
        if ((v = b64_val(*s)) < 0)
            return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

static int con_slot(const char *r, int32_t *v)       /* a slot index argument */
{
    if (!con_arg(r, v) || *v < 0 || *v >= store_n) {
        con_puts("err no such slot\r\n");
        return 0;
    }
    return 1;
}

static const char *con_rest(const char *r)           /* past the next number */
{
    while (*r == ' ')
        r++;
    while (*r && *r != ' ')
        r++;
    while (*r == ' ')
        r++;
    return r;
}

static void con_err(int e)
{
    con_puts("err ");
    con_dec(e);
    con_puts(e == -3 ? " no room\r\n" : e == -2 ? " too big for the FM-1 (or a bad size)\r\n" : e == -4 ? " checksum\r\n" : e == -6 ? " not a song image\r\n" :
             e == -5 ? " no flash\r\n" : e == -1 ? " out of memory\r\n" : "\r\n");
}

static void con_put_end(void)
{
    int err;
    uint32_t off = store_put_end(con.put_crc, 1, &err);
    con.put = 0;
    if (off) {
        con_puts("saved ");
        con_dec(store_find(off));
        con_puts("\r\n");
        ui_message("SONG RECEIVED");
    } else {
        con_err(err);
        ui_message("SONG NOT RECEIVED");
    }
    sys_audio_go();
}

static void con_put_line(const char *p)              /* a "d BASE64" line of a put */
{
    uint8_t b[160];
    int n;
    if (p[0] != 'd' || p[1] != ' ') {
        con.put = 0;
        store_scan();
        sys_audio_go();
        con_puts("err put aborted\r\n");
        return;
    }
    n = b64_decode(p + 2, b);
    if (n < 0 || con.put_n + (uint32_t)n > con.put_size) {
        con.put = 0;
        store_scan();
        sys_audio_go();
        con_puts("err bad data\r\n");
        return;
    }
    if (n && store_put_data(0, b, (uint32_t)n)) {
        con_err(put_w.err);
        con.put = 0;
        store_scan();
        sys_audio_go();
        return;
    }
    con.put_n += (uint32_t)n;
    if (con.put_n - con.put_ack >= 4096u && con.put_n < con.put_size) {
        con.put_ack = con.put_n;
        con_puts("ack ");
        con_dec((int32_t)con.put_n);
        con_puts("\r\n");
    }
    if (con.put_n == con.put_size)
        con_put_end();
}

static int con_songs(const char *p)                  /* 1: it was one of these commands */
{
    const char *r;
    int32_t v;
    if (con_cmd(p, "raw", &r)) {
        con.raw = 1;
        con_puts("ok\r\n");
    } else if (con_cmd(p, "term", &r)) {
        con.raw = 0;
        con_puts("ok\r\n");
    } else if (con_cmd(p, "info", &r)) {
        con_puts("fw " FELUCCA_VERSION "\r\n");
        con_kv("slots", store_n);
        con_kv("slots_max", STORE_MAX);
        con_kv("flash_free", (int32_t)(store_free_sectors() * STORE_SECT));
        con_kv("flash_total", (int32_t)((STORE_NSECT - 1u) * STORE_SECT));   /* (not the state sector) */
        con_kv("slot_room", (int32_t)store_room());   /* the biggest song a put takes now */
        con_kv("slot_max", (int32_t)(STATE_OFF - STORE_LO - SLOT_HDR));   /* ... with every other song deleted */
        con_kv("slot_loaded", ui.img_off ? store_find(ui.img_off) : -1);
        con_kv("builtin_loaded", ui.song);
        con_kv("changed", ui.changed);
        con_kv("playing", player_playing());
        con_puts("name ");
        con_puts(song.hdr.name);
        con_puts("\r\nok\r\n");
    } else if (con_cmd(p, "ls", &r)) {
        int i;
        for (i = 0; i < store_n; i++) {
            con_puts("slot ");
            con_dec(i);
            con_putc(' ');
            con_dec((int32_t)store_slots[i].size);
            con_putc(' ');
            con_dec((int32_t)store_slots[i].seq);
            con_putc(' ');
            con_puts(store_slots[i].name);
            con_puts("\r\n");
        }
        con_puts("ok\r\n");
    } else if (con_cmd(p, "get", &r)) {
        uint32_t i, n;
        const uint8_t *d;
        if (!con_slot(r, &v))
            return 1;
        d = store_image(v);
        n = store_slots[v].size;
        con_puts("data ");
        con_dec((int32_t)n);
        con_putc(' ');
        con_hex(((const slot_hdr *)stp_ptr(store_slots[v].off))->crc);
        con_puts("\r\n");
        for (i = 0; i < n && !con.stalled; i += 48u) {
            uint32_t k = n - i > 48u ? 48u : n - i, j;
            for (j = 0; j < k; j += 3u) {
                uint32_t b = (uint32_t)d[i + j] << 16 | (j + 1u < k ? (uint32_t)d[i + j + 1u] << 8 : 0u) |
                             (j + 2u < k ? d[i + j + 2u] : 0u);
                con_putc(B64[b >> 18]);
                con_putc(B64[(b >> 12) & 63u]);
                con_putc(j + 1u < k ? B64[(b >> 6) & 63u] : '=');
                con_putc(j + 2u < k ? B64[b & 63u] : '=');
            }
            con_puts("\r\n");
            fm1_wdt_feed();
        }
        con_puts("ok\r\n");
    } else if (con_cmd(p, "put", &r)) {
        int32_t size;
        uint32_t crc = 0;
        int e;
        const char *q;
        if (!con_arg(r, &size) || size <= 0) {
            con_puts("err put SIZE CRC NAME\r\n");
            return 1;
        }
        q = con_rest(r);
        while (*q && *q != ' ') {
            int h = *q >= '0' && *q <= '9' ? *q - '0' : *q >= 'a' && *q <= 'f' ? *q - 'a' + 10 :
                    *q >= 'A' && *q <= 'F' ? *q - 'A' + 10 : -1;
            if (h < 0)
                break;
            crc = crc << 4 | (uint32_t)h;
            q++;
        }
        while (*q == ' ')
            q++;
        ui_play(0);
        sys_audio_hold();          /* (the flash writes stop it anyway) */
        e = store_put_begin((uint32_t)size, q, slot_moved);
        if (e) {
            sys_audio_go();
            con_err(e);
            return 1;
        }
        con.put = 1;
        con.put_n = con.put_ack = 0;
        con.put_size = (uint32_t)size;
        con.put_crc = crc;
        ui_message("RECEIVING A SONG");
        con_puts("ready\r\n");
    } else if (con_cmd(p, "del", &r)) {
        if (!con_slot(r, &v))
            return 1;
        if (store_slots[v].off == ui.img_off) {    /* the song loaded: xmod-theme (built in) instead */
            int b, k;
            if (ui.changed && !str_eq(con_rest(r), "force")) {
                con_puts("err unsaved changes (del I force)\r\n");
                return 1;
            }
            for (b = 0, k = 0; k < NSONGS; k++)
                if (str_eq(SONGS[k].name, "xmod-theme"))
                    b = k;
            if (!NSONGS) {
                con_puts("err in use: load another song first\r\n");
                return 1;
            }
            ui_load(b);
            ui_message("SONG DELETED: BUILT-IN LOADED");
        }
        if (store_slots[v].off == ui.slot_off)
            ui.slot_off = 0;
        store_delete(v);
        con_puts("ok\r\n");
    } else if (con_cmd(p, "ren", &r)) {
        int e;
        uint32_t off;
        if (!con_slot(r, &v))
            return 1;
        ui_play(0);
        sys_audio_hold();
        off = store_rename(v, con_rest(r), slot_moved, &e);
        sys_audio_go();
        if (off) {
            if (ui.img_off == off) {            /* (the song loaded: its name too) */
                uint32_t i;
                const char *nm = store_slots[store_find(off)].name;
                for (i = 0; i < sizeof song.hdr.name; i++)
                    song.hdr.name[i] = i < 23u ? nm[i] : 0;
            }
            con_puts("ok ");
            con_dec(store_find(off));
            con_puts("\r\n");
        } else
            con_err(e);
    } else if (con_cmd(p, "load", &r)) {
        if (!con_slot(r, &v))
            return 1;
        if (ui.changed && !str_eq(con_rest(r), "force")) {
            con_puts("err unsaved changes (load I force)\r\n");
            return 1;
        }
        save_load(v);
        con_puts(ui.loaded_ok ? "ok\r\n" : "err bad song\r\n");
    } else {
        return 0;
    }
    return 1;
}

static void con_exec(const char *p)
{
    const char *r;
    int32_t v;
    const player_state *st = player_get_state();
    while (*p == ' ')
        p++;
    if (con_songs(p))
        return;
    if (con_cmd(p, "help", &r)) {
        con_puts("help stat songs play [N] stop mute N unmute N solo N unsolo N uboot reboot\r\n"
                 "press|hold|release BTN, turn ENC N, key N, scr (screen text), inp (buttons held)\r\n"
                 "song manager: raw term info ls get I put SIZE CRC NAME del I ren I NAME load I [force]\r\n");
    } else if (con_cmd(p, "stat", &r)) {
        con_kv("song", ui.song + 1);
        con_kv("playing", st->playing);
        con_kv("pos", st->song_pos);
        con_kv("pattern", st->pattern);
        con_kv("row", st->row);
        con_kv("speed", st->speed);
        con_kv("bpm", st->bpm);
        con_kv("channels", st->num_channels);
        con_kv("cpu_pct", (int32_t)(audio_cpu_q8 * 100u / 256u));
        con_kv("render_us_last", (int32_t)audio_us_last);
        con_kv("render_us_max", (int32_t)audio_us_max);
        con_kv("half_us", (int32_t)HALF_US);
        con_kv("halves", (int32_t)audio_halves);
        con_kv("late", (int32_t)audio_late);
        con_kv("clips", (int32_t)audio_clips);
        con_kv("overloads", (int32_t)audio_overloads);
        con_kv("master_q12", (int32_t)audio_q12);
        con_kv("ui_frames", (int32_t)ui.frames);
        con_kv("uptime_ms", (int32_t)fm1_ms);
        con_kv("lcd_timeouts", (int32_t)fm1_lcd_timeouts);
    } else if (con_cmd(p, "songs", &r)) {
        int i;
        for (i = 0; i < NSONGS; i++) {
            con_dec(i + 1);
            con_puts(i == ui.song ? " * " : "   ");
            con_puts(SONGS[i].name);
            con_puts("  ");
            con_dec((int32_t)SONGS[i].size);
            con_puts(" B\r\n");
        }
    } else if (con_cmd(p, "play", &r)) {
        if (con_arg(r, &v) && v >= 1 && v <= NSONGS)
            ui_load(v - 1);
        ui_play(1);
        con_puts("ok\r\n");
    } else if (con_cmd(p, "stop", &r)) {
        ui_play(0);
        con_puts("ok\r\n");
    } else if (con_cmd(p, "mute", &r) || con_cmd(p, "unmute", &r) || con_cmd(p, "solo", &r) ||
               con_cmd(p, "unsolo", &r)) {
        int on = p[0] != 'u';
        if (con_arg(r, &v) && v >= 1 && v <= FMT_MAX_CHANNELS) {
            if (p[0] == 's' || p[2] == 's')
                ui.solo[v - 1] = (uint8_t)on;
            else
                ui.muted[v - 1] = (uint8_t)on;
            ui_apply_mutes();
            con_puts("ok\r\n");
        } else
            con_puts("channel 1..32\r\n");
    } else if (con_cmd(p, "press", &r) || con_cmd(p, "hold", &r) || con_cmd(p, "release", &r)) {
        int id;
        while (*r == ' ')
            r++;
        id = btn_id(r, str_len(r));
        if (id < 0) {
            con_puts("button: OCT- OCT+ FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC\r\n");
            return;
        }
        if (p[0] == 'p') {                     /* pressed and released in one frame */
            inj.pressed |= 1u << id;
            inj.released |= 1u << id;
        } else if (p[0] == 'h') {
            inj.pressed |= 1u << id;
            inj_held |= 1u << id;
        } else {
            inj.released |= 1u << id;
            inj_held &= ~(1u << id);
        }
        con_puts("ok\r\n");
    } else if (con_cmd(p, "turn", &r)) {
        const char *e;
        int id, neg = 0;
        while (*r == ' ')
            r++;
        for (e = r; *e && *e != ' '; e++)
            ;
        id = enc_id(r, (uint32_t)(e - r));
        while (*e == ' ')
            e++;
        if (*e == '-') {
            neg = 1;
            e++;
        }
        if (id < 0 || !con_arg(e, &v)) {
            con_puts("turn SELECT|ALGO|K1|K2|K3|K4|PRESET N\r\n");
            return;
        }
        inj.enc[id] += neg ? -v : v;
        con_puts("ok\r\n");
    } else if (con_cmd(p, "key", &r)) {
        if (con_arg(r, &v) && v >= 0 && v <= 26) {
            inj.notes |= 1u << v;
            con_puts("ok\r\n");
        } else
            con_puts("key 0..26\r\n");
    } else if (con_cmd(p, "scr", &r)) {         /* the screen's text */
        uint32_t i, k, n = ui.page == PG_GLO ? G_NLINES : nl;
        con_kv("page", ui.page);
        for (i = 0; i < n; i++) {
            const gline *l = ui.page == PG_GLO ? &glines[i] : &L[i];
            uint32_t cols = gl_cols(l), end = cols;
            if (l->y == GL_UNUSED)
                continue;
            while (end && l->t[end - 1u] == ' ')
                end--;
            con_dec(l->y);
            con_puts(l->y < 10 ? "  |" : l->y < 100 ? " |" : "|");
            for (k = 0; k < end; k++)
                con_putc(l->t[k]);
            con_puts("\r\n");
        }
    } else if (con_cmd(p, "inp", &r)) {
        con_kv("buttons", (int32_t)fm1_in.buttons);
        con_kv("notes", (int32_t)fm1_in.notes);
    } else if (con_cmd(p, "uboot", &r)) {
        usb.uboot_req = 1;
    } else if (con_cmd(p, "reboot", &r)) {
        bootguard.pending = 0;
        usb_detach();
        fm1_delay_ms(30);
        fm1_reboot();
    } else if (*p) {
        con_puts("? (help)\r\n");
    }
}

static void cdc_task(void)
{
    if (cdc.dtr && !con.dtr_seen) {
        con.raw = 0;
        con_puts("\r\n" FELUCCA_VERSION " console - 'help'\r\n> ");
    }
    if (!cdc.dtr && con.dtr_seen && con.put) {   /* the port closed half way through a put */
        con.put = 0;
        store_scan();
        sys_audio_go();
    }
    con.dtr_seen = cdc.dtr;
    while (ci_r != ci_w) {
        char c;
        RING_PUBLISH();
        c = (char)cdc_in[ci_r % CI_N];
        RING_PUBLISH();
        ci_r++;
        if (c == '\r' || c == '\n') {
            if (con.len == 0 && (c == '\n' || con.raw))
                continue;
            if (!con.raw)
                con_puts("\r\n");
            con.line[con.len] = 0;
            con.stalled = 0;
            if (con.put)
                con_put_line(con.line);
            else
                con_exec(con.line);
            con.len = 0;
            if (!con.raw)
                con_puts("> ");
        } else if (c == 0x7F || c == 0x08) {
            if (con.len) {
                con.len--;
                if (!con.raw)
                    con_puts("\b \b");
            }
        } else if (c >= ' ' && c < 0x7F && con.len + 1u < CON_LINE) {
            con.line[con.len++] = c;
            if (!con.raw)
                con_putc(c);
        }
    }
}
