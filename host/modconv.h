/* SPDX-License-Identifier: GPL-3.0-only */
/* .xm / .mod -> FMTK song image (engine/fmt.h). */
#ifndef MODCONV_H
#define MODCONV_H
#include <stddef.h>
#include <stdint.h>

/* Returns 0 and a malloc'ed image in *out (free it), or -1 with a message in err. */
int modconv(const uint8_t *file, size_t size, uint8_t **out, size_t *out_size, char *err, size_t err_size);
/* the same with 16-bit samples stored as 8-bit (about half the size: for the FM-1's flash) */
int modconv_8bit(const uint8_t *file, size_t size, uint8_t **out, size_t *out_size, char *err, size_t err_size);

#endif
