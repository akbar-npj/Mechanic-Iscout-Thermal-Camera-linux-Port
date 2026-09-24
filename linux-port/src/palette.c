/*
 * palette.c — palette loading and the temperature→RGB render core.
 *
 * See palette.h for the format and for why the temperature normalisation is
 * this port's own choice rather than a recovered one.  Deliberately free of
 * any GUI or image-library dependency: the viewer wraps the RGB buffer it
 * produces in a cv::Mat, and the tests write it out as a PPM.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c palette.c -o palette.o
 */
#include <stdio.h>
#include <string.h>

#include "palette.h"

/* --------------------------------------------------------------- loading */

int dyt_palette_load(dyt_palette_t *p, const char *path)
{
    FILE *f;
    size_t n;

    if (!p || !path)
        return -1;

    f = fopen(path, "rb");
    if (!f)
        return -1;

    n = fread(p->rgb, 1, sizeof p->rgb, f);
    /* Exactly one palette and nothing more.  A short read or a longer file
     * means the path is not a palette, and silently accepting it would paint
     * the image with whatever bytes happened to be there. */
    if (n != sizeof p->rgb || fgetc(f) != EOF) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Name it after the file's basename, for window titles. */
    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(p->name, sizeof p->name, "%s", base);
    }
    return 0;
}

/* ------------------------------------------------------------- built-ins */

/* Linear interpolation through a list of anchor colours (n × R,G,B). */
static void ramp(dyt_palette_t *p, const uint8_t *anchors, int n)
{
    int i, c;

    for (i = 0; i < DYT_PALETTE_N; i++) {
        float t = (float)i / (float)(DYT_PALETTE_N - 1) * (float)(n - 1);
        int   k = (int)t;
        float f = t - (float)k;
        const uint8_t *a, *b;

        if (k >= n - 1) { k = n - 2; f = 1.0f; }
        a = anchors + k * 3;
        b = anchors + (k + 1) * 3;

        for (c = 0; c < 3; c++)
            p->rgb[i * 3 + c] = (uint8_t)((float)a[c] +
                                          ((float)b[c] - (float)a[c]) * f + 0.5f);
    }
}

static const char *const builtin_names[DYT_PALETTE_BUILTIN_N] = {
    "iron-red", "rainbow", "red-hot", "black-hot", "white-hot", "cool-blue"
};

const char *dyt_palette_builtin_name(int id)
{
    if (id < 0 || id >= DYT_PALETTE_BUILTIN_N)
        return NULL;
    return builtin_names[id];
}

int dyt_palette_builtin(dyt_palette_t *p, int id)
{
    /* Anchor colours.  iron-red's anchors are the sampled values of the
     * vendor 1.dat from RE Docs 06 §1.2. */
    static const uint8_t iron[] = {
        0,0,0,  52,0,141,  141,0,157,  199,13,136,  231,69,24,
        245,120,0,  254,178,1,  255,227,44,  255,255,245
    };
    static const uint8_t rainbow[] = {
        0,0,128,  0,0,255,  0,255,255,  0,255,0,  255,255,0,  255,0,0
    };
    static const uint8_t red_hot[] = {
        0,0,0,  128,0,0,  255,0,0,  255,160,0,  255,255,0,  255,255,255
    };
    static const uint8_t black_hot[] = { 255,255,255,  0,0,0 };
    static const uint8_t white_hot[] = { 0,0,0,  255,255,255 };
    static const uint8_t cool_blue[] = {
        0,0,0,  0,0,160,  0,140,255,  120,220,255,  255,255,255
    };

    if (!p || id < 0 || id >= DYT_PALETTE_BUILTIN_N)
        return -1;

    switch (id) {
      case 0: ramp(p, iron,       (int)(sizeof iron      / 3)); break;
      case 1: ramp(p, rainbow,    (int)(sizeof rainbow   / 3)); break;
      case 2: ramp(p, red_hot,    (int)(sizeof red_hot   / 3)); break;
      case 3: ramp(p, black_hot,  (int)(sizeof black_hot / 3)); break;
      case 4: ramp(p, white_hot,  (int)(sizeof white_hot / 3)); break;
      default: ramp(p, cool_blue, (int)(sizeof cool_blue / 3)); break;
    }

    snprintf(p->name, sizeof p->name, "%s (builtin)", builtin_names[id]);
    return 0;
}

/* ------------------------------------------------------------ rendering */

int dyt_palette_index(float t, float lo, float hi)
{
    float u;

    if (hi <= lo)
        return (DYT_PALETTE_N - 1) / 2;   /* degenerate range: no division */

    u = (t - lo) / (hi - lo);
    if (!(u > 0.0f)) return 0;            /* also catches NaN */
    if (u >= 1.0f)   return DYT_PALETTE_N - 1;
    return (int)(u * (float)(DYT_PALETTE_N - 1) + 0.5f);
}

int dyt_render_rgb(const float *temps, int w, int h,
                   const dyt_palette_t *p, float lo, float hi,
                   uint8_t *out_rgb)
{
    int n, k;

    if (!temps || !p || !out_rgb || w <= 0 || h <= 0)
        return -1;

    n = w * h;
    for (k = 0; k < n; k++) {
        float t = temps[k];
        uint8_t *px = out_rgb + (size_t)k * 3;

        if (!(t == t)) {                  /* NaN: non-physical pixel */
            px[0] = px[1] = px[2] = 0x80;
            continue;
        }
        {
            int idx = dyt_palette_index(t, lo, hi);
            px[0] = p->rgb[idx * 3 + 0];
            px[1] = p->rgb[idx * 3 + 1];
            px[2] = p->rgb[idx * 3 + 2];
        }
    }
    return 0;
}

int dyt_render_minmax(const float *temps, int n, float *lo, float *hi)
{
    float a = 0.0f, b = 0.0f;
    int seen = 0, k;

    if (!temps || n <= 0)
        return -1;

    for (k = 0; k < n; k++) {
        float t = temps[k];
        if (!(t == t)) continue;          /* skip NaN */
        if (!seen) { a = b = t; seen = 1; }
        else {
            if (t < a) a = t;
            if (t > b) b = t;
        }
    }
    if (!seen)
        return -1;

    if (lo) *lo = a;
    if (hi) *hi = b;
    return 0;
}
