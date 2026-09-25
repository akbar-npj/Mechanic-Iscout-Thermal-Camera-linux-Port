/*
 * sr.c — the super-resolution policy helpers.  See sr.h for the reasoning.
 *
 * Pure buffer transforms: no runtime, no device, no allocation.  The model
 * call itself lives behind mnn.h and the buffers belong to the session.
 */
#include <stddef.h>

#include "sr.h"

const char *dyt_sr_name(dyt_sr_t m)
{
    switch (m) {
    case DYT_SR_OFF:     return "off";
    case DYT_SR_VISIBLE: return "visible";
    case DYT_SR_THERMAL: return "thermal";
    default:             return "?";
    }
}

int dyt_sr_thermal_grey(const float *temps, int n, float lo, float hi,
                        uint8_t *out)
{
    float span;
    int   i;

    if (!temps || !out || n <= 0)
        return -1;

    /* A flat frame has no span to normalise against, and `hi <= lo` is what
     * dyt_palette_index() also refuses to divide by.  Every sample becomes
     * the non-physical grey, so a flat frame reads as flat mid-grey rather
     * than as garbage from a division by zero. */
    if (!(hi > lo)) {
        for (i = 0; i < n; i++)
            out[i] = DYT_SR_NAN_GREY;
        return 0;
    }

    span = hi - lo;
    for (i = 0; i < n; i++) {
        float t = temps[i];
        float u;

        if (!(t == t)) {                 /* NaN: no reading */
            out[i] = DYT_SR_NAN_GREY;
            continue;
        }

        u = (t - lo) / span;
        if (!(u > 0.0f)) u = 0.0f;       /* also catches a NaN from the divide */
        if (u > 1.0f)    u = 1.0f;

        /* The same rounding dyt_palette_index() uses, so grey g and palette
         * index g are the same level and the round trip below is exact. */
        out[i] = (uint8_t)(u * 255.0f + 0.5f);
    }
    return 0;
}

int dyt_sr_grey_to_temps(const uint8_t *grey, int n, float lo, float hi,
                         float *out)
{
    float span;
    int   i;

    if (!grey || !out || n <= 0)
        return -1;

    if (!(hi > lo)) {
        for (i = 0; i < n; i++)
            out[i] = lo;
        return 0;
    }

    span = hi - lo;
    for (i = 0; i < n; i++)
        out[i] = lo + ((float)grey[i] / 255.0f) * span;

    return 0;
}

int dyt_sr_pack(const uint8_t *grey, int n, uint8_t *out)
{
    int i;

    if (!grey || !out || n <= 0)
        return -1;

    /* The model reads in[i*2] — the low byte of each little-endian uint16 —
     * so the plane goes in at the even offsets and the odd byte carries the
     * neutral chroma the model's +32768 bias restores on the way out.  Read
     * and written as bytes, so this states the layout instead of depending on
     * the host's endianness. */
    for (i = 0; i < n; i++) {
        out[2 * i]     = grey[i];
        out[2 * i + 1] = DYT_SR_PACK_HIGH;
    }
    return 0;
}

int dyt_sr_unpack(const uint8_t *packed, int n, uint8_t *out)
{
    int i;

    if (!packed || !out || n <= 0)
        return -1;

    for (i = 0; i < n; i++)
        out[i] = packed[2 * i];

    return 0;
}

int dyt_sr_nearest2(const uint8_t *in, int w, int h, uint8_t *out)
{
    int y;

    if (!in || !out || w <= 0 || h <= 0)
        return -1;

    /* Each source pixel becomes a 2x2 block, so the two output rows are the
     * same bytes duplicated. */
    for (y = 0; y < h; y++) {
        const uint8_t *src = in + (size_t)y * (size_t)w;
        uint8_t *r0 = out + (size_t)(2 * y)     * (size_t)(2 * w);
        uint8_t *r1 = out + (size_t)(2 * y + 1) * (size_t)(2 * w);
        int x;

        for (x = 0; x < w; x++) {
            r0[2 * x] = r0[2 * x + 1] = src[x];
            r1[2 * x] = r1[2 * x + 1] = src[x];
        }
    }
    return 0;
}

int dyt_sr_nearest2_f(const float *in, int w, int h, float *out)
{
    int y;

    if (!in || !out || w <= 0 || h <= 0)
        return -1;

    for (y = 0; y < h; y++) {
        const float *src = in + (size_t)y * (size_t)w;
        float *r0 = out + (size_t)(2 * y)     * (size_t)(2 * w);
        float *r1 = out + (size_t)(2 * y + 1) * (size_t)(2 * w);
        int x;

        for (x = 0; x < w; x++) {
            r0[2 * x] = r0[2 * x + 1] = src[x];
            r1[2 * x] = r1[2 * x + 1] = src[x];
        }
    }
    return 0;
}
