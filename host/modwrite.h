/* SPDX-License-Identifier: GPL-3.0-only */
/* FMTK song image -> .xm / .mod (host/modwrite.c). */
#ifndef MODWRITE_H
#define MODWRITE_H
#include <stddef.h>
#include <stdint.h>

/* 0 and a malloc'ed file in *out, or -1 with a message in err. warn: what a .mod could not take. */
int fmtk_to_xm(const uint8_t *img, size_t size, uint8_t **out, size_t *out_n, char *err, size_t en);
int fmtk_to_mod(const uint8_t *img, size_t size, uint8_t **out, size_t *out_n, char *warn, size_t wn,
                char *err, size_t en);

#endif
