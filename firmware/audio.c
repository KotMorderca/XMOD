/* SPDX-License-Identifier: GPL-3.0-only */
/* I2S output: the ALNK0 half-buffer interrupt renders HALF_FRAMES stereo frames with the player
 * (engine/player.c), scales them by MASTER and hands them to the codec as 24-bit samples.
 * The DAC runs at 24 MHz / 544 = 44117.6 Hz (Felucca's audio.c): the player is told that rate. */
#define DAC_RATE_NUM 24000000u
#define DAC_RATE_DEN 544u
#define OUT_SHIFT 7                 /* 16-bit range -> 24-bit, -6 dBFS ceiling (as Felucca) */

static volatile uint32_t audio_q12 = 2048;      /* MASTER, 4096 = unity (main loop) */
static volatile uint8_t audio_run;              /* 0: silence, the player untouched (song changes) */
static volatile uint8_t audio_in_isr;           /* TIMER5 nests in the render (main.c) */
static volatile uint32_t audio_halves, audio_late;
static volatile uint32_t audio_us_last, audio_us_max;   /* render time of a half, us */
static volatile uint32_t audio_cpu_q8;          /* load, 256 = 100 %, ~0.1 s average */
static volatile int32_t audio_peak_l, audio_peak_r;    /* output peaks since the main loop last looked */
static volatile uint32_t audio_clips;
static volatile uint32_t t5_nested_ticks;
static uint8_t audio_over;                      /* bit k: the half k halves ago was over 90 % */
static volatile uint32_t audio_overloads;
#define HALF_US ((HALF_FRAMES * DAC_RATE_DEN) / (DAC_RATE_NUM / 1000000u))   /* ~5802 us */

static int32_t mixbuf[2u * HALF_FRAMES];

/* MIDI clock out on USB (after each render): Start when playback starts, a clock (F8) per replayer
 * tick (24 per quarter note at the song's BPM), Stop when it stops. Up to a half buffer (5.8 ms) of
 * jitter: the ticks of a half go out together. */
static uint32_t clk_ticks;
static uint8_t clk_playing;
#define MIDI_RT(b) (0x0Fu | (uint32_t)(b) << 8)  /* a one-byte USB-MIDI packet (CIN F) */

static void midi_clock_out(void)
{
    uint8_t pl = (uint8_t)player_playing();
    uint32_t t = player_ticks(), n;
    if (pl && !clk_playing) {
        midi_out_event(MIDI_RT(0xFA));
        clk_ticks = 0;
    } else if (!pl && clk_playing) {
        midi_out_event(MIDI_RT(0xFC));
    }
    clk_playing = pl;
    if (!pl)
        return;
    n = t - clk_ticks;
    clk_ticks = t;
    if (n > 16u)
        n = 16u;
    while (n--)
        midi_out_event(MIDI_RT(0xF8));
}

void fm1_alnk0_irq(void)                        /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p;
    uint32_t t0;
    audio_in_isr = 1;
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    t5_nested_ticks = 0;
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), i, us, g = audio_q12;
        int32_t *o = &abuf[half * HALF_WORDS];
        int32_t pl = audio_peak_l, pr = audio_peak_r;
#if FELUCCA_UAC
        uac_render_start();                     /* (Felucca's usb.c: the USB audio input's ring) */
#endif
        if (audio_run) {
            player_render(mixbuf, HALF_FRAMES);
            midi_clock_out();
        } else
            for (i = 0; i < 2u * HALF_FRAMES; i++)
                mixbuf[i] = 0;
        for (i = 0; i < 2u * HALF_FRAMES; i += 2u) {
            int32_t l = (mixbuf[i] * (int32_t)g) >> 12, r = (mixbuf[i + 1u] * (int32_t)g) >> 12;
            if (l > 32767) { l = 32767; audio_clips++; }
            if (l < -32768) { l = -32768; audio_clips++; }
            if (r > 32767) { r = 32767; audio_clips++; }
            if (r < -32768) { r = -32768; audio_clips++; }
            if (l > pl) pl = l; else if (-l > pl) pl = -l;
            if (r > pr) pr = r; else if (-r > pr) pr = -r;
            o[i] = l * (1 << OUT_SHIFT);
            o[i + 1u] = r * (1 << OUT_SHIFT);
            /* USB audio: the mix at full level, whatever MASTER (the speaker, the headphones) is at */
            mixbuf[i] = mixbuf[i] > 32767 ? 32767 : mixbuf[i] < -32768 ? -32768 : mixbuf[i];
            mixbuf[i + 1u] = mixbuf[i + 1u] > 32767 ? 32767 : mixbuf[i + 1u] < -32768 ? -32768 : mixbuf[i + 1u];
        }
#if FELUCCA_UAC
        uac_tap(mixbuf, HALF_FRAMES);
#endif
        audio_peak_l = pl;
        audio_peak_r = pr;
        fm1_audio_ack_half();
        audio_halves++;
        us = (fm1_ticks() - t0 - t5_nested_ticks) / FM1_TICKS_PER_US;
        audio_us_last = us;
        if (us > audio_us_max)
            audio_us_max = us;
        audio_cpu_q8 = (audio_cpu_q8 * 15u + (us * 256u) / HALF_US) / 16u;
        /* overload guard: two halves in a row over 90 % stop the player, so the main loop (the
         * watchdog, USB, the update entry) always gets its time */
        audio_over = (uint8_t)(audio_over << 1 | (us > HALF_US * 9u / 10u));
        if ((audio_over & 3u) == 3u && audio_run) {
            audio_run = 0;
            audio_overloads++;
        }
        if (fm1_audio_free_half() != half)
            audio_late++;                       /* the DMA moved on while we rendered */
    }
    audio_in_isr = 0;
}
extern void isr_alnk0(void);

static void audio_init(void)
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}

/* MASTER pot -> gain: squared, full turn = unity */
static void audio_master_poll(void)
{
    static int32_t knob = -1;
    int32_t a = fm1_adc_read(FM1_ADC_MASTER);
    if (a < 0)
        return;
    knob = knob < 0 ? a * 16 : knob + (a * 16 - knob) / 8;
    {
        uint32_t k = (uint32_t)knob / 16u;      /* 0..1023 */
        audio_q12 = (k * k) >> 8;               /* 0..4088 */
    }
}
