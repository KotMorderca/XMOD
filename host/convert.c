/* SPDX-License-Identifier: GPL-3.0-only */
/* Song files between formats, by their extensions:
 *
 *   convert [--8bit] IN.{xm,mod,fmtk} OUT.{fmtk,xm,mod}
 *
 * --8bit: 16-bit samples stored as 8-bit (about half the size, for the FM-1's flash).
 *
 * (the web song manager does the same in the browser, from the same C) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modconv.h"
#include "modwrite.h"

static const char *ext(const char *p)
{
    const char *d = strrchr(p, '.');
    return d ? d + 1 : "";
}

int main(int argc, char **argv)
{
    FILE *f;
    long len;
    uint8_t *file, *img, *out;
    size_t n, img_n, out_n;
    char err[256], warn[256] = "";
    const char *e;
    int to8 = argc > 1 && !strcmp(argv[1], "--8bit");
    if (to8) {
        argv++;
        argc--;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: convert [--8bit] IN.{xm,mod,fmtk} OUT.{fmtk,xm,mod}\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "%s: cannot read\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc((size_t)len);
    n = fread(file, 1, (size_t)len, f);
    fclose(f);
    if (n >= 4 && !memcmp(file, "FMTK", 4)) {
        img = file;
        img_n = n;
    } else if ((to8 ? modconv_8bit : modconv)(file, n, &img, &img_n, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", argv[1], err);
        return 1;
    }
    e = ext(argv[2]);
    if (!strcmp(e, "fmtk")) {
        out = img;
        out_n = img_n;
    } else if (!strcmp(e, "xm") ? fmtk_to_xm(img, img_n, &out, &out_n, err, sizeof err) :
               !strcmp(e, "mod") ? fmtk_to_mod(img, img_n, &out, &out_n, warn, sizeof warn, err, sizeof err) :
               (snprintf(err, sizeof err, "OUT: .fmtk, .xm or .mod"), -1)) {
        fprintf(stderr, "%s: %s\n", argv[2], err);
        return 1;
    }
    if (warn[0])
        fprintf(stderr, "%s\n", warn);
    f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 1, out_n, f) != out_n) {
        fprintf(stderr, "%s: cannot write\n", argv[2]);
        return 1;
    }
    fclose(f);
    return 0;
}
