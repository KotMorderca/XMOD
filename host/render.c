/* SPDX-License-Identifier: GPL-3.0-only */
/* Render a song to a WAV file with the engine (the same code the FM-1 runs).
 *
 *   render SONG.xm|.mod|.fmtk OUT.wav [--seconds S] [--frames N] [--rate NUM[/DEN]]
 *          [--amp A] [--image OUT.fmtk] [--trace] [--bench]
 *
 * --rate 24000000/544 is the FM-1's output rate. --trace prints every row as it plays.
 * --bench prints how long the rendering took (per second of audio). */
#include "../engine/player.h"
#include "../engine/song.h"
#include "modconv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long sz;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)sz + 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void wr32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); fputc(v >> 24, f); }
static void wr16(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }

static const char *note_name(uint8_t n, char *b)
{
    static const char *nm = "C-C#D-D#E-F-F#G-G#A-A#B-";
    if (n == 0)
        return "...";
    if (n == 97)
        return "===";
    n--;
    b[0] = nm[(n % 12) * 2];
    b[1] = nm[(n % 12) * 2 + 1];
    b[2] = (char)('0' + n / 12);
    b[3] = 0;
    return b;
}

typedef struct { uint8_t *b; uint32_t n; } sink_t;
static int sink(void *ctx, const void *d, uint32_t n)
{
    sink_t *s = ctx;
    if (d)
        memcpy(s->b + s->n, d, n);
    else
        memset(s->b + s->n, 0, n);
    s->n += n;
    return 0;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *image_out = NULL;
    double seconds = 60.0;
    long frames_req = -1;
    uint32_t rate_num = 44100, rate_den = 1, rate;
    int amp = 4, trace = 0, bench = 0, resave = 0, ft2 = 0, sep = 20, i;
    size_t n, img_n;
    uint8_t *file, *img;
    char err[256];
    FILE *f;
    int32_t buf[2 * 1024];
    long total, done = 0;
    clock_t t0;
    int last_row = -1, last_pos = -1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames_req = atol(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
            char *slash;
            rate_num = (uint32_t)strtoul(argv[++i], &slash, 10);
            rate_den = *slash == '/' ? (uint32_t)strtoul(slash + 1, NULL, 10) : 1;
        } else if (!strcmp(argv[i], "--amp") && i + 1 < argc)
            amp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--image") && i + 1 < argc)
            image_out = argv[++i];
        else if (!strcmp(argv[i], "--trace"))
            trace = 1;
        else if (!strcmp(argv[i], "--bench"))
            bench = 1;
        else if (!strcmp(argv[i], "--resave"))
            resave = 1;
        else if (!strcmp(argv[i], "--ft2"))
            ft2 = 1;
        else if (!strcmp(argv[i], "--sep") && i + 1 < argc)
            sep = atoi(argv[++i]);
        else if (!in)
            in = argv[i];
        else if (!out)
            out = argv[i];
        else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!in || !out) {
        fprintf(stderr, "usage: render SONG OUT.wav [--seconds S] [--frames N] [--rate NUM[/DEN]] "
                        "[--amp A] [--image OUT.fmtk] [--trace] [--bench] [--resave] [--ft2] [--sep PERCENT]\n");
        return 2;
    }
    file = read_file(in, &n);
    if (!file) {
        fprintf(stderr, "%s: cannot read\n", in);
        return 1;
    }
    if (n >= 4 && !memcmp(file, "FMTK", 4)) {
        img = file;
        img_n = n;
    } else if (modconv(file, n, &img, &img_n, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", in, err);
        return 1;
    }
    if (image_out) {
        FILE *o = fopen(image_out, "wb");
        if (!o || fwrite(img, 1, img_n, o) != img_n) {
            fprintf(stderr, "%s: cannot write\n", image_out);
            return 1;
        }
        fclose(o);
    }
    player_init(rate_num, rate_den);
    player_set_amp(amp);
    player_set_pt_mode(!ft2);           /* (a .mod: ProTracker mode unless --ft2) */
    player_set_stereo_sep(sep);
    if (player_load(img, (uint32_t)img_n)) {
        fprintf(stderr, "%s: bad image\n", in);
        return 1;
    }
    if (resave) {                    /* play what song_save makes of it (it must sound the same) */
        uint32_t sz;
        sink_t s = {0, 0};
        if (song_save(NULL, NULL, &sz) || !(s.b = malloc(sz)) || song_save(sink, &s, NULL) || s.n != sz ||
            player_load(s.b, sz)) {
            fprintf(stderr, "%s: resave failed\n", in);
            return 1;
        }
    }
    {
        const fmt_header *h = player_song();
        fprintf(stderr, "%s: \"%s\" (%s) %d ch, %d orders, %d patterns, %d instruments, %d samples, image %zu B\n",
                in, h->name, h->tracker, h->num_channels, h->song_length, h->num_patterns, h->num_instr,
                h->num_samples, img_n);
    }
    rate = (uint32_t)((rate_num + rate_den / 2) / rate_den);
    total = frames_req >= 0 ? frames_req : (long)(seconds * rate_num / rate_den);
    f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "%s: cannot write\n", out);
        return 1;
    }
    fwrite("RIFF", 1, 4, f);
    wr32(f, 36 + (uint32_t)total * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, 16);
    wr16(f, 1);
    wr16(f, 2);
    wr32(f, rate);
    wr32(f, rate * 4);
    wr16(f, 4);
    wr16(f, 16);
    fwrite("data", 1, 4, f);
    wr32(f, (uint32_t)total * 4);
    player_play(0);
    t0 = clock();
    while (done < total) {
        long k = total - done > 1024 ? 1024 : total - done;
        player_render(buf, (uint32_t)k);
        for (i = 0; i < 2 * k; i++) {
            int32_t v = buf[i];
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            wr16(f, (uint32_t)(uint16_t)(int16_t)v);
        }
        done += k;
        if (trace) {
            const player_state *s = player_get_state();
            if (s->row != last_row || s->song_pos != last_pos) {
                player_cell c[FMT_MAX_CHANNELS];
                int ch, nc = player_get_row(s->pattern, s->row, c, FMT_MAX_CHANNELS);
                char nb[4];
                last_row = s->row;
                last_pos = s->song_pos;
                printf("%7.2f %02X/%02X:%02X sp%02d bpm%3d |", (double)done * rate_den / rate_num,
                       s->song_pos, s->pattern, s->row, s->speed, s->bpm);
                for (ch = 0; ch < nc && ch < 8; ch++)
                    printf(" %s%02X%02X%X%02X", note_name(c[ch].note, nb), c[ch].instr, c[ch].vol,
                           c[ch].efx, c[ch].efx_data);
                printf("\n");
            }
        }
    }
    if (bench) {
        double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
        fprintf(stderr, "render: %.3f s CPU for %.1f s audio (%.2f%%)\n", cpu, (double)total / rate,
                100.0 * cpu * rate / (double)total);
    }
    fclose(f);
    return 0;
}
