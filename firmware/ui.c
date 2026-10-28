/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1 side of the pages (pages.c): input, LEDs, the audio held around song changes, and the
 * mixer's (pages_glo.c) CPU and USB lines. */

static void sys_audio_hold(void)
{
    audio_run = 0;                 /* the ISR stops calling the player before we touch it */
    __asm__ volatile("" ::: "memory");
    fm1_delay_ms(7);               /* (a half buffer in flight finishes) */
}

static void sys_audio_go(void) { audio_run = 1; }
static uint32_t sys_lock(void) { return irq_save(); }
static void sys_unlock(uint32_t f) { irq_restore(f); }
static void sys_led(uint32_t id, int on) { fm1_led_key(id, on); }

/* the song slots (store.c) on Felucca's flash driver (storage_hw.c) */
static int stp_ok(void) { return flash_ok; }
static int stp_read(uint32_t off, void *d, uint32_t n) { return st_read(off, d, n); }
static int stp_erase(uint32_t off) { return st_erase(off); }
static int stp_prog(uint32_t off, const void *s, uint32_t n) { return st_prog(off, s, n); }   /* (s in RAM) */
static const uint8_t *stp_ptr(uint32_t off) { return (const uint8_t *)FL_XIP(off); }
static void stp_idle(void) { fm1_wdt_feed(); }

#define UI_POOL __attribute__((section(".pool")))   /* the undo log, the clipboard: the big RAM */
#define STORE_SECTION UI_POOL
#include "pages.c"

/* ------------------------------------------------------------- the GLO page's stats --- */
/* the mixer's two lines of the FM-1: the audio's CPU load and the USB state (reset: the page opens) */
static uint32_t cpu_max_q8;

static void glo_stats(gline *cpu_line, gline *usb_line, int reset)
{
    gline *l;
    uint32_t x;
    if (reset) {
        cpu_max_q8 = 0;
        audio_us_max = 0;
        return;
    }
    {
        static uint32_t seen;
        if (audio_overloads != seen) {
            seen = audio_overloads;
            ui_message("CPU OVERLOAD: STOPPED");
        }
    }

    l = cpu_line;
    gl_clear(l, ATTR(C_TEXT, C_BG));
    {
        uint32_t cpu = audio_cpu_q8;
        if (cpu > cpu_max_q8)
            cpu_max_q8 = cpu;
        x = gl_puts(l, 1, "CPU ", ATTR(C_DIM, C_BG));
        x = gl_dec(l, x, cpu * 100u / 256u, 3, cpu > 200u ? ATTR(C_WARN, C_BG) : ATTR(C_TEXT, C_BG));
        x = gl_puts(l, x, "% MAX ", ATTR(C_DIM, C_BG));
        x = gl_dec(l, x, cpu_max_q8 * 100u / 256u, 3, ATTR(C_TEXT, C_BG));
        x = gl_puts(l, x, "% ", ATTR(C_DIM, C_BG));
        x = gl_dec(l, x, audio_us_max, 4, ATTR(C_TEXT, C_BG));
        x = gl_puts(l, x, "us LATE ", ATTR(C_DIM, C_BG));
        gl_dec(l, x, audio_late, 1, audio_late ? ATTR(C_WARN, C_BG) : ATTR(C_TEXT, C_BG));
    }

    /* USB: up / configured, bus resets, MIDI packets in / out, serial port open, MIDI events in */
    l = usb_line;
    gl_clear(l, ATTR(C_DIM, C_BG));
    x = gl_puts(l, 1, "USB ", ATTR(C_DIM, C_BG));
    x = gl_puts(l, x, usb.up ? (usb.config ? "OK " : "UP ") : "-- ", usb.config ? ATTR(C_OK, C_BG) : ATTR(C_WARN, C_BG));
    {   /* P polls per second (should be ~2000), Q setup packets, T SIE timeouts, A re-attaches */
        static uint32_t last_polls, last_ms, rate;
        if (fm1_ms - last_ms >= 1000u) {
            rate = usb_polls - last_polls;
            last_polls = usb_polls;
            last_ms = fm1_ms;
        }
        x = gl_puts(l, x, "P", ATTR(C_DIM, C_BG));
        x = gl_dec(l, x, rate, 1, rate < 1800u ? ATTR(C_WARN, C_BG) : ATTR(C_TEXT, C_BG));
    }
    x = gl_puts(l, x, " Q", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, usb.setups, 1, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, " T", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, usb.timeouts, 1, usb.timeouts ? ATTR(C_WARN, C_BG) : ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, " A", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, usb.retries, 1, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, " R", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, usb.resets, 1, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, " I", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, usb.rx_pkts, 1, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, " O", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, usb.tx_pkts, 1, ATTR(C_TEXT, C_BG));
    x = gl_puts(l, x, " S", ATTR(C_DIM, C_BG));
    x = gl_dec(l, x, cdc.dtr, 1, ATTR(C_TEXT, C_BG));
    gl_puts(l, x, usb.suspended ? " Z" : "", ATTR(C_WARN, C_BG));

}

/* ------------------------------------------------------------------- the frame --- */
static void ui_init(void)
{
    gfx_init();
    pages_init();
}

/* input from the console (tests): added to the next frame's */
static ui_in inj;
static uint32_t inj_held;

static void ui_frame(void)
{
    ui_in in;
    uint32_t k;
    in.pressed = fm1_input_edges(&in.released) | inj.pressed;
    in.released |= inj.released;
    in.held = fm1_in.buttons | inj_held | inj.pressed;
    in.notes = fm1_input_note_edges() | inj.notes;
    in.notes_held = fm1_in.notes | inj.notes;
    for (k = 0; k < NENC; k++)
        in.enc[k] = fm1_enc_take(k) + inj.enc[k];
    inj.pressed = inj.released = inj.notes = 0;
    for (k = 0; k < NENC; k++)
        inj.enc[k] = 0;
    pages_frame(&in);
}

/* a full-screen notice (update, UBOOT) */
static void ui_notice(const char *a, const char *b)
{
    gline l;
    lcd_fill(0, 0, 240, 240, PALETTE[C_BG]);
    gl_init(&l, 0, 100);
    gl_clear(&l, ATTR(C_HI, C_BG));
    gl_puts(&l, (40u - str_len(a)) / 2u, a, ATTR(C_HI, C_BG));
    gl_draw(&l);
    if (b) {
        gl_init(&l, 0, 120);
        gl_clear(&l, ATTR(C_TEXT, C_BG));
        gl_puts(&l, (40u - str_len(b)) / 2u, b, ATTR(C_TEXT, C_BG));
        gl_draw(&l);
    }
    lcd_sync();
}

static void ui_redraw_all(void) { ui.redraw = 1; }
