/* SPDX-License-Identifier: GPL-3.0-only */
/* The few things Felucca's usb.c, storage_hw.c and ota.c take from Felucca's core.h. */
#pragma once

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* slot store before the index update */

/* the boot-loop guard: a crash or hang in the first 30 s counts; two in a row -> UBOOT (main.c) */
#define BOOTGUARD_MAGIC 0x42475244u
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));

/* the I2S double buffer (audio.c); storage_hw.c zeroes it around flash erases */
#define HALF_FRAMES 256u
#define HALF_WORDS (HALF_FRAMES * 2u)
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

static void ed_service(void) {}   /* (usb.c calls the web editor's SysEx hook; there is no editor) */

static volatile uint32_t usb_polls;   /* usb_poll calls (main.c TIMER5), for the mixer page */
