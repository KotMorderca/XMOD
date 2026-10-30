/* SPDX-License-Identifier: GPL-3.0-only */
/* Boot, the 10 kHz system tick and the main loop. The boot order, the boot-loop guard, the
 * fault screen and the ways back to the update mode follow Felucca's main.c:
 *   - a crash or watchdog reset in the first 30 s twice in a row: UBOOT (the chip's update mode);
 *   - OCT- + OCT+ held 5 s: UBOOT;
 *   - SysEx F0 22 24 35 7D F7 or the console's `uboot`: UBOOT;
 *   - the M-UPGRADE update (Felucca's installer, the official updater): ota.c. */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];

void fm1_timer5_irq(void)                       /* via isr_timer5, 10 kHz */
{
    static uint32_t sub, owed;
    uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
    fm1_timer5_ack();
    fm1_input_tick();
    {
        static uint32_t last, acc;
        acc += t0 - last;
        last = t0;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
    }
    if (usb_due)
        owed |= 1u;                             /* 2 kHz: all USB traffic here */
    if (++sub == 10u)
        sub = 0;
    if (audio_in_isr) {                         /* nested in the render: scan only */
#if FELUCCA_UAC
        if (usb_due)
            uac_service();                      /* (the USB audio packets cannot wait for the render) */
#endif
        t5_nested_ticks += fm1_ticks() - t0;
        return;
    }
    if (owed & 1u) {
        usb_poll();
        usb_polls++;
    }
    owed = 0;
}
extern void isr_timer5(void);

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char a[24], b[24];
    uint32_t t0;
    fm1_audio_stop();
    fm1_lcd_backlight(1);
    str_cpy(a, "CRASH  VEC ", sizeof a);
    hexs(a + 11, c->vec);
    str_cpy(b, "PC ", sizeof b);
    hexs(b + 3, c->pc);
    ui_notice(a, b);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

/* ota.c's hooks (Felucca's ota_hw.c with our screen) */
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    audio_silence();
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    char b[24];
    if (step < 9u) {
        ui_notice("UPDATE", STEP[step < 6u ? step : 0]);
        return;
    }
    {
        uint32_t k = 0, v = (uint32_t)-code;
        str_cpy(b, "FAILED  -", sizeof b);
        while (b[k])
            k++;
        if (v >= 10u)
            b[k++] = (char)('0' + v / 10u);
        b[k++] = (char)('0' + v % 10u);
        b[k] = 0;
    }
    ui_notice("UPDATE", b);
    fm1_delay_ms(1500);
}
static void ota_commit(const uint8_t *parm)
{
    bootguard.pending = 0;
    state_put(0u, 0u);             /* an install: XMOD's next start (this build again too) is its first (pages.c) */
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}

/* MIDI in (USB): transport and notes go to pages.c ui_midi; the ring must not fill: Felucca's usb.c holds the
 * whole EP1 OUT endpoint (NAK) while it is nearly full, and the updater's SysEx comes that way */
static uint32_t midi_in_count;
static void midi_drain(void)
{
    while (mi_r != mi_w) {
        uint32_t pkt;
        RING_PUBLISH();
        pkt = midi_in_q[mi_r % MQ];
        RING_PUBLISH();
        mi_r++;
        midi_in_count++;
        ui_midi(pkt);                           /* (pages.c: transport, notes) */
    }
    midi_in_overflow = 0;
}

static void enter_uboot(const char *why)
{
    fm1_audio_stop();
    ui_notice("UBOOT", why);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;
    fm1_enter_uboot();
}

static void fm1_main(void)
{
    uint32_t f;
    f = irq_save();
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
    irq_restore(f);
    if (flash_ok) {
        xmod_plain_window_init();          /* (xmod.c: from the app area's tail, layout.h) */
        ota_boot_cleanup();                     /* staging area left by an update */
    }
    lcd_init();
    gfx_init();
    splash_show();                              /* (splash.c: stays 3 s, below) */
    fm1_input_init();
    fm1_adc_init();
    (void)fm1_adc_read(FM1_ADC_MASTER);
    audio_master_poll();
    player_init(DAC_RATE_NUM, DAC_RATE_DEN);
    audio_init();
    usb_start();
    fm1_timer5_start(isr_timer5, 4);            /* above ALNK0 (3): the scan nests into the render */
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    {   /* the splash: 3 s on the screen (USB comes up meanwhile) */
        uint32_t t0 = fm1_ms;
        while (fm1_ms - t0 < 3000u)
            fm1_wdt_feed();
    }
    ui_init();

    for (;;) {
        uint32_t m = fm1_ms;
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        audio_master_poll();
        {   /* OCT- + OCT+ held 5 s: UBOOT */
            static uint32_t t0;
            static uint8_t shown;
            uint32_t both = (1u << BTN_OCTDN) | (1u << BTN_OCTUP);
            if ((fm1_in.buttons & both) != both) {
                t0 = fm1_ms;
                if (shown) {
                    shown = 0;
                    ui_redraw_all();
                }
            } else if (fm1_ms - t0 > 2000u && !shown) {
                shown = 1;
                ui_notice("HOLD FOR UBOOT", "release to cancel");
            } else if (fm1_ms - t0 > 5000u) {
                enter_uboot("update mode");
            }
            if (shown) {
                cdc_task();
                continue;
            }
        }
        midi_drain();
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0;
            audio_run = 0;
            if (flash_ok)
                ota_session();                  /* returns only if nothing was committed */
            ui_redraw_all();
            audio_run = 1;
        }
        if (usb.uboot_req)
            enter_uboot("USB request");
        cdc_task();
        ui_frame();
        while (fm1_ms - m < 20u) {              /* ~50 frames/s at most */
            midi_drain();
            ota_service();
            cdc_task();
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 2u) {
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
