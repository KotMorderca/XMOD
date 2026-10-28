/* SPDX-License-Identifier: GPL-3.0-only */
/* The song converters for the web song manager (web/), built to WebAssembly (host/Makefile wasm):
 * the same C as on the PC. JS calls:
 *   p = buf_alloc(n), copy the file in, convert(kind, p, n) -> 0 / -1, buf_free(p);
 *   result_ptr() / result_len(): the output (valid until the next convert), message_ptr(): the error
 *   or the warning (a NUL-terminated string).
 * kind: 0 .xm/.mod -> FMTK, 1 FMTK -> .xm, 2 FMTK -> .mod, 3 .xm/.mod -> FMTK with 8-bit samples. */
#include <stdlib.h>
#include <string.h>
#include "modconv.h"
#include "modwrite.h"

#define EXPORT(name) __attribute__((export_name(name)))

static uint8_t *res;
static size_t res_n;
static char msg[256];

EXPORT("buf_alloc") void *buf_alloc(size_t n) { return malloc(n ? n : 1); }
EXPORT("buf_free") void buf_free(void *p) { free(p); }
EXPORT("result_ptr") const uint8_t *result_ptr(void) { return res; }
EXPORT("result_len") size_t result_len(void) { return res_n; }
EXPORT("message_ptr") const char *message_ptr(void) { return msg; }

EXPORT("convert") int convert(int kind, const uint8_t *in, size_t n)
{
    char err[256];
    int r;
    free(res);
    res = NULL;
    res_n = 0;
    msg[0] = err[0] = 0;
    if (kind == 0)
        r = modconv(in, n, &res, &res_n, err, sizeof err);
    else if (kind == 1)
        r = fmtk_to_xm(in, n, &res, &res_n, err, sizeof err);
    else if (kind == 3)
        r = modconv_8bit(in, n, &res, &res_n, err, sizeof err);
    else if (kind == 2)
        r = fmtk_to_mod(in, n, &res, &res_n, msg, sizeof msg, err, sizeof err);
    else {
        strcpy(err, "unknown conversion");
        r = -1;
    }
    if (r)
        strcpy(msg, err);
    return r;
}
