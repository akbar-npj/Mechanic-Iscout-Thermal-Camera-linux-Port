/*
 * imgwrite.c — minimal PPM (P6) reader/writer, plus a PNG writer.
 *
 * The PPM half has no dependencies at all.  The PNG half is a thin wrapper
 * over the vendored stb_image_write (see imgwrite.h).
 *
 * build:  cc -O2 -g -Wall -Wextra -I. -Ithird_party/stb -c imgwrite.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgwrite.h"
#include "stb_image_write.h"

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

/* ------------------------------------------------------------------- PNG --- */

/* stb writes through a callback; send the bytes straight to the file. */
typedef struct {
    FILE *f;
    int   err;
} png_sink;

static void png_write(void *ctx, void *data, int size)
{
    png_sink *s = ctx;

    if (size <= 0 || s->err)
        return;
    if (fwrite(data, 1, (size_t)size, s->f) != (size_t)size)
        s->err = 1;
}

int dyt_write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    png_sink s;
    int      ok;

    if (!path || !rgb || w <= 0 || h <= 0)
        return -1;

    s.f = fopen(path, "wb");
    if (!s.f)
        return -1;
    s.err = 0;

    ok = stbi_write_png_to_func(png_write, &s, w, h, 3, rgb, w * 3);

    if (fclose(s.f) != 0)
        s.err = 1;

    return (ok && !s.err) ? 0 : -1;
}
