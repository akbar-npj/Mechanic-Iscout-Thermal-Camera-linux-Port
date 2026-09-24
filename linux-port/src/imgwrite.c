/*
 * imgwrite.c — minimal PPM (P6) reader/writer.
 *
 * build:  cc -O2 -g -Wall -Wextra -I. -c imgwrite.c -o imgwrite.o
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgwrite.h"

int dyt_write_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f;

    if (!path || !rgb || w <= 0 || h <= 0)
        return -1;

    f = fopen(path, "wb");
    if (!f)
        return -1;

    if (fprintf(f, "P6\n%d %d\n255\n", w, h) < 0) { fclose(f); return -1; }
    if (fwrite(rgb, 3, (size_t)w * (size_t)h, f) != (size_t)w * (size_t)h) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Read one whitespace-delimited header token, skipping '#' comments.  Consumes
 * exactly the single whitespace character that terminates the token, so the
 * binary raster after the maxval is left untouched (PPM spec). */
static int ppm_token(FILE *f, char *buf, size_t n)
{
    size_t i = 0;
    int c;

    for (;;) {
        c = fgetc(f);
        if (c == EOF) return -1;
        if (c == '#') {
            do { c = fgetc(f); } while (c != EOF && c != '\n');
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        break;
    }

    while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        if (i + 1 < n) buf[i++] = (char)c;
        c = fgetc(f);
    }
    buf[i] = '\0';
    return 0;
}

int dyt_read_ppm(const char *path, uint8_t **rgb, int *w, int *h)
{
    FILE *f;
    char tok[32];
    int ww, hh, maxv;
    size_t n;
    uint8_t *buf;

    if (!path || !rgb || !w || !h)
        return -1;

    f = fopen(path, "rb");
    if (!f)
        return -1;

    if (ppm_token(f, tok, sizeof tok) != 0 || strcmp(tok, "P6") != 0)
        goto fail;
    if (ppm_token(f, tok, sizeof tok) != 0) goto fail;
    ww = atoi(tok);
    if (ppm_token(f, tok, sizeof tok) != 0) goto fail;
    hh = atoi(tok);
    if (ppm_token(f, tok, sizeof tok) != 0) goto fail;
    maxv = atoi(tok);

    if (ww <= 0 || hh <= 0 || maxv != 255)
        goto fail;

    n = (size_t)ww * (size_t)hh * 3;
    buf = malloc(n);
    if (!buf)
        goto fail;
    if (fread(buf, 1, n, f) != n) {
        free(buf);
        goto fail;
    }
    fclose(f);

    *rgb = buf;
    *w = ww;
    *h = hh;
    return 0;

fail:
    fclose(f);
    return -1;
}
